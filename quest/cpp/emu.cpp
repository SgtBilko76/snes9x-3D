#include "emu.h"
#include "audio.h"
#include "log.h"

#include "snes9x.h"
#include "memmap.h"
#include "apu/apu.h"
#include "dsp.h"
#include "gfx.h"
#include "display.h"
#include "controls.h"
#include "snapshot.h"
#include "fscompat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

void S9xVRMakeDirs(const std::string &base);

namespace emu {
namespace {

constexpr int kMaxW = MAX_SNES_WIDTH;
constexpr int kMaxH = MAX_SNES_HEIGHT;

struct Buffer {
	std::vector<uint16_t> pixels = std::vector<uint16_t>(kMaxW * kMaxH);
	std::vector<uint8_t>  depth  = std::vector<uint8_t>(kMaxW * kMaxH);
	std::vector<uint16_t> layer_pixels[kLayerCount];
	std::vector<uint8_t>  layer_depth[kLayerCount];
	uint8_t layer_priority[kLayerCount] = {};
	std::vector<uint8_t> mode7_depth = std::vector<uint8_t>(240, 255);
	std::vector<uint8_t> object_depth = std::vector<uint8_t>(kMaxW * kMaxH, 255);
	bool object_depth_valid = false;
	SDSP1Projection projections[S9X_DSP1_PROJECTION_MAX];
	int projection_count = 0;
	bool layer_mode7[kLayerCount] = {};
	bool mode7 = false;
	bool layered = false;
	int width = 0;
	int height = 0;
	uint64_t serial = 0;

	Buffer()
	{
		for (int layer = 0; layer < kLayerCount; layer++)
		{
			layer_pixels[layer].resize(kMaxW * kMaxH);
			layer_depth[layer].resize(kMaxW * kMaxH);
		}
	}
};

Buffer g_buffers[3];
std::mutex g_frame_mutex;
int g_write_index = 0;    // being filled by the emulation thread
int g_ready_index = -1;   // latest finished frame
int g_read_index = -1;    // handed out to the render thread

std::thread g_thread;
std::atomic<int> g_dump_countdown{-1};
std::atomic<bool> g_dump_mode7_only{false};
std::string g_dump_path;
std::atomic<bool> g_running{false};
std::atomic<bool> g_paused{false};
std::atomic<uint64_t> g_serial{0};
std::atomic<bool> g_layer_split{true};
std::atomic<bool> g_has_game{false};
std::atomic<uint64_t> g_dumped_serial{0};

std::atomic<int> g_save_request{-1};
std::atomic<int> g_load_request{-1};
std::mutex g_rom_mutex;
std::string g_rom_request;
std::mutex g_message_mutex;
std::string g_state_message;

std::string SlotPath(int slot)
{
	char extension[8];
	snprintf(extension, sizeof(extension), ".%03d", slot);
	return S9xGetFilename(extension, SNAPSHOT_DIR);
}

void SetStateMessage(const std::string &message)
{
	std::lock_guard<std::mutex> lock(g_message_mutex);
	g_state_message = message;
	LOGI("%s", message.c_str());
}

// Runs on the emulation thread, between frames.
void ServiceRomRequest()
{
	std::string path;
	{
		std::lock_guard<std::mutex> lock(g_rom_mutex);
		path.swap(g_rom_request);
	}

	if (path.empty())
		return;

	// The battery save belongs to the cartridge on its way out, so it has to
	// be written before the new one replaces the name it is derived from.
	if (g_has_game.load())
		Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());

	if (!Memory.LoadROM(path.c_str()))
	{
		SetStateMessage("Could not load " + S9xBasename(path));
		return;
	}

	Memory.LoadSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());
	S9xReportControllers();
	g_has_game.store(true);

	SetStateMessage(std::string("Loaded ") + Memory.ROMName);
}

void ServiceStateRequests()
{
	int slot = g_save_request.exchange(-1);
	if (slot >= 0)
	{
		const bool ok = S9xFreezeGame(SlotPath(slot).c_str());
		SetStateMessage(ok ? "Saved to slot " + std::to_string(slot)
		                   : "Could not save slot " + std::to_string(slot));
	}

	slot = g_load_request.exchange(-1);
	if (slot >= 0)
	{
		if (access(SlotPath(slot).c_str(), F_OK) != 0)
		{
			SetStateMessage("Slot " + std::to_string(slot) + " is empty");
			return;
		}

		const bool ok = S9xUnfreezeGame(SlotPath(slot).c_str());
		SetStateMessage(ok ? "Loaded slot " + std::to_string(slot)
		                   : "Could not load slot " + std::to_string(slot));
	}
}

// Button ids handed to S9xReportButton.  Arbitrary, but must be stable.
constexpr uint32_t kButtonIdBase = 100;

const char *kButtonCommands[BTN_COUNT] = {
	"Joypad1 Up", "Joypad1 Down", "Joypad1 Left", "Joypad1 Right",
	"Joypad1 A",  "Joypad1 B",    "Joypad1 X",    "Joypad1 Y",
	"Joypad1 L",  "Joypad1 R",    "Joypad1 Start","Joypad1 Select"
};

// Debug aid: drop a file called "dump" next to the roms directory and the next
// run writes one frame out as a PPM, which is the only way to look at the
// emulated picture without putting the headset on.
void MaybeDumpFrame(const Buffer &buf)
{
	int remaining = g_dump_countdown.load(std::memory_order_relaxed);
	if (remaining < 0)
		return;

	// Wait for the game to actually reach Mode 7 when asked to, rather than
	// capturing whatever is on screen a couple of seconds after boot.
	if (g_dump_mode7_only.load(std::memory_order_relaxed) && !buf.mode7)
		return;

	if (remaining > 0)
	{
		g_dump_countdown.store(remaining - 1, std::memory_order_relaxed);
		return;
	}

	g_dump_countdown.store(-1, std::memory_order_relaxed);

	FILE *file = fopen(g_dump_path.c_str(), "wb");
	if (!file)
		return;

	fprintf(file, "P6\n%d %d\n255\n", buf.width, buf.height);
	for (int y = 0; y < buf.height; y++)
		for (int x = 0; x < buf.width; x++)
		{
			uint16_t pixel = buf.pixels[y * kMaxW + x];
			uint8_t rgb[3] = {
				static_cast<uint8_t>((((pixel >> 11) & 0x1f) * 255 + 15) / 31),
				static_cast<uint8_t>((((pixel >> 5) & 0x3f) * 255 + 31) / 63),
				static_cast<uint8_t>(((pixel & 0x1f) * 255 + 15) / 31),
			};
			fwrite(rgb, 1, 3, file);
		}

	fclose(file);

	// The priority buffer alongside it, which is what drives the stereo pass.
	const std::string depth_path = g_dump_path.substr(0, g_dump_path.rfind('.')) + "_depth.pgm";
	if (FILE *depth = fopen(depth_path.c_str(), "wb"))
	{
		fprintf(depth, "P5\n%d %d\n255\n", buf.width, buf.height);
		for (int y = 0; y < buf.height; y++)
			fwrite(&buf.depth[y * kMaxW], 1, buf.width, depth);
		fclose(depth);
	}

	// And each split layer, so the composite can be checked against its parts.
	if (buf.layered)
		for (int layer = 0; layer < kLayerCount; layer++)
		{
			if (buf.layer_priority[layer] == 0)
				continue;

			const std::string base = g_dump_path.substr(0, g_dump_path.rfind('.')) +
			                         "_layer" + std::to_string(layer);

			if (FILE *colour = fopen((base + ".ppm").c_str(), "wb"))
			{
				fprintf(colour, "P6\n%d %d\n255\n", buf.width, buf.height);
				for (int y = 0; y < buf.height; y++)
					for (int x = 0; x < buf.width; x++)
					{
						uint16_t pixel = buf.layer_pixels[layer][y * kMaxW + x];
						uint8_t rgb[3] = {
							static_cast<uint8_t>((((pixel >> 11) & 0x1f) * 255 + 15) / 31),
							static_cast<uint8_t>((((pixel >> 5) & 0x3f) * 255 + 31) / 63),
							static_cast<uint8_t>(((pixel & 0x1f) * 255 + 15) / 31),
						};
						fwrite(rgb, 1, 3, colour);
					}
				fclose(colour);
			}

			if (FILE *cover = fopen((base + "_z.pgm").c_str(), "wb"))
			{
				fprintf(cover, "P5\n%d %d\n255\n", buf.width, buf.height);
				for (int y = 0; y < buf.height; y++)
					fwrite(&buf.layer_depth[layer][y * kMaxW], 1, buf.width, cover);
				fclose(cover);
			}

			LOGI("layer %d: dominant priority %d", layer, buf.layer_priority[layer]);
		}

	if (buf.object_depth_valid)
		if (FILE *objects = fopen((g_dump_path.substr(0, g_dump_path.rfind('.')) +
		                           "_objdepth.pgm").c_str(), "wb"))
		{
			fprintf(objects, "P5\n%d %d\n255\n", buf.width, buf.height);
			for (int y = 0; y < buf.height; y++)
				fwrite(&buf.object_depth[y * kMaxW], 1, buf.width, objects);
			fclose(objects);
		}

	// Diagnostic: the DSP-1 projections this frame, next to the sprites the
	// PPU is actually showing, so the two can be lined up.
	if (FILE *objects = fopen((g_dump_path.substr(0, g_dump_path.rfind('.')) +
	                           "_objects.txt").c_str(), "w"))
	{
		fprintf(objects, "# dsp1 projections: %d (total since boot %u)\n",
		        buf.projection_count, DSP1ProjectionTotal);
		for (int i = 0; i < buf.projection_count; i++)
			fprintf(objects, "P %d %d %d\n", buf.projections[i].H,
			        buf.projections[i].V, buf.projections[i].M);

		fprintf(objects, "# sprites: index hpos vpos width visibletiles\n");
		for (int i = 0; i < 128; i++)
			fprintf(objects, "S %d %d %d %d %d\n", i, PPU.OBJ[i].HPos,
			        PPU.OBJ[i].VPos, GFX.OBJWidths[i], GFX.OBJVisibleTiles[i]);
		fclose(objects);
	}

	g_dumped_serial.store(buf.serial);
	LOGI("dumped frame %llu to %s (%dx%d)", (unsigned long long) buf.serial,
	     g_dump_path.c_str(), buf.width, buf.height);
}

// Turns the per-scanline Mode 7 matrices into a depth per row.
//
// The matrix maps screen pixels to texture coordinates, so the length of one
// screen pixel's step across the texture, sqrt(A^2 + C^2), is how much ground
// that row covers -- which is proportional to how far away it is.  Disparity
// goes as 1/distance, so the reciprocal of that step, normalised across the
// rows the frame actually uses, is exactly the depth the stereo pass wants.
//
// A scene with one matrix for every line, like a rotating title screen, comes
// out flat, which is correct: it is a picture being spun, not a plane being
// looked along.
void UpdateMode7Depth(Buffer &buf, int height)
{
	float inverse_step[240];
	float nearest = 0.0f;
	float farthest = 1e30f;
	bool any = false;

	for (int line = 0; line < height && line < 240; line++)
	{
		inverse_step[line] = -1.0f;

		if (GFX.LineBGMode[line] != 7)
			continue;

		const SLineMatrixData &matrix = LineMatrixData[line];
		const float a = matrix.MatrixA / 256.0f;
		const float c = matrix.MatrixC / 256.0f;
		const float step = std::sqrt(a * a + c * c);

		// Near the horizon the step runs away to nothing; clamp rather than
		// divide by it.
		const float inverse = step > 1.0f / 4096.0f ? 1.0f / step : 0.0f;

		inverse_step[line] = inverse;
		nearest = std::max(nearest, inverse);
		farthest = std::min(farthest, inverse);
		any = true;
	}

	buf.mode7 = any;
	if (!any)
		return;

	const float range = nearest - farthest;

	for (int line = 0; line < 240; line++)
	{
		if (line >= height || inverse_step[line] < 0.0f)
		{
			buf.mode7_depth[line] = 255;   // not Mode 7: sits on the screen plane
			continue;
		}

		const float depth = range > 1e-6f ? (inverse_step[line] - farthest) / range : 1.0f;
		buf.mode7_depth[line] =
			static_cast<uint8_t>(std::clamp(depth, 0.0f, 1.0f) * 255.0f + 0.5f);
	}
}

// Gives every sprite the depth of the ground beneath it.
//
// Super Mario Kart turned out never to ask the DSP-1 to project anything -- it
// only uses the chip for the per-scanline Mode 7 matrices -- so the karts'
// distances are nowhere to be read directly. They are standing on the road,
// though, and the road's distance per row is already known, so a sprite's base
// row gives its distance. The whole sprite then takes that one depth, which is
// what keeps it standing upright rather than leaning away with the ground.
void UpdateObjectDepth(Buffer &buf, int width, int height)
{
	buf.object_depth_valid = false;

	if (!buf.mode7)
		return;

	// Sprites that are not over a Mode 7 row, such as a status bar, keep the
	// screen plane.
	memset(buf.object_depth.data(), 255, buf.object_depth.size());

	for (int sprite = 0; sprite < 128; sprite++)
	{
		const int size = GFX.OBJWidths[sprite];
		if (size <= 0)
			continue;

		const int left = PPU.OBJ[sprite].HPos;
		const int top = static_cast<int>(PPU.OBJ[sprite].VPos);
		const int base = std::clamp(top + size - 1, 0, height - 1);

		if (GFX.LineBGMode[base] != 7)
			continue;

		const uint8_t depth = buf.mode7_depth[base];

		for (int y = std::max(0, top); y < std::min(height, top + size); y++)
			for (int x = std::max(0, left); x < std::min(width, left + size); x++)
				buf.object_depth[y * kMaxW + x] = depth;
	}

	buf.object_depth_valid = true;
}

void ApplyDefaultSettings()
{
	memset(&Settings, 0, sizeof(Settings));

	Settings.MouseMaster = FALSE;
	Settings.SuperScopeMaster = FALSE;
	Settings.JustifierMaster = FALSE;
	Settings.MultiPlayer5Master = FALSE;

	Settings.FrameTimePAL = 20000;
	Settings.FrameTimeNTSC = 16667;

	Settings.SixteenBitSound = TRUE;
	Settings.Stereo = TRUE;
	Settings.SoundPlaybackRate = audio::kSampleRate;
	Settings.SoundInputRate = 31950;
	Settings.DynamicRateControl = TRUE;
	Settings.DynamicRateLimit = 5;

	Settings.Transparency = TRUE;
	Settings.AutoDisplayMessages = TRUE;
	Settings.InitialInfoStringTimeout = 120;
	Settings.HDMATimingHack = 100;
	Settings.BlockInvalidVRAMAccessMaster = TRUE;
	Settings.StopEmulation = TRUE;
	Settings.WrongMovieStateProtection = TRUE;
	Settings.DumpStreamsMaxFrames = -1;
	Settings.StretchScreenshots = 1;
	Settings.SnapshotScreenshots = FALSE;
	Settings.SkipFrames = 0;
	Settings.TurboSkipFrames = 15;
	Settings.MaxSpriteTilesPerLine = 34;

	// These have defaults that are not zero, and Settings starts zeroed, so
	// leaving them out does not give the default -- it gives nothing.
	//
	// The clock multiplier is a percentage the Super FX budget is scaled by,
	// so at zero the chip is handed no cycles at all and a Star Fox renders a
	// black screen while appearing to run perfectly.  The three cycle counts
	// are what ONE_CYCLE, SLOW_ONE_CYCLE and TWO_CYCLES resolve to, which is
	// every CPU timing decision the core makes.
	Settings.SuperFXClockMultiplier = 100;
	Settings.OneClockCycle = 6;
	Settings.OneSlowClockCycle = 8;
	Settings.TwoClockCycles = 12;

	Settings.InterpolationMethod = DSP_INTERPOLATION_GAUSSIAN;

	CPU.Flags = 0;
}

void SetupControls()
{
	S9xUnmapAllControls();
	S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
	S9xSetController(1, CTL_NONE,   0, 0, 0, 0);
	S9xVerifyControllers();

	for (int i = 0; i < BTN_COUNT; i++)
		S9xMapButton(kButtonIdBase + i, S9xGetCommandT(kButtonCommands[i]), false);

	S9xReportControllers();
}

void SamplesAvailable(void *)
{
	static int16_t mix[16384];

	int samples = S9xGetSampleCount();
	if (samples <= 0)
		return;
	if (samples > static_cast<int>(sizeof(mix) / sizeof(mix[0])))
		samples = sizeof(mix) / sizeof(mix[0]);

	S9xMixSamples(reinterpret_cast<uint8_t *>(mix), samples);
	audio::Push(mix, samples);
}

void EmuThread()
{
	while (g_running.load(std::memory_order_relaxed))
	{
		// A pending frame dump keeps running while paused, so the picture can
		// be checked with the headset off the head.
		if (g_paused.load(std::memory_order_relaxed) &&
		    g_dump_countdown.load(std::memory_order_relaxed) < 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			continue;
		}

		ServiceRomRequest();
		ServiceStateRequests();

		if (!g_has_game.load(std::memory_order_relaxed))
		{
			// Nothing to run yet, but a ROM may still be chosen from the menu.
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			continue;
		}

		// Blocks until the audio ring has room, which paces us to ~60.1 Hz.
		audio::WaitForRoom();
		S9xMainLoop();
	}
}

} // namespace

bool Start(const std::string &base_dir, const std::string &rom_path)
{
	S9xVRMakeDirs(base_dir);
	ApplyDefaultSettings();

	// access() rather than fopen(): a file dropped in by "adb shell touch"
	// belongs to the shell user and the app cannot open it for reading.
	g_dump_path = base_dir + "/frame.ppm";
	if (access((base_dir + "/dump").c_str(), F_OK) == 0)
	{
		remove((base_dir + "/dump").c_str());
		g_dump_countdown.store(120);
		g_dump_mode7_only.store(access((base_dir + "/dump7").c_str(), F_OK) == 0);
	}

	if (!Memory.Init() || !S9xInitAPU())
	{
		LOGE("core: out of memory during init");
		return false;
	}

	S9xInitSound(64);
	S9xSetSoundMute(FALSE);
	S9xSetSamplesAvailableCallback(SamplesAvailable, nullptr);

	if (!S9xGraphicsInit())
	{
		LOGE("core: graphics init failed");
		return false;
	}

	if (rom_path.empty())
	{
		LOGW("core: started with no cartridge");
	}
	else if (!Memory.LoadROM(rom_path.c_str()))
	{
		LOGE("core: failed to load ROM %s", rom_path.c_str());
	}
	else
	{
		Memory.LoadSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());
		g_has_game.store(true);
	}

	SetupControls();
	GFX.SplitLayers = g_layer_split.load() ? TRUE : FALSE;

	if (g_has_game.load())
		LOGI("core: loaded %s (%dx%d), superfx x%u, cycles %d/%d/%d",
		     Memory.ROMName, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight,
		     Settings.SuperFXClockMultiplier, Settings.OneClockCycle,
		     Settings.OneSlowClockCycle, Settings.TwoClockCycles);

	g_running.store(true);
	g_thread = std::thread(EmuThread);
	return true;
}

void Stop()
{
	if (!g_running.exchange(false))
		return;

	audio::WakeAll();
	if (g_thread.joinable())
		g_thread.join();

	if (g_has_game.load())
		Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());
	S9xGraphicsDeinit();
	S9xDeinitAPU();
	Memory.Deinit();
}

void SetPaused(bool paused)
{
	g_paused.store(paused);
	S9xSetSoundMute(paused ? TRUE : FALSE);
}

bool AcquireFrame(Frame &out, uint64_t since)
{
	std::lock_guard<std::mutex> lock(g_frame_mutex);

	if (g_ready_index < 0 || g_buffers[g_ready_index].serial <= since)
		return false;

	g_read_index = g_ready_index;
	const Buffer &buf = g_buffers[g_read_index];

	out.pixels = buf.pixels.data();
	out.depth = buf.depth.data();
	out.width = buf.width;
	out.height = buf.height;
	out.pitch_pixels = kMaxW;
	out.serial = buf.serial;
	out.layered = buf.layered;
	out.mode7 = buf.mode7;
	out.mode7_depth = buf.mode7_depth.data();
	out.object_depth = buf.object_depth.data();
	out.object_depth_valid = buf.object_depth_valid;

	for (int layer = 0; layer < kLayerCount; layer++)
	{
		out.layer_pixels[layer] = buf.layer_pixels[layer].data();
		out.layer_depth[layer] = buf.layer_depth[layer].data();
		out.layer_priority[layer] = buf.layer_priority[layer];
		out.layer_mode7[layer] = buf.layer_mode7[layer];
	}

	return true;
}

int FramesPerSecond()
{
	return Memory.ROMFramesPerSecond > 0 ? Memory.ROMFramesPerSecond : 60;
}

uint64_t DumpedSerial() { return g_dumped_serial.load(); }

void RequestLoadRom(const std::string &path)
{
	std::lock_guard<std::mutex> lock(g_rom_mutex);
	g_rom_request = path;
}

void RequestSaveState(int slot) { g_save_request.store(slot); }
void RequestLoadState(int slot) { g_load_request.store(slot); }

bool StateExists(int slot)
{
	return access(SlotPath(slot).c_str(), F_OK) == 0;
}

std::string LastStateMessage()
{
	std::lock_guard<std::mutex> lock(g_message_mutex);
	return g_state_message;
}

void SetLayerSplit(bool enabled)
{
	g_layer_split.store(enabled);
	GFX.SplitLayers = enabled ? TRUE : FALSE;
}

bool LayerSplitEnabled() { return g_layer_split.load(); }

bool HasGame() { return g_has_game.load(); }

void SetButton(Button button, bool pressed)
{
	S9xReportButton(kButtonIdBase + button, pressed);
}

// Called by the core at the end of every rendered frame.
void PublishFrame()
{
	Buffer &buf = g_buffers[g_write_index];

	const int width = IPPU.RenderedScreenWidth;
	const int height = IPPU.RenderedScreenHeight;

	for (int y = 0; y < height; y++)
	{
		memcpy(&buf.pixels[y * kMaxW],
		       reinterpret_cast<const uint8_t *>(GFX.Screen) + y * GFX.Pitch,
		       width * sizeof(uint16_t));
		memcpy(&buf.depth[y * kMaxW], GFX.ZBuffer + y * GFX.RealPPL, width);
	}

	UpdateMode7Depth(buf, height);

	// BG1 carries the Mode 7 plane; BG2 does too when EXTBG is on.
	UpdateObjectDepth(buf, width, height);

	buf.layer_mode7[0] = buf.mode7;
	buf.layer_mode7[1] = buf.mode7 && (Memory.FillRAM[0x2133] & 0x40) != 0;
	for (int layer = 2; layer < kLayerCount; layer++)
		buf.layer_mode7[layer] = false;

	buf.projection_count = std::min(DSP1ProjectionCount, S9X_DSP1_PROJECTION_MAX);
	memcpy(buf.projections, DSP1Projections,
	       buf.projection_count * sizeof(SDSP1Projection));
	DSP1ProjectionCount = 0;

	buf.layered = GFX.SplitLayers;
	if (buf.layered)
	{
		for (int layer = 0; layer < kLayerCount; layer++)
		{
			// One histogram pass over the coverage tells us both whether the
			// layer was drawn at all and which priority it mostly sits at,
			// which is what decides its depth.
			uint32_t histogram[256] = {};

			for (int y = 0; y < height; y++)
			{
				const uint8_t *source = GFX.LayerZBuffer[layer] + y * GFX.RealPPL;
				uint8_t *target = &buf.layer_depth[layer][y * kMaxW];
				memcpy(target, source, width);

				for (int x = 0; x < width; x++)
					histogram[source[x]]++;
			}

			uint32_t best = 0;
			uint8_t dominant = 0;
			for (int value = 1; value < 256; value++)
				if (histogram[value] > best)
				{
					best = histogram[value];
					dominant = static_cast<uint8_t>(value);
				}

			buf.layer_priority[layer] = dominant;
			if (dominant == 0)
				continue;   // nothing drawn, no need to copy the colours

			for (int y = 0; y < height; y++)
				memcpy(&buf.layer_pixels[layer][y * kMaxW],
				       reinterpret_cast<const uint8_t *>(GFX.LayerScreen[layer]) +
				           y * GFX.Pitch,
				       width * sizeof(uint16_t));
		}
	}

	buf.width = width;
	buf.height = height;
	buf.serial = g_serial.fetch_add(1) + 1;

	MaybeDumpFrame(buf);

	std::lock_guard<std::mutex> lock(g_frame_mutex);
	g_ready_index = g_write_index;
	// Move on to a buffer that is neither the newest nor the one in flight.
	for (int i = 0; i < 3; i++)
		if (i != g_ready_index && i != g_read_index)
		{
			g_write_index = i;
			break;
		}
}

} // namespace emu

bool8 S9xDeinitUpdate(int width, int height)
{
	(void) width;
	(void) height;
	emu::PublishFrame();
	return TRUE;
}
