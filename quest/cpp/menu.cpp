#include "menu.h"

#include "emu.h"
#include "var8x10font.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace menu {
namespace {

constexpr int kFontWidth = 8;
constexpr int kFontHeight = 10;

// The options page is drawn at four times the font's own size and the ROM
// browser at twice, so that a long file name still fits across the panel while
// the options stay comfortable to read.
constexpr int kOptionScale = 4;
constexpr int kRomScale = 2;

constexpr int kMarginX = 64;
constexpr int kTitleY = 48;
constexpr int kDividerY = 108;

constexpr int kOptionFirstRow = 126;
constexpr int kOptionRowHeight = 62;

constexpr int kRomFirstRow = 128;
constexpr int kRomRowHeight = 34;

// RGB565.
constexpr uint16_t kPanel     = 0x18E3;
constexpr uint16_t kBorder    = 0x5AEB;
constexpr uint16_t kText      = 0xFFFF;
constexpr uint16_t kDimText   = 0xAD55;
constexpr uint16_t kHighlight = 0x02BF;
constexpr uint16_t kTitle     = 0xFD20;

enum Item { kSlot, kSaveState, kLoadState, kLoadRom, kRomFolder, kStorage, kDistance,
            kWidth, kStereo, kDepthMode, kSmoothing, kRecenter, kCloseItem, kItemCount };

enum class Page { Options, Roms, Folders };

bool g_open = false;
bool g_dirty = true;
Page g_page = Page::Options;

int g_selected = 0;
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
int g_last_split = -1;
int g_last_filter = -1;
bool g_mode_held = false;
bool g_mode_was_held = false;

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

	// Give the tail a share of the room, then fill the rest with the head.
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

std::string Elide(const std::string &text, int available, int scale);

std::string Format(const char *format, float value)
{
	char buffer[64];
	snprintf(buffer, sizeof(buffer), format, value);
	return buffer;
}

std::string ValueText(const vr::ScreenGeometry &screen, int item)
{
	switch (item)
	{
		case kRomFolder: return g_rom_dir;
		case kStorage:   return g_storage_access ? std::string("granted")
		                                         : std::string("not granted");
		case kSlot:      return std::to_string(g_slot) +
		                        (emu::StateExists(g_slot) ? "  (used)" : "  (empty)");
		case kDistance:  return Format("%.1f m", screen.radius);
		case kWidth:     return Format("%.0f deg", screen.central_angle * 57.2958f);
		case kStereo:    return screen.stereo <= 0.0005f
		                        ? std::string("off")
		                        : Format("%.0f mm", screen.stereo * 1000.0f);
		case kDepthMode: return screen.layer_split ? std::string("Layers")
		                                           : std::string("Warp");
		case kSmoothing: return screen.filter == 0 ? std::string("Pixels")
		                      : screen.filter == 1 ? std::string("Sharp")
		                                           : std::string("Soft");
		default:         return std::string();
	}
}

const char *LabelText(int item)
{
	switch (item)
	{
		case kSlot:      return "Save slot";
		case kSaveState: return "Save game";
		case kLoadState: return "Load game";
		case kLoadRom:   return "Load ROM...";
		case kRomFolder: return "ROM folder";
		case kStorage:   return "Storage access";
		case kDistance:  return "Screen distance";
		case kWidth:     return "Screen width";
		case kStereo:    return "Stereo depth";
		case kDepthMode: return "Depth mode";
		case kSmoothing: return "Smoothing";
		case kRecenter:  return "Recenter screen";
		case kCloseItem: return "Close";
		default:         return "";
	}
}

int VisibleRomRows(int height)
{
	return std::max(1, (height - kRomFirstRow - 70) / kRomRowHeight);
}

Action UpdateOptions(vr::ScreenGeometry &screen, int vertical, float horizontal,
                     bool activate, float delta_seconds)
{
	if (vertical != 0)
	{
		g_selected = (g_selected + vertical + kItemCount) % kItemCount;
		g_dirty = true;
	}

	if (horizontal != 0.0f)
	{
		switch (g_selected)
		{
			case kDistance:
				screen.radius = std::clamp(screen.radius + horizontal * 2.0f * delta_seconds,
				                           vr::kMinRadius, vr::kMaxRadius);
				break;

			case kWidth:
				screen.central_angle =
					std::clamp(screen.central_angle + horizontal * 0.5f * delta_seconds,
					           vr::kMinAngle, vr::kMaxAngle);
				break;

			case kStereo:
				screen.stereo = std::clamp(screen.stereo + horizontal * 0.02f * delta_seconds,
				                           0.0f, vr::kMaxStereo);
				break;

			case kDepthMode:
				// A toggle, so only act on the moment the stick is pushed.
				if (!g_mode_held)
					screen.layer_split = horizontal > 0.0f;
				break;

			case kSmoothing:
				if (!g_mode_held)
					screen.filter = std::clamp(screen.filter + (horizontal > 0.0f ? 1 : -1),
					                           0, vr::kFilterCount - 1);
				break;

			case kSlot:
				if (!g_mode_held)
					g_slot = (g_slot + (horizontal > 0.0f ? 1 : emu::kStateSlots - 1)) %
					         emu::kStateSlots;
				break;

			default:
				break;
		}
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

		case kRomFolder:
			// The app answers with the contents, which opens the page.
			g_chosen_folder = g_rom_dir;
			return Action::OpenFolder;

		case kStorage:
			return Action::GrantStorage;

		default: break;
	}

	return Action::None;
}

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

// "Use this folder", then the parent, then whatever is inside.
int FolderEntryCount()
{
	return 2 + static_cast<int>(g_subfolders.size());
}

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
		g_page = Page::Options;
		g_message_timer = 3.0f;
		return Action::UseFolder;
	}

	if (g_folder_selected == 1)
	{
		const size_t slash = g_folder.rfind('/');
		g_chosen_folder = (slash == std::string::npos || slash == 0) ? "/"
		                                                            : g_folder.substr(0, slash);
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
	DrawText(pixels, width, height, kMarginX, kTitleY, "ROM FOLDER", kTitle, kOptionScale);
	FillRect(pixels, width, height, kMarginX, kDividerY, width - 2 * kMarginX, 2, kBorder);

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

void DrawOptions(const vr::ScreenGeometry &screen, uint16_t *pixels, int width, int height)
{
	DrawText(pixels, width, height, kMarginX, kTitleY, "VR OPTIONS", kTitle, kOptionScale);

	const std::string version = std::string("Snes9x VR ") + SNES9X_VR_VERSION;
	DrawText(pixels, width, height,
	         width - kMarginX - TextWidth(version, kRomScale), kTitleY + 6,
	         version, kDimText, kRomScale);

	const std::string credit = "Meta Quest port by Sgt. Bilko";
	DrawText(pixels, width, height,
	         width - kMarginX - TextWidth(credit, kRomScale), kTitleY + 28,
	         credit, kDimText, kRomScale);

	FillRect(pixels, width, height, kMarginX, kDividerY, width - 2 * kMarginX, 2, kBorder);

	for (int item = 0; item < kItemCount; item++)
	{
		const int y = kOptionFirstRow + item * kOptionRowHeight;
		const bool selected = item == g_selected;

		if (selected)
			FillRect(pixels, width, height, kMarginX - 24, y - 12,
			         width - 2 * (kMarginX - 24),
			         kFontHeight * kOptionScale + 24, kHighlight);

		const char *label = LabelText(item);
		DrawText(pixels, width, height, kMarginX, y, label,
		         selected ? kText : kDimText, kOptionScale);

		std::string value = ValueText(screen, item);
		if (value.empty())
			continue;

		// Values share the row with their label, so a long one -- a ROM path,
		// say -- is shortened rather than drawn back over the label.
		const int room = width - 2 * kMarginX -
		                 TextWidth(label, kOptionScale) - 3 * kOptionScale;
		value = Elide(value, room, kOptionScale);

		DrawText(pixels, width, height,
		         width - kMarginX - TextWidth(value, kOptionScale), y,
		         value, selected ? kText : kDimText, kOptionScale);
	}
}

void DrawRoms(uint16_t *pixels, int width, int height)
{
	DrawText(pixels, width, height, kMarginX, kTitleY, "LOAD ROM", kTitle, kOptionScale);
	FillRect(pixels, width, height, kMarginX, kDividerY, width - 2 * kMarginX, 2, kBorder);

	if (g_roms.empty())
	{
		DrawText(pixels, width, height, kMarginX, kRomFirstRow,
		         "No ROMs in files/roms", kDimText, kRomScale);
	}

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

	if (count > rows)
	{
		char position[48];
		snprintf(position, sizeof(position), "%d of %d",
		         g_rom_selected + 1, static_cast<int>(g_roms.size()));
		DrawText(pixels, width, height,
		         width - kMarginX - TextWidth(position, kRomScale), kTitleY + 12,
		         position, kDimText, kRomScale);
	}
}

} // namespace

void Toggle()
{
	g_open = !g_open;
	g_page = Page::Options;
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

void SetStorageAccess(bool granted)
{
	if (granted != g_storage_access)
		g_dirty = true;
	g_storage_access = granted;
}

const std::string &CurrentFolder() { return g_folder; }
const std::string &ChosenFolder() { return g_chosen_folder; }

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
	    screen.stereo != g_last_stereo || screen.layer_split != (g_last_split == 1) ||
	    screen.filter != g_last_filter)
	{
		g_last_radius = screen.radius;
		g_last_angle = screen.central_angle;
		g_last_stereo = screen.stereo;
		g_last_split = screen.layer_split ? 1 : 0;
		g_last_filter = screen.filter;
		g_dirty = true;
	}

	// The ROM page knows the panel height from the last draw; 992 is what the
	// app uses and the only cost of being wrong is a scroll step.
	if (g_page == Page::Folders)
		return UpdateFolders(vertical, activate, 992);

	if (g_page == Page::Roms)
		return UpdateRoms(vertical, activate, 992);

	return UpdateOptions(screen, vertical, horizontal, activate, delta_seconds);
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

	if (g_page == Page::Folders)
		DrawFolders(pixels, width, height);
	else if (g_page == Page::Roms)
		DrawRoms(pixels, width, height);
	else
		DrawOptions(screen, pixels, width, height);

	const std::string footer = g_message_timer > 0.0f
		? emu::LastStateMessage()
		: std::string("Stick: move / change    A: select");

	DrawText(pixels, width, height, kMarginX, height - 60, footer,
	         g_message_timer > 0.0f ? kTitle : kDimText, kRomScale * 2);
}

} // namespace menu
