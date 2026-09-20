#include "input.h"
#include "emu.h"
#include "log.h"

#include <android/keycodes.h>

#include <cstring>
#include <vector>

namespace input {
namespace {

constexpr float kStickDeadzone = 0.45f;
// Generous, because nothing here is worth a screen that creeps towards the
// player on its own while a controller rests on a table.
constexpr float kAdjustDeadzone = 0.55f;

XrActionSet g_action_set = XR_NULL_HANDLE;

XrAction g_dpad_action = XR_NULL_HANDLE;
XrAction g_menu_action = XR_NULL_HANDLE;

struct ButtonAction {
	emu::Button button;
	const char *name;
	const char *binding;
	XrAction action = XR_NULL_HANDLE;
	bool pressed = false;
};

// Touch has exactly twelve inputs to spare once the left stick becomes the
// d-pad, which is what a SNES pad needs.
ButtonAction g_buttons[] = {
	{ emu::BTN_Y,      "snes_y",      "/user/hand/left/input/x/click"        },
	{ emu::BTN_X,      "snes_x",      "/user/hand/left/input/y/click"        },
	{ emu::BTN_B,      "snes_b",      "/user/hand/right/input/a/click"       },
	{ emu::BTN_A,      "snes_a",      "/user/hand/right/input/b/click"       },
	{ emu::BTN_L,      "snes_l",      "/user/hand/left/input/trigger/value"  },
	{ emu::BTN_R,      "snes_r",      "/user/hand/right/input/trigger/value" },
	{ emu::BTN_SELECT, "snes_select", "/user/hand/left/input/squeeze/value"  },
	{ emu::BTN_START,  "snes_start",  "/user/hand/right/input/squeeze/value" },
};

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

void SetDpad(int index, emu::Button button, bool pressed)
{
	if (g_dpad_state[index] == pressed)
		return;
	g_dpad_state[index] = pressed;
	emu::SetButton(button, pressed);
}

} // namespace

bool Init(XrInstance instance, XrSession session)
{
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

	for (auto &entry : g_buttons)
		entry.action = make_action(entry.name, entry.name, XR_ACTION_TYPE_BOOLEAN_INPUT);

	std::vector<XrActionSuggestedBinding> bindings;
	bindings.push_back({g_dpad_action, ToPath(instance, "/user/hand/left/input/thumbstick")});
	bindings.push_back({g_menu_action, ToPath(instance, "/user/hand/left/input/menu/click")});

	for (auto &entry : g_buttons)
		bindings.push_back({entry.action, ToPath(instance, entry.binding)});

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

// Releases anything the pad is holding, so opening the menu mid-input does not
// leave a button stuck down in the game.
void ReleasePad()
{
	for (auto &entry : g_buttons)
		if (entry.pressed)
		{
			entry.pressed = false;
			emu::SetButton(entry.button, false);
		}

	static const emu::Button kDpad[4] = {emu::BTN_LEFT, emu::BTN_RIGHT,
	                                     emu::BTN_DOWN, emu::BTN_UP};
	for (int i = 0; i < 4; i++)
		if (g_dpad_state[i])
		{
			g_dpad_state[i] = false;
			emu::SetButton(kDpad[i], false);
		}
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

	// Face and shoulder buttons.
	bool activate_down = false;
	for (auto &entry : g_buttons)
	{
		get.action = entry.action;
		XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
		if (XR_FAILED(xrGetActionStateBoolean(session, &get, &state)) || !state.isActive)
			continue;

		const bool pressed = state.currentState == XR_TRUE || g_pad_buttons[entry.button];

		// The SNES B button doubles as the menu's select.
		if (entry.button == emu::BTN_B)
			activate_down = pressed;

		if (menu_open)
			continue;

		if (pressed != entry.pressed)
		{
			entry.pressed = pressed;
			emu::SetButton(entry.button, pressed);
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
			const int direction = y > kStickDeadzone ? -1 : (y < -kStickDeadzone ? 1 : 0);

			if (direction != g_repeat_direction)
			{
				g_repeat_direction = direction;
				g_repeat_timer = 0.0f;
				controls.menu_vertical = direction;
			}
			else if (direction != 0)
			{
				g_repeat_timer += delta_seconds;
				if (g_repeat_timer > 0.25f)
				{
					g_repeat_timer = 0.15f;
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

	if (g_pad_buttons[button] != pressed)
	{
		g_pad_buttons[button] = pressed;
		emu::SetButton(button, pressed);
	}

	return true;
}

bool HandleAndroidAxis(float x, float y)
{
	struct { emu::Button button; bool pressed; } states[] = {
		{ emu::BTN_LEFT,  x < -kStickDeadzone },
		{ emu::BTN_RIGHT, x >  kStickDeadzone },
		{ emu::BTN_UP,    y < -kStickDeadzone },
		{ emu::BTN_DOWN,  y >  kStickDeadzone },
	};

	for (auto &state : states)
		if (g_pad_buttons[state.button] != state.pressed)
		{
			g_pad_buttons[state.button] = state.pressed;
			emu::SetButton(state.button, state.pressed);
		}

	return true;
}

} // namespace input
