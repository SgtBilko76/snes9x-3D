#pragma once

#include "emu.h"

#include <openxr/openxr.h>

#include <string>

namespace input {

// The physical inputs a SNES button can be put on.  OpenXR bindings are fixed
// once the session starts, so each of these gets an action of its own and the
// mapping to a SNES button is ours to change.
enum Physical {
	kRightTrigger, kLeftTrigger,
	kRightA, kRightB,
	kLeftX, kLeftY,
	kRightGrip, kLeftGrip,
	kRightStick,
	kPhysicalCount
};

const char *PhysicalName(int physical);

// The SNES button on that input, or -1 for none.
int Binding(int physical);
void SetBinding(int physical, int button);
void ResetBindings();

// For the config file: "3,4,1,..." over all physical inputs.
std::string SerialiseBindings();
void ParseBindings(const std::string &text);

// What the player asked for this frame, beyond the SNES pad itself.
struct Controls {
	bool menu_toggle = false;     // edge-triggered
	int menu_vertical = 0;        // edge-triggered with repeat: -1 up, +1 down
	float menu_horizontal = 0.0f; // held, -1..1
	bool menu_activate = false;   // edge-triggered
};

bool Init(XrInstance instance, XrSession session);
void Shutdown();

// Polls the controllers.  While the menu is open the SNES pad is left alone
// and the sticks drive the menu instead.
void Sync(XrSession session, bool menu_open, float delta_seconds, Controls &controls);

// Feeds a button coming from an Android gamepad rather than a Touch controller.
bool HandleAndroidKey(int32_t keycode, bool pressed);
bool HandleAndroidAxis(float x, float y);

} // namespace input
