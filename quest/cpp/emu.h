#pragma once

#include <cstdint>
#include <string>

namespace emu {

// SNES pad buttons, in the order used by the control mapping table.
enum Button {
	BTN_UP = 0, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_A, BTN_B, BTN_X, BTN_Y,
	BTN_L, BTN_R, BTN_START, BTN_SELECT,
	BTN_COUNT
};

constexpr int kLayerCount = 5;   // BG1 to BG4, then sprites

// One finished SNES frame.  `depth` is the core's per-pixel priority buffer
// (GFX.ZBuffer); layer ordering lives in there.
//
// With the layer split on, each background and the sprites also arrive on
// their own, which is what lets a displaced layer uncover the one behind it
// instead of smearing.  `layer_priority` is that layer's dominant priority
// value, and zero means the layer was not drawn this frame.
struct Frame {
	const uint16_t *pixels = nullptr;   // RGB565
	const uint8_t  *depth  = nullptr;
	int width = 0;
	int height = 0;
	int pitch_pixels = 0;               // stride of all buffers, in pixels
	uint64_t serial = 0;

	bool layered = false;
	const uint16_t *layer_pixels[kLayerCount] = {};
	const uint8_t  *layer_depth[kLayerCount] = {};
	uint8_t layer_priority[kLayerCount] = {};

	// Mode 7 draws a ground plane by changing its matrix every scanline, so
	// each row of the screen sits at a different distance.  That makes it the
	// one case where the SNES hands us real depth rather than layer order:
	// `mode7_depth` is that distance per row, normalised, and the layers it
	// applies to are flagged rather than taking a single flat depth.
	bool mode7 = false;
	const uint8_t *mode7_depth = nullptr;   // one entry per scanline
	bool layer_mode7[kLayerCount] = {};

	// Depth for the sprite layer, per pixel.  A sprite standing on a Mode 7
	// plane belongs at the distance of the ground under its feet, not at the
	// distance of whatever row each of its own pixels happens to fall on, so
	// every pixel of a sprite carries the depth taken at its base.
	const uint8_t *object_depth = nullptr;
	bool object_depth_valid = false;
};

// Turns the per-layer render passes on or off.  Costs one tile-rendering pass
// per active layer, so it is worth being able to switch off.
// Serial of the frame written out by the debug dump, or zero if none was.
// Lets the eye capture pick the same frame, which matters because anything
// on screen has usually moved by the next one.
uint64_t DumpedSerial();

// Frames per second the loaded cartridge runs at: 60 for NTSC, 50 for PAL.
// The display rate is chosen from this, because the SNES cannot be made to
// run at anything else without altering the speed of the game.
int FramesPerSecond();

void SetLayerSplit(bool enabled);
bool LayerSplitEnabled();

// --- Save states ------------------------------------------------------------
//
// Slots are per ROM: the core names them after the loaded cartridge. Requests
// are carried out on the emulation thread between frames, because freezing
// halfway through one would capture the CPU and the PPU disagreeing about
// where they are.

constexpr int kStateSlots = 10;

void RequestSaveState(int slot);
void RequestLoadState(int slot);

// Whether that slot already holds a saved game.
bool StateExists(int slot);

// A short line describing the last save, load or ROM change, for the menu.
std::string LastStateMessage();

// Swaps the running cartridge.  Carried out on the emulation thread, like the
// save states; the current game's battery save is written out first.
void RequestLoadRom(const std::string &path);

// `rom_path` may be empty: the core comes up with no cartridge, which is what
// happens when the configured ROM folder cannot be read. The menu still works,
// so the player can grant storage access or point it somewhere else.
bool Start(const std::string &base_dir, const std::string &rom_path);

// Whether a cartridge is actually loaded.
bool HasGame();
void Stop();
void SetPaused(bool paused);

// Latest finished frame.  Returns false if nothing new arrived since the
// caller's `since` serial.  The returned pointers stay valid until the next
// AcquireFrame call from the same thread.
bool AcquireFrame(Frame &out, uint64_t since);

void SetButton(Button button, bool pressed);

} // namespace emu
