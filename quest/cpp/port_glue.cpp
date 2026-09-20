// Frontend callbacks required by the Snes9x core.  Everything filesystem
// related is rooted at the app's external files directory, which needs no
// runtime permission on Android.

#include "emu.h"
#include "log.h"

#include "snes9x.h"
#include "memmap.h"
#include "display.h"
#include "gfx.h"
#include "controls.h"
#include "snapshot.h"
#include "fscompat.h"
#include "conffile.h"

#include <sys/stat.h>
#include <string>

std::string s9x_base_dir;

static const char *dir_names[LAST_DIR + 1] =
{
	"",         // DEFAULT_DIR
	"",         // HOME_DIR
	"",         // ROMFILENAME_DIR
	"roms",
	"sram",
	"snapshots",
	"screenshots",
	"spc",
	"cheats",
	"patches",
	"bios",
	"logs",
	"sat",
	""
};

void S9xVRMakeDirs(const std::string &base)
{
	s9x_base_dir = base;
	mkdir(base.c_str(), 0775);
	for (int i = 0; i <= LAST_DIR; i++)
		if (dir_names[i][0])
			mkdir((base + "/" + dir_names[i]).c_str(), 0775);
}

std::string S9xGetDirectory(enum s9x_getdirtype dirtype)
{
	if (dir_names[dirtype][0])
		return s9x_base_dir + "/" + dir_names[dirtype];

	switch (dirtype)
	{
		case HOME_DIR:
		case DEFAULT_DIR:
			return s9x_base_dir;

		case ROMFILENAME_DIR:
		{
			std::string path = Memory.ROMFilename;
			size_t pos = path.rfind('/');
			return pos == std::string::npos ? s9x_base_dir : path.substr(0, pos);
		}

		default:
			return s9x_base_dir;
	}
}

std::string S9xGetFilenameInc(std::string ext, enum s9x_getdirtype dirtype)
{
	SplitPath path = splitpath(Memory.ROMFilename);
	std::string dir = S9xGetDirectory(dirtype);

	if (!ext.empty() && ext[0] != '.')
		ext = "." + ext;

	struct stat buf;
	for (unsigned int i = 0; i < 1000; i++)
	{
		std::string candidate = dir + "/" + path.stem + "." + std::to_string(i) + ext;
		if (stat(candidate.c_str(), &buf) != 0)
			return candidate;
	}

	return dir + "/" + path.stem + ext;
}

void S9xMessage(int type, int number, const char *message)
{
	if (!message)
		return;

	if (type == S9X_ERROR || type == S9X_FATAL_ERROR)
		LOGE("%s", message);
	else
		LOGI("%s", message);

	S9xSetInfoString(message);
}

bool8 S9xOpenSnapshotFile(const char *filepath, bool8 read_only, STREAM *file)
{
	*file = OPEN_STREAM(filepath, read_only ? "rb" : "wb");
	return *file != 0;
}

void S9xCloseSnapshotFile(STREAM file)
{
	CLOSE_STREAM(file);
}

void S9xAutoSaveSRAM()
{
	Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());
}

// The audio ring buffer paces emulation, so there is nothing to sleep for here.
void S9xSyncSpeed() {}

void S9xExit()
{
	S9xAutoSaveSRAM();
	exit(0);
}

// The AAudio stream is opened by audio::Start() before the core comes up.
bool8 S9xOpenSoundDevice() { return TRUE; }

// --- Unused port hooks ------------------------------------------------------
bool8 S9xInitUpdate() { return TRUE; }
bool8 S9xContinueUpdate(int, int) { return TRUE; }
void S9xInitInputDevices() {}
void S9xHandlePortCommand(s9xcommand_t, short, short) {}
bool S9xPollButton(uint32, bool *) { return false; }
bool S9xPollAxis(uint32, short *) { return false; }
bool S9xPollPointer(uint32, short *, short *) { return false; }
void S9xToggleSoundChannel(int) {}
void S9xParsePortConfig(ConfigFile &, int) {}
const char * S9xStringInput(const char *message) { return message; }
void S9xExtraUsage() {}
void S9xParseArg(char **, int &, int) {}
void S9xSetPalette() {}
