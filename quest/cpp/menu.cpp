#include "menu.h"

#include "emu.h"
#include "input.h"
#include "var8x10font.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace menu {
namespace {

constexpr int kFontWidth = 8;
constexpr int kFontHeight = 10;

// The options pages are drawn at four times the font's own size and the
// browsers at twice, so a long file name still fits across the panel while the
// options stay comfortable to read.
constexpr int kOptionScale = 4;
constexpr int kRomScale = 2;

constexpr int kMarginX = 64;
constexpr int kTitleY = 48;
constexpr int kDividerY = 108;

constexpr int kOptionFirstRow = 132;
constexpr int kOptionRowHeight = 66;

constexpr int kRomFirstRow = 128;
constexpr int kRomRowHeight = 34;

constexpr int kControlRowHeight = 52;
constexpr int kControlScale = 3;

// RGB565.
constexpr uint16_t kPanel     = 0x18E3;
constexpr uint16_t kBorder    = 0x5AEB;
constexpr uint16_t kText      = 0xFFFF;
constexpr uint16_t kDimText   = 0xAD55;
constexpr uint16_t kHighlight = 0x02BF;
constexpr uint16_t kTitle     = 0xFD20;

// The main page holds what you reach for while playing. Anything set once and
// left alone lives a level down, so the list stays short enough to read.
enum Item { kLoadRom, kSlot, kSaveState, kLoadState, kControls, kSettings,
            kRecenter, kCloseItem, kItemCount };

enum SettingItem { kDistance, kWidth, kStereo, kDepthMode, kSmoothing, kGamma,
                   kRomFolder, kStorage, kSettingsBack, kSettingCount };

enum class Page { Options, Settings, Controls, Roms, Folders };

bool g_open = false;
bool g_dirty = true;
Page g_page = Page::Options;

int g_selected = 0;
int g_setting_selected = 0;
int g_control_selected = 0;
int g_slot = 0;
float g_message_timer = 0.0f;

std::string g_rom_dir;
bool g_storage_access = false;

std::string g_folder;
std::string g_chosen_folder;
std::vector<std::string> g_subfolders;
int g_folder_selected = 0;
int g_folder_scroll = 0;

std::vector<std::string> g_roms;
std::string g_chosen_rom;
int g_rom_selected = 0;
int g_rom_scroll = 0;

float g_last_radius = -1.0f;
float g_last_angle = -1.0f;
float g_last_stereo = -1.0f;
float g_last_gamma = -1.0f;
int g_last_split = -1;
int g_last_filter = -1;
bool g_mode_held = false;
bool g_mode_was_held = false;

// --- text -------------------------------------------------------------------

int GlyphWidth(uint8_t c)
{
	if (c < 32)
		return kFontWidth;
	return kFontWidth - var8x10font_kern[c - 32][0] - var8x10font_kern[c - 32][1];
}

int TextWidth(const std::string &text, int scale)
{
	int width = 0;
	for (unsigned char c : text)
		width += (GlyphWidth(c) - 1) * scale;
	return width;
}

void FillRect(uint16_t *pixels, int stride, int height,
              int x0, int y0, int w, int h, uint16_t colour)
{
	for (int y = std::max(0, y0); y < std::min(height, y0 + h); y++)
		for (int x = std::max(0, x0); x < std::min(stride, x0 + w); x++)
			pixels[y * stride + x] = colour;
}

int DrawChar(uint16_t *pixels, int stride, int height, int x, int y,
             uint8_t c, uint16_t colour, int scale)
{
	if (c < 32)
		return 0;

	const int index = c - 32;
	const int width = GlyphWidth(c);
	const int line = (index >> 4) * kFontHeight;
	const int offset = (index & 15) * kFontWidth + var8x10font_kern[index][0];

	for (int row = 0; row < kFontHeight; row++)
		for (int column = 0; column < width; column++)
			if (var8x10font[line + row][offset + column] == '#')
				FillRect(pixels, stride, height,
				         x + column * scale, y + row * scale, scale, scale, colour);

	return (width - 1) * scale;
}

void DrawText(uint16_t *pixels, int stride, int height, int x, int y,
              const std::string &text, uint16_t colour, int scale)
{
	for (unsigned char c : text)
		x += DrawChar(pixels, stride, height, x, y, c, colour, scale);
}

// Shortens from the middle, keeping both ends. ROM names differ in their
// region and revision suffixes as often as in their titles, and a path's last
// component is the part worth reading, so cutting the end off either would
// throw away what identifies it.
std::string Elide(const std::string &text, int available, int scale)
{
	if (TextWidth(text, scale) <= available)
		return text;

	const int ellipsis = TextWidth("...", scale);
	if (available <= ellipsis)
		return "...";

	const int tail_budget = (available - ellipsis) * 2 / 5;

	size_t tail = 0;
	while (tail < text.size() &&
	       TextWidth(text.substr(text.size() - tail - 1), scale) <= tail_budget)
		tail++;

	const int head_budget =
		available - ellipsis - TextWidth(text.substr(text.size() - tail), scale);

	size_t head = 0;
	while (head + tail < text.size() &&
	       TextWidth(text.substr(0, head + 1), scale) <= head_budget)
		head++;

	return text.substr(0, head) + "..." + text.substr(text.size() - tail);
}

std::string Format(const char *format, float value)
{
	char buffer[64];
	snprintf(buffer, sizeof(buffer), format, value);
	return buffer;
}

// One row of a label with an optional value on the right, shortened so a long
// value cannot run back over its label.
void DrawRow(uint16_t *pixels, int width, int height, int y, bool selected,
             const std::string &label, const std::string &value, int scale)
{
	if (selected)
		FillRect(pixels, width, height, kMarginX - 24, y - 12,
		         width - 2 * (kMarginX - 24), kFontHeight * scale + 24, kHighlight);

	DrawText(pixels, width, height, kMarginX, y, label,
	         selected ? kText : kDimText, scale);

	if (value.empty())
		return;

	const int room = width - 2 * kMarginX - TextWidth(label, scale) - 3 * scale;
	const std::string shown = Elide(value, room, scale);

	DrawText(pixels, width, height, width - kMarginX - TextWidth(shown, scale), y,
	         shown, selected ? kText : kDimText, scale);
}

void DrawHeader(uint16_t *pixels, int width, int height, const char *title)
{
	DrawText(pixels, width, height, kMarginX, kTitleY, title, kTitle, kOptionScale);

	const std::string version = std::string("Snes9x 3D ") + SNES9X_VR_VERSION;
	DrawText(pixels, width, height,
	         width - kMarginX - TextWidth(version, kRomScale), kTitleY + 6,
	         version, kDimText, kRomScale);

	const std::string credit = "Meta Quest port by Sgt. Bilko";
	DrawText(pixels, width, height,
	         width - kMarginX - TextWidth(credit, kRomScale), kTitleY + 28,
	         credit, kDimText, kRomScale);

	FillRect(pixels, width, height, kMarginX, kDividerY, width - 2 * kMarginX, 2,
	         kBorder);
}

int VisibleRomRows(int height)
{
	return std::max(1, (height - kRomFirstRow - 70) / kRomRowHeight);
}

// --- main page ---------------------------------------------------------------

const char *LabelText(int item)
{
	switch (item)
	{
		case kLoadRom:   return "Load ROM...";
		case kSlot:      return "Save slot";
		case kSaveState: return "Save game";
		case kLoadState: return "Load game";
		case kControls:  return "Controls...";
		case kSettings:  return "Settings...";
		case kRecenter:  return "Recenter screen";
		case kCloseItem: return "Close";
		default:         return "";
	}
}

std::string ValueText(int item)
{
	if (item == kSlot)
		return std::to_string(g_slot) +
		       (emu::StateExists(g_slot) ? "  (used)" : "  (empty)");
	return std::string();
}

Action UpdateOptions(int vertical, float horizontal, bool activate)
{
	if (vertical != 0)
	{
		g_selected = (g_selected + vertical + kItemCount) % kItemCount;
		g_dirty = true;
	}

	if (horizontal != 0.0f && !g_mode_held && g_selected == kSlot)
	{
		g_slot = (g_slot + (horizontal > 0.0f ? 1 : emu::kStateSlots - 1)) %
		         emu::kStateSlots;
		g_dirty = true;
	}

	if (!activate)
		return Action::None;

	g_dirty = true;

	switch (g_selected)
	{
		case kRecenter:  return Action::Recenter;
		case kCloseItem: Close(); break;

		case kSaveState:
			g_message_timer = 3.0f;
			return Action::SaveState;

		case kLoadState:
			g_message_timer = 3.0f;
			return Action::LoadState;

		case kLoadRom:
			g_page = Page::Roms;
			g_rom_selected = 0;
			g_rom_scroll = 0;
			break;

		case kControls:
			g_page = Page::Controls;
			g_control_selected = 0;
			break;

		case kSettings:
			g_page = Page::Settings;
			g_setting_selected = 0;
			break;

		default: break;
	}

	return Action::None;
}

void DrawOptions(uint16_t *pixels, int width, int height)
{
	DrawHeader(pixels, width, height, "VR OPTIONS");

	for (int item = 0; item < kItemCount; item++)
		DrawRow(pixels, width, height, kOptionFirstRow + item * kOptionRowHeight,
		        item == g_selected, LabelText(item), ValueText(item), kOptionScale);
}

// --- settings page ------------------------------------------------------------

const char *SettingLabel(int item)
{
	switch (item)
	{
		case kDistance:     return "Screen distance";
		case kWidth:        return "Screen width";
		case kStereo:       return "Stereo depth";
		case kDepthMode:    return "Depth mode";
		case kSmoothing:    return "Smoothing";
		case kGamma:        return "Gamma";
		case kRomFolder:    return "ROM folder...";
		case kStorage:      return "Storage access";
		case kSettingsBack: return "< Back";
		default:            return "";
	}
}

std::string SettingValue(const vr::ScreenGeometry &screen, int item)
{
	switch (item)
	{
		case kDistance:  return Format("%.1f m", screen.radius);
		case kWidth:     return Format("%.0f deg", screen.central_angle * 57.2958f);
		case kStereo:    return screen.stereo <= 0.0005f
		                        ? std::string("off")
		                        : Format("%.0f mm", screen.stereo * 1000.0f);
		case kDepthMode: return screen.layer_split ? std::string("Layers")
		                                           : std::string("Warp");
		case kSmoothing:
		{
			static const char *kNames[] = {"Pixels", "Sharp", "Smooth", "Softer",
			                               "Soft"};
			return kNames[std::clamp(screen.filter, 0, vr::kFilterCount - 1)];
		}
		case kGamma:     return Format("%.2f", screen.gamma);
		case kRomFolder: return g_rom_dir;
		case kStorage:   return g_storage_access ? std::string("granted")
		                                         : std::string("not granted");
		default:         return std::string();
	}
}

Action UpdateSettings(vr::ScreenGeometry &screen, int vertical, float horizontal,
                      bool activate, float delta_seconds)
{
	if (vertical != 0)
	{
		g_setting_selected =
			(g_setting_selected + vertical + kSettingCount) % kSettingCount;
		g_dirty = true;
	}

	if (horizontal != 0.0f)
	{
		switch (g_setting_selected)
		{
			case kDistance:
				screen.radius =
					std::clamp(screen.radius + horizontal * 2.0f * delta_seconds,
					           vr::kMinRadius, vr::kMaxRadius);
				break;

			case kWidth:
				screen.central_angle =
					std::clamp(screen.central_angle + horizontal * 0.5f * delta_seconds,
					           vr::kMinAngle, vr::kMaxAngle);
				break;

			case kStereo:
				screen.stereo =
					std::clamp(screen.stereo + horizontal * 0.02f * delta_seconds,
					           0.0f, vr::kMaxStereo);
				break;

			case kGamma:
				screen.gamma =
					std::clamp(screen.gamma + horizontal * 0.5f * delta_seconds,
					           vr::kMinGamma, vr::kMaxGamma);
				break;

			case kDepthMode:
				// A toggle, so only act on the moment the stick is pushed.
				if (!g_mode_held)
					screen.layer_split = horizontal > 0.0f;
				break;

			case kSmoothing:
				if (!g_mode_held)
					screen.filter =
						std::clamp(screen.filter + (horizontal > 0.0f ? 1 : -1),
						           0, vr::kFilterCount - 1);
				break;

			default:
				break;
		}
	}

	if (!activate)
		return Action::None;

	g_dirty = true;

	switch (g_setting_selected)
	{
		case kRomFolder:
			g_chosen_folder = g_rom_dir;
			return Action::OpenFolder;

		case kStorage:
			return Action::GrantStorage;

		case kSettingsBack:
			g_page = Page::Options;
			break;

		default:
			break;
	}

	return Action::None;
}

void DrawSettings(const vr::ScreenGeometry &screen, uint16_t *pixels,
                  int width, int height)
{
	DrawHeader(pixels, width, height, "SETTINGS");

	for (int item = 0; item < kSettingCount; item++)
		DrawRow(pixels, width, height, kOptionFirstRow + item * kOptionRowHeight,
		        item == g_setting_selected, SettingLabel(item),
		        SettingValue(screen, item), kOptionScale);
}

// --- controls page ------------------------------------------------------------

const char *ButtonName(int button)
{
	static const char *kNames[emu::BTN_COUNT] = {
		"Up", "Down", "Left", "Right", "A", "B", "X", "Y",
		"L", "R", "Start", "Select"
	};

	if (button < 0 || button >= emu::BTN_COUNT)
		return "-";
	return kNames[button];
}

int ControlEntryCount() { return input::kPhysicalCount + 2; }

Action UpdateControls(int vertical, float horizontal, bool activate)
{
	const int count = ControlEntryCount();

	if (vertical != 0)
	{
		g_control_selected = (g_control_selected + vertical + count) % count;
		g_dirty = true;
	}

	if (horizontal != 0.0f && !g_mode_held && g_control_selected < input::kPhysicalCount)
	{
		// Cycles through the SNES buttons and back round through "none".
		const int step = horizontal > 0.0f ? 1 : -1;
		const int span = emu::BTN_COUNT + 1;
		int next = input::Binding(g_control_selected) + 1 + step;
		next = (next % span + span) % span;

		input::SetBinding(g_control_selected, next - 1);
		g_dirty = true;
		return Action::BindingsChanged;
	}

	if (!activate)
		return Action::None;

	g_dirty = true;

	if (g_control_selected == input::kPhysicalCount)
	{
		input::ResetBindings();
		return Action::BindingsChanged;
	}

	if (g_control_selected == input::kPhysicalCount + 1)
		g_page = Page::Options;

	return Action::None;
}

void DrawControls(uint16_t *pixels, int width, int height)
{
	DrawHeader(pixels, width, height, "CONTROLS");

	DrawText(pixels, width, height, kMarginX, kDividerY + 14,
	         "Left stick is always the d-pad.", kDimText, kRomScale);

	for (int index = 0; index < ControlEntryCount(); index++)
	{
		const int y = kRomFirstRow + 34 + index * kControlRowHeight;
		const bool selected = index == g_control_selected;

		std::string label;
		std::string value;

		if (index < input::kPhysicalCount)
		{
			label = input::PhysicalName(index);
			value = ButtonName(input::Binding(index));
		}
		else if (index == input::kPhysicalCount)
			label = "Reset to defaults";
		else
			label = "< Back";

		DrawRow(pixels, width, height, y, selected, label, value, kControlScale);
	}
}

// --- ROM browser --------------------------------------------------------------

Action UpdateRoms(int vertical, bool activate, int height)
{
	// One past the end is the way back out.
	const int count = static_cast<int>(g_roms.size()) + 1;

	if (vertical != 0)
	{
		g_rom_selected = (g_rom_selected + vertical + count) % count;
		g_dirty = true;

		const int rows = VisibleRomRows(height);
		g_rom_scroll = std::clamp(g_rom_scroll, g_rom_selected - rows + 1, g_rom_selected);
		g_rom_scroll = std::clamp(g_rom_scroll, 0, std::max(0, count - rows));
	}

	if (!activate)
		return Action::None;

	g_dirty = true;

	if (g_rom_selected >= static_cast<int>(g_roms.size()))
	{
		g_page = Page::Options;
		return Action::None;
	}

	g_chosen_rom = g_roms[g_rom_selected];
	g_message_timer = 3.0f;
	g_page = Page::Options;
	return Action::LoadRom;
}

void DrawRoms(uint16_t *pixels, int width, int height)
{
	DrawHeader(pixels, width, height, "LOAD ROM");

	if (g_roms.empty())
		DrawText(pixels, width, height, kMarginX, kRomFirstRow,
		         "No ROMs in this folder", kDimText, kRomScale);

	const int rows = VisibleRomRows(height);
	const int count = static_cast<int>(g_roms.size()) + 1;
	const int available = width - 2 * kMarginX;

	for (int row = 0; row < rows; row++)
	{
		const int index = g_rom_scroll + row;
		if (index >= count)
			break;

		const int y = kRomFirstRow + row * kRomRowHeight;
		const bool selected = index == g_rom_selected;

		if (selected)
			FillRect(pixels, width, height, kMarginX - 16, y - 6,
			         width - 2 * (kMarginX - 16),
			         kFontHeight * kRomScale + 12, kHighlight);

		const std::string label = index < static_cast<int>(g_roms.size())
			? Elide(g_roms[index], available, kRomScale)
			: std::string("< Back");

		DrawText(pixels, width, height, kMarginX, y, label,
		         selected ? kText : kDimText, kRomScale);
	}
}

// --- folder browser -----------------------------------------------------------

// "Use this folder", then the parent, then whatever is inside.
int FolderEntryCount() { return 2 + static_cast<int>(g_subfolders.size()); }

Action UpdateFolders(int vertical, bool activate, int height)
{
	const int count = FolderEntryCount();

	if (vertical != 0)
	{
		g_folder_selected = (g_folder_selected + vertical + count) % count;
		g_dirty = true;

		const int rows = VisibleRomRows(height);
		g_folder_scroll = std::clamp(g_folder_scroll, g_folder_selected - rows + 1,
		                             g_folder_selected);
		g_folder_scroll = std::clamp(g_folder_scroll, 0, std::max(0, count - rows));
	}

	if (!activate)
		return Action::None;

	g_dirty = true;

	if (g_folder_selected == 0)
	{
		g_rom_dir = g_folder;
		g_page = Page::Settings;
		g_message_timer = 3.0f;
		return Action::UseFolder;
	}

	if (g_folder_selected == 1)
	{
		const size_t slash = g_folder.rfind('/');
		g_chosen_folder =
			(slash == std::string::npos || slash == 0) ? "/" : g_folder.substr(0, slash);
	}
	else
	{
		g_chosen_folder = (g_folder == "/" ? "" : g_folder) + "/" +
		                  g_subfolders[g_folder_selected - 2];
	}

	return Action::OpenFolder;
}

void DrawFolders(uint16_t *pixels, int width, int height)
{
	DrawHeader(pixels, width, height, "ROM FOLDER");

	const int available = width - 2 * kMarginX;
	DrawText(pixels, width, height, kMarginX, kDividerY + 14,
	         Elide(g_folder, available, kRomScale), kText, kRomScale);

	const int rows = VisibleRomRows(height) - 1;
	const int count = FolderEntryCount();

	for (int row = 0; row < rows; row++)
	{
		const int index = g_folder_scroll + row;
		if (index >= count)
			break;

		const int y = kRomFirstRow + 28 + row * kRomRowHeight;
		const bool selected = index == g_folder_selected;

		if (selected)
			FillRect(pixels, width, height, kMarginX - 16, y - 6,
			         width - 2 * (kMarginX - 16),
			         kFontHeight * kRomScale + 12, kHighlight);

		std::string label;
		if (index == 0)
			label = "[ Use this folder ]";
		else if (index == 1)
			label = "..";
		else
			label = Elide(g_subfolders[index - 2], available, kRomScale);

		DrawText(pixels, width, height, kMarginX, y, label,
		         selected ? kText : kDimText, kRomScale);
	}
}

} // namespace

// --- interface ----------------------------------------------------------------

void Toggle()
{
	g_open = !g_open;
	g_page = Page::Options;
	g_dirty = true;
}

void OpenRomList()
{
	g_open = true;
	g_page = Page::Roms;
	g_rom_selected = 0;
	g_rom_scroll = 0;
	g_dirty = true;
}

void Close()
{
	g_open = false;
	g_dirty = true;
}

bool IsOpen() { return g_open; }

int SelectedSlot() { return g_slot; }

void SetRomList(std::vector<std::string> names)
{
	g_roms = std::move(names);
	g_rom_selected = 0;
	g_rom_scroll = 0;
	g_dirty = true;
}

const std::string &SelectedRom() { return g_chosen_rom; }

void SetRomDir(std::string path)
{
	g_rom_dir = std::move(path);
	g_dirty = true;
}

void SetFolderList(std::string path, std::vector<std::string> subfolders)
{
	g_folder = std::move(path);
	g_subfolders = std::move(subfolders);
	g_folder_selected = 0;
	g_folder_scroll = 0;
	g_page = Page::Folders;
	g_dirty = true;
}

const std::string &CurrentFolder() { return g_folder; }
const std::string &ChosenFolder() { return g_chosen_folder; }

void SetStorageAccess(bool granted)
{
	if (granted != g_storage_access)
		g_dirty = true;
	g_storage_access = granted;
}

Action Update(vr::ScreenGeometry &screen, int vertical, float horizontal,
              bool activate, float delta_seconds)
{
	if (!g_open)
		return Action::None;

	g_mode_held = g_mode_was_held && horizontal != 0.0f;
	g_mode_was_held = horizontal != 0.0f;

	if (g_message_timer > 0.0f)
	{
		g_message_timer -= delta_seconds;
		if (g_message_timer <= 0.0f)
			g_dirty = true;
	}

	if (screen.radius != g_last_radius || screen.central_angle != g_last_angle ||
	    screen.stereo != g_last_stereo || screen.gamma != g_last_gamma ||
	    screen.layer_split != (g_last_split == 1) || screen.filter != g_last_filter)
	{
		g_last_radius = screen.radius;
		g_last_angle = screen.central_angle;
		g_last_stereo = screen.stereo;
		g_last_gamma = screen.gamma;
		g_last_split = screen.layer_split ? 1 : 0;
		g_last_filter = screen.filter;
		g_dirty = true;
	}

	// The browsers know the panel height from the last draw; 992 is what the
	// app uses and the only cost of being wrong is a scroll step.
	switch (g_page)
	{
		case Page::Settings:
			return UpdateSettings(screen, vertical, horizontal, activate, delta_seconds);
		case Page::Controls:
			return UpdateControls(vertical, horizontal, activate);
		case Page::Roms:
			return UpdateRoms(vertical, activate, 992);
		case Page::Folders:
			return UpdateFolders(vertical, activate, 992);
		default:
			return UpdateOptions(vertical, horizontal, activate);
	}
}

bool NeedsRedraw()
{
	const bool dirty = g_dirty;
	g_dirty = false;
	return dirty;
}

void Draw(const vr::ScreenGeometry &screen, uint16_t *pixels, int width, int height)
{
	FillRect(pixels, width, height, 0, 0, width, height, kPanel);
	FillRect(pixels, width, height, 0, 0, width, 4, kBorder);
	FillRect(pixels, width, height, 0, height - 4, width, 4, kBorder);
	FillRect(pixels, width, height, 0, 0, 4, height, kBorder);
	FillRect(pixels, width, height, width - 4, 0, 4, height, kBorder);

	switch (g_page)
	{
		case Page::Settings: DrawSettings(screen, pixels, width, height); break;
		case Page::Controls: DrawControls(pixels, width, height); break;
		case Page::Roms:     DrawRoms(pixels, width, height); break;
		case Page::Folders:  DrawFolders(pixels, width, height); break;
		default:             DrawOptions(pixels, width, height); break;
	}

	const std::string footer = g_message_timer > 0.0f
		? emu::LastStateMessage()
		: std::string("Stick: move / change    A: select");

	DrawText(pixels, width, height, kMarginX, height - 60, footer,
	         g_message_timer > 0.0f ? kTitle : kDimText, kRomScale * 2);
}

} // namespace menu
