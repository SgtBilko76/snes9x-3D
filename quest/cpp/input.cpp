#include "input.h"
#include "log.h"

#include <android/keycodes.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace input {
namespace {

constexpr float kStickDeadzone = 0.45f;
constexpr float kAdjustDeadzone = 0.55f;

// The menu wants a deliberate push rather than a game's quick response: a
// further throw before a row change counts, a longer wait before it starts
// repeating, and a slower repeat once it does.
constexpr float kMenuDeadzone = 0.70f;
constexpr float kMenuFirstRepeat = 0.45f;
constexpr float kMenuRepeat = 0.25f;

XrActionSet g_action_set = XR_NULL_HANDLE;
XrAction g_dpad_action = XR_NULL_HANDLE;
XrAction g_menu_action = XR_NULL_HANDLE;

struct PhysicalInput {
	const char *name;
	const char *action;
	const char *binding;
	XrAction handle = XR_NULL_HANDLE;
	bool down = false;
};

// Order matches the Physical enum.
PhysicalInput g_inputs[kPhysicalCount] = {
	{ "Right trigger", "right_trigger", "/user/hand/right/input/trigger/value" },
	{ "Left trigger",  "left_trigger",  "/user/hand/left/input/trigger/value"  },
	{ "Right A",       "right_a",       "/user/hand/right/input/a/click"       },
	{ "Right B",       "right_b",       "/user/hand/right/input/b/click"       },
	{ "Left X",        "left_x",        "/user/hand/left/input/x/click"        },
	{ "Left Y",        "left_y",        "/user/hand/left/input/y/click"        },
	{ "Right grip",    "right_grip",    "/user/hand/right/input/squeeze/value" },
	{ "Left grip",     "left_grip",     "/user/hand/left/input/squeeze/value"  },
	{ "Right stick",   "right_stick",   "/user/hand/right/input/thumbstick/click" },
};

int g_binding[kPhysicalCount];

// What each SNES button is reported as, so a rebind can release the old one.
bool g_button_held[emu::BTN_COUNT] = {};

bool g_dpad_state[4] = {false, false, false, false};
bool g_menu_was_down = false;
bool g_activate_was_down = false;
int g_repeat_direction = 0;
float g_repeat_timer = 0.0f;

// Gamepad state is merged with the controller state so either can drive the pad.
bool g_pad_buttons[emu::BTN_COUNT] = {};

XrPath ToPath(XrInstance instance, const char *text)
{
	XrPath path = XR_NULL_PATH;
	XrResult result = xrStringToPath(instance, text, &path);
	if (XR_FAILED(result))
		LOGE("xr: bad path %s (%d)", text, result);
	return path;
}

void ReportButton(emu::Button button, bool pressed)
{
	if (g_button_held[button] == pressed)
		return;
	g_button_held[button] = pressed;
	emu::SetButton(button, pressed);
}

void SetDpad(int index, emu::Button button, bool pressed)
{
	if (g_dpad_state[index] == pressed)
		return;
	g_dpad_state[index] = pressed;
	ReportButton(button, pressed);
}

} // namespace

const char *PhysicalName(int physical)
{
	if (physical < 0 || physical >= kPhysicalCount)
		return "";
	return g_inputs[physical].name;
}

int Binding(int physical)
{
	if (physical < 0 || physical >= kPhysicalCount)
		return -1;
	return g_binding[physical];
}

void SetBinding(int physical, int button)
{
	if (physical < 0 || physical >= kPhysicalCount)
		return;

	// Whatever was on it stops being pressed, or it would stick down.
	const int previous = g_binding[physical];
	if (previous >= 0)
		ReportButton(static_cast<emu::Button>(previous), false);

	g_binding[physical] = button;
	g_inputs[physical].down = false;
}

void ResetBindings()
{
	g_binding[kLeftX] = emu::BTN_Y;
	g_binding[kLeftY] = emu::BTN_X;
	g_binding[kRightA] = emu::BTN_B;
	g_binding[kRightB] = emu::BTN_A;
	g_binding[kLeftTrigger] = emu::BTN_L;
	g_binding[kRightTrigger] = emu::BTN_R;
	g_binding[kLeftGrip] = emu::BTN_SELECT;
	g_binding[kRightGrip] = emu::BTN_START;
	g_binding[kRightStick] = -1;
}

std::string SerialiseBindings()
{
	std::string text;
	for (int i = 0; i < kPhysicalCount; i++)
	{
		if (i)
			text += ",";
		text += std::to_string(g_binding[i]);
	}
	return text;
}

void ParseBindings(const std::string &text)
{
	int index = 0;
	size_t start = 0;

	while (index < kPhysicalCount && start <= text.size())
	{
		const size_t comma = text.find(',', start);
		const std::string piece =
			text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

		if (!piece.empty())
		{
			const int button = atoi(piece.c_str());
			g_binding[index] = (button >= 0 && button < emu::BTN_COUNT) ? button : -1;
		}

		index++;
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
}

bool Init(XrInstance instance, XrSession session)
{
	ResetBindings();

	XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(set_info.actionSetName, "gameplay");
	strcpy(set_info.localizedActionSetName, "Gameplay");
	set_info.priority = 0;

	if (XR_FAILED(xrCreateActionSet(instance, &set_info, &g_action_set)))
	{
		LOGE("xr: cannot create action set");
		return false;
	}

	auto make_action = [&](const char *name, const char *localized,
	                       XrActionType type) -> XrAction {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		strcpy(info.actionName, name);
		strcpy(info.localizedActionName, localized);
		info.actionType = type;

		XrAction action = XR_NULL_HANDLE;
		if (XR_FAILED(xrCreateAction(g_action_set, &info, &action)))
			LOGE("xr: cannot create action %s", name);
		return action;
	};

	g_dpad_action = make_action("dpad", "D-pad", XR_ACTION_TYPE_VECTOR2F_INPUT);
	g_menu_action = make_action("menu", "Options menu", XR_ACTION_TYPE_BOOLEAN_INPUT);

	for (auto &entry : g_inputs)
		entry.handle = make_action(entry.action, entry.name, XR_ACTION_TYPE_BOOLEAN_INPUT);

	std::vector<XrActionSuggestedBinding> bindings;
	bindings.push_back({g_dpad_action, ToPath(instance, "/user/hand/left/input/thumbstick")});
	bindings.push_back({g_menu_action, ToPath(instance, "/user/hand/left/input/menu/click")});

	for (auto &entry : g_inputs)
		bindings.push_back({entry.handle, ToPath(instance, entry.binding)});

	XrInteractionProfileSuggestedBinding suggested{
		XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggested.interactionProfile =
		ToPath(instance, "/interaction_profiles/oculus/touch_controller");
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());

	XrResult result = xrSuggestInteractionProfileBindings(instance, &suggested);
	if (XR_FAILED(result))
	{
		LOGE("xr: cannot suggest bindings (%d)", result);
		return false;
	}

	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &g_action_set;

	result = xrAttachSessionActionSets(session, &attach);
	if (XR_FAILED(result))
	{
		LOGE("xr: cannot attach action sets (%d)", result);
		return false;
	}

	return true;
}

void Shutdown()
{
	if (g_action_set != XR_NULL_HANDLE)
		xrDestroyActionSet(g_action_set);
	g_action_set = XR_NULL_HANDLE;
}

void Sync(XrSession session, bool menu_open, float delta_seconds, Controls &controls)
{
	XrActiveActionSet active{g_action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;

	if (XR_FAILED(xrSyncActions(session, &sync)))
		return;

	XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};

	// The menu button toggles, whether the menu is open or not.
	get.action = g_menu_action;
	XrActionStateBoolean menu{XR_TYPE_ACTION_STATE_BOOLEAN};
	if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &menu)) && menu.isActive)
	{
		const bool down = menu.currentState == XR_TRUE;
		controls.menu_toggle = down && !g_menu_was_down;
		g_menu_was_down = down;
	}

	for (int i = 0; i < kPhysicalCount; i++)
	{
		get.action = g_inputs[i].handle;
		XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
		if (XR_FAILED(xrGetActionStateBoolean(session, &get, &state)) || !state.isActive)
			continue;

		g_inputs[i].down = state.currentState == XR_TRUE;
	}

	// Right A always selects in the menu, whatever it is bound to, so the menu
	// stays usable no matter how the pad has been arranged.
	const bool activate_down = g_inputs[kRightA].down || g_pad_buttons[emu::BTN_B];

	if (!menu_open)
	{
		for (int button = 0; button < emu::BTN_COUNT; button++)
		{
			if (button >= emu::BTN_UP && button <= emu::BTN_RIGHT)
				continue;   // the d-pad comes from the stick below

			bool pressed = g_pad_buttons[button];
			for (int i = 0; i < kPhysicalCount && !pressed; i++)
				if (g_binding[i] == button)
					pressed = g_inputs[i].down;

			ReportButton(static_cast<emu::Button>(button), pressed);
		}
	}

	get.action = g_dpad_action;
	XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
	const bool stick_ok =
		XR_SUCCEEDED(xrGetActionStateVector2f(session, &get, &stick)) && stick.isActive;

	if (menu_open)
	{
		controls.menu_activate = activate_down && !g_activate_was_down;
		g_activate_was_down = activate_down;

		if (stick_ok)
		{
			const float x = stick.currentState.x;
			const float y = stick.currentState.y;

			// Up on the stick is +y, but the menu counts rows downwards.
			const int direction = y > kMenuDeadzone ? -1 : (y < -kMenuDeadzone ? 1 : 0);

			if (direction != g_repeat_direction)
			{
				g_repeat_direction = direction;
				g_repeat_timer = 0.0f;
				controls.menu_vertical = direction;
			}
			else if (direction != 0)
			{
				g_repeat_timer += delta_seconds;
				if (g_repeat_timer > kMenuFirstRepeat)
				{
					g_repeat_timer = kMenuFirstRepeat - kMenuRepeat;
					controls.menu_vertical = direction;
				}
			}

			if (x > kAdjustDeadzone || x < -kAdjustDeadzone)
				controls.menu_horizontal = x;
		}

		return;
	}

	g_activate_was_down = activate_down;
	g_repeat_direction = 0;

	// Left thumbstick drives the d-pad.
	if (stick_ok)
	{
		SetDpad(0, emu::BTN_LEFT,  stick.currentState.x < -kStickDeadzone || g_pad_buttons[emu::BTN_LEFT]);
		SetDpad(1, emu::BTN_RIGHT, stick.currentState.x >  kStickDeadzone || g_pad_buttons[emu::BTN_RIGHT]);
		SetDpad(2, emu::BTN_DOWN,  stick.currentState.y < -kStickDeadzone || g_pad_buttons[emu::BTN_DOWN]);
		SetDpad(3, emu::BTN_UP,    stick.currentState.y >  kStickDeadzone || g_pad_buttons[emu::BTN_UP]);
	}
}

bool HandleAndroidKey(int32_t keycode, bool pressed)
{
	emu::Button button;

	switch (keycode)
	{
		case AKEYCODE_BUTTON_A:      button = emu::BTN_B; break;
		case AKEYCODE_BUTTON_B:      button = emu::BTN_A; break;
		case AKEYCODE_BUTTON_X:      button = emu::BTN_Y; break;
		case AKEYCODE_BUTTON_Y:      button = emu::BTN_X; break;
		case AKEYCODE_BUTTON_L1:     button = emu::BTN_L; break;
		case AKEYCODE_BUTTON_R1:     button = emu::BTN_R; break;
		case AKEYCODE_BUTTON_START:  button = emu::BTN_START; break;
		case AKEYCODE_BUTTON_SELECT: button = emu::BTN_SELECT; break;
		case AKEYCODE_DPAD_UP:       button = emu::BTN_UP; break;
		case AKEYCODE_DPAD_DOWN:     button = emu::BTN_DOWN; break;
		case AKEYCODE_DPAD_LEFT:     button = emu::BTN_LEFT; break;
		case AKEYCODE_DPAD_RIGHT:    button = emu::BTN_RIGHT; break;
		default: return false;
	}

	g_pad_buttons[button] = pressed;
	return true;
}

bool HandleAndroidAxis(float x, float y)
{
	g_pad_buttons[emu::BTN_LEFT]  = x < -kStickDeadzone;
	g_pad_buttons[emu::BTN_RIGHT] = x >  kStickDeadzone;
	g_pad_buttons[emu::BTN_UP]    = y < -kStickDeadzone;
	g_pad_buttons[emu::BTN_DOWN]  = y >  kStickDeadzone;
	return true;
}

} // namespace input
