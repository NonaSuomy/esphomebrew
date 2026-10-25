//       _________ __                 __
//      /   _____//  |_____________ _/  |______     ____  __ __  ______
//      \_____  \\   __\_  __ \__  \\   __\__  \   / ___\|  |  \/  ___/
//      /        \|  |  |  | \// __ \|  |  / __ \_/ /_/  >  |  /\___ |
//     /_______  /|__|  |__|  (____  /__| (____  /\___  /|____//____  >
//             \/                  \/          \//_____/            \/
//  ______________________                           ______________________
//                        T H E   W A R   B E G I N S
//         Stratagus - A free fantasy real time strategy game engine
//
/**@name stratagus.cpp - The main file. */
//
//      (c) Copyright 1998-2015 by Lutz Sammer, Francois Beerten,
//                                 Jimmy Salmon, Pali Rohár and cybermind
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; only version 2 of the License.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
//      02111-1307, USA.
//

//@{

/**
** @mainpage
**
** @section Introduction Introduction
**
** Welcome to the source code documentation of the Stratagus engine.
** Extract the source documentation with doxygen (http://www.doxygen.org) tool.
**
** Any help to improve this documention is welcome. If you didn't
** understand something or you found an error or a wrong spelling
** or wrong grammar please write an email (including a patch :).
**
** @section Information Information
**
** Visit the https://github.com/Wargus/stratagus web page for the latest news and
** <A HREF="../index.html">Stratagus Info</A> for other documentation.
**
** @section Modules Modules
**
** This are the main modules of the Stratagus engine.
**
** @subsection Map Map
**
** Handles the map. A map is made from tiles.
**
** @see map.h @see map.cpp @see tileset.h @see tileset.cpp
**
** @subsection Unit Unit
**
** Handles units. Units are ships, flyers, buildings, creatures,
** machines.
**
** @see unit.h @see unit.cpp @see unittype.h @see unittype.cpp
**
** @subsection Missile Missile
**
** Handles missiles. Missiles are all other sprites on map
** which are no unit.
**
** @see missile.h @see missile.cpp
**
** @subsection Player Player
**
** Handles players, all units are owned by a player. A player
** could be controlled by a human or a computer.
**
** @see player.h @see player.cpp @see ::CPlayer
**
** @subsection Sound Sound
**
** Handles the high and low level of the sound. There are the
** background music support, voices and sound effects.
** Following low level backends are supported: OSS and SDL.
**
** @todo adpcm file format support for sound effects
** @todo better separation of low and high level, assembler mixing
** support.
** @todo Streaming support of ogg/mp3 files.
**
** @see sound.h @see sound.cpp
** @see script_sound.cpp @see sound_id.cpp @see sound_server.cpp
** @see unitsound.cpp
** @see sdl_audio.cpp
** @see ogg.cpp @see wav.cpp
**
** @subsection Video Video
**
** Handles the high and low level of the graphics.
** This also contains the sprite and linedrawing routines.
**
** See page @ref VideoModule for more information upon supported
** features and video platforms.
**
** @see video.h @see video.cpp
**
** @subsection Network Network
**
** Handles the high and low level of the network protocol.
** The network protocol is needed for multiplayer games.
**
** See page @ref NetworkModule for more information upon supported
** features and API.
**
** @see network.h @see network.cpp
**
** @subsection Pathfinder Pathfinder
**
** @see pathfinder.h @see pathfinder.cpp
**
** @subsection AI AI
**
** There are currently two AI's. The old one is very hardcoded,
** but does things like placing buildings better than the new.
** The old AI shouldn't be used.  The new is very flexible, but
** very basic. It includes none optimations.
**
** See page @ref AiModule for more information upon supported
** features and API.
**
** @see ai_local.h
** @see ai.h @see ai.cpp
**
** @subsection CCL CCL
**
** CCL is Craft Configuration Language, which is used to
** configure and customize Stratagus.
**
** @see script.h @see script.cpp
**
** @subsection Icon Icon
**
** @see icons.h @see icons.cpp
**
** @subsection Editor Editor
**
** This is the integrated editor, it shouldn't be a perfect
** editor. It is used to test new features of the engine.
**
** See page @ref EditorModule for more information upon supported
** features and API.
**
** @see editor.h @see editor.cpp
*/

/*----------------------------------------------------------------------------
--  Includes
----------------------------------------------------------------------------*/

#ifdef BUILD_VENDORED_SDL
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Imm32.lib")
#endif

#include <cctype>

#ifdef USE_BEOS
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

extern void beos_init(int argc, char **argv);

#endif

#ifdef MAC_BUNDLE
#define Button ButtonOSX
#include <Carbon/Carbon.h>
#undef Button
#endif

#include "stratagus.h"
#ifdef STRATAGUS_PAX
#include "pax_debug.h"
#endif

#include "ai.h"
#include "editor.h"
#include "filesystem.h"
#include "game.h"
#include "interface.h"
#include "iolib.h"
#include "map.h"
#include "netconnect.h"
#include "network.h"
#include "online_service.h"
#include "parameters.h"
#include "player.h"
#include "replay.h"
#include "results.h"
#include "settings.h"
#include "sound_server.h"
#include "title.h"
#include "translate.h"
#include "ui.h"
#include "unit_manager.h"
#include "version.h"
#include "video.h"
#include "widgets.h"
	#ifdef STRATAGUS_PAX
#include <SDL_image.h>
	#endif
#include "util.h"

#include "missile.h" //for FreeBurningBuildingFrames

#include <SDL.h>
#include <guisan.hpp>
#include <string_view>
#include <optional>
#include <utility>

#ifdef STRATAGUS_PAX
extern void StartMap(const std::string &filename, bool clean);
#endif

#ifdef USE_STACKTRACE
#include <stdexcept>
#include <stacktrace/call_stack.hpp>
#include <stacktrace/stack_exception.hpp>
#else
#include "st_backtrace.h"
#endif

#include <cstdlib>
#include <cstdio>

#ifdef STRATAGUS_PAX
#include <cstdint>
#include <cstring>

#include "pax_menu.h"
extern "C" void pax_stratagus_audio_start();
extern "C" void pax_stratagus_pointer_start();
#endif

#ifdef USE_WIN32
# include <dbghelp.h>
# include <windows.h>
# ifndef NO_STDIO_REDIRECT
#  define REDIRECT_OUTPUT
# endif
# ifndef REDIRECT_OUTPUT
#  include "SetupConsole_win32.h"
# endif
#endif

/*----------------------------------------------------------------------------
--  Variables
----------------------------------------------------------------------------*/

#ifdef STRATAGUS_PAX
const char *StratagusLibPath = PAX_STRATAGUS_DATA_ROOT; /// Selected game's SD directory
#else
std::string StratagusLibPath;        /// Path for data directory
#endif

/// Name, Version, Copyright
const char NameLine[] = NAME " v" VERSION ", " COPYRIGHT;

std::string CliMapName;          /// Filename of the map given on the command line
std::string MenuRace;

bool EnableDebugPrint;           /// if enabled, print the debug messages
bool EnableAssert;               /// if enabled, halt on assertion failures
bool EnableUnitDebug;            /// if enabled, a unit info dump will be created
bool IsRestart;                  /// if true, the game skips some things like title screens

std::vector<std::string> OriginalArgv;

#ifdef DEBUG
bool IsDebugEnabled {true};      /// Is debug enabled? Flag to pass into lua code.
#else
bool IsDebugEnabled {false};     /// Is debug enabled? Flag to pass into lua code.
#endif
bool EnableWallsInSinglePlayer {false}; /// Enables ability to build walls in the single player games
										/// used for debug purposes

/*============================================================================
==  MAIN
============================================================================*/

/**
**  Pre menu setup.
*/
void PreMenuSetup()
{
	//
	//  Initial menus require some gfx.
	//
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: entered");
	#endif
	SetDefaultTextColors(FontYellow, FontWhite);

	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: before LoadFonts");
	#endif
	#ifdef STRATAGUS_PAX
	/* The PAX software renderer defers font-sheet decoding; the first
	 * mission frame can be rendered without the desktop font atlas. */
	pax_stratagus_debug("PreMenuSetup: skipping font atlas on PAX");
	#else
	LoadFonts();
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: after LoadFonts");
	#endif

	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: before InitVideoCursors");
	#endif
	InitVideoCursors();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: after InitVideoCursors");
	#endif

	if (MenuRace.empty()) {
		#ifdef STRATAGUS_PAX
		pax_stratagus_debug("PreMenuSetup: before LoadCursors default");
		#endif
		LoadCursors(PlayerRaces.Name[0]);
	} else {
		#ifdef STRATAGUS_PAX
		pax_stratagus_debug("PreMenuSetup: before LoadCursors menu race");
		#endif
		LoadCursors(MenuRace);
	}
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: after LoadCursors");
	#endif

	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: before InitSettings");
	#endif
	InitSettings();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: after InitSettings");
	#endif

	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: before InitUserInterface");
	#endif
	InitUserInterface();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: after InitUserInterface");
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: before UI.Load");
	#endif
	UI.Load();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("PreMenuSetup: after UI.Load");
	#endif
}

/**
**  Run the guichan main menus loop.
**
**  @return          0 for success, else exit.
*/
static int MenuLoop()
{
	int status;

	initGuichan();
	InterfaceState = IfaceState::Menu;
	//  Clear screen
	Video.ClearScreen();
	Invalidate();

	ButtonUnderCursor = -1;
	OldButtonUnderCursor = -1;
	CursorState = CursorStates::Point;
	GameCursor = UI.Point.Cursor;

	// FIXME delete this when switching to full guichan GUI
	const fs::path filename = LibraryFileName("scripts/guichan.lua");
	status = LuaLoadFile(filename);

	// We clean up later in Exit
	return status;
}

//----------------------------------------------------------------------------

/**
**  Print headerline, copyright, ...
*/
static void PrintHeader()
{
	std::string CompileOptions =
#ifdef DEBUG
		"DEBUG "
#endif
#ifdef USE_ZLIB
		"ZLIB "
#endif
#ifdef USE_BZ2LIB
		"BZ2LIB "
#endif
#ifdef USE_VORBIS
		"VORBIS "
#endif
#ifdef USE_THEORA
		"THEORA "
#endif
#ifdef USE_WIN32
		"WIN32 "
#endif
#ifdef USE_LINUX
		"LINUX "
#endif
#ifdef USE_BSD
		"BSD "
#endif
#ifdef USE_BEOS
		"BEOS "
#endif
#ifdef USE_MAC
		"MAC "
#endif
		"";

	fprintf(stdout,
			"%s\n  written by Lutz Sammer, Fabrice Rossi, Vladi Shabanski, Patrice Fortier,\n"
			"  Jon Gabrielson, Andreas Arens, Nehal Mistry, Jimmy Salmon, Pali Rohar,\n"
			"  cybermind and others.\n"
			"\t" HOMEPAGE "\n"
			"Compile options %s",
			NameLine, CompileOptions.c_str());
}

void PrintLicense()
{
	printf("\n"
		   "\n"
		   "Stratagus may be copied only under the terms of the GNU General Public License\n"
		   "which may be found in the Stratagus source kit.\n"
		   "\n"
		   "DISCLAIMER:\n"
		   "This software is provided as-is.  The author(s) can not be held liable for any\n"
		   "damage that might arise from the use of this software.\n"
		   "Use it at your own risk.\n"
		   "\n");
}


/**
**  Exit the game.
**
**  @param err  Error code to pass to shell.
*/
void Exit(int err)
{
#ifdef STRATAGUS_PAX
	pax_stratagus_audio_stop();
#endif
	if (GameRunning) {
		StopGame(GameExit);
		return;
	}
	GameCycle = 0;
	StopMusic();
	QuitSound();
	NetworkQuitGame();

	ExitNetwork1();
	CleanModules();
	FreeBurningBuildingFrames();
	FreeSounds();
	FreeGraphics();
	FreePlayerColors();
	FreeButtonStyles();
	DeInitOnlineService();
	freeGuichan();
	fprintf(stdout, "Frames %lu, Slow frames %ld = %ld%%\n",
			   FrameCounter, SlowFrameCounter,
			   (SlowFrameCounter * 100) / (FrameCounter ? FrameCounter : 1));
	lua_settop(Lua, 0);
	lua_close(Lua);

	DeInitImageLoaders();

	delete UnitManager;

	fprintf(stdout, "%s", _("Thanks for playing Stratagus.\n"));
#ifdef STRATAGUS_PAX
	return;
#endif
	exit(err);
}

/**
**  Do a fatal exit.
**  Called on out of memory or crash.
**
**  @param err  Error code to pass to shell.
*/
void ExitFatal(int err)
{
#ifdef USE_STACKTRACE
	throw stacktrace::stack_runtime_error((const char*)err);
#else
	print_backtrace();
#endif
	exit(err);
}

/**
**  Display the usage.
*/
static void Usage()
{
	PrintHeader();
	printf(
		"\n\nUsage: %s [OPTIONS] [map.smp|map.smp.gz]\n"
		"\t-a\t\tEnables asserts check in engine code (for debugging)\n"
		"\t-b\t\tBenchmark mode. Runs as fast as possible and reports FPS.\n"
		"\t-c file.lua\tConfiguration start file (default stratagus.lua)\n"
		"\t-d datapath\tPath to stratagus data (default current directory)\n"
		"\t-D depth\tVideo mode depth = pixel per point\n"
		"\t-e\t\tStart editor (instead of game)\n"
		"\t-E file.lua\tEditor configuration start file (default editor.lua)\n"
		"\t-F\t\tFull screen video mode\n"
		"\t-g\t\tForce software rendering (implies no shaders)\n"
		"\t-G \"options\"\tGame options (passed to game scripts)\n"
		"\t-h\t\tHelp shows this page\n"
		"\t-i\t\tEnables unit info dumping into log (for debugging)\n"
		"\t-I addr\t\tNetwork address to use\n"
		"\t-l\t\tDisable command log\n"
		"\t-N name\t\tName of the player\n"
		"\t-p\t\tEnables debug messages printing in console\n"
		"\t-P port\t\tNetwork port to use\n"
		"\t-r\t\tIndicate a rapid start. Skips a few things like title screens\n"
		"\t-s sleep\tNumber of frames for the AI to sleep before it starts\n"
		"\t-S speed\tSync speed (100 = 30 frames/s)\n"
		"\t-u userpath\tPath where stratagus saves preferences, log and savegame. Use 'userhome' to force platform-default userhome directory.\n"
		"\t-v mode\t\tVideo mode resolution in format <xres>x<yres>\n"
		"\t-W\t\tWindowed video mode. Optionally takes a window size in <xres>x<yres>\n"
		"map is relative to StratagusLibPath=datapath, use ./map for relative to cwd\n",
		Parameters::Instance.applicationName.c_str());
}

#ifdef REDIRECT_OUTPUT

static fs::path stdoutFile;
static fs::path stderrFile;

static void CleanupOutput()
{
	fclose(stdout);
	fclose(stderr);

	if (fs::file_size(stdoutFile) == 0) {
		fs::remove(stdoutFile);
	}
	if (fs::file_size(stderrFile) == 0) {
		fs::remove(stderrFile);
	}
}

static void RedirectOutput()
{
	const auto& path = Parameters::Instance.GetUserDirectory();

	fs::create_directories(path);

	stdoutFile = path / "stdout.txt";
	stderrFile = path / "stderr.txt";

	if (!freopen(stdoutFile.string().c_str(), "w", stdout)) {
		printf("freopen stdout failed");
	}
	if (!freopen(stderrFile.string().c_str(), "w", stderr)) {
		printf("freopen stderr failed");
	}
	atexit(CleanupOutput);
}
#endif

void ParseCommandLine(int argc, char **argv, Parameters &parameters)
{
#ifdef DEBUG
	ErrorPrint("optind(%d), argc(%d) at startup\n", optind, argc);
	for (int i = 0; i < argc; i++) {
		fprintf(stderr, "%s ", argv[i]);
	}
	fprintf(stderr, "\n");
#endif

	const auto parseWindowSize = [](std::string_view size) -> std::optional<std::pair<int, int>> {
		const size_t separator = size.find('x');
		if (separator == std::string_view::npos || separator + 1 == size.size()) {
			return {};
		}
		const int width = to_number(size.substr(0, separator));
		const int height = to_number(size.substr(separator + 1));
		if (!width || !height) {
			return {};
		}
		return std::make_pair(width, height);
	};

	// POSIX getopt does not support optional arguments, so to support it only
	// for -W, we scan once and remove the optional argument if it's given.
	for (int i = 1; i < argc; ++i) {
		std::string_view s = argv[i];
		if (!s.empty() && s.front() == '-' && s.back() == 'W') {
			int j = i + 1;
			if (j < argc) {
				auto sz = parseWindowSize(argv[i + 1]);
				if (sz) {
					Video.WindowWidth = sz.value().first;
					Video.WindowHeight = sz.value().second;
					for (; j + 1 < argc; ++j) {
						argv[j] = argv[j + 1];
					}
					argv[j + 1] = NULL;
					--argc;
				}
			}
		}
	}

	for (;;) {
		switch (getopt(argc, argv, "abc:d:D:eE:FgG:hiI:lN:oOP:prs:S:u:v:W?-")) {
			case 'a':
				EnableAssert = true;
				continue;
			case 'b':
				parameters.benchmark = true;
				continue;
			case 'c':
				parameters.luaStartFilename = optarg;
				if (parameters.luaStartFilename.extension() != ".lua") {
					parameters.luaStartFilename.concat(".lua");
				}
				continue;
			case 'd': {
#ifndef STRATAGUS_PAX
				StratagusLibPath = optarg;
				size_t index;
				while ((index = StratagusLibPath.find('\\')) != std::string::npos) {
					StratagusLibPath[index] = '/';
				}
#else
				(void)optarg;
#endif
				continue;
			}
			case 'D':
				Video.Depth = to_number(optarg);
				continue;
			case 'e':
				Editor.Running = EditorCommandLine;
				continue;
			case 'E':
				parameters.luaEditorStartFilename = optarg;
				continue;
			case 'F':
				VideoForceFullScreen = 1;
				Video.FullScreen = 1;
				continue;
			case 'g':
				SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
				continue;
			case 'G':
				parameters.luaScriptArguments = optarg;
				continue;
			case 'i':
				EnableUnitDebug = true;
				continue;
			case 'I':
				CNetworkParameter::Instance.localHost = optarg;
				continue;
			case 'l':
				CommandLogDisabled = true;
				continue;
			case 'N':
				parameters.LocalPlayerName = optarg;
				continue;
			case 'P':
				CNetworkParameter::Instance.localPort = to_number(optarg);
				continue;
			case 'p':
				EnableDebugPrint = true;
				continue;
			case 'r':
				IsRestart = true;
				continue;
			case 's':
				AiSleepCycles = to_number(optarg);
				continue;
			case 'S':
				RefreshRate = to_number(optarg);
				continue;
			case 'u':
				Parameters::Instance.SetUserDirectory(
					optarg == std::string_view("userhome") ? "" : optarg);
				continue;
			case 'v': {
				auto sz = parseWindowSize(optarg);
				if (sz) {
					Video.Width = sz.value().first;
					Video.Height = sz.value().second;
				} else {
					ErrorPrint("%s: incorrect format of video mode resolution -- '%s'\n", argv[0], optarg);
					Usage();
					exit(-1);
				}
				continue;
			}
			case 'W':
				VideoForceFullScreen = 1;
				Video.FullScreen = 0;
				continue;
			case -1:
				break;
			case '?':
			case 'h':
			default:
				Usage();
				exit(-1);
		}
		break;
	}

	if (argc - optind > 1) {
		ErrorPrint(
			"too many map files. if you meant to pass game arguments, these go after '--'\n");
		Usage();
		ExitFatal(-1);
	}

	if (argc - optind) {
		size_t index;
		CliMapName = argv[optind];
		while ((index = CliMapName.find('\\')) != std::string::npos) {
			CliMapName[index] = '/';
		}
	}
}

#ifdef USE_WIN32
static LONG WINAPI CreateDumpFile(EXCEPTION_POINTERS *ExceptionInfo)
{
	HANDLE hFile = CreateFile(L("crash.dmp"), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	MINIDUMP_EXCEPTION_INFORMATION mei;
	mei.ThreadId = GetCurrentThreadId();
	mei.ClientPointers = TRUE;
	mei.ExceptionPointers = ExceptionInfo;
	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, nullptr, nullptr);
	ErrorPrint("Stratagus crashed!\n"
	           "A mini dump file \"crash.dmp\" has been created in the Stratagus folder.\n"
	           "Please send it to our bug tracker: https://github.com/Wargus/stratagus/issues\n"
	           "and tell us what caused this bug to occur.\n");
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/**
**  The main program: initialise, parse options and arguments.
**
**  @param argc  Number of arguments.
**  @param argv  Vector of arguments.
*/
int stratagusMain(int argc, char **argv)
try {
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("stratagusMain entered");
	#endif
	#ifndef STRATAGUS_PAX
	for (int i = 0; i < argc; i++) {
		OriginalArgv.push_back(std::string(argv[i]));
	}
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("OriginalArgv complete");
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("before StratagusLibPath");
	#endif
#ifdef USE_BEOS
	//  Parse arguments for BeOS
	beos_init(argc, argv);
#endif
#ifdef USE_WIN32
	SetUnhandledExceptionFilter(CreateDumpFile);
#endif
#if defined(USE_WIN32) && ! defined(REDIRECT_OUTPUT)
	SetupConsole();
#endif
	//  Setup some defaults.
#ifndef MAC_BUNDLE
	#ifndef STRATAGUS_PAX
	StratagusLibPath = ".";
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StratagusLibPath complete");
	#endif
#else
	freopen("/tmp/stdout.txt", "w", stdout);
	freopen("/tmp/stderr.txt", "w", stderr);
	// Look for the specified data set inside the application bundle
	// This should be a subdir of the Resources directory
	CFURLRef pluginRef = CFBundleCopyResourceURL(CFBundleGetMainBundle(),
												 CFSTR(MAC_BUNDLE_DATADIR), nullptr, nullptr);
	CFStringRef macPath = CFURLCopyFileSystemPath(pluginRef,  kCFURLPOSIXPathStyle);
	const char *pathPtr = CFStringGetCStringPtr(macPath, CFStringGetSystemEncoding());
	Assert(pathPtr);
	StratagusLibPath = pathPtr;
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("before Parameters instance");
	#endif
		Parameters &parameters = Parameters::Instance;
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("before SetDefaultValues");
	#endif
#if !defined(STRATAGUS_PAX)
		parameters.SetDefaultValues();
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("SetDefaultValues complete");
	#endif
#ifndef STRATAGUS_PAX
	parameters.SetLocalPlayerNameFromEnv();
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("SetLocalPlayerNameFromEnv complete");
	#endif

#ifdef REDIRECT_OUTPUT
		RedirectOutput();
#endif

#ifndef STRATAGUS_PAX
	if (argc > 0) {
		parameters.applicationName = argv[0];
	}
#endif

	// FIXME: Parse options before or after scripts?
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("before ParseCommandLine");
	#endif
#ifndef STRATAGUS_PAX
	ParseCommandLine(argc, argv, parameters);
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("ParseCommandLine complete");
	#endif
	// Init the random number generator.
	InitSyncRand();

#ifndef STRATAGUS_PAX
	fs::create_directories(parameters.GetUserDirectory());
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("user directory complete");
	#endif

	// Init Lua and register lua functions!
	InitLua();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("InitLua complete");
	#endif
	#ifdef STRATAGUS_PAX
#if defined(STRATAGUS_PAX_DIRECT_MISSION)
	/* The ESPHome logger eventually trips a null source pointer after the
	 * startup probe has emitted its early diagnostics.  Keep this isolation
	 * run silent while testing the Lua/module path itself. */
	pax_stratagus_debug_mute();
#endif
	pax_stratagus_debug("LuaRegisterModules begin");
	LuaRegisterModules();
	pax_stratagus_debug("LuaRegisterModules complete");
	#else
	LuaRegisterModules();
	#endif

	// Initialise AI module.  The title/menu frame does not need AI state, and
	// its desktop allocator-heavy bootstrap is deferred for the PAX path.
#if !defined(STRATAGUS_PAX) || defined(PAX_STRATAGUS_WARCRAFT)
	InitAiModule();
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("InitAiModule deferred");
	#endif

	// Setup sound card, must be done before loading sounds, so that
	// SDL_mixer can auto-convert to the target format
#ifndef STRATAGUS_PAX
	if (InitSound()) {
		InitMusic();
	}
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("sound initialization complete");
	#endif

#if defined(STRATAGUS_PAX) && defined(STRATAGUS_PAX_DIRECT_MISSION)
	/* Isolate the embedded Lua bootstrap from the global unit-manager
	 * construction.  The latter is the first allocator boundary that fails on
	 * the current PAX image, while the bootstrap itself can be tested safely
	 * without a map or video surface. */
	pax_stratagus_debug("direct probe: before LoadCclPax");
	LoadCclPax();
	pax_stratagus_debug("direct probe: after LoadCclPax");
	return 0;
#endif

    // init globals
#ifdef STRATAGUS_PAX
    pax_stratagus_debug("before CUnitManager");
#endif
    UnitManager = new CUnitManager();
#ifdef STRATAGUS_PAX
    pax_stratagus_debug("CUnitManager complete");
    pax_stratagus_debug("before FogOfWar");
#endif
    FogOfWar = std::make_unique<CFogOfWar>();
#ifdef STRATAGUS_PAX
    pax_stratagus_debug("FogOfWar complete");
#endif
#ifndef STRATAGUS_PAX
    InitOnlineService();
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("InitOnlineService complete");
	#endif

	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LoadCcl begin");
	#endif
#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LoadCcl deferred until mission start");
#ifdef PAX_STRATAGUS_WARCRAFT
	// Warcraft retains its own CCL/UI/campaign definitions and native menus.
	// Do not bootstrap Terran units against a Warcraft SD data root.
	parameters.SetUserDirectory(PAX_STRATAGUS_DATA_ROOT "/user");
	Video.Width=640; Video.Height=480;
	// The PAX loader needs an early frame while the full Warcraft script
	// tree loads. Native HUD definitions can also resolve display colors.
	// This black loading frame is not evidence of a working native menu.
	InitVideo();
	Video.ResizeScreen(640,480);
	Video.ClearScreen();
	RealizeVideoMemory();
	CclInConfigFile = true;
	parameters.luaStartFilename = "scripts/stratagus.lua";
	for (int i=1; i+1<argc; ++i) {
		if (!strcmp(argv[i], "-c")) parameters.luaStartFilename = argv[++i];
	}
	LuaLoadFile(LibraryFileName(parameters.luaStartFilename));
	CclInConfigFile = false;
	LuaGarbageCollect();
#endif
#else
	LoadCcl(parameters.luaStartFilename, parameters.luaScriptArguments);
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LoadCcl complete");
	#endif

	#ifdef STRATAGUS_PAX_LUA_ONLY
	/* Diagnostic boundary: exercise the embedded CCL bootstrap without any
	 * video initialization, SD-backed menu asset, or display callback. */
	LoadCclPax();
	return 0;
	#endif
	// Setup video display
#ifndef PAX_STRATAGUS_WARCRAFT
	InitVideo();
#endif
#if defined(STRATAGUS_PAX) && !defined(PAX_STRATAGUS_WARCRAFT)
	// The original menu uses real RGB565 surfaces, like the working game.
	// No 32-bit placeholder text/background writes may touch this canvas.
	Video.ResizeScreen(800, 480);
#if defined(STRATAGUS_PAX_DIRECT_MISSION)
	int startMission = 1;
#else
	int startMission = pax_stratagus_menu_run(TheScreen);
#endif
	if (startMission && !pax_exit_requested) {
		pax_stratagus_debug("mission 1 selected: loading game scripts");
		LoadCclPax();
		PreMenuSetup();
		initGuichan();
		std::string mission;
		// Restart, menu return and loading run only after native map cleanup,
		// outside the active UI/Lua callback. Keep definitions/artwork intact.
		while (startMission && !pax_exit_requested) {
			extern const char *pax_stratagus_take_load_path();
			extern void pax_stratagus_save_reload_completed();
			if (startMission==2) {
				const char *save=pax_stratagus_take_load_path();
				if (!save) break;
				CleanPlayers(); LoadGame(save); mission=CurrentMapPath;
				pax_stratagus_save_reload_completed(); StartMap(save,false);
			} else {
				if (startMission!=4) mission=startMission==3?"campaigns/terran/tutorial/scenario.smp":"campaigns/terran/01/scenario.smp";
				StartMap(mission,true);
			}
			if (pax_exit_requested || GameResult==GameExit) break;
			extern bool pax_stratagus_reload_pending();
			if (pax_stratagus_reload_pending()) startMission=2;
			else if (GameResult==GameRestart) startMission=4;
			else startMission=pax_stratagus_menu_run(TheScreen,false);
		}
	}
	Exit(0);
	return 0;
#endif

	PrintHeader();
	PrintLicense();

#ifndef DEBUG           // For debug it's better not to have:
	srand(time(nullptr));  // Random counter = random each start
#endif

	//  Show title screens.
#ifdef PAX_STRATAGUS_WARCRAFT
	pax_stratagus_audio_start();
	pax_stratagus_pointer_start();
#endif
	SetDefaultTextColors(FontYellow, FontWhite);
	LoadFonts();
	SetClipping(0, 0, Video.Width - 1, Video.Height - 1);
	Video.ClearScreen();
	if (!IsRestart) {
		ShowTitleScreens();
	}
#ifdef PAX_STRATAGUS_WARCRAFT
	// A remote close can end a streamed intro. Do not load menu assets
	// afterward: the launcher allows only a short cooperative-close window.
	// Return through engine_worker so audio/tasks stop before image unload.
	if (pax_exit_requested) { Exit(0); return 0; }
#endif

	// Init player data
	ThisPlayer = nullptr;
	//Don't clear the Players structure as it would erase the allowed units.
	// memset(Players, 0, sizeof(Players));
	NumPlayers = 0;

	UnitManager->Init(); // Units memory management
	PreMenuSetup();     // Load everything needed for menus

#ifdef PAX_STRATAGUS_WARCRAFT
	pax_stratagus_debug("Warcraft: native menu loop begin");
#endif
	MenuLoop();

	Exit(0);
	return 0;
} catch (const std::exception &e) {
#ifdef STRATAGUS_PAX
	pax_stratagus_debug((std::string("native exception: ") + e.what()).c_str());
#endif
	ErrorPrint("Stratagus crashed!\n"
		        "Please send this call stack to our bug tracker: "
		        "https://github.com/Wargus/stratagus/issues\n"
		        "and tell us what caused this bug to occur.\n"
		        " === exception state traceback === \n");
	ErrorPrint("%s", e.what());
	exit(1);
}
catch (const gcn::Exception &ex) {
#ifdef STRATAGUS_PAX
	pax_stratagus_debug((std::string("native Guichan exception: ") + ex.getMessage()).c_str());
#endif
	ErrorPrint("Stratagus crashed in Guichan!\n"
	           "Please send this call stack to our bug tracker: "
	           "https://github.com/Wargus/stratagus/issues\n"
	           "and tell us what caused this bug to occur.\n"
	           " === exception state traceback === \n");
	ErrorPrint("%s:%d (%s): %s",
	           ex.getFilename().c_str(),
	           ex.getLine(),
	           ex.getFunction().c_str(),
	           ex.getMessage().c_str());
	exit(1);
}
catch (...) {
#ifdef STRATAGUS_PAX
	pax_stratagus_debug("native exception: unknown type");
#endif
	ErrorPrint("Stratagus crashed!\n"
	           "Please send this call stack to our bug tracker: "
	           "https://github.com/Wargus/stratagus/issues\n"
	           "and tell us what caused this bug to occur.\n"
	           " === exception state traceback === \n");
	exit(1);
}
//@}
