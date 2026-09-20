#pragma once

// Set by the build; the fallback keeps a plain cmake build compiling.
#ifndef SNES9X_VR_VERSION
#define SNES9X_VR_VERSION "dev"
#endif

namespace vr {

struct ScreenGeometry {
	// Metres from the viewer to the screen. The apparent size comes from the
	// width angle rather than this, so distance changes where the screen sits
	// without changing how big it looks.
	float radius = 3.0f;
	float central_angle = 0.80f;  // radians of arc the screen covers
	float aspect = 4.0f / 3.0f;

	// Maximum separation between the eyes, in metres, for the farthest layer.
	// Expressed as a distance rather than in pixels because what matters is
	// how it compares to the eyes themselves: at one interpupillary distance
	// the far layer sits at infinity, and beyond that the eyes would have to
	// diverge, which nobody can fuse.
	float stereo = 0.02f;

	// Render each background and the sprites separately so a displaced layer
	// uncovers the one behind it, rather than warping the finished picture.
	// Costs one tile-rendering pass per active layer.
	bool layer_split = true;

	// How wide the blend across a texel edge is: 0 none, then progressively
	// wider, up to ordinary bilinear at the top.
	int filter = 1;

	// Trim on the way to the display; 1.0 leaves the picture alone, higher
	// darkens it.
	float gamma = 1.0f;
};

constexpr float kMinGamma = 0.6f;
constexpr float kMaxGamma = 1.8f;

constexpr int kFilterCount = 5;

constexpr float kMinRadius = 1.0f;
constexpr float kMaxRadius = 12.0f;
constexpr float kMinAngle = 0.25f;
constexpr float kMaxAngle = 1.9f;

// Half a typical interpupillary distance: the far layer then sits at roughly
// twice the screen distance, which reads as depth without straining.
constexpr float kMaxStereo = 0.032f;

} // namespace vr
