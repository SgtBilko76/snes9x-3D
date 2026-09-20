#include "renderer.h"
#include "log.h"

#include <cstdio>
#include <map>
#include <vector>
#include <string>

namespace renderer {
namespace {

constexpr int kTexWidth = 512;   // MAX_SNES_WIDTH
constexpr int kTexHeight = 478;  // MAX_SNES_HEIGHT

GLuint g_frame_texture = 0;
GLuint g_depth_texture = 0;
GLuint g_ramp_texture = 0;
GLuint g_mode7_texture = 0;
GLuint g_object_texture = 0;
bool g_object_depth = false;
GLuint g_layer_colour = 0;      // GL_TEXTURE_2D_ARRAY, one slice per layer
GLuint g_layer_coverage = 0;
uint8_t g_layer_priority[emu::kLayerCount] = {};
bool g_layered = false;
bool g_layer_mode7[emu::kLayerCount] = {};
int g_filter = 1;
GLuint g_overlay_texture = 0;
GLuint g_overlay_program = 0;
int g_overlay_width = 0;
int g_overlay_height = 0;
GLuint g_program = 0;
GLuint g_vao = 0;
GLint  g_loc_source_size = -1;
GLint  g_loc_visible = -1;
GLint  g_loc_shift = -1;
GLint  g_loc_frame = -1;
GLint  g_loc_depth = -1;
GLint  g_loc_ramp = -1;

// How recently each priority value carried a meaningful part of the picture.
// Decayed rather than rebuilt per frame: if the set of layers were re-ranked
// the instant a sprite layer appeared or vanished, the whole scene would jump
// in depth.
float g_layer_presence[256] = {};

// Priority value -> depth, the same ramp the shader samples, kept on the CPU
// so per-layer shifts can be worked out before drawing.
uint8_t g_ramp[256] = {};

std::string g_eye_dump_prefix;
int g_eye_dumps_pending = 0;

std::map<GLuint, GLuint> g_framebuffers;  // swapchain texture -> FBO

// Visible fraction of the source texture, updated per frame because the core
// switches between 256/512 wide and 224/239 tall modes.
float g_visible_u = 1.0f;
float g_visible_v = 1.0f;

const char *kVertexShader = R"(#version 300 es
out vec2 vUV;
void main()
{
    // Full-screen triangle; no vertex buffer needed.
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

// "Sharp bilinear": interpolate only across the last texel of each edge, which
// keeps the pixel grid crisp on a large virtual screen without the shimmer
// nearest-neighbour picks up under timewarp reprojection.
// The core writes a per-pixel priority value into GFX.ZBuffer as it composites
// the layers, so layer ordering is already there for free.  Turning that into
// stereo means displacing each pixel horizontally by an amount that depends on
// its layer: the frontmost layer stays on the screen plane and everything
// behind it is pushed back, so nothing ever pops out past the screen edge.
//
// The displacement is resolved by backward search -- for each output pixel,
// find the source pixel that lands on it -- which duplicates rather than tears
// at layer boundaries.  Splitting the layers properly comes next.
const char *kFragmentShader = R"(#version 300 es
precision mediump float;
precision mediump sampler2DArray;

#define LAYERS 5

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uFrame;
uniform sampler2D uDepth;
uniform sampler2D uRamp;    // priority value -> depth, built on the CPU
uniform vec2 uSourceSize;   // texels of the whole texture
uniform vec2 uVisible;      // fraction of the texture the frame occupies
uniform float uShift;       // signed max displacement, as a fraction of image width

// Layer split: each background and the sprites arrive separately, so each can
// be displaced as a whole and whatever sits behind it shows through where it
// moves.  uLayerShift is already resolved per layer on the CPU.
uniform sampler2DArray uLayerColour;
uniform sampler2DArray uLayerCoverage;
uniform float uLayerShift[LAYERS];
uniform float uLayerActive[LAYERS];
uniform float uLayerMode7[LAYERS];
uniform float uBackdropShift;
uniform sampler2D uMode7Depth;    // one entry per scanline
uniform sampler2D uObjectDepth;   // per pixel, for the sprite layer
uniform float uObjectLayer;       // which layer that is, or -1
uniform bool uLayered;
uniform float uFilter;      // 0 pixels, 1 sharp, 2 soft

// Screen-space size of one source texel.  Worked out once in main(), because
// derivatives are only defined where every fragment in a quad takes the same
// path, and the sampling below happens inside branches that differ per pixel.
vec2 gTexelScale;

// How much of the neighbouring texel to mix in, per axis.
//
//   0 pixels -- no mixing at all, a hard pixel grid
//   1 sharp  -- mix only across the last texel at each edge, which keeps the
//               grid crisp while taking the stair-steps off it
//   2 soft   -- ordinary bilinear
vec2 FilterWeights(vec2 frac)
{
    if (uFilter < 0.5)
        return step(vec2(0.5), frac);

    if (uFilter > 1.5)
        return frac;

    return clamp((frac - 0.5) / gTexelScale + 0.5, 0.0, 1.0);
}

// Same idea for the composite, which has real neighbours everywhere and so can
// lean on the hardware filter: nudge the sample point instead of weighting.
vec2 SharpUV(vec2 uv)
{
    vec2 texel = uv * uSourceSize;
    vec2 centre = floor(texel) + 0.5;
    vec2 frac = texel - centre;

    if (uFilter < 0.5)
        return centre / uSourceSize;

    if (uFilter > 1.5)
        return uv;

    return (centre + clamp(frac / gTexelScale, -0.5, 0.5)) / uSourceSize;
}

// A layer's buffer holds nothing outside its own coverage, so an ordinary
// bilinear tap near a layer edge would blend the picture with whatever was
// last left there.  Weighting each tap by its coverage keeps the filtering and
// drops the contributions that are not real.
vec4 SampleLayer(vec2 uv, float layer)
{
    vec2 texel = uv * uSourceSize - 0.5;
    vec2 corner = floor(texel);
    vec2 weights = FilterWeights(texel - corner);

    vec3 total = vec3(0.0);
    float sum = 0.0;

    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
        {
            vec2 point = (corner + vec2(float(i), float(j)) + 0.5) / uSourceSize;
            float covered = step(0.5 / 255.0,
                                 texture(uLayerCoverage, vec3(point, layer)).r);
            float weight = (i == 0 ? 1.0 - weights.x : weights.x) *
                           (j == 0 ? 1.0 - weights.y : weights.y) * covered;

            total += texture(uLayerColour, vec3(point, layer)).rgb * weight;
            sum += weight;
        }

    // Every tap fell outside the layer, which happens at a one-texel sliver;
    // fall back to the texel actually under the sample point.
    if (sum <= 0.0)
        return texture(uLayerColour, vec3(uv, layer));

    return vec4(total / sum, 1.0);
}

// Priority values are an ordering, not a distance: two adjacent layers differ
// by one, wherever in the range they happen to sit.  The ramp spreads whatever
// layers this scene actually uses evenly between the backdrop and the screen
// plane.
float LayerDepth(vec2 uv)
{
    float priority = texture(uDepth, uv).r;
    return texture(uRamp, vec2(priority * (255.0 / 256.0) + (0.5 / 256.0), 0.5)).r;
}

void main()
{
    vec2 base = vec2(vUV.x, 1.0 - vUV.y) * uVisible;
    gTexelScale = max(fwidth(base * uSourceSize), vec2(1e-4));

    if (uLayered)
    {
        // Whichever layer has the highest priority at this pixel wins, which
        // is the same rule the core's own compositing uses.
        float bestPriority = 0.0;
        vec2 bestUV = base;
        float bestLayer = -1.0;

        // Mode 7 rows each sit at their own distance, so that layer's
        // displacement is read per row instead of being one value for the
        // whole layer.  This is the one case where the depth is real
        // geometry rather than an ordering.
        float row = base.y / uVisible.y;
        float mode7Shift = uBackdropShift * (1.0 - texture(uMode7Depth, vec2(row, 0.5)).r);

        for (int i = 0; i < LAYERS; i++)
        {
            if (uLayerActive[i] < 0.5)
                continue;

            float displacement;
            if (uObjectLayer == float(i))
                displacement = uBackdropShift *
                               (1.0 - texture(uObjectDepth, base).r);
            else if (uLayerMode7[i] > 0.5)
                displacement = mode7Shift;
            else
                displacement = uLayerShift[i];

            vec2 uv = vec2(clamp(base.x + displacement, 0.0, uVisible.x), base.y);
            float priority = texture(uLayerCoverage, vec3(uv, float(i))).r;

            if (priority > bestPriority)
            {
                bestPriority = priority;
                bestUV = uv;
                bestLayer = float(i);
            }
        }

        if (bestLayer < 0.0)
        {
            // Nothing covered this pixel, so it is backdrop.  Take it from the
            // composite, displaced as far as anything goes.
            vec2 uv = vec2(clamp(base.x + uBackdropShift, 0.0, uVisible.x), base.y);
            fragColor = texture(uFrame, SharpUV(uv));
        }
        else
        {
            fragColor = SampleLayer(bestUV, bestLayer);
        }

        return;
    }

    // No split available: displace the composite by the depth found at each
    // pixel, resolved by backward search.  Duplicates rather than tears at
    // layer boundaries.
    vec2 uv = base;

    if (uShift != 0.0)
    {
        for (int i = 0; i < 4; i++)
        {
            float displacement = uShift * uVisible.x * (1.0 - LayerDepth(uv));
            uv.x = clamp(base.x + displacement, 0.0, uVisible.x);
        }
    }

    fragColor = texture(uFrame, SharpUV(uv));
}
)";

const char *kOverlayFragmentShader = R"(#version 300 es
precision mediump float;

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uOverlay;

void main()
{
    fragColor = texture(uOverlay, vec2(vUV.x, 1.0 - vUV.y));
}
)";

GLuint CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char info[1024];
		glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
		LOGE("gl: shader compile failed: %s", info);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

// Ranks the priority values this frame uses and spreads them evenly across the
// depth range, so that adjacent layers separate by a usable amount no matter
// how close together their priority numbers happen to be.
void UpdateLayerRamp(const emu::Frame &frame)
{
	// A layer has to cover a little ground before it earns a depth slot,
	// otherwise a handful of stray pixels compresses everything else.
	constexpr uint32_t kMinPixels = 64;
	constexpr float kDecay = 0.99f;
	constexpr float kForgotten = 0.05f;

	uint32_t histogram[256] = {};
	for (int y = 0; y < frame.height; y++)
	{
		const uint8_t *row = frame.depth + y * frame.pitch_pixels;
		for (int x = 0; x < frame.width; x++)
			histogram[row[x]]++;
	}

	int layers = 0;
	for (int value = 0; value < 256; value++)
	{
		g_layer_presence[value] *= kDecay;
		if (histogram[value] >= kMinPixels)
			g_layer_presence[value] = 1.0f;
		if (g_layer_presence[value] > kForgotten)
			layers++;
	}

	const float steps = layers > 1 ? static_cast<float>(layers - 1) : 1.0f;
	int rank = 0;
	uint8_t last = 0;

	for (int value = 0; value < 256; value++)
	{
		if (g_layer_presence[value] > kForgotten)
		{
			last = static_cast<uint8_t>(255.0f * rank / steps + 0.5f);
			rank++;
		}
		g_ramp[value] = last;
	}

	glBindTexture(GL_TEXTURE_2D, g_ramp_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RED, GL_UNSIGNED_BYTE, g_ramp);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void DumpBoundFramebuffer(int width, int height, const std::string &path)
{
	std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

	FILE *file = fopen(path.c_str(), "wb");
	if (!file)
		return;

	fprintf(file, "P6\n%d %d\n255\n", width, height);
	for (int y = height - 1; y >= 0; y--)          // GL reads bottom-up
		for (int x = 0; x < width; x++)
		{
			const uint8_t *pixel = &pixels[(static_cast<size_t>(y) * width + x) * 4];
			fwrite(pixel, 1, 3, file);
		}

	fclose(file);
	LOGI("dumped eye image to %s", path.c_str());
}

GLuint FramebufferFor(GLuint color_texture)
{
	auto it = g_framebuffers.find(color_texture);
	if (it != g_framebuffers.end())
		return it->second;

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, color_texture, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		LOGE("gl: incomplete framebuffer 0x%x", status);

	g_framebuffers[color_texture] = fbo;
	return fbo;
}

} // namespace

bool Init()
{
	GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
	if (!vs || !fs)
		return false;

	g_program = glCreateProgram();
	glAttachShader(g_program, vs);
	glAttachShader(g_program, fs);
	glLinkProgram(g_program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = GL_FALSE;
	glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		char info[1024];
		glGetProgramInfoLog(g_program, sizeof(info), nullptr, info);
		LOGE("gl: program link failed: %s", info);
		return false;
	}

	g_loc_source_size = glGetUniformLocation(g_program, "uSourceSize");
	g_loc_visible = glGetUniformLocation(g_program, "uVisible");
	g_loc_shift = glGetUniformLocation(g_program, "uShift");
	g_loc_frame = glGetUniformLocation(g_program, "uFrame");
	g_loc_depth = glGetUniformLocation(g_program, "uDepth");
	g_loc_ramp = glGetUniformLocation(g_program, "uRamp");

	glGenTextures(1, &g_frame_texture);
	glBindTexture(GL_TEXTURE_2D, g_frame_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB565, kTexWidth, kTexHeight);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &g_depth_texture);
	glBindTexture(GL_TEXTURE_2D, g_depth_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, kTexWidth, kTexHeight);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &g_layer_colour);
	glBindTexture(GL_TEXTURE_2D_ARRAY, g_layer_colour);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGB565, kTexWidth, kTexHeight,
	               emu::kLayerCount);
	// Nearest, not linear: a layer's buffer holds nothing outside its own
	// coverage, so interpolating near a layer edge would blend the picture
	// with whatever was last left there.  The composite texture has real
	// neighbours and can afford the sharp-bilinear filter; these cannot.
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &g_layer_coverage);
	glBindTexture(GL_TEXTURE_2D_ARRAY, g_layer_coverage);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R8, kTexWidth, kTexHeight,
	               emu::kLayerCount);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &g_mode7_texture);
	glBindTexture(GL_TEXTURE_2D, g_mode7_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 240, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &g_object_texture);
	glBindTexture(GL_TEXTURE_2D, g_object_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, kTexWidth, kTexHeight);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &g_ramp_texture);
	glBindTexture(GL_TEXTURE_2D, g_ramp_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 256, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// The overlay shares the vertex shader; it only needs a plain blit.
	{
		GLuint overlay_vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
		GLuint overlay_fs = CompileShader(GL_FRAGMENT_SHADER, kOverlayFragmentShader);
		if (!overlay_vs || !overlay_fs)
			return false;

		g_overlay_program = glCreateProgram();
		glAttachShader(g_overlay_program, overlay_vs);
		glAttachShader(g_overlay_program, overlay_fs);
		glLinkProgram(g_overlay_program);
		glDeleteShader(overlay_vs);
		glDeleteShader(overlay_fs);
	}

	glGenTextures(1, &g_overlay_texture);
	glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenVertexArrays(1, &g_vao);

	LOGI("gl: renderer ready (%s)", glGetString(GL_VERSION));
	return true;
}

void Shutdown()
{
	for (auto &entry : g_framebuffers)
		glDeleteFramebuffers(1, &entry.second);
	g_framebuffers.clear();

	if (g_frame_texture)
		glDeleteTextures(1, &g_frame_texture);
	if (g_depth_texture)
		glDeleteTextures(1, &g_depth_texture);
	if (g_ramp_texture)
		glDeleteTextures(1, &g_ramp_texture);
	if (g_mode7_texture)
		glDeleteTextures(1, &g_mode7_texture);
	if (g_object_texture)
		glDeleteTextures(1, &g_object_texture);
	if (g_layer_colour)
		glDeleteTextures(1, &g_layer_colour);
	if (g_layer_coverage)
		glDeleteTextures(1, &g_layer_coverage);
	if (g_overlay_texture)
		glDeleteTextures(1, &g_overlay_texture);
	if (g_overlay_program)
		glDeleteProgram(g_overlay_program);
	if (g_vao)
		glDeleteVertexArrays(1, &g_vao);
	if (g_program)
		glDeleteProgram(g_program);
}

void UploadFrame(const emu::Frame &frame)
{
	if (!frame.pixels || frame.width <= 0 || frame.height <= 0)
		return;

	glBindTexture(GL_TEXTURE_2D, g_frame_texture);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.pitch_pixels);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
	                GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame.pixels);

	glBindTexture(GL_TEXTURE_2D, g_depth_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
	                GL_RED, GL_UNSIGNED_BYTE, frame.depth);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	UpdateLayerRamp(frame);

	g_layered = frame.layered;

	if (frame.mode7_depth)
	{
		glBindTexture(GL_TEXTURE_2D, g_mode7_texture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 240, 1, GL_RED, GL_UNSIGNED_BYTE,
		                frame.mode7_depth);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	}

	for (int layer = 0; layer < emu::kLayerCount; layer++)
		g_layer_mode7[layer] = frame.layer_mode7[layer];

	g_object_depth = frame.object_depth_valid;
	if (g_object_depth)
	{
		glBindTexture(GL_TEXTURE_2D, g_object_texture);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.pitch_pixels);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
		                GL_RED, GL_UNSIGNED_BYTE, frame.object_depth);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}

	if (g_layered)
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.pitch_pixels);

		for (int layer = 0; layer < emu::kLayerCount; layer++)
		{
			g_layer_priority[layer] = frame.layer_priority[layer];
			if (g_layer_priority[layer] == 0)
				continue;   // not drawn this frame

			glBindTexture(GL_TEXTURE_2D_ARRAY, g_layer_colour);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
			                frame.width, frame.height, 1,
			                GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame.layer_pixels[layer]);

			glBindTexture(GL_TEXTURE_2D_ARRAY, g_layer_coverage);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
			                frame.width, frame.height, 1,
			                GL_RED, GL_UNSIGNED_BYTE, frame.layer_depth[layer]);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		}

		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}

	g_visible_u = static_cast<float>(frame.width) / kTexWidth;
	g_visible_v = static_cast<float>(frame.height) / kTexHeight;
}

void SetFilter(int filter)
{
	g_filter = filter;
}

void UploadOverlay(const uint16_t *pixels, int width, int height)
{
	glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 2);

	if (width != g_overlay_width || height != g_overlay_height)
	{
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0,
		             GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
		g_overlay_width = width;
		g_overlay_height = height;
	}
	else
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
		                GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
	}

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void DrawOverlay(GLuint color_texture, int width, int height)
{
	glBindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(color_texture));
	glViewport(0, 0, width, height);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(g_overlay_program);
	glBindVertexArray(g_vao);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
	glUniform1i(glGetUniformLocation(g_overlay_program, "uOverlay"), 0);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RequestEyeDump(const std::string &path_prefix)
{
	g_eye_dump_prefix = path_prefix;
	g_eye_dumps_pending = 2;
}

void ClearSwapchainImage(GLuint color_texture, int width, int height,
                         float red, float green, float blue)
{
	glBindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(color_texture));
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(red, green, blue, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DrawEye(GLuint color_texture, int width, int height, float shift)
{
	glBindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(color_texture));
	glViewport(0, 0, width, height);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(g_program);
	glBindVertexArray(g_vao);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_frame_texture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, g_depth_texture);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, g_ramp_texture);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D_ARRAY, g_layer_colour);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D_ARRAY, g_layer_coverage);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, g_mode7_texture);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_2D, g_object_texture);

	glUniform1i(g_loc_frame, 0);
	glUniform1i(g_loc_depth, 1);
	glUniform1i(g_loc_ramp, 2);
	glUniform1i(glGetUniformLocation(g_program, "uLayerColour"), 3);
	glUniform1i(glGetUniformLocation(g_program, "uLayerCoverage"), 4);
	glUniform1i(glGetUniformLocation(g_program, "uMode7Depth"), 5);
	glUniform1i(glGetUniformLocation(g_program, "uObjectDepth"), 6);

	// The sprites are the last layer; they only take their own depth map when
	// one was built, which needs a Mode 7 plane to stand on.
	glUniform1f(glGetUniformLocation(g_program, "uObjectLayer"),
	            g_object_depth ? static_cast<float>(emu::kLayerCount - 1) : -1.0f);
	glUniform2f(g_loc_source_size, kTexWidth, kTexHeight);
	glUniform2f(g_loc_visible, g_visible_u, g_visible_v);
	glUniform1f(g_loc_shift, shift);
	glUniform1f(glGetUniformLocation(g_program, "uFilter"),
	            static_cast<float>(g_filter));

	glUniform1i(glGetUniformLocation(g_program, "uLayered"), g_layered ? 1 : 0);

	if (g_layered)
	{
		// Each layer moves as a whole, by an amount set by where its dominant
		// priority lands on the depth ramp.
		float shifts[emu::kLayerCount];
		float active[emu::kLayerCount];
		float mode7[emu::kLayerCount];

		for (int layer = 0; layer < emu::kLayerCount; layer++)
		{
			const uint8_t priority = g_layer_priority[layer];
			active[layer] = priority != 0 ? 1.0f : 0.0f;
			mode7[layer] = g_layer_mode7[layer] ? 1.0f : 0.0f;

			const float depth = g_ramp[priority] / 255.0f;
			shifts[layer] = shift * g_visible_u * (1.0f - depth);
		}

		glUniform1fv(glGetUniformLocation(g_program, "uLayerShift"),
		             emu::kLayerCount, shifts);
		glUniform1fv(glGetUniformLocation(g_program, "uLayerActive"),
		             emu::kLayerCount, active);
		glUniform1fv(glGetUniformLocation(g_program, "uLayerMode7"),
		             emu::kLayerCount, mode7);
		glUniform1f(glGetUniformLocation(g_program, "uBackdropShift"),
		            shift * g_visible_u);
	}

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	if (g_eye_dumps_pending > 0)
	{
		DumpBoundFramebuffer(width, height,
		                     g_eye_dump_prefix +
		                     (g_eye_dumps_pending == 2 ? "_left.ppm" : "_right.ppm"));
		g_eye_dumps_pending--;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace renderer
