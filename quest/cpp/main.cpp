// Snes9x for Meta Quest: an OpenXR frontend that puts the SNES output on a
// curved screen floating in front of the player.
//
// Emulation runs on its own thread paced by the audio ring buffer, so the
// 60.1 Hz SNES refresh never stalls the 72/90 Hz compositor; the runtime's
// timewarp handles the mismatch.

#include "audio.h"
#include "emu.h"
#include "input.h"
#include "log.h"
#include "menu.h"
#include "renderer.h"
#include "screen.h"

#include <android/native_activity.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>

// Scoped storage hides non-media files in folders the app does not own, so a
// ROM folder anywhere but the app's own directory needs all-files access.
// There is no native API for either question, so both go through JNI.
bool HasAllFilesAccess(android_app *app)
{
	JNIEnv *env = nullptr;
	if (app->activity->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
		return false;

	jclass environment = env->FindClass("android/os/Environment");
	if (!environment)
	{
		env->ExceptionClear();
		return false;
	}

	jmethodID is_manager = env->GetStaticMethodID(environment, "isExternalStorageManager", "()Z");
	if (!is_manager)
	{
		env->ExceptionClear();
		return false;   // before Android 11 there was no such thing
	}

	const bool granted = env->CallStaticBooleanMethod(environment, is_manager) == JNI_TRUE;
	env->DeleteLocalRef(environment);
	return granted;
}

// Opens the system panel where the player grants it.
void RequestAllFilesAccess(android_app *app)
{
	JNIEnv *env = nullptr;
	if (app->activity->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
		return;

	jclass settings = env->FindClass("android/provider/Settings");
	jclass uri_class = env->FindClass("android/net/Uri");
	jclass intent_class = env->FindClass("android/content/Intent");
	if (!settings || !uri_class || !intent_class)
	{
		env->ExceptionClear();
		return;
	}

	jfieldID action_id = env->GetStaticFieldID(
		settings, "ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION", "Ljava/lang/String;");
	if (!action_id)
	{
		env->ExceptionClear();
		LOGW("storage: this Android has no all-files access screen");
		return;
	}

	jobject action = env->GetStaticObjectField(settings, action_id);

	jmethodID parse = env->GetStaticMethodID(uri_class, "parse",
	                                         "(Ljava/lang/String;)Landroid/net/Uri;");
	jstring package = env->NewStringUTF("package:com.snes9x.vr");
	jobject uri = env->CallStaticObjectMethod(uri_class, parse, package);

	jmethodID ctor = env->GetMethodID(intent_class, "<init>",
	                                  "(Ljava/lang/String;Landroid/net/Uri;)V");
	jobject intent = env->NewObject(intent_class, ctor, action, uri);

	jclass activity = env->GetObjectClass(app->activity->clazz);
	jmethodID start = env->GetMethodID(activity, "startActivity",
	                                   "(Landroid/content/Intent;)V");
	env->CallVoidMethod(app->activity->clazz, start, intent);

	if (env->ExceptionCheck())
	{
		env->ExceptionClear();
		LOGW("storage: could not open the all-files access screen");
		return;
	}

	LOGI("storage: opened the all-files access screen");
}

// Creates the app's data directories; defined by the port glue.
void S9xVRMakeDirs(const std::string &base);

namespace {

constexpr int kSwapchainWidth = 2048;
constexpr int kSwapchainHeight = 1536;

// The backdrop is a flat colour, so it needs no resolution to speak of.
constexpr int kBackdropSize = 256;

// The options menu panel.
constexpr int kMenuWidth = 1024;
constexpr int kMenuHeight = 992;

struct App {
	android_app *android = nullptr;

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local_space = XR_NULL_HANDLE;
	XrSpace view_space = XR_NULL_HANDLE;
	// One screen swapchain per eye.  In mono only the first is used.
	XrSwapchain screen_swapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	std::vector<XrSwapchainImageOpenGLESKHR> screen_images[2];

	// A backdrop projection layer sits behind the screen.  Submitting only a
	// cylinder leaves the Quest shell sitting on its loading environment: the
	// compositor wants a projection layer describing the scene.
	XrSwapchain backdrop_swapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	std::vector<XrSwapchainImageOpenGLESKHR> backdrop_images[2];
	XrCompositionLayerProjectionView projection_views[2]{};

	XrSwapchain menu_swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageOpenGLESKHR> menu_images;
	std::vector<uint16_t> menu_pixels = std::vector<uint16_t>(kMenuWidth * kMenuHeight);

	XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
	bool session_running = false;
	bool resumed = false;
	bool exit_requested = false;
	bool have_cylinder_layer = false;
	bool have_refresh_rate = false;

	std::vector<float> refresh_rates;
	float display_rate = 0.0f;   // what the runtime is actually running at
	float desired_rate = 0.0f;
	int rate_chosen_for_fps = 0;

	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;
	EGLSurface egl_surface = EGL_NO_SURFACE;
	EGLConfig egl_config = nullptr;

	vr::ScreenGeometry screen;
	XrPosef screen_pose{{0, 0, 0, 1}, {0, 0, 0}};
	bool screen_placed = false;   // something is on screen
	bool screen_tracked = false;  // ...and it was placed from a real head pose

	uint64_t last_frame_serial = 0;
	bool dump_enabled = false;
	bool dump_mode7_only = false;
	uint64_t next_dump_serial = 0;
	bool last_frame_mode7 = false;
	std::string base_dir;
	std::string rom_dir;
	std::string bindings;
};

App g_app;

// --- helpers ---------------------------------------------------------------

bool Check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return true;

	char name[XR_MAX_RESULT_STRING_SIZE] = "";
	if (g_app.instance != XR_NULL_HANDLE)
		xrResultToString(g_app.instance, result, name);
	LOGE("xr: %s failed (%s)", what, name[0] ? name : std::to_string(result).c_str());
	return false;
}

XrPosef YawOnly(const XrPosef &pose)
{
	// Keep the screen upright: drop pitch and roll, keep the heading.
	const XrQuaternionf &q = pose.orientation;
	float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z),
	                       1.0f - 2.0f * (q.y * q.y + q.x * q.x));

	XrPosef result{};
	result.position = pose.position;
	result.orientation.x = 0.0f;
	result.orientation.y = std::sin(yaw * 0.5f);
	result.orientation.z = 0.0f;
	result.orientation.w = std::cos(yaw * 0.5f);
	return result;
}

std::string Format(const char *format, float value)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), format, value);
	return buffer;
}

std::string ConfigPath() { return g_app.base_dir + "/vr_screen.cfg"; }

std::string DefaultRomDir() { return g_app.base_dir + "/roms"; }

void LoadScreenConfig()
{
	g_app.rom_dir = DefaultRomDir();

	FILE *file = fopen(ConfigPath().c_str(), "r");
	if (!file)
	{
		// Nothing saved yet, so this is a first run: show the options menu so
		// the screen controls are not something you have to know about.
		menu::Toggle();
		return;
	}

	char line[1024];
	while (fgets(line, sizeof(line), file))
	{
		char *equals = strchr(line, '=');
		if (!equals)
			continue;

		*equals = '\0';
		std::string key = line;
		std::string value = equals + 1;

		while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
			value.pop_back();

		if (key == "radius")
			g_app.screen.radius = std::clamp(strtof(value.c_str(), nullptr),
			                                 vr::kMinRadius, vr::kMaxRadius);
		else if (key == "angle")
			g_app.screen.central_angle = std::clamp(strtof(value.c_str(), nullptr),
			                                        vr::kMinAngle, vr::kMaxAngle);
		else if (key == "stereo")
			g_app.screen.stereo = std::clamp(strtof(value.c_str(), nullptr),
			                                 0.0f, vr::kMaxStereo);
		else if (key == "split")
			g_app.screen.layer_split = atoi(value.c_str()) != 0;
		else if (key == "gamma")
			g_app.screen.gamma = std::clamp(strtof(value.c_str(), nullptr),
			                                vr::kMinGamma, vr::kMaxGamma);
		else if (key == "filter")
			g_app.screen.filter = std::clamp(atoi(value.c_str()), 0, vr::kFilterCount - 1);
		else if (key == "roms" && !value.empty())
			g_app.rom_dir = value;
		else if (key == "bindings")
			g_app.bindings = value;   // applied after input::Init sets defaults
	}

	fclose(file);
}

void SaveScreenConfig()
{
	FILE *file = fopen(ConfigPath().c_str(), "w");
	if (!file)
		return;

	fprintf(file, "radius=%f\n", g_app.screen.radius);
	fprintf(file, "angle=%f\n", g_app.screen.central_angle);
	fprintf(file, "stereo=%f\n", g_app.screen.stereo);
	fprintf(file, "split=%d\n", g_app.screen.layer_split ? 1 : 0);
	fprintf(file, "filter=%d\n", g_app.screen.filter);
	fprintf(file, "gamma=%f\n", g_app.screen.gamma);
	fprintf(file, "roms=%s\n", g_app.rom_dir.c_str());
	fprintf(file, "bindings=%s\n", input::SerialiseBindings().c_str());
	fclose(file);
}

// Subdirectories of `path`, for the folder browser.
std::vector<std::string> ListFolders(const std::string &path)
{
	DIR *dir = opendir(path.c_str());
	if (!dir)
	{
		LOGW("cannot open %s: %s", path.c_str(), strerror(errno));
		return {};
	}

	std::vector<std::string> names;
	while (dirent *entry = readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name == "." || name == "..")
			continue;

		// Some filesystems do not fill in d_type, so fall back to a stat.
		bool is_directory = entry->d_type == DT_DIR;
		if (entry->d_type == DT_UNKNOWN)
		{
			struct stat info;
			is_directory = stat((path + "/" + name).c_str(), &info) == 0 &&
			               S_ISDIR(info.st_mode);
		}

		if (is_directory)
			names.push_back(name);
	}
	closedir(dir);

	std::sort(names.begin(), names.end());
	return names;
}

std::vector<std::string> ListRoms(const std::string &rom_dir)
{
	DIR *dir = opendir(rom_dir.c_str());
	if (!dir)
	{
		LOGE("cannot open %s: %s", rom_dir.c_str(), strerror(errno));
		return {};
	}

	std::vector<std::string> names;
	while (dirent *entry = readdir(dir))
	{
		std::string name = entry->d_name;
		size_t dot = name.rfind('.');
		if (dot == std::string::npos)
			continue;

		std::string ext = name.substr(dot);
		for (char &c : ext)
			c = tolower(c);

		if (ext == ".sfc" || ext == ".smc" || ext == ".fig" || ext == ".swc" ||
		    ext == ".zip")
			names.push_back(name);
	}
	closedir(dir);

	std::sort(names.begin(), names.end());
	return names;
}

// --- EGL -------------------------------------------------------------------

bool InitEGL()
{
	g_app.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_app.egl_display == EGL_NO_DISPLAY || !eglInitialize(g_app.egl_display, nullptr, nullptr))
	{
		LOGE("egl: cannot initialise display");
		return false;
	}

	const EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
		EGL_NONE
	};

	EGLint config_count = 0;
	if (!eglChooseConfig(g_app.egl_display, config_attribs, &g_app.egl_config, 1, &config_count) ||
	    config_count == 0)
	{
		LOGE("egl: no suitable config");
		return false;
	}

	const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	g_app.egl_context = eglCreateContext(g_app.egl_display, g_app.egl_config,
	                                     EGL_NO_CONTEXT, context_attribs);
	if (g_app.egl_context == EGL_NO_CONTEXT)
	{
		LOGE("egl: cannot create context");
		return false;
	}

	const EGLint surface_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	g_app.egl_surface = eglCreatePbufferSurface(g_app.egl_display, g_app.egl_config,
	                                            surface_attribs);
	if (g_app.egl_surface == EGL_NO_SURFACE ||
	    !eglMakeCurrent(g_app.egl_display, g_app.egl_surface, g_app.egl_surface,
	                    g_app.egl_context))
	{
		LOGE("egl: cannot make context current");
		return false;
	}

	return true;
}

// --- OpenXR ----------------------------------------------------------------

bool InitLoader(android_app *app)
{
	PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	                                    reinterpret_cast<PFN_xrVoidFunction *>(&initialize_loader))))
	{
		LOGE("xr: loader does not expose xrInitializeLoaderKHR");
		return false;
	}

	XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	init.applicationVM = app->activity->vm;
	init.applicationContext = app->activity->clazz;

	return Check(initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&init)),
	             "xrInitializeLoaderKHR");
}

bool HasExtension(const std::vector<XrExtensionProperties> &available, const char *name)
{
	for (const auto &extension : available)
		if (strcmp(extension.extensionName, name) == 0)
			return true;
	return false;
}

bool InitInstance(android_app *app)
{
	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
	std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());

	std::vector<const char *> extensions = {
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	};

	g_app.have_cylinder_layer =
		HasExtension(available, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
	if (g_app.have_cylinder_layer)
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
	else
		LOGW("xr: no cylinder layer support, falling back to a flat screen");

	g_app.have_refresh_rate = HasExtension(available, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (g_app.have_refresh_rate)
		extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	else
		LOGW("xr: cannot choose a display rate, judder will depend on the default");

	XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	create.next = &android_info;
	create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	create.enabledExtensionNames = extensions.data();
	strcpy(create.applicationInfo.applicationName, "Snes9x VR");
	create.applicationInfo.applicationVersion = 1;
	strcpy(create.applicationInfo.engineName, "Snes9x");
	create.applicationInfo.engineVersion = 1;
	create.applicationInfo.apiVersion = XR_API_VERSION_1_0;

	if (!Check(xrCreateInstance(&create, &g_app.instance), "xrCreateInstance"))
		return false;

	XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	return Check(xrGetSystem(g_app.instance, &system_info, &g_app.system), "xrGetSystem");
}

bool InitSession()
{
	// Required before session creation, even though we ignore the result.
	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
	xrGetInstanceProcAddr(g_app.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction *>(&get_requirements));
	if (get_requirements)
	{
		XrGraphicsRequirementsOpenGLESKHR requirements{
			XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
		get_requirements(g_app.instance, g_app.system, &requirements);
	}

	XrGraphicsBindingOpenGLESAndroidKHR binding{
		XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	binding.display = g_app.egl_display;
	binding.config = g_app.egl_config;
	binding.context = g_app.egl_context;

	XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
	create.next = &binding;
	create.systemId = g_app.system;

	if (!Check(xrCreateSession(g_app.instance, &create, &g_app.session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space.poseInReferenceSpace = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};

	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!Check(xrCreateReferenceSpace(g_app.session, &space, &g_app.local_space),
	           "xrCreateReferenceSpace(LOCAL)"))
		return false;

	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!Check(xrCreateReferenceSpace(g_app.session, &space, &g_app.view_space),
	           "xrCreateReferenceSpace(VIEW)"))
		return false;

	return true;
}

bool InitSwapchain()
{
	uint32_t format_count = 0;
	xrEnumerateSwapchainFormats(g_app.session, 0, &format_count, nullptr);
	std::vector<int64_t> formats(format_count);
	xrEnumerateSwapchainFormats(g_app.session, format_count, &format_count, formats.data());

	// An sRGB target, because the runtime reads a plain RGBA8 one as linear
	// and encodes it again on the way to the display, which lifts every
	// midtone. The stored bits end up the same either way -- the shaders undo
	// the hardware's encode -- but this way the compositor is told what they
	// are.
	constexpr int64_t kSrgb8Alpha8 = 0x8C43;

	int64_t chosen = formats.empty() ? kSrgb8Alpha8 : formats[0];
	for (int64_t format : formats)
		if (format == kSrgb8Alpha8)
		{
			chosen = format;
			break;
		}

	XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	create.format = chosen;
	create.sampleCount = 1;
	create.width = kSwapchainWidth;
	create.height = kSwapchainHeight;
	create.faceCount = 1;
	create.arraySize = 1;
	create.mipCount = 1;

	uint32_t image_count = 0;
	for (int eye = 0; eye < 2; eye++)
	{
		if (!Check(xrCreateSwapchain(g_app.session, &create, &g_app.screen_swapchain[eye]),
		           "xrCreateSwapchain(screen)"))
			return false;

		xrEnumerateSwapchainImages(g_app.screen_swapchain[eye], 0, &image_count, nullptr);
		g_app.screen_images[eye].resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
		xrEnumerateSwapchainImages(
			g_app.screen_swapchain[eye], image_count, &image_count,
			reinterpret_cast<XrSwapchainImageBaseHeader *>(g_app.screen_images[eye].data()));
	}

	LOGI("xr: screen swapchains %dx%d, %u images each, format 0x%llx",
	     kSwapchainWidth, kSwapchainHeight, image_count, (unsigned long long) chosen);

	// Backdrop swapchains.  These only ever hold a flat colour, so a small
	// image is plenty no matter what the runtime recommends.
	uint32_t view_count = 0;
	xrEnumerateViewConfigurationViews(g_app.instance, g_app.system,
	                                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                  0, &view_count, nullptr);
	if (view_count != 2)
	{
		LOGE("xr: expected a stereo view configuration, got %u views", view_count);
		return false;
	}

	for (int eye = 0; eye < 2; eye++)
	{
		XrSwapchainCreateInfo eye_create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		eye_create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		eye_create.format = chosen;
		eye_create.sampleCount = 1;
		eye_create.width = kBackdropSize;
		eye_create.height = kBackdropSize;
		eye_create.faceCount = 1;
		eye_create.arraySize = 1;
		eye_create.mipCount = 1;

		if (!Check(xrCreateSwapchain(g_app.session, &eye_create, &g_app.backdrop_swapchain[eye]),
		           "xrCreateSwapchain(backdrop)"))
			return false;

		uint32_t count = 0;
		xrEnumerateSwapchainImages(g_app.backdrop_swapchain[eye], 0, &count, nullptr);
		g_app.backdrop_images[eye].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
		xrEnumerateSwapchainImages(
			g_app.backdrop_swapchain[eye], count, &count,
			reinterpret_cast<XrSwapchainImageBaseHeader *>(g_app.backdrop_images[eye].data()));
	}

	XrSwapchainCreateInfo menu_create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	menu_create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	menu_create.format = chosen;
	menu_create.sampleCount = 1;
	menu_create.width = kMenuWidth;
	menu_create.height = kMenuHeight;
	menu_create.faceCount = 1;
	menu_create.arraySize = 1;
	menu_create.mipCount = 1;

	if (!Check(xrCreateSwapchain(g_app.session, &menu_create, &g_app.menu_swapchain),
	           "xrCreateSwapchain(menu)"))
		return false;

	uint32_t menu_count = 0;
	xrEnumerateSwapchainImages(g_app.menu_swapchain, 0, &menu_count, nullptr);
	g_app.menu_images.resize(menu_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
	xrEnumerateSwapchainImages(
		g_app.menu_swapchain, menu_count, &menu_count,
		reinterpret_cast<XrSwapchainImageBaseHeader *>(g_app.menu_images.data()));

	return true;
}

// The SNES runs at 60 or 50 frames a second and cannot be made to run at
// anything else without changing the speed of the game.  So instead of forcing
// the emulator to the headset's rate, the headset is asked for a rate the
// emulator divides into: at 120 Hz an NTSC game gets exactly two display
// frames per emulated frame, and scrolling stops stuttering.  Anything else
// repeats frames on an uneven cadence, which is what shows up as judder.
float ScoreRefreshRate(float rate, int fps)
{
	const float ratio = rate / static_cast<float>(fps);
	const float nearest = std::round(ratio);

	if (nearest < 1.0f)
		return -1.0f;   // slower than the game: frames would be dropped

	// How far off a whole number of display frames per emulated frame.
	const float error = std::fabs(ratio - nearest);

	// A clean multiple is worth far more than a high rate, but among equally
	// clean rates the faster one reprojects head motion more smoothly.
	return (1.0f - error * 8.0f) * 100.0f + rate * 0.1f;
}

void ChooseRefreshRate(int fps)
{
	if (!g_app.have_refresh_rate || g_app.refresh_rates.empty())
		return;

	PFN_xrRequestDisplayRefreshRateFB request = nullptr;
	xrGetInstanceProcAddr(g_app.instance, "xrRequestDisplayRefreshRateFB",
						  reinterpret_cast<PFN_xrVoidFunction *>(&request));
	if (!request)
		return;

	float best = 0.0f;
	float best_score = -1e9f;

	for (float rate : g_app.refresh_rates)
	{
		const float score = ScoreRefreshRate(rate, fps);
		if (score > best_score)
		{
			best_score = score;
			best = rate;
		}
	}

	g_app.rate_chosen_for_fps = fps;

	if (best <= 0.0f || best == g_app.desired_rate)
		return;

	g_app.desired_rate = best;

	const XrResult result = request(g_app.session, best);
	if (XR_SUCCEEDED(result))
	{
		g_app.display_rate = best;
		LOGI("xr: asked for %.1f Hz for a %d fps game (%.2f frames each)",
			 best, fps, best / fps);
	}
	else
	{
		LOGW("xr: runtime refused %.1f Hz (%d)", best, result);
	}

	// What the runtime actually gave us, which is not always what was asked
	// for: 120 Hz has to be enabled in the headset's own display settings.
	PFN_xrGetDisplayRefreshRateFB current = nullptr;
	xrGetInstanceProcAddr(g_app.instance, "xrGetDisplayRefreshRateFB",
	                      reinterpret_cast<PFN_xrVoidFunction *>(&current));
	if (current)
	{
		float actual = 0.0f;
		if (XR_SUCCEEDED(current(g_app.session, &actual)))
			g_app.display_rate = actual;
	}
}

// The runtime accepts the request and applies it whenever it is ready, so the
// rate has to be checked rather than assumed.  Asking again costs nothing when
// it is already right, and covers the case where the system moved it back.
void EnsureRefreshRate()
{
	if (!g_app.have_refresh_rate || g_app.desired_rate <= 0.0f)
		return;

	PFN_xrGetDisplayRefreshRateFB current = nullptr;
	xrGetInstanceProcAddr(g_app.instance, "xrGetDisplayRefreshRateFB",
	                      reinterpret_cast<PFN_xrVoidFunction *>(&current));
	if (!current)
		return;

	float actual = 0.0f;
	if (XR_FAILED(current(g_app.session, &actual)))
		return;

	if (actual != g_app.display_rate)
	{
		LOGI("xr: display now running at %.1f Hz", actual);
		g_app.display_rate = actual;
	}

	if (actual == g_app.desired_rate)
		return;

	PFN_xrRequestDisplayRefreshRateFB request = nullptr;
	xrGetInstanceProcAddr(g_app.instance, "xrRequestDisplayRefreshRateFB",
	                      reinterpret_cast<PFN_xrVoidFunction *>(&request));
	if (request)
		request(g_app.session, g_app.desired_rate);
}

void QueryRefreshRates()
{
	if (!g_app.have_refresh_rate)
		return;

	PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
	xrGetInstanceProcAddr(g_app.instance, "xrEnumerateDisplayRefreshRatesFB",
						  reinterpret_cast<PFN_xrVoidFunction *>(&enumerate));
	if (!enumerate)
		return;

	uint32_t count = 0;
	enumerate(g_app.session, 0, &count, nullptr);
	g_app.refresh_rates.resize(count);
	enumerate(g_app.session, count, &count, g_app.refresh_rates.data());

	std::string list;
	for (float rate : g_app.refresh_rates)
		list += Format("%.0f ", rate);
	LOGI("xr: display rates available: %s", list.c_str());
}

void PlaceScreen(XrTime time)
{
	constexpr XrSpaceLocationFlags kNeeded =
		XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	const bool located =
		XR_SUCCEEDED(xrLocateSpace(g_app.view_space, g_app.local_space, time, &location)) &&
		(location.locationFlags & kNeeded) == kNeeded;

	if (located)
	{
		g_app.screen_pose = YawOnly(location.pose);
		g_app.screen_placed = true;
		g_app.screen_tracked = true;
		LOGI("screen placed at (%.2f %.2f %.2f)", g_app.screen_pose.position.x,
		     g_app.screen_pose.position.y, g_app.screen_pose.position.z);
		return;
	}

	if (g_app.screen_placed)
		return;

	// Head tracking is not up yet, which happens while the headset is still
	// being put on.  Show the screen at the space origin rather than
	// submitting nothing: an empty layer list is an indistinguishable black
	// void, and it gets corrected as soon as a real pose arrives.
	g_app.screen_pose = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
	g_app.screen_placed = true;
	LOGW("head pose not tracked (flags 0x%llx); screen parked at the origin",
	     (unsigned long long) location.locationFlags);
}

// Rotates a vector by a quaternion, for placing the menu panel relative to the
// screen.
XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	const float x = q.y * v.z - q.z * v.y + q.w * v.x;
	const float y = q.z * v.x - q.x * v.z + q.w * v.y;
	const float z = q.x * v.y - q.y * v.x + q.w * v.z;

	return {
		v.x + 2.0f * (q.y * z - q.z * y),
		v.y + 2.0f * (q.z * x - q.x * z),
		v.z + 2.0f * (q.x * y - q.y * x),
	};
}

// Fills the two backdrop swapchains and fills in the projection views.
// Returns false if the views could not be located this frame.
bool RenderBackdrop(XrTime predicted_time)
{
	XrViewState view_state{XR_TYPE_VIEW_STATE};
	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = predicted_time;
	locate.space = g_app.local_space;

	uint32_t view_count = 0;
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	if (XR_FAILED(xrLocateViews(g_app.session, &locate, &view_state, 2, &view_count, views)))
		return false;

	constexpr XrViewStateFlags kNeeded =
		XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	if ((view_state.viewStateFlags & kNeeded) != kNeeded || view_count != 2)
		return false;

	for (int eye = 0; eye < 2; eye++)
	{
		uint32_t index = 0;
		if (XR_FAILED(xrAcquireSwapchainImage(g_app.backdrop_swapchain[eye], nullptr, &index)))
			return false;

		XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait.timeout = XR_INFINITE_DURATION;
		if (XR_FAILED(xrWaitSwapchainImage(g_app.backdrop_swapchain[eye], &wait)))
			return false;

		// A dim neutral surround: dark enough to let the screen carry the
		// image, not so black that the room disappears entirely.
		renderer::ClearSwapchainImage(g_app.backdrop_images[eye][index].image,
		                              kBackdropSize, kBackdropSize,
		                              renderer::SrgbToLinear(0.02f),
		                              renderer::SrgbToLinear(0.02f),
		                              renderer::SrgbToLinear(0.03f));

		xrReleaseSwapchainImage(g_app.backdrop_swapchain[eye], nullptr);

		XrCompositionLayerProjectionView &projection = g_app.projection_views[eye];
		projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		projection.next = nullptr;
		projection.pose = views[eye].pose;
		projection.fov = views[eye].fov;
		projection.subImage.swapchain = g_app.backdrop_swapchain[eye];
		projection.subImage.imageRect = {{0, 0}, {kBackdropSize, kBackdropSize}};
		projection.subImage.imageArrayIndex = 0;
	}

	return true;
}

// Writes the composite of the frame the renderer is about to draw, so an eye
// capture can be compared against exactly the picture it came from.
void DumpFrameComposite(const emu::Frame &frame, const std::string &path)
{
	FILE *file = fopen(path.c_str(), "wb");
	if (!file)
		return;

	fprintf(file, "P6\n%d %d\n255\n", frame.width, frame.height);
	for (int y = 0; y < frame.height; y++)
		for (int x = 0; x < frame.width; x++)
		{
			const uint16_t pixel = frame.pixels[y * frame.pitch_pixels + x];
			const uint8_t rgb[3] = {
				static_cast<uint8_t>((((pixel >> 11) & 0x1f) * 255 + 15) / 31),
				static_cast<uint8_t>((((pixel >> 5) & 0x3f) * 255 + 31) / 63),
				static_cast<uint8_t>(((pixel & 0x1f) * 255 + 15) / 31),
			};
			fwrite(rgb, 1, 3, file);
		}

	fclose(file);
}

bool RenderMenu()
{
	if (!menu::IsOpen())
		return false;

	uint32_t index = 0;
	if (XR_FAILED(xrAcquireSwapchainImage(g_app.menu_swapchain, nullptr, &index)))
		return false;

	XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(g_app.menu_swapchain, &wait)))
		return false;

	if (menu::NeedsRedraw())
	{
		menu::Draw(g_app.screen, g_app.menu_pixels.data(), kMenuWidth, kMenuHeight);
		renderer::UploadOverlay(g_app.menu_pixels.data(), kMenuWidth, kMenuHeight);
	}

	renderer::DrawOverlay(g_app.menu_images[index].image, kMenuWidth, kMenuHeight);
	xrReleaseSwapchainImage(g_app.menu_swapchain, nullptr);
	return true;
}

// Renders the screen, one pass per eye when stereo is on.  Returns the number
// of eyes actually drawn.
int RenderFrame()
{
	emu::Frame frame;
	if (emu::AcquireFrame(frame, g_app.last_frame_serial))
	{
		renderer::UploadFrame(frame);
		g_app.last_frame_serial = frame.serial;

		g_app.last_frame_mode7 = frame.mode7;

		if (g_app.dump_enabled && frame.serial >= g_app.next_dump_serial &&
		    (!g_app.dump_mode7_only || frame.mode7))
		{
			g_app.next_dump_serial = frame.serial + 600;
			DumpFrameComposite(frame, g_app.base_dir + "/eye_frame.ppm");
			renderer::RequestEyeDump(g_app.base_dir + "/eye");
			LOGI("eye dump at emu frame %llu, layered=%d mode7=%d",
			     (unsigned long long) frame.serial, (int) frame.layered,
			     (int) frame.mode7);
		}
	}

	const bool stereo = g_app.screen.stereo > 0.0f;
	const int eyes = stereo ? 2 : 1;

	for (int eye = 0; eye < eyes; eye++)
	{
		uint32_t index = 0;
		if (XR_FAILED(xrAcquireSwapchainImage(g_app.screen_swapchain[eye], nullptr, &index)))
			return eye;

		XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait.timeout = XR_INFINITE_DURATION;
		if (XR_FAILED(xrWaitSwapchainImage(g_app.screen_swapchain[eye], &wait)))
			return eye;

		// The left eye sees far layers displaced left and the right eye sees
		// them displaced right, which puts them behind the screen plane.
		// Half the separation each, as a fraction of the screen's width.
		const float screen_width = g_app.screen.radius * g_app.screen.central_angle;
		const float fraction = g_app.screen.stereo * 0.5f / screen_width;
		const float shift = stereo ? (eye == 0 ? fraction : -fraction) : 0.0f;

		renderer::DrawEye(g_app.screen_images[eye][index].image,
		                  kSwapchainWidth, kSwapchainHeight, shift);

		xrReleaseSwapchainImage(g_app.screen_swapchain[eye], nullptr);
	}

	return eyes;
}

void SubmitFrame(XrTime predicted_time, int eyes_drawn, bool have_backdrop,
                 bool have_menu)
{
	XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	projection.layerFlags = 0;
	projection.space = g_app.local_space;
	projection.viewCount = 2;
	projection.views = g_app.projection_views;

	const float width = g_app.screen.radius * g_app.screen.central_angle;

	XrCompositionLayerCylinderKHR cylinder[2];
	XrCompositionLayerQuad quad[2];

	const XrEyeVisibility visibility[2] = {XR_EYE_VISIBILITY_LEFT, XR_EYE_VISIBILITY_RIGHT};

	const XrCompositionLayerBaseHeader *layers[4];
	uint32_t layer_count = 0;

	// Backdrop first, screen on top.
	if (have_backdrop)
		layers[layer_count++] =
			reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);

	for (int eye = 0; eye < eyes_drawn; eye++)
	{
		XrSwapchainSubImage sub_image{};
		sub_image.swapchain = g_app.screen_swapchain[eye];
		sub_image.imageRect = {{0, 0}, {kSwapchainWidth, kSwapchainHeight}};
		sub_image.imageArrayIndex = 0;

		cylinder[eye] = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
		cylinder[eye].layerFlags = 0;
		cylinder[eye].space = g_app.local_space;
		cylinder[eye].eyeVisibility = eyes_drawn == 2 ? visibility[eye] : XR_EYE_VISIBILITY_BOTH;
		cylinder[eye].subImage = sub_image;
		cylinder[eye].pose = g_app.screen_pose;
		cylinder[eye].radius = g_app.screen.radius;
		cylinder[eye].centralAngle = g_app.screen.central_angle;
		cylinder[eye].aspectRatio = g_app.screen.aspect;

		// Same screen, flat, when the runtime has no cylinder layers.
		quad[eye] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
		quad[eye].layerFlags = 0;
		quad[eye].space = g_app.local_space;
		quad[eye].eyeVisibility = cylinder[eye].eyeVisibility;
		quad[eye].subImage = sub_image;
		quad[eye].pose = g_app.screen_pose;
		quad[eye].pose.position.z -= g_app.screen.radius;
		quad[eye].size = {width, width / g_app.screen.aspect};

		layers[layer_count++] = g_app.have_cylinder_layer
			? reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder[eye])
			: reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad[eye]);
	}

	// The menu panel floats between the player and the screen.
	XrCompositionLayerQuad menu_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	if (have_menu)
	{
		const float distance = std::min(1.8f, g_app.screen.radius * 0.5f);
		const float panel_width = 1.1f;

		menu_layer.layerFlags = 0;
		menu_layer.space = g_app.local_space;
		menu_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		menu_layer.subImage.swapchain = g_app.menu_swapchain;
		menu_layer.subImage.imageRect = {{0, 0}, {kMenuWidth, kMenuHeight}};
		menu_layer.subImage.imageArrayIndex = 0;
		menu_layer.pose = g_app.screen_pose;

		const XrVector3f forward =
			Rotate(g_app.screen_pose.orientation, {0.0f, 0.0f, -distance});
		menu_layer.pose.position.x += forward.x;
		menu_layer.pose.position.y += forward.y;
		menu_layer.pose.position.z += forward.z;

		menu_layer.size = {panel_width,
		                   panel_width * kMenuHeight / static_cast<float>(kMenuWidth)};

		layers[layer_count++] =
			reinterpret_cast<const XrCompositionLayerBaseHeader *>(&menu_layer);
	}

	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
	end.displayTime = predicted_time;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = layer_count;
	end.layers = layers;

	xrEndFrame(g_app.session, &end);
}

void HandleSessionStateChange(XrSessionState state)
{
	LOGI("xr: session state %d", (int) state);
	g_app.session_state = state;

	switch (state)
	{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
			begin.primaryViewConfigurationType =
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if (Check(xrBeginSession(g_app.session, &begin), "xrBeginSession"))
			{
				g_app.session_running = true;
				emu::SetPaused(false);
			}
			break;
		}

		case XR_SESSION_STATE_FOCUSED:
			// Put the screen in front of wherever the player is actually
			// looking now.  Placing it on the first frame instead pins it to
			// whatever direction the headset happened to face while it was
			// still being picked up.
			g_app.screen_placed = false;
			g_app.screen_tracked = false;
			break;

		case XR_SESSION_STATE_STOPPING:
			emu::SetPaused(true);
			g_app.session_running = false;
			xrEndSession(g_app.session);
			break;

		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			g_app.exit_requested = true;
			break;

		default:
			break;
	}
}

void PollXrEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

	while (true)
	{
		event = {XR_TYPE_EVENT_DATA_BUFFER};
		if (xrPollEvent(g_app.instance, &event) != XR_SUCCESS)
			break;

		switch (event.type)
		{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				HandleSessionStateChange(
					reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state);
				break;

			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				g_app.exit_requested = true;
				break;

			default:
				break;
		}
	}
}

// --- Android glue ----------------------------------------------------------

void OnAppCmd(android_app *app, int32_t cmd)
{
	(void) app;

	switch (cmd)
	{
		case APP_CMD_RESUME:
			g_app.resumed = true;
			break;

		case APP_CMD_PAUSE:
			g_app.resumed = false;
			emu::SetPaused(true);
			break;

		case APP_CMD_DESTROY:
			g_app.exit_requested = true;
			break;

		default:
			break;
	}
}

int32_t OnInputEvent(android_app *app, AInputEvent *event)
{
	(void) app;

	const int32_t type = AInputEvent_getType(event);
	const int32_t source = AInputEvent_getSource(event);

	if (type == AINPUT_EVENT_TYPE_KEY)
	{
		const int32_t action = AKeyEvent_getAction(event);
		if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP)
			return 0;

		return input::HandleAndroidKey(AKeyEvent_getKeyCode(event),
		                               action == AKEY_EVENT_ACTION_DOWN) ? 1 : 0;
	}

	if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_JOYSTICK))
	{
		input::HandleAndroidAxis(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0),
		                         AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
		return 1;
	}

	return 0;
}

void PumpAndroidEvents(bool blocking)
{
	int events = 0;
	android_poll_source *source = nullptr;

	while (ALooper_pollOnce(blocking ? -1 : 0, nullptr, &events,
	                        reinterpret_cast<void **>(&source)) >= 0)
	{
		if (source)
			source->process(g_app.android, source);

		if (g_app.android->destroyRequested)
		{
			g_app.exit_requested = true;
			return;
		}

		blocking = false;
	}
}

// Tears everything down and ends the process.  The process really does have to
// go: the Snes9x core keeps global state that was never built to be torn down
// and brought back up in place, and if the activity is recreated while we are
// still alive, native_app_glue starts a second android_main on top of the
// first -- two OpenXR instances and two emulators sharing one set of globals,
// which is what leaves the shell stuck on its loading screen.
[[noreturn]] void Shutdown(android_app *app)
{
	emu::Stop();
	audio::Stop();
	renderer::Shutdown();
	input::Shutdown();

	for (XrSwapchain screen : g_app.screen_swapchain)
		if (screen != XR_NULL_HANDLE)
			xrDestroySwapchain(screen);
	if (g_app.menu_swapchain != XR_NULL_HANDLE)
		xrDestroySwapchain(g_app.menu_swapchain);
	for (XrSwapchain eye : g_app.backdrop_swapchain)
		if (eye != XR_NULL_HANDLE)
			xrDestroySwapchain(eye);
	if (g_app.session != XR_NULL_HANDLE)
		xrDestroySession(g_app.session);
	if (g_app.instance != XR_NULL_HANDLE)
		xrDestroyInstance(g_app.instance);

	app->activity->vm->DetachCurrentThread();
	ANativeActivity_finish(app->activity);

	_exit(0);
}

} // namespace

void android_main(android_app *app)
{
	g_app.android = app;
	app->onAppCmd = OnAppCmd;
	app->onInputEvent = OnInputEvent;

	JNIEnv *env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);

	const char *external = app->activity->externalDataPath;
	g_app.base_dir = external ? external : app->activity->internalDataPath;
	LOGI("data directory: %s", g_app.base_dir.c_str());

	// Has to happen before we go looking for a ROM: a directory created by
	// "adb shell mkdir" belongs to the shell user and the app cannot read it.
	S9xVRMakeDirs(g_app.base_dir);
// Repeats rather than firing once: a game usually needs to be left alone
	// for a while before it shows the thing worth capturing, such as Mario
	// Kart's attract demo.
	g_app.dump_enabled = access((g_app.base_dir + "/dump").c_str(), F_OK) == 0;
	g_app.next_dump_serial = 120;

	// A "dump7" marker holds the capture back until the game is actually in
	// Mode 7, which for most games is a small part of the time.
	g_app.dump_mode7_only = access((g_app.base_dir + "/dump7").c_str(), F_OK) == 0;

	LoadScreenConfig();

	if (!InitLoader(app) || !InitInstance(app) || !InitEGL() ||
	    !InitSession() || !InitSwapchain() || !renderer::Init())
	{
		LOGE("startup failed");
		Shutdown(app);
	}

	QueryRefreshRates();

	if (!input::Init(g_app.instance, g_app.session))
		LOGW("input: controllers unavailable, gamepad only");

	// Init lays down the defaults, so a saved mapping goes on after it.
	if (!g_app.bindings.empty())
		input::ParseBindings(g_app.bindings);

	const std::vector<std::string> roms = ListRoms(g_app.rom_dir);
	menu::SetRomList(roms);
	menu::SetRomDir(g_app.rom_dir);
	menu::SetStorageAccess(HasAllFilesAccess(app));

	// Nothing is loaded on startup: picking a game is the player's first move,
	// not something to be guessed at alphabetically.  With no ROMs at all the
	// same screen is still the way to grant storage access or point the ROM
	// folder somewhere readable, so a bad setting cannot lock the app up.
	if (roms.empty())
		LOGW("no ROM in %s", g_app.rom_dir.c_str());
	else
		LOGI("%d ROMs available", (int) roms.size());

	menu::OpenRomList();

	if (!audio::Start() || !emu::Start(g_app.base_dir, std::string()))
	{
		LOGE("emulator failed to start");
		Shutdown(app);
	}

	XrTime previous_time = 0;

	while (!g_app.exit_requested)
	{
		PumpAndroidEvents(!g_app.resumed && !g_app.session_running);
		PollXrEvents();

		if (!g_app.session_running)
			continue;

		XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frame_state{XR_TYPE_FRAME_STATE};
		if (XR_FAILED(xrWaitFrame(g_app.session, &wait_info, &frame_state)))
			continue;

		XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
		xrBeginFrame(g_app.session, &begin_info);

		input::Controls controls;

		// Clamped: the session restarts across a sleep/wake cycle, and the
		// gap in predicted display times would otherwise land as one huge
		// step on whatever the sticks happen to be resting at.
		float delta_seconds =
			previous_time ? (frame_state.predictedDisplayTime - previous_time) * 1e-9f : 0.0f;
		delta_seconds = std::clamp(delta_seconds, 0.0f, 0.1f);
		previous_time = frame_state.predictedDisplayTime;

		input::Sync(g_app.session, menu::IsOpen(), delta_seconds, controls);

		if (controls.menu_toggle)
		{
			// Rescan on the way in, so ROMs pushed while the app was running
			// show up without a restart.
			if (!menu::IsOpen())
			{
				menu::SetRomList(ListRoms(g_app.rom_dir));
				menu::SetStorageAccess(HasAllFilesAccess(app));
			}
			menu::Toggle();
		}

		const menu::Action action =
			menu::Update(g_app.screen, controls.menu_vertical, controls.menu_horizontal,
			             controls.menu_activate, delta_seconds);

		if (g_app.screen.layer_split != emu::LayerSplitEnabled())
			emu::SetLayerSplit(g_app.screen.layer_split);

		renderer::SetFilter(g_app.screen.filter);
		renderer::SetGamma(g_app.screen.gamma);

		// Re-picked when the cartridge changes, since PAL and NTSC want
		// different display rates.
		if (emu::FramesPerSecond() != g_app.rate_chosen_for_fps)
			ChooseRefreshRate(emu::FramesPerSecond());

		switch (action)
		{
			case menu::Action::SaveState:
				emu::RequestSaveState(menu::SelectedSlot());
				break;

			case menu::Action::LoadState:
				emu::RequestLoadState(menu::SelectedSlot());
				break;

			case menu::Action::LoadRom:
				emu::RequestLoadRom(g_app.rom_dir + "/" + menu::SelectedRom());
				break;

			case menu::Action::OpenFolder:
				menu::SetFolderList(menu::ChosenFolder(),
				                    ListFolders(menu::ChosenFolder()));
				break;

			case menu::Action::BindingsChanged:
				SaveScreenConfig();
				break;

			case menu::Action::GrantStorage:
				RequestAllFilesAccess(app);
				break;

			case menu::Action::UseFolder:
				g_app.rom_dir = menu::CurrentFolder();
				menu::SetRomList(ListRoms(g_app.rom_dir));
				SaveScreenConfig();
				LOGI("ROM folder set to %s", g_app.rom_dir.c_str());
				break;

			default:
				break;
		}

		if (action == menu::Action::Recenter || !g_app.screen_tracked)
			PlaceScreen(frame_state.predictedDisplayTime);

		const bool should_render = frame_state.shouldRender == XR_TRUE;
		bool have_backdrop = false;
		bool have_menu = false;
		int eyes_drawn = 0;
		if (should_render)
		{
			have_backdrop = RenderBackdrop(frame_state.predictedDisplayTime);
			eyes_drawn = RenderFrame();
			have_menu = RenderMenu();
		}

		SubmitFrame(frame_state.predictedDisplayTime, eyes_drawn, have_backdrop, have_menu);

		// Once the emulator has settled, capture both eyes if asked to.

		static uint64_t frames = 0;

		// Cheap, and only every couple of seconds.
		if (frames % 120 == 0)
			EnsureRefreshRate();

		if (++frames % 300 == 0)
			LOGI("xr: %llu frames, backdrop=%d tracked=%d, screen at (%.2f %.2f %.2f) "
			     "r=%.1f s=%.0fmm split=%d mode7=%d menu=%d %.0fHz, emu frame %llu",
			     (unsigned long long) frames, (int) have_backdrop,
			     (int) g_app.screen_tracked,
			     g_app.screen_pose.position.x, g_app.screen_pose.position.y,
			     g_app.screen_pose.position.z, g_app.screen.radius,
			     g_app.screen.stereo * 1000.0f, (int) g_app.screen.layer_split,
			     (int) g_app.last_frame_mode7, (int) menu::IsOpen(), g_app.display_rate,
			     (unsigned long long) g_app.last_frame_serial);
	}

	SaveScreenConfig();
	Shutdown(app);
}
