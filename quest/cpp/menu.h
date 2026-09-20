#pragma once

#include "screen.h"

#include <cstdint>
#include <string>
#include <vector>

namespace menu {

// What the player asked the menu to do this frame, beyond editing a value.
enum class Action { None, Recenter, SaveState, LoadState, LoadRom,
                    OpenFolder, UseFolder, GrantStorage, BindingsChanged };

void Toggle();

// Opens straight onto the ROM list, for starting up with nothing loaded.
void OpenRomList();

void Close();
bool IsOpen();

// One frame of navigation.  `vertical` is edge-triggered (-1 up, +1 down),
// `horizontal` is held (-1..1), `activate` is edge-triggered.
Action Update(vr::ScreenGeometry &screen, int vertical, float horizontal,
              bool activate, float delta_seconds);

// Which save slot the menu is pointing at.
int SelectedSlot();

// The ROMs the browser offers, and which one was chosen.
void SetRomList(std::vector<std::string> names);
const std::string &SelectedRom();

// The folder browser: where it is, what is under it, and where it wants to go.
void SetRomDir(std::string path);
void SetFolderList(std::string path, std::vector<std::string> subfolders);
const std::string &CurrentFolder();
const std::string &ChosenFolder();

// Whether the app may read folders outside its own directory.
void SetStorageAccess(bool granted);

// True when the pixels changed and the texture needs re-uploading.
bool NeedsRedraw();
void Draw(const vr::ScreenGeometry &screen, uint16_t *pixels, int width, int height);

} // namespace menu
