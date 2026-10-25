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
/**@name game.cpp - The game set-up and creation. */
//
//      (c) Copyright 1998-2007 by Lutz Sammer, Andreas Arens,
//      Jimmy Salmon and Andrettin
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

/*----------------------------------------------------------------------------
--  Includes
----------------------------------------------------------------------------*/

#include "stratagus.h"

#include "game.h"

#include "actions.h"
#include "ai.h"
#include "animation.h"
#include "commands.h"
#include "construct.h"
#include "depend.h"
#include "editor.h"
#include "font.h"
#include "interface.h"
#include "iolib.h"
#include "map.h"
#include "minimap.h"
#include "missile.h"
#include "netconnect.h"
#include "network.h"
#include "online_service.h"
#include "parameters.h"
#include "pathfinder.h"
#include "player.h"
#include "replay.h"
#include "results.h"
#include "settings.h"
#include "sound.h"
#include "sound_server.h"
#include "spells.h"

#ifdef STRATAGUS_PAX
extern "C" void pax_stratagus_debug(const char *message);
#endif
#include "tileset.h"
#include "translate.h"
#include "trigger.h"
#include "ui.h"
#include "unit.h"
#include "unit_manager.h"
#include "unittype.h"
#include "upgrade.h"
#include "version.h"
#include "video.h"

#include <memory>
#include <SDL_image.h>

extern void CleanGame();

/*----------------------------------------------------------------------------
--  Variables
----------------------------------------------------------------------------*/

Settings GameSettings;  /// Game Settings
static bool LcmPreventRecurse;   /// prevent recursion through LoadGameMap
GameResults GameResult;                      /// Outcome of the game

std::string GameName;
std::string FullGameName;

unsigned long GameCycle;             /// Game simulation cycle counter
unsigned long FastForwardCycle;      /// Cycle to fast forward to in a replay

bool UseHPForXp = false;              /// true if gain XP by dealing damage, false if by killing.

/*----------------------------------------------------------------------------
--  Functions
----------------------------------------------------------------------------*/

extern std::unique_ptr<gcn::Gui> Gui;

/**
**  Save game settings.
**
**  @param file  Save file handle
*/
void SaveGameSettings(CFile &file)
{
	file.printf("\n");
	GameSettings.Save([&](std::string field) {
		file.printf("GameSettings.%s\n", field.c_str());
	});
	file.printf("\n");
}

/// forward declaration
void CreateGame(const fs::path &filename, CMap *map);

void StartMap(const std::string &filename, bool clean)
{
	#ifdef STRATAGUS_PAX
	extern volatile int pax_exit_requested;
	// Closing a nested briefing unwinds its menu, but the campaign Lua then
	// continues into RunMap. Do not begin another load during app shutdown.
	if (pax_exit_requested) { GameResult=GameExit; return; }
	pax_stratagus_debug("StartMap: entered");
	#endif
	gcn::Widget *oldTop = Gui->getTop();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: got old GUI top");
	#endif
	auto container = std::make_unique<gcn::Container>();
	container->setDimension(gcn::Rectangle(0, 0, Video.Width, Video.Height));
	container->setOpaque(false);
	Gui->setTop(container.get());
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: GUI container set");
	#endif

	NetConnectRunning = 0;
	InterfaceState = IfaceState::Normal;

	//  Create the game.
	DebugPrint("Creating game with map: %s\n", filename.c_str());
	if (clean) {
		CleanPlayers();
	}
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: players cleaned");
	#endif
	const auto [nc, rc] = GetDefaultTextColors();

	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: before CreateGame");
	#endif
	CreateGame(filename, &Map);
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: after CreateGame");
	#endif

	UI.StatusLine.Set(NameLine);
	SetMessage("%s", _("Do it! Do it now!"));

	//  Play the game.
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: before GameMainLoop");
	#endif
	GameMainLoop(clean);
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("StartMap: after GameMainLoop");
	#endif

	//  Clear screen
	Video.ClearScreen();
	Invalidate();

	CleanGame();
	InterfaceState = IfaceState::Menu;
	SetDefaultTextColors(nc, rc);

	Gui->setTop(oldTop);
#if defined(STRATAGUS_PAX) && !defined(PAX_STRATAGUS_WARCRAFT)
	extern void pax_stratagus_results_show(SDL_Surface *);
	pax_stratagus_results_show(TheScreen);
#endif
}

/*----------------------------------------------------------------------------
--  Map loading/saving
----------------------------------------------------------------------------*/

/**
**  Load a Stratagus map.
**
**  @param smpdir   smp directory
**  @param mapname  map filename
*/
static void LoadStratagusMap(const fs::path &smpdir, const fs::path &mapname)
{
	fs::path mapfull = mapname;
	// Try the same directory as the smp file first
	// Not found, try StratagusLibPath and the smp's dir
	// Not found again, try StratagusLibPath
	// Not found, try mapname by itself as a last resort
	for (auto candidate : {smpdir / mapname,
	                       fs::path(StratagusLibPath) / smpdir / mapname,
	                       fs::path(StratagusLibPath) / mapname,
	                       fs::path(mapname)}) {
		const std::string_view extra_extensions[] = {
			"",
#ifdef USE_ZLIB
			".gz",
#endif
#ifdef USE_BZ2LIB
			".bz2",
#endif
		};
		if (ranges::any_of(extra_extensions, [&](const auto extension) {
				auto extra_candidate = candidate;
				extra_candidate.replace_extension(candidate.extension().string() + extension.data());
				return fs::exists(extra_candidate);
		})) {
			mapfull = candidate;
			break;
		}
	}

#ifdef STRATAGUS_PAX
	// The PAX file service only accepts absolute paths.  The normal desktop
	// loader can keep this path relative because the host filesystem supplies
	// the working-directory lookup, but that lookup does not exist in the
	// sandboxed app loader.  Resolve the selected map through the same logical
	// asset resolver used by Lua Load() before opening it.
	if (!mapfull.is_absolute()) {
		mapfull = fs::path(LibraryFileName(mapfull.string()));
	}
	char pax_map_path[192];
	snprintf(pax_map_path, sizeof(pax_map_path), "LoadStratagusMap: %s", mapfull.string().c_str());
	pax_stratagus_debug(pax_map_path);
#endif

	if (LcmPreventRecurse) {
		ErrorPrint("recursive use of load map!\n");
		ExitFatal(-1);
	}
	InitPlayers();
	LcmPreventRecurse = true;
	if (LuaLoadFile(mapfull) == -1) {
		ErrorPrint("Can't load lua file: '%s'\n", mapfull.u8string().c_str());
		ExitFatal(-1);
	}
	#ifdef STRATAGUS_PAX
		pax_stratagus_debug("Lua LoadStratagusMap complete");
	#endif
	LcmPreventRecurse = false;

#if 0
	// Not true if multiplayer levels!
	if (!ThisPlayer) { /// ARI: bomb if nothing was loaded!
		ErrorPrint("'%s': invalid map\n", mapname.u8string().c_str());
		ExitFatal(-1);
	}
#endif
	if (!Map.Info.MapWidth || !Map.Info.MapHeight) {
		ErrorPrint("'%s': invalid map\n", mapname.u8string().c_str());
		ExitFatal(-1);
	}
	Map.Info.Filename = mapname.string();
}

// Write a small image of map preview
static void WriteMapPreview(const fs::path &mapname, CMap &map)
{
	const int rectSize = 5; // size of rectange used for player start spots
	const SDL_PixelFormat *fmt = MinimapSurface->format;
	SDL_Surface *preview = SDL_CreateRGBSurface(SDL_SWSURFACE,
												UI.Minimap.W, UI.Minimap.H, 32, fmt->Rmask, fmt->Gmask, fmt->Bmask, 0);
	SDL_BlitSurface(MinimapSurface, nullptr, preview, nullptr);

	SDL_LockSurface(preview);

	SDL_Rect rect;
	for (int i = 0; i < PlayerMax - 1; ++i) {
		if (Players[i].Type != PlayerTypes::PlayerNobody) {
			rect.x = Players[i].StartPos.x * UI.Minimap.W / map.Info.MapWidth - rectSize / 2;
			rect.y = Players[i].StartPos.y * UI.Minimap.H / map.Info.MapHeight - rectSize / 2;
			rect.w = rect.h = rectSize;
			SDL_FillRect(preview, &rect, Players[i].Color);
		}
	}

	SDL_UnlockSurface(preview);
	IMG_SavePNG(preview, mapname.string().c_str());
	SDL_FreeSurface(preview);
}

std::string PlayerTypeNames[static_cast<int>(PlayerTypes::PlayerRescueActive) + 1] = {
	"",
	"",
	"neutral",
	"nobody",
	"computer",
	"person",
	"rescue-passive",
	"rescue-active"
};

// Write the map presentation file
static bool WriteMapPresentation(const fs::path &mapname, CMap &map, Vec2i newSize)
{
	int numplayers = 0;
	int topplayer = PlayerMax - 2;

	try {
		std::unique_ptr<FileWriter> f = CreateFileWriter(mapname);
		f->printf("-- Stratagus Map Presentation\n");
		f->printf("-- File generated by the Stratagus V" VERSION " builtin map editor.\n");
		// MAPTODO Copyright notice in generated file
		f->printf("-- File licensed under the GNU GPL version 2.\n\n");

		f->printf("DefinePlayerTypes(");
		while (topplayer > 0 && map.Info.PlayerType[topplayer] == PlayerTypes::PlayerNobody) {
			--topplayer;
		}
		for (int i = 0; i <= topplayer; ++i) {
			f->printf("%s\"%s\"", (i ? ", " : ""), PlayerTypeNames[static_cast<int>(map.Info.PlayerType[i])].c_str());
			if (map.Info.PlayerType[i] == PlayerTypes::PlayerPerson) {
				++numplayers;
			}
		}
		f->printf(")\n");

		if (newSize.x == 0 || newSize.y == 0) {
			newSize.x = map.Info.MapWidth;
			newSize.y = map.Info.MapHeight;
		}

		f->printf("PresentMap(\"%s\", %d, %d, %d, %d%s)\n",
				  map.Info.Description.c_str(), numplayers, newSize.x, newSize.y,
				  map.Info.MapUID + 1,
				  Map.Info.IsHighgroundsEnabled()? ", \"highgrounds-enabled\"" : "");

		if (map.Info.Filename.find(".sms") == std::string::npos && !map.Info.Filename.empty()) {
			f->printf("DefineMapSetup(\"%s\")\n", map.Info.Filename.c_str());
		}
	} catch (const FileException &) {
		ErrorPrint("ERROR: cannot write the map presentation\n");
		return false;
	}
	return true;
}


/**
**  Write the map setup file.
**
**  @param mapSetup      map filename
**  @param map           map to save
**  @param writeTerrain  write the tiles map in the .sms
*/
static bool WriteMapSetup(const fs::path &mapSetup, CMap &map, int writeTerrain, Vec2i newSize, Vec2i offset)
{
	try {
		std::unique_ptr<FileWriter> f = CreateFileWriter(mapSetup);

		f->printf("-- Stratagus Map Setup\n");
		f->printf("-- File generated by the Stratagus V" VERSION " builtin map editor.\n");
		// MAPTODO Copyright notice in generated file
		f->printf("-- File licensed under the GNU GPL version 2.\n\n");

		f->printf("-- preamble\n");
		f->printf("if CanAccessFile(__file__ .. \".preamble\") then Load(__file__ .. \".preamble\", Editor.Running == 0) end\n\n");
		if (!Map.Info.Preamble.empty()) {
			std::unique_ptr<FileWriter> preamble = CreateFileWriter(mapSetup.string() + ".preamble");
			preamble->write(Map.Info.Preamble);
		}

		f->printf("-- player configuration\n");
		for (int i = 0; i < PlayerMax; ++i) {
			if (Map.Info.PlayerType[i] == PlayerTypes::PlayerNobody) {
				continue;
			}
			f->printf("SetStartView(%d, %d, %d)\n", i, Players[i].StartPos.x, Players[i].StartPos.y);
			f->printf("SetPlayerData(%d, \"Resources\", \"%s\", %d)\n",
					  i, DefaultResourceNames[WoodCost].c_str(),
					  Players[i].Resources[WoodCost]);
			f->printf("SetPlayerData(%d, \"Resources\", \"%s\", %d)\n",
					  i, DefaultResourceNames[GoldCost].c_str(),
					  Players[i].Resources[GoldCost]);
			f->printf("SetPlayerData(%d, \"Resources\", \"%s\", %d)\n",
					  i, DefaultResourceNames[OilCost].c_str(),
					  Players[i].Resources[OilCost]);
			f->printf("SetPlayerData(%d, \"RaceName\", \"%s\")\n",
					  i, PlayerRaces.Name[Players[i].Race].c_str());
			f->printf("SetAiType(%d, \"%s\")\n",
					  i, Players[i].AiName.c_str());
		}
		f->printf("\n");

		f->printf("-- load tilesets\n");
		f->printf("LoadTileModels(\"%s\")\n\n", map.TileModelsFileName.string().c_str());

		if (writeTerrain) {
			if (newSize.x != 0 && newSize.y != 0) {
				f->printf("for x=0,%d,1 do\n", newSize.x - 1);
				f->printf("    for y=0,%d,1 do\n", newSize.y - 1);
				f->printf("        SetTile(%d, x, y, 0)\n", Map.Tileset.getDefaultTileIndex());
				f->printf("    end\n");
				f->printf("end\n");
			} else {
				newSize.x = map.Info.MapWidth;
				newSize.y = map.Info.MapHeight;
			}

			f->printf("-- Tiles Map\n");
			for (int i = 0; i < map.Info.MapHeight; ++i) {
				for (int j = 0; j < map.Info.MapWidth; ++j) {
					const CMapField &mf = map.Fields[j + i * map.Info.MapWidth];
					const int tile = mf.getGraphicTile();
					const int32_t n = mf.getTileIndex();
					const int value = mf.Value;
					const int elevation = mf.getElevation();
					const int x = j + offset.x;
					const int y = i + offset.y;
					if (x < newSize.x && y < newSize.y) {
						f->printf("SetTile(%3d, %d, %d, %d, %d)\n", n, x, y, value, elevation);
					}
				}
			}
		}

		if (newSize.x == 0 || newSize.y == 0) {
			newSize.x = map.Info.MapWidth;
			newSize.y = map.Info.MapHeight;
		}

		f->printf("\n-- set map default stat and map sound for unit types\n");
		for (const CUnitType *typePtr : getUnitTypes()) {
			const CUnitType &type = *typePtr;
			for (unsigned int j = 0; j < MaxCosts; ++j) {
				if (type.MapDefaultStat.Costs[j] != type.DefaultStat.Costs[j]) {
					f->printf("SetMapStat(\"%s\", \"Costs\", %d, \"%s\")\n", type.Ident.c_str(), type.MapDefaultStat.Costs[j], DefaultResourceNames[j].c_str());
				}
			}
			for (unsigned int j = 0; j < MaxCosts; ++j) {
				if (type.MapDefaultStat.ImproveIncomes[j] != type.DefaultStat.ImproveIncomes[j]) {
					f->printf("SetMapStat(\"%s\", \"ImproveProduction\", %d, \"%s\")\n", type.Ident.c_str(), type.MapDefaultStat.ImproveIncomes[j], DefaultResourceNames[j].c_str());
				}
			}
			for (size_t j = 0; j < UnitTypeVar.GetNumberVariable(); ++j) {
				if (type.MapDefaultStat.Variables[j] != type.DefaultStat.Variables[j]) {
					f->printf("SetMapStat(\"%s\", \"%s\", %d, \"Value\")\n", type.Ident.c_str(), UnitTypeVar.VariableNameLookup[j].data(), type.MapDefaultStat.Variables[j].Value);
					f->printf("SetMapStat(\"%s\", \"%s\", %d, \"Max\")\n", type.Ident.c_str(), UnitTypeVar.VariableNameLookup[j].data(), type.MapDefaultStat.Variables[j].Max);
					f->printf("SetMapStat(\"%s\", \"%s\", %d, \"Enable\")\n", type.Ident.c_str(), UnitTypeVar.VariableNameLookup[j].data(), type.MapDefaultStat.Variables[j].Enable);
					f->printf("SetMapStat(\"%s\", \"%s\", %d, \"Increase\")\n", type.Ident.c_str(), UnitTypeVar.VariableNameLookup[j].data(), type.MapDefaultStat.Variables[j].Increase);
				}
			}

			if (type.MapSound.Selected.Name != type.Sound.Selected.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"selected\")\n", type.Ident.c_str(), type.MapSound.Selected.Name.c_str());
			}
			if (type.MapSound.Acknowledgement.Name != type.Sound.Acknowledgement.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"acknowledge\")\n", type.Ident.c_str(), type.MapSound.Acknowledgement.Name.c_str());
			}
			if (type.MapSound.Attack.Name != type.Sound.Attack.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"attack\")\n", type.Ident.c_str(), type.MapSound.Attack.Name.c_str());
			}
			if (type.MapSound.Build.Name != type.Sound.Build.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"build\")\n", type.Ident.c_str(), type.MapSound.Build.Name.c_str());
			}
			if (type.MapSound.Ready.Name != type.Sound.Ready.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"ready\")\n", type.Ident.c_str(), type.MapSound.Ready.Name.c_str());
			}
			if (type.MapSound.Repair.Name != type.Sound.Repair.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"repair\")\n", type.Ident.c_str(), type.MapSound.Repair.Name.c_str());
			}
			for (unsigned int j = 0; j < MaxCosts; ++j) {
				if (type.MapSound.Harvest[j].Name != type.Sound.Harvest[j].Name) {
					f->printf("SetMapSound(\"%s\", \"%s\", \"harvest\", \"%s\")\n", type.Ident.c_str(), type.MapSound.Harvest[j].Name.c_str(), DefaultResourceNames[j].c_str());
				}
			}
			if (type.MapSound.Help.Name != type.Sound.Help.Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"help\")\n", type.Ident.c_str(), type.MapSound.Help.Name.c_str());
			}
			if (type.MapSound.Dead[ANIMATIONS_DEATHTYPES].Name != type.Sound.Dead[ANIMATIONS_DEATHTYPES].Name) {
				f->printf("SetMapSound(\"%s\", \"%s\", \"dead\")\n", type.Ident.c_str(), type.MapSound.Dead[ANIMATIONS_DEATHTYPES].Name.c_str());
			}
			int death;
			for (death = 0; death < ANIMATIONS_DEATHTYPES; ++death) {
				if (type.MapSound.Dead[death].Name != type.Sound.Dead[death].Name) {
					f->printf("SetMapSound(\"%s\", \"%s\", \"dead\", \"%s\")\n", type.Ident.c_str(), type.MapSound.Dead[death].Name.c_str(), ExtraDeathTypes[death].c_str());
				}
			}
		}

		f->printf("\n-- place units\n");
		f->printf("if (MapUnitsInit ~= nil) then MapUnitsInit() end\n");
		std::vector<const CUnit *> teleporters;
		for (const CUnit *unitPtr : UnitManager->GetUnits()) {
			const CUnit &unit = *unitPtr;
			const int x = unit.tilePos.x + offset.x;
			const int y = unit.tilePos.y + offset.y;
			if (x < newSize.x && y < newSize.y) {
				f->printf("unit = CreateUnit(\"%s\", %d, {%d, %d})\n",
						  unit.Type->Ident.c_str(),
						  unit.Player->Index,
						  x, y);
				if (unit.Type->GivesResource) {
					f->printf("SetResourcesHeld(unit, %d)\n", unit.ResourcesHeld);
				}
				if (!unit.Active) { //Active is true by default
					f->printf("SetUnitVariable(unit, \"Active\", false)\n");
				}
				if (unit.Type->BoolFlag[TELEPORTER_INDEX].value && unit.Goal) {
					teleporters.push_back(&unit);
				}
			}
		}
		f->printf("\n\n");
		for (const CUnit* unitPtr : teleporters) {
			const CUnit &unit = *unitPtr;
			f->printf("SetTeleportDestination(%d, %d)\n", UnitNumber(unit), UnitNumber(*unit.Goal));
		}
		f->printf("\n\n");

		f->printf("-- postamble\n");
		f->printf("if CanAccessFile(__file__ .. \".postamble\") then Load(__file__ .. \".postamble\", Editor.Running == 0) end\n\n");
		if (!Map.Info.Postamble.empty()) {
			std::unique_ptr<FileWriter> postamble = CreateFileWriter(mapSetup.string() + ".postamble");
			postamble->write(Map.Info.Postamble);
		}

	} catch (const FileException &) {
		ErrorPrint("Can't save map setup: '%s'\n", mapSetup.u8string().c_str());
		return false;
	}
	return true;
}



/**
**  Save a Stratagus map.
**
**  @param mapName   map filename
**  @param map       map to save
**  @param writeTerrain   write the tiles map in the .sms
*/
bool SaveStratagusMap(const fs::path &mapName, CMap &map, int writeTerrain, Vec2i newSize, Vec2i offset)
{
	if (!map.Info.MapWidth || !map.Info.MapHeight) {
		ErrorPrint("'%s': invalid Stratagus map\n", mapName.u8string().c_str());
		ExitFatal(-1);
	}

	std::string extraExtension; // typically compression extension
	fs::path mapSetup = mapName;
	while (mapSetup.has_extension() && mapSetup.extension() != ".smp") {
		extraExtension = mapSetup.extension().string() + extraExtension;
		mapSetup.replace_extension();
	}
	if (mapSetup.extension() != ".smp") {
		ErrorPrint("'%s': invalid Stratagus map filename\n", mapName.u8string().c_str());
		return false;
	}

	fs::path previewName = mapSetup;
	previewName.replace_extension(".png");
	WriteMapPreview(previewName, map);

	if (!WriteMapPresentation(mapName, map, newSize)) {
		return false;
	}

	mapSetup.replace_extension(".sms" + extraExtension);
	return WriteMapSetup(mapSetup, map, writeTerrain, newSize, offset);
}

/**
**  Load any map.
**
**  @param filename  map filename
**  @param map       map loaded
*/
static void LoadMap(const fs::path &filename, CMap &map)
{
	auto name = filename;
#ifdef USE_ZLIB
	if (name.extension() == ".gz") {
		name.replace_extension();
	}
#endif
#ifdef USE_BZ2LIB
	if (name.extension() == ".bz2") {
		name.replace_extension();
	}
#endif
	if (name.extension() == ".smp") {
		if (map.Info.Filename.empty()) {
			// The map info hasn't been loaded yet => do it now
			LoadStratagusMapInfo(filename);
		}
		Assert(!map.Info.Filename.empty());
#if defined(STRATAGUS_PAX) && !defined(PAX_STRATAGUS_WARCRAFT)
		// Parse the large generated tileset while the map's 7 MB of field
		// pages are still unallocated. The scenario reuses this immutable set.
		const bool tutorial = filename.string().find("/tutorial/") != std::string::npos;
		CclCommand(tutorial ? "LoadTileModels('luagen/tilesets/platform.lua')"
		                    : "LoadTileModels('luagen/tilesets/badlands.lua')");
		LuaGarbageCollect();
#endif
		map.Create();
		LoadStratagusMap(filename.parent_path(), map.Info.Filename);
		return;
	}

	ErrorPrint("Unrecognized map format\n");
	ExitFatal(-1);
}

/**
**  Set the game paused or unpaused
**
**  @param paused  True to pause game, false to unpause.
*/
void SetGamePaused(bool paused)
{
	GamePaused = paused;
}

/**
**  Get the game paused or unpaused
**
**  @return  True if the game is paused, false otherwise
*/
bool GetGamePaused()
{
	return GamePaused;
}

/**
**  Set the game speed in range 0 .. 100
**
**  @param speed  New game speed.
*/
void SetGameSpeed(int speed)
{
	if (GameCycle == 0 || FastForwardCycle < GameCycle) {
		CyclesPerSecond = (static_cast<double>(speed) / 100) * CYCLES_PER_SECOND + static_cast<double>(CYCLES_PER_SECOND) / 3;
		SetVideoSync();
	}
}

/**
**  Get the game speed
**
**  @return  Game speed
*/
int GetGameSpeed()
{
	return ((static_cast<double>(CyclesPerSecond) - static_cast<double>(CYCLES_PER_SECOND) / 3) / CYCLES_PER_SECOND) * 100;
}

/*----------------------------------------------------------------------------
--  Game types
----------------------------------------------------------------------------*/

/**
**  Free for all
*/
static void GameTypeFreeForAll()
{
	for (int i = 0; i < PlayerMax - 1; ++i) {
		for (int j = i + 1; j < PlayerMax - 1; ++j) {
			CommandDiplomacy(i, EDiplomacy::Enemy, j);
			CommandDiplomacy(j, EDiplomacy::Enemy, i);
		}
	}
}

/**
**  Top vs Bottom
*/
static void GameTypeTopVsBottom()
{
	const int middle = Map.Info.MapHeight / 2;

	for (int i = 0; i < PlayerMax - 1; ++i) {
		const bool top_i = Players[i].StartPos.y <= middle;

		for (int j = i + 1; j < PlayerMax - 1; ++j) {
			const bool top_j = Players[j].StartPos.y <= middle;

			if (top_i == top_j) {
				CommandDiplomacy(i, EDiplomacy::Allied, j);
				Players[i].ShareVisionWith(Players[j]);
				CommandDiplomacy(j, EDiplomacy::Allied, i);
				Players[j].ShareVisionWith(Players[i]);
			} else {
				CommandDiplomacy(i, EDiplomacy::Enemy, j);
				CommandDiplomacy(j, EDiplomacy::Enemy, i);
			}
		}
	}
}

/**
**  Left vs Right
*/
static void GameTypeLeftVsRight()
{
	const int middle = Map.Info.MapWidth / 2;

	for (int i = 0; i < PlayerMax - 1; ++i) {
		const bool left_i = Players[i].StartPos.x <= middle;

		for (int j = i + 1; j < PlayerMax - 1; ++j) {
			const bool left_j = Players[j].StartPos.x <= middle;

			if (left_i == left_j) {
				CommandDiplomacy(i, EDiplomacy::Allied, j);
				Players[i].ShareVisionWith(Players[j]);
				CommandDiplomacy(j, EDiplomacy::Allied, i);
				Players[j].ShareVisionWith(Players[i]);
			} else {
				CommandDiplomacy(i, EDiplomacy::Enemy, j);
				CommandDiplomacy(j, EDiplomacy::Enemy, i);
			}
		}
	}
}

/**
**  Man vs Machine
*/
static void GameTypeManVsMachine()
{
	for (int i = 0; i < PlayerMax - 1; ++i) {
		if (Players[i].Type != PlayerTypes::PlayerPerson && Players[i].Type != PlayerTypes::PlayerComputer) {
			continue;
		}
		for (int j = i + 1; j < PlayerMax - 1; ++j) {
			if (Players[j].Type != PlayerTypes::PlayerPerson && Players[j].Type != PlayerTypes::PlayerComputer) {
				continue;
			}
			if (Players[i].Type == Players[j].Type) {
				CommandDiplomacy(i, EDiplomacy::Allied, j);
				Players[i].ShareVisionWith(Players[j]);
				CommandDiplomacy(j, EDiplomacy::Allied, i);
				Players[j].ShareVisionWith(Players[i]);
			} else {
				CommandDiplomacy(i, EDiplomacy::Enemy, j);
				CommandDiplomacy(j, EDiplomacy::Enemy, i);
			}
		}
	}
}

/**
**  Man vs Machine with Humans on a Team
*/
static void GameTypeManTeamVsMachine()
{
	for (int i = 0; i < PlayerMax - 1; ++i) {
		if (Players[i].Type != PlayerTypes::PlayerPerson && Players[i].Type != PlayerTypes::PlayerComputer) {
			continue;
		}
		for (int j = 0; j < PlayerMax - 1; ++j) {
			if (i != j) {
				if (Players[i].Type == Players[j].Type) {
					CommandDiplomacy(i, EDiplomacy::Allied, j);
					Players[i].ShareVisionWith(Players[j]);
				} else {
					CommandDiplomacy(i, EDiplomacy::Enemy, j);
				}
			}
		}
		if (Players[i].Type == PlayerTypes::PlayerPerson) {
			Players[i].Team = 2;
		} else {
			Players[i].Team = 1;
		}
	}
}

/**
**  Machine vs Machine
*/
static void GameTypeMachineVsMachine()
{
	Map.Reveal();
	for (int i = 0; i < PlayerMax - 1; ++i) {
		if (Players[i].Type == PlayerTypes::PlayerComputer) {
			for (int j = i + 1; j < PlayerMax - 1; ++j) {
				if (Players[j].Type == PlayerTypes::PlayerComputer) {
					CommandDiplomacy(i, EDiplomacy::Enemy, j);
					CommandDiplomacy(j, EDiplomacy::Enemy, i);
				} else {
					CommandDiplomacy(i, EDiplomacy::Neutral, j);
					CommandDiplomacy(j, EDiplomacy::Neutral, i);
				}
			}
		}
	}
}

/**
 ** Machine vs Machine Training
 */
static void GameTypeMachineVsMachineTraining()
{
	Assert(!IsNetworkGame());
	GameTypeMachineVsMachine();
	FastForwardCycle = LONG_MAX;
	SyncHash = 0;
	InitSyncRand();
	SetEffectsEnabled(false);
	SetMusicEnabled(false);
	for (int i = 0; i < MyRand() % 100; i++) {
		SyncRand();
	}
}

/*----------------------------------------------------------------------------
--  Game creation
----------------------------------------------------------------------------*/

static bool has_any_extension(fs::path filename, std::initializer_list<fs::path> extensions)
{
	while (filename.has_extension()) {
		if (ranges::contains(extensions, filename.extension())) {
			return true;
		}
		filename.replace_extension();
	}
	return false;
}

/**
**  CreateGame.
**
**  Load map, graphics, sounds, etc
**
**  @param filename  map filename
**  @param map       map loaded
**
**  @todo FIXME: use in this function InitModules / LoadModules!!!
*/
void CreateGame(const fs::path &filename, CMap *map)
{
	fs::path load_filename = filename;
#ifdef STRATAGUS_PAX
	// StartMap historically receives a logical relative path.  Normalize it at
	// the game boundary so every map-related open uses the PAX service's
	// absolute /sd path, including the initial .smp metadata load.
	if (!load_filename.empty() && !load_filename.is_absolute()) {
		load_filename = fs::path(LibraryFileName(load_filename.string()));
	}
#endif
	if (SaveGameLoading) {
		SaveGameLoading = false;
		// Load game, already created game with Init/LoadModules
		CommandLog(nullptr, nullptr, EFlushMode::On, -1, -1, nullptr, nullptr, -1);
		return;
	}

	InitPlayers();

	if (IsNetworkGame()) {
		// if is a network game, it is necessary to reinitialize the syncrand
		// variables before beginning to load the map, due to random map
		// generation
		SyncHash = 0;
		InitSyncRand();
	}

	if (Map.Info.Filename.empty() && has_any_extension(load_filename, {".smp", ".SMP"})) {
		const std::string path = load_filename.string();
		LuaLoadFile(path);
	}

	for (int i = 0; i < PlayerMax; ++i) {
		PlayerTypes playertype = Map.Info.PlayerType[i];
		if (GameSettings.Presets[i].Type != PlayerTypes::MapDefault) {
			playertype = GameSettings.Presets[i].Type;
		}
		CreatePlayer(playertype);
		if (GameSettings.Presets[i].Team != SettingsPresetMapDefault) {
			// why this calculation? Well. The CreatePlayer function assigns some
			// default team values, starting from up to PlayerMax + some constant (2 at the time
			// of this writing). So to not accidentally interfere with those teams, we assign the team
			// offset by 2 * PlayerMax.
			Players[i].Team = GameSettings.Presets[i].Team + 2 * PlayerMax;
			for (int j = 0; j < i; j++) {
				if (GameSettings.Presets[j].Team != SettingsPresetMapDefault) {
					if (GameSettings.Presets[j].Team != GameSettings.Presets[i].Team) {
						Players[i].SetDiplomacyEnemyWith(Players[j]);
						Players[j].SetDiplomacyEnemyWith(Players[i]);
					} else {
						Players[i].SetDiplomacyAlliedWith(Players[j]);
						Players[j].SetDiplomacyAlliedWith(Players[i]);
						Players[i].ShareVisionWith(Players[j]);
						Players[j].ShareVisionWith(Players[i]);
					}
				}
			}
		}
	}
	if (!ThisPlayer) {
		if (!IsNetworkGame()) {
			// In demo or kiosk mode, pick first empty slot
			if (auto it = ranges::find_if(
					Players, [](const CPlayer &p) { return p.Type == PlayerTypes::PlayerNobody; });
			    it != std::end(Players)) {
				ThisPlayer = &*it;
			}
		} else {
			// this is bad - we are starting a network game, but ThisPlayer is not assigned!!
			ErrorPrint("FATAL ENGINE BUG! "
			           "We are starting a network game, but ThisPlayer is not assigned!\n"
			           "\tNetPlayers: %d\n"
			           "\tNumPlayers: %d\n"
			           "\tNetLocalPlayerNumber: %d\n",
			           NetPlayers,
			           NumPlayers,
			           NetLocalPlayerNumber);
			EnableDebugPrint = true;
			DebugPlayers();
			printf("ServerSetupState\n");
			ServerSetupState.Save(+[](std::string f) { printf("%s", f.c_str()); });
			printf("LocalSetupState\n");
			LocalSetupState.Save(+[](std::string f) { printf("%s", f.c_str()); });
			printf("GameSettings\n");
			GameSettings.Save(+[](std::string f) { printf("%s\n", f.c_str()); });
			ExitFatal(-1);
		}
	}
	if (!load_filename.empty()) {
		if (CurrentMapPath != load_filename) {
			strcpy_s(CurrentMapPath, sizeof(CurrentMapPath), load_filename.string().c_str());
		}

		//
		// Load the map.
		//
		InitUnitTypes(1);
		#ifdef STRATAGUS_PAX
		// The native HUD bootstrap leaves disposable Lua argument tables.
		// Reclaim them before the large terrain chunk, not only after LoadMap.
		LuaGarbageCollect();
		#endif
		LoadMap(load_filename, *map);
		#ifdef STRATAGUS_PAX
		pax_stratagus_debug("CreateGame after LoadMap");
		#endif
		ApplyUpgrades();
		#ifdef STRATAGUS_PAX
		pax_stratagus_debug("CreateGame after ApplyUpgrades");
		#endif
	}
	CclCommand("if (MapLoaded ~= nil) then MapLoaded() end");
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after MapLoaded command");
	// The tileset/scenario loaders build large temporary Lua tables.  Collect
	// them before reserving the screen, sprite and HUD buffers in PSRAM.
	LuaGarbageCollect();
	pax_stratagus_debug("CreateGame after map Lua collection");
	#endif

	GameCycle = 0;
	FastForwardCycle = 0;
	SyncHash = 0;
	InitSyncRand();

	NetworkOnStartGame();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after NetworkOnStartGame");
	#endif

#if 0
	GamePaused = true;
#endif

	if (FlagRevealMap != MapRevealModes::cHidden) {
		Map.Reveal(FlagRevealMap);
	}
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after Map.Reveal");
	#endif

	//
	// Setup game types
	//
	// FIXME: implement more game types
	if (GameSettings.GameType != GameTypes::SettingsGameTypeMapDefault) {
		switch (GameSettings.GameType) {
			case GameTypes::SettingsGameTypeMelee:
				break;
			case GameTypes::SettingsGameTypeFreeForAll:
				GameTypeFreeForAll();
				break;
			case GameTypes::SettingsGameTypeTopVsBottom:
				GameTypeTopVsBottom();
				break;
			case GameTypes::SettingsGameTypeLeftVsRight:
				GameTypeLeftVsRight();
				break;
			case GameTypes::SettingsGameTypeManVsMachine:
				GameTypeManVsMachine();
				break;
			case GameTypes::SettingsGameTypeManTeamVsMachine:
				GameTypeManTeamVsMachine();
				break;
			case GameTypes::SettingsGameTypeMachineVsMachine:
				GameTypeMachineVsMachine();
				break;
			case GameTypes::SettingsGameTypeMachineVsMachineTraining:
				GameTypeMachineVsMachineTraining();
				break;

				// Future game type ideas
#if 0
			case GameTypes::SettingsGameTypeOneOnOne:
				break;
			case GameTypes::SettingsGameTypeCaptureTheFlag:
				break;
			case GameTypes::SettingsGameTypeGreed:
				break;
			case GameTypes::SettingsGameTypeSlaughter:
				break;
			case GameTypes::SettingsGameTypeSuddenDeath:
				break;
			case GameTypes::SettingsGameTypeTeamMelee:
				break;
			case GameTypes::SettingsGameTypeTeamCaptureTheFlag:
				break;
#endif
            default:
				break;
		}
	}

	//
	// Graphic part
	//
	SetPlayersPalette();
	LoadIcons();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after LoadIcons");
	#endif

	LoadCursors(PlayerRaces.Name[ThisPlayer->Race]);
	UnitUnderCursor = nullptr;

	InitMissileTypes();
#ifndef DYNAMIC_LOAD
	LoadMissileSprites();
#endif
	InitConstructions();
	LoadConstructions();
	LoadUnitTypes();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after LoadUnitTypes");
	#endif
	LoadDecorations();

	InitUserInterface();
	UI.Load();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after UI.Load");
	// Enhanced fog keeps several 1028x1028 buffers for this map.  Use the
	// upstream tiled renderer on the fixed-memory PAX target instead.
	FogOfWar->SetType(FogOfWarTypes::cTiledLegacy);
	#endif

	Map.Init();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after Map.Init");
	#endif
	UI.Minimap.Create();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after Minimap.Create");
	#endif
	PreprocessMap();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame after PreprocessMap");
	#endif

	//
	// Sound part
	//
	LoadUnitSounds();
	MapUnitSounds();
#ifdef STRATAGUS_PAX
	#ifndef PAX_STRATAGUS_WARCRAFT
	GameSounds.Click.Sound=SoundForName("button");
	for(unsigned r=0;r<PlayerRaces.Count;++r) {
		GameSounds.PlacementError[r].Sound=SoundForName("buzz");
		GameSounds.PlacementSuccess[r].Sound=SoundForName("building-placed");
		const char *race=PlayerRaces.Name[r].c_str();
		GameSounds.NotEnoughRes[r][1].Sound=SoundForName(std::string("pax-")+race+"-minerals");
		GameSounds.NotEnoughRes[r][2].Sound=SoundForName(std::string("pax-")+race+"-gas");
		GameSounds.NotEnoughFood[r].Sound=SoundForName(std::string("pax-")+race+"-supply");
		GameSounds.ResearchComplete[r].Sound=SoundForName(std::string("pax-")+race+"-research");
	}
	#endif
#endif
	if (SoundEnabled()) {
		InitSoundClient();
	}

	//
	// Spells
	//
	InitSpells();

	//
	// Init players?
	//
	DebugPlayers();
	PlayersInitAi();

	//
	// Upgrades
	//
	InitUpgrades();

	//
	// Dependencies
	//
	InitDependencies();

	//
	// Buttons (botpanel)
	//
	InitButtons();

	//
	// Triggers
	//
	InitTriggers();

	SetDefaultTextColors(UI.NormalFontColor, UI.ReverseFontColor);

#if 0
	if (!UI.SelectedViewport) {
		UI.SelectedViewport = UI.Viewports;
	}
#endif
	UI.SelectedViewport->Center(Map.TilePosToMapPixelPos_Center(ThisPlayer->StartPos));

	//
	// Various hacks which must be done after the map is loaded.
	//
	// FIXME: must be done after map is loaded
	InitPathfinder();
	//
	// FIXME: The palette is loaded after the units are created.
	// FIXME: This loops fixes the colors of the units.
	//
	for (CUnit *unit : UnitManager->GetUnits()) {
		if (unit->Type->OnReady) {
			unit->Type->OnReady(UnitNumber(*unit));
		}
	}

	GameResult = GameNoResult;

	CommandLog(nullptr, nullptr, EFlushMode::On, -1, -1, nullptr, nullptr, -1);
	Video.ClearScreen();
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("CreateGame complete");
	#endif
}

/**
**  Init Game Setting to default values
**
**  @todo  FIXME: this should not be executed for restart levels!
*/
void InitSettings()
{
	GameSettings.Init();
	Preference.InitializeSettingsFromPreferences(GameSettings);
}

// call the lua function: CleanGame_Lua.
static void CleanGame_Lua()
{
	lua_getglobal(Lua, "CleanGame_Lua");
	if (lua_isfunction(Lua, -1)) {
		LuaCall(0, 1);
	} else {
		lua_pop(Lua, 1);
	}
}

/**
**  Cleanup game.
**
**  Call each module to clean up.
**  Contrary to CleanModules, maps can be restarted
**  without reloading all lua files.
*/
void CleanGame()
{
	EndReplayLog();
	CleanMessages();

	RestoreColorCyclingSurface();
	CleanGame_Lua();
	CleanTriggers();
	CleanAi();
	CleanGroups();
	CleanMissiles();
	CleanUnits();
	CleanSelections();
	Map.Clean();
	CleanReplayLog();
	FreePathfinder();
	CursorBuilding = nullptr;
	UnitUnderCursor = nullptr;
	GameEstablishing = false;
}

/**
** <b>Description</b>
**
**  Return of game name.
**
**  @param l  Lua state.
**
** Example:
**
** <div class="example"><code><strong>SetGameName</strong>("Wargus Map - Chapter 1")</code></div>
*/
static int CclSetGameName(lua_State *l)
{
	const int args = lua_gettop(l);
	if (args > 1 || (args == 1 && (!lua_isnil(l, 1) && !lua_isstring(l, 1)))) {
		LuaError(l, "incorrect argument");
	}
	if (args == 1 && !lua_isnil(l, 1)) {
		GameName = lua_tostring(l, 1);
	}

#ifndef STRATAGUS_PAX
	if (!GameName.empty()) {
		fs::create_directories(Parameters::Instance.GetUserDirectory() / GameName);
	}
#endif
	return 0;
}

static int CclSetFullGameName(lua_State *l)
{
	const int args = lua_gettop(l);
	if (args > 1 || (args == 1 && (!lua_isnil(l, 1) && !lua_isstring(l, 1)))) {
		LuaError(l, "incorrect argument");
	}
	if (args == 1 && !lua_isnil(l, 1)) {
		FullGameName = lua_tostring(l, 1);
	}
	return 0;
}

/**
** <b>Description</b>
**
**  Set God mode.
**
**  @param l  Lua state.
**
**  @return   The old mode.
**
** Example:
**
** <div class="example"><code>-- God Mode enabled
**		<strong>SetGodMode</strong>(true)
**		-- God Mode disabled
**		<strong>SetGodMode</strong>(false)</code></div>
*/
static int CclSetGodMode(lua_State *l)
{
	LuaCheckArgs(l, 1);
	GodMode = LuaToBoolean(l, 1);
	return 0;
}

/**
** <b>Description</b>
**
**  Get God mode.
**
**  @param l  Lua state.
**
**  @return   God mode.
**
** Example:
**
** <div class="example"><code>g_mode = <strong>GetGodMode</strong>()
**		print(g_mode)</code></div>
*/
static int CclGetGodMode(lua_State *l)
{
	LuaCheckArgs(l, 0);
	lua_pushboolean(l, GodMode);
	return 1;
}

/**
**  Set resource harvesting speed (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeedResourcesHarvest(lua_State *l)
{
	LuaCheckArgs(l, 3);

	const int player = LuaToNumber(l, 1);
	const std::string_view resource = LuaToString(l, 2);
	const int resId = GetResourceIdByName(l, resource);

	Players[player].SpeedResourcesHarvest[resId] = LuaToNumber(l, 3);
	return 0;
}

/**
**  Set resource returning speed (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeedResourcesReturn(lua_State *l)
{
	LuaCheckArgs(l, 3);

	const int player = LuaToNumber(l, 1);
	const std::string_view resource = LuaToString(l, 2);
	const int resId = GetResourceIdByName(l, resource);

	Players[player].SpeedResourcesReturn[resId] = LuaToNumber(l, 3);
	return 0;
}

/**
**  Set building speed (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeedBuild(lua_State *l)
{
	LuaCheckArgs(l, 2);
	const int player = LuaToNumber(l, 1);
	Players[player].SpeedBuild = LuaToNumber(l, 2);
	return 0;
}

/**
**  Get building speed (deprecated).
**
**  @param l  Lua state.
**
**  @return   Building speed.
*/
static int CclGetSpeedBuild(lua_State *l)
{
	LuaCheckArgs(l, 1);
	const int player = LuaToNumber(l, 1);
	lua_pushnumber(l, Players[player].SpeedBuild);
	return 1;
}

/**
**  Set training speed (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeedTrain(lua_State *l)
{
	LuaCheckArgs(l, 2);
	const int player = LuaToNumber(l, 1);
	Players[player].SpeedTrain = LuaToNumber(l, 2);
	return 0;
}

/**
**  Get training speed (deprecated).
**
**  @param l  Lua state.
**
**  @return   Training speed.
*/
static int CclGetSpeedTrain(lua_State *l)
{
	LuaCheckArgs(l, 1);
	const int player = LuaToNumber(l, 1);
	lua_pushnumber(l, Players[player].SpeedTrain);
	return 1;
}

/**
**  For debug increase upgrading speed (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeedUpgrade(lua_State *l)
{
	LuaCheckArgs(l, 2);
	const int player = LuaToNumber(l, 1);
	Players[player].SpeedUpgrade = LuaToNumber(l, 2);

	lua_pushnumber(l, Players[player].SpeedUpgrade);
	return 1;
}

/**
**  For debug increase researching speed (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeedResearch(lua_State *l)
{
	LuaCheckArgs(l, 2);
	const int player = LuaToNumber(l, 1);
	Players[player].SpeedResearch = LuaToNumber(l, 2);

	lua_pushnumber(l, Players[player].SpeedResearch);
	return 1;
}

/**
**  For debug increase all speeds (deprecated).
**
**  @param l  Lua state.
*/
static int CclSetSpeeds(lua_State *l)
{
	LuaCheckArgs(l, 1);
	const int speed = LuaToNumber(l, 1);
	for (int i = 0; i < PlayerMax; ++i) {
		for (int j = 0; j < MaxCosts; ++j) {
			Players[i].SpeedResourcesHarvest[j] = speed;
			Players[i].SpeedResourcesReturn[j] = speed;
		}
		Players[i].SpeedBuild = Players[i].SpeedTrain = Players[i].SpeedUpgrade = Players[i].SpeedResearch = speed;
	}

	lua_pushnumber(l, speed);
	return 1;
}

/**
**  Define default incomes for a new player.
**
**  @param l  Lua state.
*/
static int CclDefineDefaultIncomes(lua_State *l)
{
	const int args = lua_gettop(l);
	for (int i = 0; i < MaxCosts && i < args; ++i) {
		DefaultIncomes[i] = LuaToNumber(l, i + 1);
	}
	return 0;
}

/**
**  Define default action for the resources.
**
**  @param l  Lua state.
*/
static int CclDefineDefaultActions(lua_State *l)
{
	for (unsigned int i = 0; i < MaxCosts; ++i) {
		DefaultActions[i].clear();
	}
	const unsigned int args = lua_gettop(l);
	for (unsigned int i = 0; i < MaxCosts && i < args; ++i) {
		DefaultActions[i] = LuaToString(l, i + 1);
	}
	return 0;
}

/**
**  Define default names for the resources.
**
**  @param l  Lua state.
*/
static int CclDefineDefaultResourceNames(lua_State *l)
{
	for (unsigned int i = 0; i < MaxCosts; ++i) {
		DefaultResourceNames[i].clear();
	}
	const unsigned int args = lua_gettop(l);
	for (unsigned int i = 0; i < MaxCosts && i < args; ++i) {
		DefaultResourceNames[i] = LuaToString(l, i + 1);
	}
	return 0;
}

/**
**  Define default names for the resources.
**
**  @param l  Lua state.
*/
static int CclDefineDefaultResourceAmounts(lua_State *l)
{
	const unsigned int args = lua_gettop(l);

	if (args & 1) {
		LuaError(l, "incorrect argument");
	}
	for (unsigned int j = 0; j < args; ++j) {
		const std::string_view resource = LuaToString(l, j + 1);
		const int resId = GetResourceIdByName(l, resource);

		++j;
		DefaultResourceAmounts[resId] = LuaToNumber(l, j + 1);
	}
	return 0;
}

/**
**  Define max amounts for the resources.
**
**  @param l  Lua state.
*/
static int CclDefineDefaultResourceMaxAmounts(lua_State *l)
{
	const int args = std::min<int>(lua_gettop(l), MaxCosts);

	for (int i = 0; i < args; ++i) {
		DefaultResourceMaxAmounts[i] = LuaToNumber(l, i + 1);
	}
	for (int i = args; i < MaxCosts; ++i) {
		DefaultResourceMaxAmounts[i] = -1;
	}
	return 0;
}

/**
**  Affect UseHPForXp.
**
**  @param l  Lua state.
**
**  @return 0.
*/
static int ScriptSetUseHPForXp(lua_State *l)
{
	LuaCheckArgs(l, 1);
	UseHPForXp = LuaToBoolean(l, 1);
	return 0;
}

/**
** <b>Description</b>
**
**  Set the local player name
**
**  @param l  Lua state.
**
** Example:
**
** <div class="example"><code><strong>SetLocalPlayerName</strong>("Stormreaver Clan")</code></div>
*/
static int CclSetLocalPlayerName(lua_State *l)
{
	LuaCheckArgs(l, 1);
	Parameters::Instance.LocalPlayerName = LuaToString(l, 1);
	return 0;
}

/**
** <b>Description</b>
**
**  Get the local player name
**
**  @param l  Lua state.
**
** Example:
**
** <div class="example"><code><strong>GetLocalPlayerName</strong>()</code></div>
*/
static int CclGetLocalPlayerName(lua_State *l)
{
	LuaCheckArgs(l, 0);
	lua_pushstring(l, Parameters::Instance.LocalPlayerName.c_str());
	return 1;
}

/**
** <b>Description</b>
**
**  Get Stratagus Version
**
** Example:
**
** <div class="example"><code>version = <strong>GetStratagusVersion</strong>()
**		print(version)</code></div>
*/
static int CclGetStratagusVersion(lua_State *l)
{
	LuaCheckArgs(l, 0);
	lua_pushstring(l, VERSION);
	return 1;
}

/**
** <b>Description</b>
**
**  Get Stratagus Homepage
**
** Example:
**
** <div class="example"><code>url = <strong>GetStratagusHomepage</strong>()
**	print(url)</code></div>
*/
static int CclGetStratagusHomepage(lua_State *l)
{
	LuaCheckArgs(l, 0);
	lua_pushstring(l, HOMEPAGE);
	return 1;
}

static int CclSetMenuRace(lua_State *l)
{
	LuaCheckArgs(l, 1);
	MenuRace = LuaToString(l, 1);
	return 0;
}

/**
**  Load the SavedGameInfo Header
**
**  @param l  Lua state.
*/
static int CclSavedGameInfo(lua_State *l)
{
	LuaCheckArgs(l, 1);
	if (!lua_istable(l, 1)) {
		LuaError(l, "incorrect argument");
	}

	for (lua_pushnil(l); lua_next(l, 1); lua_pop(l, 1)) {
		const std::string_view value = LuaToString(l, -2);

		if (value == "SaveFile") {
			std::string_view filename = LuaToString(l, -1);

			if (std::size(CurrentMapPath) <= filename.size()) {
				LuaError(l, "SaveFile too long");
			}
			ranges::copy(filename, CurrentMapPath);
			if (LuaLoadFile(fs::path(StratagusLibPath) / filename) == -1) {
				ErrorPrint("Load failed: '%s'\n", filename.data());
			}
		} else if (value == "SyncHash") {
			SyncHash = LuaToNumber(l, -1);
		} else if (value == "SyncRandSeed") {
			SyncRandSeed = LuaToNumber(l, -1);
		} else {
			LuaError(l, "Unsupported tag: %s", value.data());
		}
	}
	return 0;
}

void LuaRegisterModules()
{
#ifdef STRATAGUS_PAX
#define PAX_REG(label, fn) fn()
#else
#define PAX_REG(label, fn) fn()
#endif
	lua_register(Lua, "SetGameName", CclSetGameName);
	lua_register(Lua, "SetFullGameName", CclSetFullGameName);

	lua_register(Lua, "SetGodMode", CclSetGodMode);
	lua_register(Lua, "GetGodMode", CclGetGodMode);

	lua_register(Lua, "SetSpeedResourcesHarvest", CclSetSpeedResourcesHarvest);
	lua_register(Lua, "SetSpeedResourcesReturn", CclSetSpeedResourcesReturn);
	lua_register(Lua, "SetSpeedBuild", CclSetSpeedBuild);
	lua_register(Lua, "GetSpeedBuild", CclGetSpeedBuild);
	lua_register(Lua, "SetSpeedTrain", CclSetSpeedTrain);
	lua_register(Lua, "GetSpeedTrain", CclGetSpeedTrain);
	lua_register(Lua, "SetSpeedUpgrade", CclSetSpeedUpgrade);
	lua_register(Lua, "SetSpeedResearch", CclSetSpeedResearch);
	lua_register(Lua, "SetSpeeds", CclSetSpeeds);

	lua_register(Lua, "DefineDefaultIncomes", CclDefineDefaultIncomes);
	lua_register(Lua, "DefineDefaultActions", CclDefineDefaultActions);
	lua_register(Lua, "DefineDefaultResourceNames", CclDefineDefaultResourceNames);
	lua_register(Lua, "DefineDefaultResourceAmounts", CclDefineDefaultResourceAmounts);
	lua_register(Lua, "DefineDefaultResourceMaxAmounts", CclDefineDefaultResourceMaxAmounts);

	lua_register(Lua, "SetUseHPForXp", ScriptSetUseHPForXp);
	lua_register(Lua, "SetLocalPlayerName", CclSetLocalPlayerName);
	lua_register(Lua, "GetLocalPlayerName", CclGetLocalPlayerName);

	lua_register(Lua, "SetMenuRace", CclSetMenuRace);

	lua_register(Lua, "GetStratagusVersion", CclGetStratagusVersion);
	lua_register(Lua, "GetStratagusHomepage", CclGetStratagusHomepage);

	lua_register(Lua, "SavedGameInfo", CclSavedGameInfo);

#ifdef STRATAGUS_PAX
	/*
	 * The PAX first-map bootstrap only needs the Lua API used by the
	 * generated Stargus data.  Register these modules explicitly and avoid
	 * constructing the desktop/editor/network module graph on the small
	 * freestanding Lua runtime.
	 */
	pax_stratagus_debug("LuaRegisterModules: Animation");
	AnimationCclRegister();
	pax_stratagus_debug("LuaRegisterModules: Ai");
	AiCclRegister();
	pax_stratagus_debug("LuaRegisterModules: Script");
	ScriptRegister();
	pax_stratagus_debug("LuaRegisterModules: Player");
	PlayerCclRegister();
	pax_stratagus_debug("LuaRegisterModules: Map");
	MapCclRegister();
	pax_stratagus_debug("LuaRegisterModules: UnitType");
	UnitTypeCclRegister();
	pax_stratagus_debug("LuaRegisterModules: Unit");
	UnitCclRegister();
	// DefineAllow supplies the original per-unit production permissions.
	UpgradesCclRegister();
	ConstructionCclRegister();
	DependenciesCclRegister();
	// Native saves serialize group/selection/replay state as Lua calls.
	GroupCclRegister();
	SelectionCclRegister();
	ReplayCclRegister();
	pax_stratagus_debug("LuaRegisterModules: Trigger");
	TriggerCclRegister();
	pax_stratagus_debug("LuaRegisterModules: UserInterface");
	UserInterfaceCclRegister();
	MissileCclRegister();
#ifdef PAX_STRATAGUS_WARCRAFT
	// Stock Warcraft definitions use spells, decorations and video preferences.
	SpellCclRegister();
	DecorationCclRegister();
	PathfinderCclRegister();
	SoundCclRegister();
	VideoCclRegister();
	// The single-player adapter still exposes network-state queries used by
	// stock Warcraft campaign setup and command/menu scripts.
	NetworkCclRegister();
#endif
	pax_stratagus_debug("LuaRegisterModules: minimal set complete");
	return;
#else
	PAX_REG("Ai", AiCclRegister);
	PAX_REG("Animation", AnimationCclRegister);
	PAX_REG("Construction", ConstructionCclRegister);
	PAX_REG("Decoration", DecorationCclRegister);
	PAX_REG("Dependencies", DependenciesCclRegister);
	PAX_REG("Editor", EditorCclRegister);
	PAX_REG("Group", GroupCclRegister);
	PAX_REG("Map", MapCclRegister);
	PAX_REG("Missile", MissileCclRegister);
	PAX_REG("Network", NetworkCclRegister);
	PAX_REG("Pathfinder", PathfinderCclRegister);
	PAX_REG("Player", PlayerCclRegister);
	PAX_REG("Replay", ReplayCclRegister);
	PAX_REG("Script", ScriptRegister);
	PAX_REG("Selection", SelectionCclRegister);
	#ifdef STRATAGUS_PAX
	/* Sound is disabled for the silent PAX bootstrap. */
	#else
	PAX_REG("Sound", SoundCclRegister);
	#endif
	PAX_REG("Spell", SpellCclRegister);
	PAX_REG("Trigger", TriggerCclRegister);
	PAX_REG("Unit", UnitCclRegister);
	PAX_REG("UnitType", UnitTypeCclRegister);
#ifdef STRATAGUS_PAX
	// Stargus' first-map bootstrap only needs the unit-type API above.  The
	// legacy upgrade/editor tail overruns the PAX Lua/allocator boundary on
	// ESP32-P4 before it can contribute anything to map loading.
	return;
#else
	PAX_REG("Upgrades", UpgradesCclRegister);
#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LuaRegisterModules: UserInterface skipped");
	#else
	PAX_REG("UserInterface", UserInterfaceCclRegister);
	#endif
	#ifdef STRATAGUS_PAX
	// SetVideoSyncSpeed is optional on the fixed-refresh PAX canvas; avoid
	// the legacy Lua registration path that overruns the relocated Lua stack.
	pax_stratagus_debug("LuaRegisterModules: Video skipped");
	return;
	#else
	PAX_REG("Video", VideoCclRegister);
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LuaRegisterModules: OnlineService skipped");
	#else
	PAX_REG("OnlineService", OnlineServiceCclRegister);
	#endif
#undef PAX_REG
#endif
}


//@}
