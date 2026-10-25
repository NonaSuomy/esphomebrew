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
/**@name script.cpp - The configuration language. */
//
//      (c) Copyright 1998-2007 by Lutz Sammer, Jimmy Salmon and Joris Dauphin.
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

#include "script.h"
#ifdef STRATAGUS_PAX
#include "pax_debug.h"
#include "pax_menu.h"
#endif

#include "animation/animation_setplayervar.h"
#include "font.h"
#include "game.h"
#include "iolib.h"
#include "parameters.h"
#include "stratagus.h"
#include "translate.h"
#include "trigger.h"
#include "ui.h"
#include "unit.h"

#include <optional>
#include <set>
#include <signal.h>
#include <variant>

#ifdef USE_WIN32
#include <windows.h>
#include <shlobj.h>
#endif

/*----------------------------------------------------------------------------
--  Variables
----------------------------------------------------------------------------*/

lua_State *Lua;                       /// Structure to work with lua files.

bool CclInConfigFile;                  /// True while config file parsing

std::unique_ptr<INumberDesc> Damage; /// Damage calculation for missile.

static int NumberCounter = 0; /// Counter for lua function.
static int StringCounter = 0; /// Counter for lua function.

/// Useful for getComponent.
using UStrInt = std::variant<int, const char *>;

/// Get component for unit variable.
extern UStrInt GetComponent(const CUnit &unit, int index, EnumVariable e, int t);
/// Get component for unit type variable.
extern UStrInt GetComponent(const CUnitType &type, int index, EnumVariable e, int t);

/*----------------------------------------------------------------------------
--  Functions
----------------------------------------------------------------------------*/
/**
**	original lua_isstring() returns true for either a string or a number
**	this do strict checking for strings only
**/
int lua_isstring_strict(lua_State *luaStack, int idx) {
	return lua_type(luaStack, idx) == LUA_TSTRING;
}

/**
**  FIXME: docu
*/
static void lstop(lua_State *l, lua_Debug *ar)
{
	(void)ar;  // unused arg.
	lua_sethook(l, nullptr, 0, 0);
	luaL_error(l, "interrupted!");
}

/**
**  FIXME: docu
*/
static void laction(int i)
{
	// if another SIGINT happens before lstop,
	// terminate process (default action)
	signal(i, SIG_DFL);
	lua_sethook(Lua, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}

/**
**  Check error status, and print error message and exit
**  if status is different of 0.
**
**  @param status       status of the last lua call. (0: success)
**  @param exitOnError  exit the program on error
**
**  @return             0 in success, else exit.
*/
static int report(int status, bool exitOnError)
{
	if (status) {
		const char *msg = lua_tostring(Lua, -1);
		if (msg == nullptr) {
			msg = "(error with no message)";
		}
		#ifdef STRATAGUS_PAX
		char pax_error[256];
		snprintf(pax_error, sizeof(pax_error), "Lua error: %s", msg);
		pax_stratagus_debug(pax_error);
		#endif
		ErrorPrint("%s\n", msg);
		if (exitOnError) {
			::exit(1);
		} else {
			lua_pushstring(Lua, msg);
			lua_setglobal(Lua, "__last_error__");
		}
		lua_pop(Lua, 1);
	}
	return status;
}

static int luatraceback(lua_State *L)
{
	lua_getglobal(L, "debug");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 1;
	}
	lua_pushliteral(L, "traceback");
	lua_gettable(L, -2);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return 1;
	}
	lua_pushvalue(L, 1);  // pass error message
	lua_pushnumber(L, 2);  // skip this function and traceback
	lua_call(L, 2, 1);  // call debug.traceback
	return 1;
}

/**
**  Call a lua function
**
**  @param narg         Number of arguments
**  @param clear        Clear the return value(s)
**  @param exitOnError  Exit the program when an error occurs
**
**  @return             0 in success, else exit.
*/
int LuaCall(int narg, int clear, bool exitOnError)
{
	const int base = lua_gettop(Lua) - narg;  // function index
	return LuaCall(Lua, narg, clear ? 0 : LUA_MULTRET, base, exitOnError);
}

/**
**  Call a lua function
**
**  @param L            Pointer to Lua state
**  @param narg         Number of arguments
**  @param nresults     Number of return values
**  @param base         Stack index of the function to call
**  @param exitOnError  Exit the program when an error occurs
**
**  @return             0 in success, else exit.
*/
int LuaCall(lua_State *L, int narg, int nresults, int base, bool exitOnError)
{
#if 0
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	lua_call(L, 0, 1);
	const char *str = lua_tostring(L, -1);
	lua_pop(L, 2);
	ErrorPrint("\n===============\n%s\n\n", str);
#endif

	lua_pushcfunction(L, luatraceback);  // push traceback function
	lua_insert(L, base);  // put it under chunk and args
	signal(SIGINT, laction);
	const int status = lua_pcall(L, narg, nresults, base);
	signal(SIGINT, SIG_DFL);
	lua_remove(L, base);  // remove traceback function
	#ifdef STRATAGUS_PAX
	if (status) {
		pax_stratagus_debug("Lua pcall returned error");
	}
	#endif

	return report(status, exitOnError);
}

/**
**  Get the (uncompressed) content of the file into a string
*/
#ifdef STRATAGUS_PAX
#ifdef STRATAGUS_PAX_LUA_PROBE
static constexpr const char PaxBootstrapScript[] = R"pax(
SetGameName("sc")
SetFullGameName("Stargus")
preferences = {RapidStratagusIDE = false}
SetTileSize(8, 8)
DefineDefaultIncomes(0, 100, 100, 100, 100, 100, 100)
DefineDefaultActions("stop", "harvest", "harvest", "unused", "unused", "unused", "unused")
DefineDefaultResourceNames("time", "minerals", "gas", "unused", "unused", "unused", "unused")
DefineDefaultResourceAmounts("minerals", 100000, "gas", 50000)
DefineDefaultResourceMaxAmounts(-1, -1, -1, -1, -1, -1, -1)
DefinePlayerColorIndex(8, 8)
local pax_player_color = {{44, 180, 148}, {32, 144, 112}, {32, 144, 112}, {32, 144, 112}, {16, 84, 60}, {16, 84, 60}, {16, 84, 60}, {0, 40, 0}}
local pax_player_colors = {}
for i = 1,15 do
  table.insert(pax_player_colors, "pax" .. i)
  table.insert(pax_player_colors, pax_player_color)
end
DefinePlayerColors(pax_player_colors)
DefineAnimations("animations-dummy-still", {Still = {"frame 0", "wait 125"}})
function MakeSound(...) end
function MakeSoundGroup(...) end
Load("scripts/fonts.lua")
Load("scripts/sc.lua")
Load("scripts/icons.lua")
Load("scripts/widgets.lua")
Load("scripts/gameui.lua")
Load("luagen/images/image_347_neutral_min01.lua")
Load("luagen/images/image_344_neutral_geyser.lua")
)pax";
#else
static constexpr const char PaxBootstrapScript[] = R"pax(
ComputerReactionRangeFactor = 1.5
PersonReactionRangeFactor = 0.8
SetSelectionStyle("ellipse", 0.589)
function AiPassive() end
DefineAi("ai-passive", "*", "ai-passive", AiPassive)
function SinglePlayerTriggers()
  AddTrigger(function() return GetPlayerData(GetThisPlayer(), "TotalNumUnits") == 0 end,
             function() return ActionDefeat() end)
  AddTrigger(function() return GetNumOpponents(GetThisPlayer()) == 0 end,
             function() return ActionVictory() end)
end
SetGameName("sc")
SetFullGameName("Stargus")
preferences = {RapidStratagusIDE = false}
SetTileSize(8, 8)
-- Keep the original game's player caps; zero-initialized limits disable all
-- production even when the reduced first-mission unit definitions exist.
SetAllPlayersUnitLimit(400)
SetAllPlayersBuildingLimit(400)
SetAllPlayersTotalUnitLimit(1000)
DefineDefaultIncomes(0, 100, 100, 100, 100, 100, 100)
DefineDefaultActions("stop", "harvest", "harvest", "unused", "unused", "unused", "unused")
DefineDefaultResourceNames("time", "minerals", "gas", "unused", "unused", "unused", "unused")
DefineDefaultResourceAmounts("minerals", 100000, "gas", 50000)
DefineDefaultResourceMaxAmounts(-1, -1, -1, -1, -1, -1)
DefinePlayerColorIndex(8, 8)
local pax_player_color = {{44, 180, 148}, {32, 144, 112}, {32, 144, 112}, {32, 144, 112}, {16, 84, 60}, {16, 84, 60}, {16, 84, 60}, {0, 40, 0}}
local pax_player_colors = {}
for i = 1,15 do
  table.insert(pax_player_colors, "pax" .. i)
  table.insert(pax_player_colors, pax_player_color)
end
DefinePlayerColors(pax_player_colors)
DefineAnimations("animations-dummy-still", {Still = {"frame 0", "wait 125"}})
function MakeSound(...) end
function MakeSoundGroup(...) end
Load("scripts/fonts.lua")
Load("scripts/sc.lua")
Load("scripts/icons.lua")
-- ui.lua loads widgets/gameui itself. Do not initialize those twice: the
-- temporary Lua tables and eagerly loaded graphics matter on the P4 heap.
Load("scripts/ui.lua")
UI.MessageFont = Fonts["game"]
UI.MessageScrollSpeed = 5
UI.Minimap.WithTerrain = true
function RunGameMenu() PaxGameMenu() end
for _,name in ipairs({"point", "glass", "cross", "scroll", "green-hair", "yellow-hair", "red-hair",
                      "arrow-e", "arrow-ne", "arrow-n", "arrow-nw", "arrow-w", "arrow-sw", "arrow-s", "arrow-se"}) do
  DefineCursor({Name="cursor-" .. name, Race="any", File="ui/cursors/pax.png",
                HotSpot={15,15}, Size={32,32}})
end
Load("luagen/images/image_347_neutral_min01.lua")
Load("luagen/images/image_349_neutral_min02.lua")
Load("luagen/images/image_351_neutral_min03.lua")
Load("luagen/images/image_344_neutral_geyser.lua")
Load("luagen/images/image_54_zerg_zergling.lua")
Load("luagen/images/image_256_terran_vulture.lua")
Load("luagen/images/image_239_terran_marine.lua")
Load("luagen/images/image_247_terran_scv.lua")
Load("luagen/images/image_278_terran_depot.lua")
Load("luagen/images/image_275_terran_control.lua")
Load("luagen/images/image_266_terran_tbarrack.lua")
Load("luagen/images/image_325_terran_tbldlrg.lua")
Load("luagen/images/image_327_terran_tbldmed.lua")
Load("luagen/portrait/portrait-tadvisor.lua")
Load("luagen/portrait/portrait-zzergl.lua")
Load("luagen/portrait/portrait-ujim.lua")
Load("luagen/portrait/portrait-tmarine.lua")
Load("luagen/portrait/portrait-tscv.lua")
-- Keep the first-mission unit definitions in the PAX image.  The generated
-- desktop Lua uses math.ceil/floor on fractional factors; that path emits
-- unsupported double-precision division on the ESP32-P4 freestanding target.
-- These are the same generated definitions with those constant expressions
-- folded ahead of time.
DefineUnitType("unit-resource-vespene-geyser", {Name="Vespene Geyser",Image=image_344_neutral_geyser,
  Animations="animations-dummy-still",Icon="icon-terran-command-center",NumDirections=1,
  HitPoints=34465,TileSize={15,7},BoxSize={127,63},SightRange=36,
  ComputerReactionRange=54,PersonReactionRange=28,Type="land",Building=true,LandUnit=true,
  VisibleUnderFog=true,GivesResource="gas",CanHarvest=true})
DefineUnitType("unit-resource-mineral-field", {Name = "Mineral Field"
, Image = image_347_neutral_min01
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center"
, Animations = "animations-dummy-still"
, Portrait = portrait_tadvisor
, HitPoints = 34465
, TileSize = {7, 3}
, BoxSize = {63, 31}
, SightRange = 36
, ComputerReactionRange = 54
, PersonReactionRange = 28
, NumDirections = image_347_neutral_min01_NumDirections
, AirUnit = false, Type = "land", Building = true, organic = false
, LandUnit = true, Costs = {"time", 0, "minerals", 1, "gas", 1}
, PersonalSpace = {1, 1}
, Sounds = {"selected", "unit-resource-mineral-field-sound-what"}})
DefineUnitType("unit-resource-mineral-field-type-2", {Name = "Mineral Field"
, Image = image_349_neutral_min02
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center"
, Animations = "animations-dummy-still"
, Portrait = portrait_tadvisor
, HitPoints = 34465, TileSize = {7, 3}, BoxSize = {63, 31}
, SightRange = 36, ComputerReactionRange = 54, PersonReactionRange = 28
, NumDirections = image_349_neutral_min02_NumDirections
, AirUnit = false, Type = "land", Building = true, organic = false
, LandUnit = true, Costs = {"time", 0, "minerals", 1, "gas", 1}
, PersonalSpace = {1, 1}
, Sounds = {"selected", "unit-resource-mineral-field-type-2-sound-what"}})
DefineUnitType("unit-resource-mineral-field-type-3", {Name = "Mineral Field"
, Image = image_351_neutral_min03
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center"
, Animations = "animations-dummy-still"
, Portrait = portrait_tadvisor
, HitPoints = 34465, TileSize = {7, 3}, BoxSize = {63, 31}
, SightRange = 36, ComputerReactionRange = 54, PersonReactionRange = 28
, NumDirections = image_351_neutral_min03_NumDirections
, AirUnit = false, Type = "land", Building = true, organic = false
, LandUnit = true, Costs = {"time", 0, "minerals", 1, "gas", 1}
, PersonalSpace = {1, 1}
, Sounds = {"selected", "unit-resource-mineral-field-type-3-sound-what"}})
DefineUnitType("unit-zerg-zergling", {Name = "Zerg Zergling"
, Image = image_54_zerg_zergling
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center", Animations = "animations-dummy-still"
, Portrait = portrait_zzergl, HitPoints = 35, TileSize = {2, 2}, BoxSize = {15, 15}
, SightRange = 20, ComputerReactionRange = 30, PersonReactionRange = 16
, NumDirections = 1
, AirUnit = false, Type = "land", Building = false, organic = true, LandUnit = true
, Costs = {"time", 105, "minerals", 50, "gas", 0}, PersonalSpace = {1, 1}
, Sounds = {"ready", "unit-zerg-zergling-sound-ready", "acknowledge", "unit-zerg-zergling-sound-yes", "selected", "unit-zerg-zergling-sound-selected"}})
DefineUnitType("unit-hero-jim-raynor-vulture", {Name = "Jim Raynor"
, Image = image_256_terran_vulture
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center", Animations = "animations-dummy-still"
, Portrait = portrait_ujim, HitPoints = 300, TileSize = {4, 4}, BoxSize = {31, 31}
, SightRange = 32, ComputerReactionRange = 48, PersonReactionRange = 25
, NumDirections = image_256_terran_vulture_NumDirections
, AirUnit = false, Type = "land", Building = false, organic = false, LandUnit = true
, Costs = {"time", 225, "minerals", 150, "gas", 0}, PersonalSpace = {1, 1}
, Sounds = {"acknowledge", "unit-hero-jim-raynor-vulture-sound-yes", "selected", "unit-hero-jim-raynor-vulture-sound-selected"}})
DefineUnitType("unit-terran-marine", {Name = "Terran Marine"
, Image = image_239_terran_marine
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center", Animations = "animations-dummy-still"
, Portrait = portrait_tmarine, HitPoints = 40, TileSize = {2, 2}, BoxSize = {16, 19}
, SightRange = 28, ComputerReactionRange = 42, PersonReactionRange = 22
, NumDirections = 1
, AirUnit = false, Type = "land", Building = false, organic = true, LandUnit = true
, Costs = {"time", 90, "minerals", 50, "gas", 0}, PersonalSpace = {1, 1}
, Sounds = {"ready", "unit-terran-marine-sound-ready", "acknowledge", "unit-terran-marine-sound-yes", "selected", "unit-terran-marine-sound-selected"}})
DefineUnitType("unit-terran-scv", {Name = "Terran SCV"
, Image = image_247_terran_scv
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center", Animations = "animations-dummy-still"
, Portrait = portrait_tscv, HitPoints = 60, TileSize = {3, 3}, BoxSize = {22, 22}
, SightRange = 28, ComputerReactionRange = 42, PersonReactionRange = 22
, NumDirections = image_247_terran_scv_NumDirections
, AirUnit = false, Type = "land", Building = false, organic = true, LandUnit = true
, Costs = {"time", 75, "minerals", 50, "gas", 0}, PersonalSpace = {1, 1}
, Sounds = {"ready", "unit-terran-scv-sound-ready", "acknowledge", "unit-terran-scv-sound-yes", "selected", "unit-terran-scv-sound-selected"}})
DefineUnitType("unit-terran-supply-depot", {Name = "Terran Supply Depot"
, Image = image_278_terran_depot
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center", Animations = "animations-dummy-still"
, Portrait = portrait_tadvisor, HitPoints = 500, TileSize = {9, 6}, BoxSize = {76, 48}
, SightRange = 32, ComputerReactionRange = 48, PersonReactionRange = 25
, NumDirections = image_278_terran_depot_NumDirections
, AirUnit = false, Type = "land", Building = true, organic = false, LandUnit = true
, Costs = {"time", 150, "minerals", 100, "gas", 0}, PersonalSpace = {1, 1}
, Sounds = {"selected", "unit-terran-supply-depot-sound-what"}})
DefineUnitType("unit-terran-command-center", {Name = "Terran Command Center"
, Image = image_275_terran_control
, Shadow = {"offset", {-7, -7}, "scale", 1}
, Icon = "icon-terran-command-center", Animations = "animations-dummy-still"
, Portrait = portrait_tadvisor, HitPoints = 1500, TileSize = {14, 10}, BoxSize = {116, 82}
, SightRange = 40, ComputerReactionRange = 60, PersonReactionRange = 32
, NumDirections = image_275_terran_control_NumDirections
, AirUnit = false, Type = "land", Building = true, organic = false, LandUnit = true
, Costs = {"time", 450, "minerals", 400, "gas", 0}, PersonalSpace = {1, 1}
, Sounds = {"selected", "unit-terran-command-center-sound-what"}})
DefineUnitType("unit-terran-barracks", {Name="Terran Barracks", Image=image_266_terran_tbarrack,
  Icon="icon-terran-barracks", Animations="animations-dummy-still", Portrait=portrait_tadvisor,
  HitPoints=1000, TileSize={13,9}, BoxSize={104,72}, SightRange=32,
  ComputerReactionRange=48, PersonReactionRange=25, NumDirections=1,
  AirUnit=false, Type="land", Building=true, organic=false, LandUnit=true,
  Costs={"time",300,"minerals",150,"gas",0}, PersonalSpace={1,1}})
-- Native Terran construction needs a non-null construction definition even
-- before the building's first action tick. Use the original staged artwork.
for _,spec in ipairs({
  {"unit-terran-command-center", image_325_terran_tbldlrg_var},
  {"unit-terran-supply-depot", image_327_terran_tbldmed_var},
  {"unit-terran-barracks", image_325_terran_tbldlrg_var}}) do
  local construction="construction-pax-" .. spec[1]
  DefineConstruction(construction, {Files=spec[2], Constructions={
    {Percent=0,File="construction",Frame=0}, {Percent=20,File="construction",Frame=1},
    {Percent=40,File="construction",Frame=2}, {Percent=60,File="main",Frame=1}}})
  DefineUnitType(spec[1], {Construction=construction, BuilderOutside=true,
    Flip=false, DrawLevel=50, Armor=20, VisibleUnderFog=true,
    RepairHp=4, RepairCosts={"minerals",1,"gas",0}, Missile="missile-none"})
end
-- The compact sheets use five stored headings plus native mirroring.
local marine_move = {}
for _,frame in ipairs({5,10,15,20,25,30,35,0,40}) do
  for _,op in ipairs({"unbreakable begin", "frame " .. frame, "move 4", "wait 1", "unbreakable end", "wait 1"}) do
    table.insert(marine_move, op)
  end
end
DefineAnimations("animations-pax-marine", {
  Still={"frame 0", "wait 30"}, Move=marine_move,
  Attack={"unbreakable begin", "frame 45", "sound terran-marine-attack", "attack", "wait 2", "frame 50", "wait 2", "unbreakable end", "wait 1"},
  Death={"unbreakable begin", "frame 0", "wait 8", "unbreakable end", "wait 1"}})
DefineAnimations("animations-pax-zergling", {
  Still={"frame 0", "wait 30"},
  Move={"unbreakable begin", "frame 5", "move 4", "wait 1", "frame 10", "move 4", "unbreakable end", "wait 1",
        "unbreakable begin", "frame 15", "move 4", "wait 1", "frame 20", "move 4", "unbreakable end", "wait 1"},
  Attack={"unbreakable begin", "frame 25", "sound zerg-zergling-attack", "attack", "wait 2", "frame 30", "wait 2", "unbreakable end", "wait 1"},
  Death={"unbreakable begin", "frame 0", "wait 8", "unbreakable end", "wait 1"}})
DefineAnimations("animations-pax-vehicle", {
  Still={"frame 0", "wait 30"},
  Move={"unbreakable begin", "frame 0", "move 4", "wait 1", "move 4", "unbreakable end", "wait 1"},
  Attack={"unbreakable begin", "sound terran-vulture-attack", "attack", "wait 3", "unbreakable end", "wait 1"},
  Death={"unbreakable begin", "frame 0", "wait 8", "unbreakable end", "wait 1"}})
DefineMissileType("missile-none", {Size={32,32}, DrawLevel=50, Class="missile-class-none", Sleep=1, Speed=16, Range=1})
for _,id in ipairs({"unit-terran-marine", "unit-zerg-zergling", "unit-terran-scv", "unit-hero-jim-raynor-vulture"}) do
  DefineUnitType(id, {Animations="animations-pax-vehicle", DrawLevel=40,
    RightMouseAction="attack", SelectableByRectangle=true, CanAttack=true,
    CanTargetLand=true, Missile="missile-none", BasicDamage=6, PiercingDamage=2,
    MaxAttackRange=16, Priority=60, Demand=1})
end
DefineUnitType("unit-terran-marine", {Animations="animations-pax-marine", NumDirections=8, Icon="icon-terran-marine", Armor=2})
DefineUnitType("unit-zerg-zergling", {Animations="animations-pax-zergling", NumDirections=8, Icon="icon-zerg-zergling", MaxAttackRange=1, Armor=2, PiercingDamage=3})
DefineUnitType("unit-terran-scv", {NumDirections=8, Icon="icon-terran-scv"})
DefineUnitType("unit-hero-jim-raynor-vulture", {NumDirections=8, Icon="icon-terran-jim-raynor-vulture"})
DefineUnitType("unit-terran-command-center", {Supply=10, CanStore={"minerals","gas"}})
DefineAnimations("animations-pax-command-center", {
  Still={"frame 0","wait 30"}, Train={"frame 0","wait 30"}})
DefineUnitType("unit-terran-command-center", {Animations="animations-pax-command-center"})
DefineUnitType("unit-terran-supply-depot", {Supply=8, Icon="icon-terran-supply-depot"})
DefineAnimations("animations-pax-barracks", {
  Still={"frame 0","wait 30"}, Train={"frame 7","wait 4","frame 8","wait 4"}})
DefineUnitType("unit-terran-barracks", {Animations="animations-pax-barracks"})
DefineUnitType("unit-terran-scv", {RightMouseAction="harvest", CanGatherResources={
  {"resource-id","minerals","harvest-from-outside","resource-capacity",8,
   "wait-at-resource",150,"wait-at-depot",50}}})
DefineAnimations("animations-pax-scv", {
  Still={"frame 0","wait 30"},
  Move={"unbreakable begin","frame 0","move 4","wait 1","move 4","unbreakable end","wait 1"},
  Harvest_minerals={"unbreakable begin","sound terran-scv-attack","frame 10","wait 3","frame 5","wait 3","unbreakable end","wait 4"},
  Attack={"unbreakable begin","frame 10","sound terran-scv-attack","attack","wait 1","frame 5","unbreakable end","wait 1"},
  Build={"sound terran-scv-attack","frame 10","wait 4","frame 5","wait 4"},
  Repair={"unbreakable begin","sound terran-scv-attack","frame 10","wait 1","frame 5","unbreakable end","wait 1"}})
DefineUnitType("unit-terran-scv", {Animations="animations-pax-scv", RepairRange=1, MaxAttackRange=1,BasicDamage=3,PiercingDamage=2,Coward=true})
for _,id in ipairs({"unit-resource-mineral-field","unit-resource-mineral-field-type-2","unit-resource-mineral-field-type-3"}) do
  DefineUnitType(id,{GivesResource="minerals", CanHarvest=true, VisibleUnderFog=true,
    NeutralMinimapColor={0,228,252}})
end
DefineButton({Pos=1,Level=0,Icon="icon-terran-scv",Action="train-unit",Value="unit-terran-scv",
  Key="s",Hint="Build SCV",ForUnit={"unit-terran-command-center"}})
DefineButton({Pos=1,Level=0,Icon="icon-terran-marine",Action="train-unit",Value="unit-terran-marine",
  Key="m",Hint="Train Marine",ForUnit={"unit-terran-barracks"}})
DefineButton({Pos=6,Level=0,Icon="icon-rally-point",Action="move",
  Key="r",Hint="Set Rally Point",ForUnit={"unit-terran-command-center","unit-terran-barracks"}})
DefineButton({Pos=9,Level=0,Icon="icon-cancel",Action="cancel-train-unit",Value=-1,
  Key="c",Hint="Cancel Training",ForUnit={"unit-terran-command-center","unit-terran-barracks"}})
DefineButton({Pos=9,Level=0,Icon="icon-cancel",Action="cancel-build",
  Key="\27",Hint="Cancel Construction",ForUnit={"cancel-build"}})
SetTrainingQueue(true)
DefineDependency("unit-terran-supply-depot", {"unit-terran-command-center"})
DefineDependency("unit-terran-barracks", {"unit-terran-command-center"})
DefineDependency("unit-terran-marine", {"unit-terran-barracks"})
-- Normally supplied by terran/units.lua, which this compact bootstrap skips.
for _,id in ipairs({"unit-terran-scv", "unit-terran-marine", "unit-terran-command-center",
                     "unit-terran-supply-depot", "unit-terran-barracks"}) do
  DefineAllow(id, "AAAAAAAAAAAAAAAA")
end
local pax_units = {"unit-terran-marine", "unit-hero-jim-raynor-vulture", "unit-zerg-zergling"}
for _,button in ipairs({{1,"move","m"}, {2,"stop","s"}, {3,"attack","a"}, {5,"patrol","p"}, {6,"stand-ground","h"}}) do
  DefineButton({Pos=button[1], Level=0, Icon="icon-" .. (button[2] == "stand-ground" and "hold-position" or button[2]), Action=button[2], Key=button[3],
                Hint=button[2], ForUnit=pax_units})
end
for _,button in ipairs({{1,"move","m"}, {2,"stop","s"}, {3,"attack","a"},
                       {4,"repair","r"}, {5,"harvest","g"}, {6,"return-goods","c"}}) do
  local icon=(button[2]=="harvest" or button[2]=="return-goods") and "gather" or button[2]
  DefineButton({Pos=button[1],Level=0,Icon="icon-"..icon,Action=button[2],Key=button[3],
    Hint=button[2],ForUnit={"unit-terran-scv"}})
end
DefineButton({Pos=7,Level=0,Icon="icon-build",Action="button",Value=1,
  Key="b",Hint="Build Structure",ForUnit={"unit-terran-scv"}})
for _,spec in ipairs({{1,"command-center","c"},{2,"supply-depot","s"},{4,"barracks","b"}}) do
  DefineButton({Pos=spec[1],Level=1,Icon="icon-terran-"..spec[2],Action="build",
    Value="unit-terran-"..spec[2],Key=spec[3],Hint="Build "..spec[2],ForUnit={"unit-terran-scv"}})
end
DefineButton({Pos=9,Level=1,Icon="icon-cancel",Action="button",Value=0,
  Key="\27",Hint="Cancel",ForUnit={"unit-terran-scv"}})
DefineButton({Pos=9, Level=9, Icon="icon-cancel", Action="cancel", Key="\27", Hint="Cancel", ForUnit={"*"}})
)pax";
#endif
#endif
static std::optional<std::string> GetFileContent(const fs::path& file)
{
#ifdef STRATAGUS_PAX
	if (file.u8string().find("pax_bootstrap.lua") != std::string::npos) {
		return std::string(PaxBootstrapScript);
	}
#endif
	CFile fp;

	if (fp.open(file.string().c_str(), CL_OPEN_READ) == -1) {
		DebugPrint("Can't open file '%s'\n", file.u8string().c_str());
		ErrorPrint("Can't open file '%s': %s\n", file.u8string().c_str(), strerror(errno));
		return std::nullopt;
	}

	const int size = 10000;
	std::vector<char> buf;
	buf.resize(size);
	int location = 0;
	for (;;) {
		int read = fp.read(&buf[location], size);
		if (read != size) {
			location += read;
			break;
		}
		location += read;
		buf.resize(buf.size() + size);
	}
	fp.close();
	return std::string(&buf[0], location);
}

/**
**  Load a file and execute it
**
**  @param file  File to load and execute
**  @param nargs Number of arguments that caller has put on the stack
**
**  @return      0 for success, -1 if the file was not found, else exit.
*/
#ifdef STRATAGUS_PAX
static int LoadPaxSaveChunks(const fs::path &path, bool exitOnError)
{
	CFile file;
	if (file.open(path.string().c_str(), CL_OPEN_READ) == -1) return -1;
	// Serialized Lua globals can exceed 64 KB; grow only when needed.
	size_t capacity = 65536;
	constexpr size_t maxCapacity = 1024 * 1024;
	std::unique_ptr<char[]> chunk(new char[capacity]);
	if (!chunk) { lua_pushliteral(Lua, "No memory for save chunk"); report(LUA_ERRMEM, exitOnError); return LUA_ERRMEM; }
	lua_getglobal(Lua, "__file__");
	const int base = lua_gettop(Lua);
	lua_pushstring(Lua, path.string().c_str()); lua_setglobal(Lua, "__file__");
	size_t used = 0, lineStart = 0;
	int status = 0, count = 0;
	char block[4096];
	auto execute = [&]() {
		int result = luaL_loadbuffer(Lua, chunk.get(), used, path.string().c_str());
		if (result) report(result, exitOnError);
		else result = LuaCall(0, 1, exitOnError);
		lua_settop(Lua, base);
		lua_gc(Lua, LUA_GCCOLLECT, 0);
		used = lineStart = 0;
		return result;
	};
	while (!status && (count = file.read(block, sizeof(block))) > 0) {
		for (int i = 0; i < count && !status; ++i) {
			if (used == capacity) {
				if (capacity == maxCapacity) {
					lua_pushliteral(Lua, "Saved-game chunk exceeds 1 MB");
					status = LUA_ERRSYNTAX; report(status, exitOnError); break;
				}
				const size_t newCapacity = capacity * 2;
				std::unique_ptr<char[]> larger(new char[newCapacity]);
				if (!larger) {
					lua_pushliteral(Lua, "No memory to grow save chunk");
					status = LUA_ERRMEM; report(status, exitOnError); break;
				}
				memcpy(larger.get(), chunk.get(), used);
				chunk.swap(larger);
				capacity = newCapacity;
			}
			chunk[used++] = block[i];
			if (block[i] == '\n') {
				constexpr char marker[] = "-- PAX CHUNK\n";
				if (used - lineStart == sizeof(marker) - 1 &&
					!memcmp(chunk.get() + lineStart, marker, sizeof(marker) - 1)) {
					used = lineStart;
					status = execute();
				} else lineStart = used;
			}
		}
	}
	if (!status && used) status = execute();
	if (file.close() != 0 || count < 0) {
		lua_pushliteral(Lua, "Error reading native save");
		status = LUA_ERRFILE; report(status, exitOnError);
	}
	lua_settop(Lua, base); lua_setglobal(Lua, "__file__");
	return status;
}
#endif
int LuaLoadFile(const fs::path &file, const std::string &strArg, bool exitOnError)
{
	DebugPrint("Loading '%s'\n", file.u8string().c_str());
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug((std::string("LuaLoadFile begin: ") + file.string()).c_str());
	// The PAX bootstrap is compiled into the executable.  Do not copy it into
	// a temporary std::string: on the ESP32 allocator that copy can fault before
	// Lua even gets a chance to compile the chunk.
	const bool pax_bootstrap = file.filename() == "pax_bootstrap.lua";
	// Native saves can exceed the largest contiguous block after map cleanup.
	// Feed gzip input directly to Lua rather than growing a vector and making
	// two additional whole-file string copies.
	const bool pax_saved_game = file.filename().string().find(".sav") != std::string::npos;
	if (pax_saved_game) return LoadPaxSaveChunks(file, exitOnError);
	#else
	const bool pax_bootstrap = false;
	#endif

	const auto content = pax_bootstrap
#ifdef STRATAGUS_PAX
		|| pax_saved_game
#endif
		? std::optional<std::string>{} : GetFileContent(file);
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug(pax_bootstrap || content ? "LuaLoadFile content complete" : "LuaLoadFile content missing");
	const bool pax_trace_mineral_file = file.string().find("unit-resource-mineral-field.lua") != std::string::npos;
	if (pax_trace_mineral_file) {
		pax_stratagus_debug("LoadCcl mineral file content boundary");
	}
	#endif
	if (!pax_bootstrap
#ifdef STRATAGUS_PAX
		&& !pax_saved_game
#endif
		&& !content) {
		return -1;
	}
	if (!pax_bootstrap && file.string().rfind("stratagus.lua") != std::string::npos) {
		FileChecksums ^= fletcher32(*content);
		DebugPrint("FileChecksums after loading %s: %x\n", file.u8string().c_str(), FileChecksums);
	}
	// save the current __file__
	lua_getglobal(Lua, "__file__");

	#ifdef STRATAGUS_PAX
	int status;
	if (pax_bootstrap) {
		pax_stratagus_debug("LuaLoadFile using embedded bootstrap");
		status = luaL_loadbuffer(Lua, PaxBootstrapScript,
			sizeof(PaxBootstrapScript) - 1, "pax_bootstrap.lua");
	} else {
		std::string pax_content = *content;
#ifdef PAX_STRATAGUS_WARCRAFT
		// This build excludes the multiplayer transport and online service.
		// Do not enter the stock lobby and leave the player in an unusable UI.
		if (file.filename() == "network.lua" && file.parent_path().filename() == "menus") {
			pax_content += R"lua(
function RunMultiPlayerGameMenu()
  local menu = WarGameMenu(nil, 320, 112)
  menu:addLabel("Multiplayer is unavailable", 160, 12)
  menu:addLabel("This ESP PAX is single-player only.", 160, 35)
  menu:addLabel("Return to the menu to play.", 160, 54)
  local ok = menu:addHalfButton("~!OK", "o", 0, 84, function() menu:stop() end)
  ok:setX((menu:getWidth() - ok:getWidth()) / 2)
  menu:run()
end
)lua";
			pax_stratagus_debug("Warcraft: unsupported multiplayer entry guarded");
		}
#endif
		if (file.string().rfind("stratagus.lua") != std::string::npos) {
			const auto debug_start = pax_content.find("pcall(function() require(\"scripts/mobdebug\")");
			if (debug_start != std::string::npos) {
				const auto debug_end = pax_content.find('\n', debug_start);
				pax_content.replace(debug_start,
					debug_end == std::string::npos ? std::string::npos : debug_end - debug_start,
					"-- mobdebug disabled in PAX");
				pax_stratagus_debug("LuaLoadFile: mobdebug disabled");
			}
		}
		status = luaL_loadbuffer(Lua, pax_content.c_str(), pax_content.size(), file.string().c_str());
	}
	#else
	const int status = luaL_loadbuffer(Lua, content->c_str(), content->size(), file.string().c_str());
	#endif

	if (!status) {
		#ifdef STRATAGUS_PAX
		if (pax_trace_mineral_file) {
			pax_stratagus_debug("LoadCcl mineral file loaded boundary");
		}
		#endif
		lua_pushstring(Lua, fs::absolute(fs::path(file)).generic_u8string().c_str());
		lua_setglobal(Lua, "__file__");
		if (!strArg.empty()) {
			#ifdef STRATAGUS_PAX
			pax_stratagus_debug("LuaLoadFile call begin");
			#endif
			lua_pushstring(Lua, strArg.c_str());
			LuaCall(1, 1, exitOnError);
		} else {
			#ifdef STRATAGUS_PAX
			pax_stratagus_debug("LuaLoadFile call begin");
			if (pax_trace_mineral_file) {
				pax_stratagus_debug("LoadCcl mineral file call boundary");
			}
			#endif
			LuaCall(0, 1, exitOnError);
		}
		#ifdef STRATAGUS_PAX
		pax_stratagus_debug("LuaLoadFile call complete");
		if (pax_trace_mineral_file) {
			pax_stratagus_debug("LoadCcl mineral file call complete");
		}
		#endif
	} else {
		report(status, exitOnError);
	}
	// restore the old __file__
	lua_setglobal(Lua, "__file__");
	return status;
}

#ifdef STRATAGUS_PAX
/* Load the compiled-in Stargus bootstrap without constructing an fs::path.
 * The ESP32 PAX allocator faults while copying the synthetic /sd path, even
 * though the Lua chunk itself is valid. */
static int LuaLoadPaxBootstrap(const std::string &strArg, bool exitOnError)
{
	pax_stratagus_debug("LoadCclPax bootstrap begin");
	// This is a one-shot bootstrap; unlike normal CCL files it does not need
	// to preserve the caller's __file__ stack value.
	pax_stratagus_debug("LoadCclPax before loadbuffer");
	const int status = luaL_loadbuffer(Lua, PaxBootstrapScript,
		sizeof(PaxBootstrapScript) - 1, "pax_bootstrap.lua");
	pax_stratagus_debug("LoadCclPax after loadbuffer");
	if (!status) {
		lua_pushliteral(Lua, "/sd/roms/stargus/scripts/pax_bootstrap.lua");
		lua_setglobal(Lua, "__file__");
		pax_stratagus_debug("LoadCclPax call begin");
		if (!strArg.empty()) {
			lua_pushstring(Lua, strArg.c_str());
			LuaCall(1, 1, exitOnError);
		} else {
			LuaCall(0, 1, exitOnError);
		}
		pax_stratagus_debug("LoadCclPax call complete");
	} else {
		report(status, exitOnError);
	}
	return status;
}

void LoadCclPax()
{
	CclInConfigFile = true;
#ifdef STRATAGUS_PAX_LUA_NOOP
	pax_stratagus_debug("LoadCclPax no-op probe");
	CclInConfigFile = false;
	return;
#else
	pax_stratagus_debug("LoadCclPax loading embedded bootstrap");
	LuaLoadPaxBootstrap("", true);
	CclInConfigFile = false;
#ifndef STRATAGUS_PAX_LUA_NO_GC
	LuaGarbageCollect();
#endif
	pax_stratagus_debug("LoadCclPax complete");
#endif
}
#endif

/**
**  Save preferences
**
**  @param l  Lua state.
*/
static int CclSavePreferences(lua_State *l)
{
	LuaCheckArgs(l, 0);
	SavePreferences();
	return 0;
}

/**
**  Load a file and execute it.
**
**  @param l  Lua state.
**
**  @return   0 in success, else exit.
*/
static int CclLoad(lua_State *l)
{
	const int arg = lua_gettop(l);
	if (arg < 1 || arg > 2) {
		LuaError(l, "incorrect argument");
	}
	const fs::path filename = LibraryFileName(std::string{LuaToString(l, 1)});
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug((std::string("LoadCcl file: ") + filename.string()).c_str());
	#endif
	bool exitOnError = arg == 2 ? LuaToBoolean(l, 2) : true;
	if (LuaLoadFile(filename, "", exitOnError) == -1) {
		ErrorPrint("Load failed: '%s'\n", filename.u8string().c_str());
	}
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LoadCcl file complete");
	#endif
	return 0;
}

/**
**  Load a file into a buffer and return it.
**
**  @param l  Lua state.
**
**  @return   buffer or nil on failure
*/
static int CclLoadBuffer(lua_State *l)
{
	LuaCheckArgs(l, 1);
	const fs::path file = LibraryFileName(std::string{LuaToString(l, 1)});
	DebugPrint("Loading '%s'\n", file.u8string().c_str());

	if (auto content = GetFileContent(file)) {
		lua_pushstring(l, content->c_str());
		return 1;
	}
	return 0;
}

/**
**  Convert lua string in char*.
**  It checks also type and exit in case of error.
**
**  @note char* could be invalidated with lua garbage collector.
**
**  @param l     Lua state.
**  @param narg  Argument number.
**
**  @return      char* from lua.
*/
std::string_view LuaToString(lua_State *l, int narg)
{
	luaL_checktype(l, narg, LUA_TSTRING);
	return lua_tostring(l, narg);
}

/**
**  Convert lua number in C number.
**  It checks also type and exit in case of error.
**
**  @param l     Lua state.
**  @param narg  Argument number.
**
**  @return      C number from lua.
*/
int LuaToNumber(lua_State *l, int narg)
{
	luaL_checktype(l, narg, LUA_TNUMBER);
	return static_cast<int>(lua_tonumber(l, narg));
}

/**
**  Convert lua number in C float.
**  It checks also type and exit in case of error.
**
**  @param l     Lua state.
**  @param narg  Argument number.
**
**  @return      C number from lua.
*/
float LuaToFloat(lua_State *l, int narg)
{
	luaL_checktype(l, narg, LUA_TNUMBER);
	return static_cast<float>(lua_tonumber(l, narg));
}

/**
**  Convert lua number in C unsigned int.
**  It checks also type and exit in case of error.
**
**  @param l     Lua state.
**  @param narg  Argument number.
**
**  @return      C number from lua.
*/
unsigned int LuaToUnsignedNumber(lua_State *l, int narg)
{
	luaL_checktype(l, narg, LUA_TNUMBER);
	return static_cast<unsigned int>(lua_tonumber(l, narg));
}

/**
**  Convert lua boolean to bool.
**  It also checks type and exits in case of error.
**
**  @param l     Lua state.
**  @param narg  Argument number.
**
**  @return      1 for true, 0 for false from lua.
*/
bool LuaToBoolean(lua_State *l, int narg)
{
	luaL_checktype(l, narg, LUA_TBOOLEAN);
	return lua_toboolean(l, narg) != 0;
}

std::string_view LuaToString(lua_State *l, int index, int subIndex)
{
	luaL_checktype(l, index, LUA_TTABLE);
	lua_rawgeti(l, index, subIndex);
	std::string_view res = LuaToString(l, -1);
	lua_pop(l, 1);
	return res;
}

int LuaToNumber(lua_State *l, int index, int subIndex)
{
	luaL_checktype(l, index, LUA_TTABLE);
	lua_rawgeti(l, index, subIndex);
	const int res = LuaToNumber(l, -1);
	lua_pop(l, 1);
	return res;
}

unsigned int LuaToUnsignedNumber(lua_State *l, int index, int subIndex)
{
	luaL_checktype(l, index, LUA_TTABLE);
	lua_rawgeti(l, index, subIndex);
	const unsigned int res = LuaToUnsignedNumber(l, -1);
	lua_pop(l, 1);
	return res;
}

bool LuaToBoolean(lua_State *l, int index, int subIndex)
{
	luaL_checktype(l, index, LUA_TTABLE);
	lua_rawgeti(l, index, subIndex);
	const bool res = LuaToBoolean(l, -1);
	lua_pop(l, 1);
	return res;
}


/**
**  Perform lua garbage collection
*/
void LuaGarbageCollect()
{
#if LUA_VERSION_NUM >= 501
	#ifdef STRATAGUS_PAX
	char pax_gc_message[100];
	snprintf(pax_gc_message, sizeof(pax_gc_message), "Map Lua collect before: %d KiB", lua_gc(Lua, LUA_GCCOUNT, 0));
	pax_stratagus_debug(pax_gc_message);
	#endif
	DebugPrint("Garbage collect (before): %d\n", lua_gc(Lua, LUA_GCCOUNT, 0));
	lua_gc(Lua, LUA_GCCOLLECT, 0);
	DebugPrint("Garbage collect (after): %d\n", lua_gc(Lua, LUA_GCCOUNT, 0));
	#ifdef STRATAGUS_PAX
	snprintf(pax_gc_message, sizeof(pax_gc_message), "Map Lua collect after: %d KiB", lua_gc(Lua, LUA_GCCOUNT, 0));
	pax_stratagus_debug(pax_gc_message);
	#endif
#else
	DebugPrint("Garbage collect (before): %d/%d\n", lua_getgccount(Lua), lua_getgcthreshold(Lua));
	lua_setgcthreshold(Lua, 0);
	DebugPrint("Garbage collect (after): %d/%d\n", lua_getgccount(Lua), lua_getgcthreshold(Lua));
#endif
}

// ////////////////////

/**
**  Parse binary operation with number.
**
**  @param l       lua state.
**  @param type    type of binary operand
**  @return binop   Where to stock info (must be malloced)
*/
static std::unique_ptr<NumberDescBinOp> MakeBinOp(lua_State *l, EBinOp type)
{
	Assert(l);
	Assert(lua_istable(l, -1));
	Assert(lua_rawlen(l, -1) == 2);

	lua_rawgeti(l, -1, 1); // left
	auto left = CclParseNumberDesc(l);
	lua_rawgeti(l, -1, 2); // right
	auto right = CclParseNumberDesc(l);
	lua_pop(l, 1); // table.
	return std::make_unique<NumberDescBinOp>(type, std::move(left), std::move(right));
}

/**
**  Convert the string to the corresponding data (which is a unit).
**
**  @param l   lua state.
**  @param s   Ident.
**
**  @return    The reference of the unit.
**
**  @todo better check for error (restrict param).
*/
static CUnit **Str2UnitRef(lua_State *l, std::string_view s)
{
	CUnit **res; // Result.

	Assert(l);
	res = nullptr;
	if (s == "Attacker") {
		res = &TriggerData.Attacker;
	} else if (s == "Defender") {
		res = &TriggerData.Defender;
	} else if (s == "Active") {
		res = &TriggerData.Active;
	} else {
		LuaError(l, "Invalid unit reference '%s'\n", s.data());
	}
	Assert(res); // Must check for error.
	return res;
}

/**
**  Convert the string to the corresponding data (which is a unit type).
**
**  @param l   lua state.
**  @param s   Ident.
**
**  @return    The reference of the unit type.
**
**  @todo better check for error (restrict param).
*/
static CUnitType **Str2TypeRef(lua_State *l, std::string_view s)
{
	CUnitType **res = nullptr; // Result.

	Assert(l);
	if (s == "Type") {
		res = &TriggerData.Type;
	} else {
		LuaError(l, "Invalid type reference '%s'\n", s.data());
	}
	Assert(res); // Must check for error.
	return res;
}

/**
**  Return unit referernce definition.
**
**  @param l  lua state.
**
**  @return   unit referernce definition.
*/
std::unique_ptr<IUnitDesc> CclParseUnitDesc(lua_State *l)
{
	std::unique_ptr<IUnitDesc> res;

	if (lua_isstring(l, -1)) {
		res = std::make_unique<UnitDescRef>(Str2UnitRef(l, LuaToString(l, -1)));
	} else {
		LuaError(l, "Parse Error in ParseUnit\n");
	}
	lua_pop(l, 1);
	return res;
}

/**
**  Return unit type referernce definition.
**
**  @param l  lua state.
**
**  @return   unit type referernce definition.
*/
CUnitType **CclParseTypeDesc(lua_State *l)
{
	CUnitType **res = nullptr;

	if (lua_isstring(l, -1)) {
		res = Str2TypeRef(l, LuaToString(l, -1));
		lua_pop(l, 1);
	} else {
		LuaError(l, "Parse Error in ParseUnit\n");
	}
	return res;
}


/**
**  Add a Lua handler
**
**  @param l          lua state.
**  @param tablename  name of the lua table.
**  @param counter    Counter for the handler
**
**  @return handle of the function.
*/
static int ParseLuaFunction(lua_State *l, const char *tablename, int *counter)
{
	lua_getglobal(l, tablename);
	if (lua_isnil(l, -1)) {
		lua_pop(l, 1);
		lua_newtable(l);
		lua_setglobal(l, tablename);
		lua_getglobal(l, tablename);
	}
	lua_pushvalue(l, -2);
	lua_rawseti(l, -2, *counter);
	lua_pop(l, 1);
	return (*counter)++;
}

/**
**  Call a Lua handler
**
**  @param handler  handler of the lua function to call.
**
**  @return  lua function result.
*/
static int CallLuaNumberFunction(unsigned int handler)
{
	const int narg = lua_gettop(Lua);

	lua_getglobal(Lua, "_numberfunction_");
	lua_rawgeti(Lua, -1, handler);
	LuaCall(0, 0);
	if (lua_gettop(Lua) - narg != 2) {
		LuaError(Lua, "Function must return one value.");
	}
	const int res = LuaToNumber(Lua, -1);
	lua_pop(Lua, 2);
	return res;
}

/**
**  Call a Lua handler
**
**  @param handler  handler of the lua function to call.
**
**  @return         lua function result.
*/
static std::string CallLuaStringFunction(unsigned int handler)
{
	const int narg = lua_gettop(Lua);
	lua_getglobal(Lua, "_stringfunction_");
	lua_rawgeti(Lua, -1, handler);
	LuaCall(0, 0);
	if (lua_gettop(Lua) - narg != 2) {
		LuaError(Lua, "Function must return one value.");
	}
	std::string res = std::string{LuaToString(Lua, -1)};
	lua_pop(Lua, 2);
	return res;
}

/**
**  Return number.
**
**  @param l  lua state.
**
**  @return   number.
*/
std::unique_ptr<INumberDesc> CclParseNumberDesc(lua_State *l)
{
	std::unique_ptr<INumberDesc> res;

	if (lua_isnumber(l, -1)) {
		res = std::make_unique<NumberDescInt>(LuaToNumber(l, -1));
	} else if (lua_isfunction(l, -1)) {
		res = std::make_unique<NumberDescLuaFunction>(ParseLuaFunction(l, "_numberfunction_", &NumberCounter));
	} else if (lua_istable(l, -1)) {
		const int nargs = lua_rawlen(l, -1);
		if (nargs != 2) {
			LuaError(l, "Bad number of args in parse Number table\n");
		}
		lua_rawgeti(l, -1, 1); // key
		std::string_view key = LuaToString(l, -1);
		lua_pop(l, 1);
		lua_rawgeti(l, -1, 2); // table
		if (key == "Add") {
			res = MakeBinOp(l, EBinOp::Add);
		} else if (key == "Sub") {
			res = MakeBinOp(l, EBinOp::Sub);
		} else if (key == "Mul") {
			res = MakeBinOp(l, EBinOp::Mul);
		} else if (key == "Div") {
			res = MakeBinOp(l, EBinOp::Div);
		} else if (key == "Min") {
			res = MakeBinOp(l, EBinOp::Min);
		} else if (key == "Max") {
			res = MakeBinOp(l, EBinOp::Max);
		} else if (key == "Rand") {
			res = std::make_unique<NumberDescRand>(CclParseNumberDesc(l));
		} else if (key == "GreaterThan") {
			res = MakeBinOp(l, EBinOp::Gt);
		} else if (key == "GreaterThanOrEq") {
			res = MakeBinOp(l, EBinOp::GtEq);
		} else if (key == "LessThan") {
			res = MakeBinOp(l, EBinOp::Lt);
		} else if (key == "LessThanOrEq") {
			res = MakeBinOp(l, EBinOp::LtEq);
		} else if (key == "Equal") {
			res = MakeBinOp(l, EBinOp::Eq);
		} else if (key == "NotEqual") {
			res = MakeBinOp(l, EBinOp::NEq);
		} else if (key == "UnitVar") {
			Assert(lua_istable(l, -1));

			std::unique_ptr<IUnitDesc> unitDesc;
			int varIndex = -1;
			int loc = -1;
			EnumVariable component = EnumVariable::Value;
			for (lua_pushnil(l); lua_next(l, -2); lua_pop(l, 1)) {
				key = LuaToString(l, -2);
				if (key == "Unit") {
					unitDesc = CclParseUnitDesc(l);
					lua_pushnil(l);
				} else if (key == "Variable") {
					const std::string_view name = LuaToString(l, -1);
					varIndex = UnitTypeVar.VariableNameLookup[name];
					if (varIndex == -1) {
						LuaError(l, "Bad variable name: '%s'", name.data());
					}
				} else if (key == "Component") {
					component = Str2EnumVariable(l, LuaToString(l, -1));
				} else if (key == "Loc") {
					loc = LuaToNumber(l, -1);
					if (loc < 0 || 2 < loc) {
						LuaError(l, "Bad Loc number: '%d'", LuaToNumber(l, -1));
					}
				} else {
					LuaError(l, "Bad param %s for Unit", key.data());
				}
			}
			res = std::make_unique<NumberDescUnitStat>(std::move(unitDesc), varIndex, component, loc);
			lua_pop(l, 1); // pop the table.
		} else if (key == "TypeVar") {
			Assert(lua_istable(l, -1));

			CUnitType **type = nullptr;
			EnumVariable component = EnumVariable::Value;
			int varIndex = -1;
			int loc = -1;
			for (lua_pushnil(l); lua_next(l, -2); lua_pop(l, 1)) {
				key = LuaToString(l, -2);
				if (key == "Type") {
					type = CclParseTypeDesc(l);
					lua_pushnil(l);
				} else if (key == "Component") {
					component = Str2EnumVariable(l, LuaToString(l, -1));
				} else if (key == "Variable") {
					const std::string_view name = LuaToString(l, -1);
					varIndex = UnitTypeVar.VariableNameLookup[name];
					if (varIndex == -1) {
						LuaError(l, "Bad variable name: '%s'", name.data());
					}
				} else if (key == "Loc") {
					loc = LuaToNumber(l, -1);
					if (loc < 0 || 2 < loc) {
						LuaError(l, "Bad Loc number: '%d'", LuaToNumber(l, -1));
					}
				} else {
					LuaError(l, "Bad param %s for Unit", key.data());
				}
			}
			res = std::make_unique<NumberDescTypeStat>(type, varIndex, component, loc);
			lua_pop(l, 1); // pop the table.
		} else if (key == "VideoTextLength") {
			Assert(lua_istable(l, -1));

			std::unique_ptr<IStringDesc> string;
			CFont *font = nullptr;
			for (lua_pushnil(l); lua_next(l, -2); lua_pop(l, 1)) {
				key = LuaToString(l, -2);
				if (key == "Text") {
					string = CclParseStringDesc(l);
					lua_pushnil(l);
				} else if (key == "Font") {
					font = CFont::Get(LuaToString(l, -1));
					if (!font) {
						LuaError(l, "Bad Font name: '%s'", LuaToString(l, -1).data());
					}
				} else {
					LuaError(l, "Bad param %s for VideoTextLength", key.data());
				}
			}
			lua_pop(l, 1); // pop the table.
			res = std::make_unique<NumberDescVideoTextLength>(std::move(string), font);
		} else if (key == "StringFind") {
			Assert(lua_istable(l, -1));
			if (lua_rawlen(l, -1) != 2) {
				LuaError(l, "Bad param for StringFind");
			}
			lua_rawgeti(l, -1, 1); // left
			auto string = CclParseStringDesc(l);

			lua_rawgeti(l, -1, 2); // right
			auto c = LuaToString(l, -1)[0];
			res = std::make_unique<NumberDescStringFind>(std::move(string), c);

			lua_pop(l, 1); // pop the char

			lua_pop(l, 1); // pop the table.

		} else if (key == "NumIf") {
			if (lua_rawlen(l, -1) != 2 && lua_rawlen(l, -1) != 3) {
				LuaError(l, "Bad number of args in NumIf\n");
			}
			lua_rawgeti(l, -1, 1); // Condition.
			auto cond = CclParseNumberDesc(l);
			lua_rawgeti(l, -1, 2); // Then.
			auto trueValue = CclParseNumberDesc(l);
			std::unique_ptr<INumberDesc> falseValue;
			if (lua_rawlen(l, -1) == 3) {
				lua_rawgeti(l, -1, 3); // Else.
				falseValue = CclParseNumberDesc(l);
			}
			res = std::make_unique<NumberDescIf>(std::move(cond), std::move(trueValue), std::move(falseValue));

			lua_pop(l, 1); // table.
		} else if (key == "PlayerData") {
			if (lua_rawlen(l, -1) != 2 && lua_rawlen(l, -1) != 3) {
				LuaError(l, "Bad number of args in PlayerData\n");
			}
			lua_rawgeti(l, -1, 1); // Player.
			auto player = CclParseNumberDesc(l);
			lua_rawgeti(l, -1, 2); // DataType.
			auto dataType = CclParseStringDesc(l);
			std::unique_ptr<IStringDesc> resType;
			if (lua_rawlen(l, -1) == 3) {
				lua_rawgeti(l, -1, 3); // Res type.
				resType = CclParseStringDesc(l);
			}
			res = std::make_unique<NumberDescPlayerData>(std::move(player), std::move(dataType), std::move(resType));

			lua_pop(l, 1); // table.
		} else {
			lua_pop(l, 1);
			LuaError(l, "unknow condition '%s'", key.data());
		}
	} else {
		LuaError(l, "Parse Error in ParseNumber");
	}
	lua_pop(l, 1);
	return res;
}

/**
**  Return String description.
**
**  @param l  lua state.
**
**  @return   String description.
*/
std::unique_ptr<IStringDesc> CclParseStringDesc(lua_State *l)
{
	std::unique_ptr<IStringDesc> res = nullptr;

	if (lua_isstring(l, -1)) {
		res = std::make_unique<StringDescString>(std::string(LuaToString(l, -1)));
	} else if (lua_isfunction(l, -1)) {
		res = std::make_unique<StringDescLuaFunction>(ParseLuaFunction(l, "_stringfunction_", &StringCounter));
	} else if (lua_istable(l, -1)) {
		const int nargs = lua_rawlen(l, -1);
		if (nargs != 2) {
			LuaError(l, "Bad number of args in parse String table\n");
		}
		const std::string_view key = LuaToString(l, -1, 1);
		lua_rawgeti(l, -1, 2); // table
		if (key == "Concat") {
			const auto n = lua_rawlen(l, -1);
			if (n < 1) {
				LuaError(l, "Bad number of args in Concat\n");
			}
			std::vector<std::unique_ptr<IStringDesc>> strings;
			for (std::size_t i = 0; i < n; ++i) {
				lua_rawgeti(l, -1, 1 + i);
				strings.emplace_back(CclParseStringDesc(l));
			}
			lua_pop(l, 1); // table.
			res = std::make_unique<StringDescConcat>(std::move(strings));
		} else if (key == "String") {
			res = std::make_unique<StringDescNumber>(CclParseNumberDesc(l));
		} else if (key == "InverseVideo") {
			res = std::make_unique<StringDescInverseVideo>(CclParseStringDesc(l));
		} else if (key == "UnitName") {
			res = std::make_unique<StringDescUnit>(CclParseUnitDesc(l));
		} else if (key == "If") {
			if (lua_rawlen(l, -1) != 2 && lua_rawlen(l, -1) != 3) {
				LuaError(l, "Bad number of args in If\n");
			}
			lua_rawgeti(l, -1, 1); // Condition.
			auto Cond = CclParseNumberDesc(l);
			lua_rawgeti(l, -1, 2); // Then.
			auto BTrue = CclParseStringDesc(l);
			std::unique_ptr<IStringDesc> BFalse;
			if (lua_rawlen(l, -1) == 3) {
				lua_rawgeti(l, -1, 3); // Else.
				BFalse = CclParseStringDesc(l);
			}
			lua_pop(l, 1); // table.
			res = std::make_unique<StringDescIf>(std::move(Cond), std::move(BTrue), std::move(BFalse));
		} else if (key == "SubString") {
			if (lua_rawlen(l, -1) != 2 && lua_rawlen(l, -1) != 3) {
				LuaError(l, "Bad number of args in SubString\n");
			}
			lua_rawgeti(l, -1, 1); // String.
			auto String = CclParseStringDesc(l);
			lua_rawgeti(l, -1, 2); // Begin.
			auto Begin = CclParseNumberDesc(l);
			std::unique_ptr<INumberDesc> End = nullptr;
			if (lua_rawlen(l, -1) == 3) {
				lua_rawgeti(l, -1, 3); // End.
				End = CclParseNumberDesc(l);
			}
			lua_pop(l, 1); // table.
			res = std::make_unique<StringDescSubString>(std::move(String), std::move(Begin), std::move(End));
		} else if (key == "Line") {
			if (lua_rawlen(l, -1) < 2 || lua_rawlen(l, -1) > 4) {
				LuaError(l, "Bad number of args in Line\n");
			}
			lua_rawgeti(l, -1, 1); // Line.
			auto Line = CclParseNumberDesc(l);
			lua_rawgeti(l, -1, 2); // String.
			auto String = CclParseStringDesc(l);
			std::unique_ptr<INumberDesc> MaxLen = nullptr;
			if (lua_rawlen(l, -1) >= 3) {
				lua_rawgeti(l, -1, 3); // Length.
				MaxLen = CclParseNumberDesc(l);
			}
			CFont* Font = nullptr;
			if (lua_rawlen(l, -1) >= 4) {
				lua_rawgeti(l, -1, 4); // Font.
				Font = CFont::Get(LuaToString(l, -1));
				if (!Font) {
					LuaError(l, "Bad Font name: '%s'", LuaToString(l, -1).data());
				}
				lua_pop(l, 1); // font name.
			}
			lua_pop(l, 1); // table.
			res = std::make_unique<StringDescLine>(std::move(String), std::move(Line), std::move(MaxLen), Font);
		} else if (key == "PlayerName") {
			res = std::make_unique<StringDescPlayerName>(CclParseNumberDesc(l));
		} else {
			lua_pop(l, 1);
			LuaError(l, "unknow condition '%s'", key.data());
		}
	} else {
		LuaError(l, "Parse Error in ParseString");
	}
	lua_pop(l, 1);
	return res;
}

/**
**  compute the Unit expression
**
**  @param unitdesc  struct with definition of the calculation.
**
**  @return          the result unit.
*/
CUnit *EvalUnit(const IUnitDesc &unitdesc)
{
	if (!Selected.empty()) {
		TriggerData.Active = Selected[0];
	} else {
		TriggerData.Active = UnitUnderCursor;
	}
	return unitdesc.eval();
}


int NumberDescLuaFunction::eval() const /* override */
{
	return CallLuaNumberFunction(index);
}

int NumberDescInt::eval() const /* override */
{
	return n;
}

int NumberDescBinOp::eval() const /* override */
{
	switch (type) {
		case EBinOp::Add: // a + b.
			return EvalNumber(*left) + EvalNumber(*right);
		case EBinOp::Sub: // a - b.
			return EvalNumber(*left) - EvalNumber(*right);
		case EBinOp::Mul: // a * b.
			return EvalNumber(*left) * EvalNumber(*right);
		case EBinOp::Div: // a / b.
		{
			auto lhs = EvalNumber(*left);
			auto rhs = EvalNumber(*right);
			if (rhs == 0) { // FIXME : manage better this.
				return 0;
			}
			return lhs / rhs;
		}
		case EBinOp::Min: // a <= b ? a : b
			return std::min(EvalNumber(*left), EvalNumber(*right));
		case EBinOp::Max: // a >= b ? a : b
			return std::max(EvalNumber(*left), EvalNumber(*right));
		case EBinOp::Gt: // a > b  ? 1 : 0
			return (EvalNumber(*left) > EvalNumber(*right) ? 1 : 0);
		case EBinOp::GtEq: // a >= b ? 1 : 0
			return (EvalNumber(*left) >= EvalNumber(*right) ? 1 : 0);
		case EBinOp::Lt: // a < b  ? 1 : 0
			return (EvalNumber(*left) < EvalNumber(*right) ? 1 : 0);
		case EBinOp::LtEq: // a <= b ? 1 : 0
			return (EvalNumber(*left) <= EvalNumber(*right) ? 1 : 0);
		case EBinOp::Eq: // a == b ? 1 : 0
			return (EvalNumber(*left) == EvalNumber(*right) ? 1 : 0);
		case EBinOp::NEq: // a != b ? 1 : 0
			return (EvalNumber(*left) != EvalNumber(*right) ? 1 : 0);
	}
	return 0;
}

int NumberDescRand::eval() const /* override */
{
	int number = EvalNumber(*n);
	return SyncRand() % number;
}
int NumberDescUnitStat::eval() const /* override */
{
	const auto* unit = EvalUnit(*unitDesc);

	if (unit != nullptr) {
		return std::get<int>(GetComponent(*unit, varIndex, component, loc));
	} else { // ERROR.
		return 0;
	}
}
int NumberDescTypeStat::eval() const /* override */
{
	if (type != nullptr) {
		return std::get<int>(GetComponent(**type, varIndex, component, loc));
	} else { // ERROR.
		return 0;
	}
}
int NumberDescVideoTextLength::eval() const /* override */
{
	if (string == nullptr || font == nullptr) // ERROR.
	{
		return 0;
	}
	return font->Width(EvalString(*string));
}

int NumberDescStringFind::eval() const /* override */
{
	if (string == nullptr) { // ERROR.
		return 0;
	}
	auto s = EvalString(*string);
	size_t pos = s.find(c);
	return pos != std::string::npos ? (int) pos : -1;
}

int NumberDescIf::eval() const /* override */
{
	if (EvalNumber(*cond)) {
		return EvalNumber(*trueValue);
	} else if (falseValue) {
		return EvalNumber(*falseValue);
	} else {
		return 0;
	}
}
int NumberDescPlayerData::eval() const /* override */
{
	int player = EvalNumber(*playerIndex);
	std::string data = EvalString(*dataType);
	std::string res = EvalString(*resType);
	return GetPlayerData(player, data, res);
}

/**
**  compute the number expression
**
**  @param number  struct with definition of the calculation.
**
**  @return        the result number.
*/
int EvalNumber(const INumberDesc &number)
{
	return number.eval();
}

std::string StringDescLuaFunction::eval() const
{
	return CallLuaStringFunction(index);
}

std::string StringDescString::eval() const
{
	return this->s;
}

std::string StringDescConcat::eval() const
{
	std::string res;

	for (const auto& s : strings) {
		res += EvalString(*s);
	}
	return res;
}

std::string StringDescNumber::eval() const
{
	return std::to_string(EvalNumber(*number));
}

std::string StringDescInverseVideo::eval() const
{
	// FIXME handle existing "~<"/"~>".
	return "~<" + EvalString(*string) + "~>";
}

std::string StringDescUnit::eval() const
{
	const auto *unit = EvalUnit(*unitDesc);
	if (unit != nullptr) {
		return unit->Type->Name;
	} else { // ERROR.
		return std::string("");
	}
}

std::string StringDescIf::eval() const
{
	if (EvalNumber(*cond)) {
		return EvalString(*trueValue);
	} else if (falseValue) {
		return EvalString(*falseValue);
	} else {
		return std::string("");
	}
}

std::string StringDescSubString::eval() const
{
	if (string == nullptr) {
		return ""; // Error
	}
	const auto s = EvalString(*string);
	const int offset = EvalNumber(*begin);
	const int end = this->end ? EvalNumber(*this->end) : std::string::npos;

	if ((unsigned) offset > s.size() && offset > 0) {
		return std::string("");
	}
	return s.substr(offset, end - offset);
}

std::string StringDescLine::eval() const
{
	if (string == nullptr) {
		return std::string(""); // ERROR.
	}
	const auto s = EvalString(*string);
	const int line = EvalNumber(*this->line);
	const int maxlen = this->maxLen ? std::max(0, EvalNumber(*this->maxLen)) : 0;
	if (line <= 0) {
		return std::string("");
	}
	return std::string(GetLineFont(line, s, maxlen, font));
}

std::string StringDescPlayerName::eval() const
{
	return Players[EvalNumber(*playerIndex)].Name;
}

/**
**  compute the string expression
**
**  @param s  struct with definition of the calculation.
**
**  @return   the result string.
*/
std::string EvalString(const IStringDesc &s)
{
	return s.eval();
}

/*............................................................................
..  Aliases
............................................................................*/

/**
**  Make alias for some unit type Variable function.
**
**  @param l  lua State.
**  @param s  FIXME: docu
**
**  @return   the lua table {"TypeVar", {Variable = arg1,
**                           Component = "Value" or arg2}
*/
static int AliasTypeVar(lua_State *l, const char *s)
{
	Assert(0 < lua_gettop(l) && lua_gettop(l) <= 3);
	int nargs = lua_gettop(l); // number of args in lua.
	lua_newtable(l);
	lua_pushnumber(l, 1);
	lua_pushstring(l, "TypeVar");
	lua_rawset(l, -3);
	lua_pushnumber(l, 2);
	lua_newtable(l);

	lua_pushstring(l, "Type");
	lua_pushstring(l, s);
	lua_rawset(l, -3);
	lua_pushstring(l, "Variable");
	lua_pushvalue(l, 1);
	lua_rawset(l, -3);
	lua_pushstring(l, "Component");
	if (nargs >= 2) {
		lua_pushvalue(l, 2);
	} else {
		lua_pushstring(l, "Value");
	}
	lua_rawset(l, -3);
	lua_pushstring(l, "Loc");
	if (nargs >= 3) {
		//  Warning: type is for unit->Stats->Var...
		//           and Initial is for unit->Type->Var... (no upgrade modification)
		const char *sloc[] = {"Unit", "Initial", "Type", nullptr};
		int i;

		const std::string_view key = LuaToString(l, 3);
		for (i = 0; sloc[i] != nullptr; i++) {
			if (key == sloc[i]) {
				lua_pushnumber(l, i);
				break ;
			}
		}
		if (sloc[i] == nullptr) {
			LuaError(l, "Bad loc: '%s'", key.data());
		}
	} else {
		lua_pushnumber(l, 0);
	}
	lua_rawset(l, -3);

	lua_rawset(l, -3);
	return 1;
}

/**
**  Make alias for some unit Variable function.
**
**  @param l  lua State.
**  @param s  FIXME: docu
**
**  @return   the lua table {"UnitVar", {Unit = s, Variable = arg1,
**                           Component = "Value" or arg2, Loc = [012]}
*/
static int AliasUnitVar(lua_State *l, const char *s)
{
	int nargs; // number of args in lua.

	Assert(0 < lua_gettop(l) && lua_gettop(l) <= 3);
	nargs = lua_gettop(l);
	lua_newtable(l);
	lua_pushnumber(l, 1);
	lua_pushstring(l, "UnitVar");
	lua_rawset(l, -3);
	lua_pushnumber(l, 2);
	lua_newtable(l);

	lua_pushstring(l, "Unit");
	lua_pushstring(l, s);
	lua_rawset(l, -3);
	lua_pushstring(l, "Variable");
	lua_pushvalue(l, 1);
	lua_rawset(l, -3);
	lua_pushstring(l, "Component");
	if (nargs >= 2) {
		lua_pushvalue(l, 2);
	} else {
		lua_pushstring(l, "Value");
	}
	lua_rawset(l, -3);
	lua_pushstring(l, "Loc");
	if (nargs >= 3) {
		//  Warning: type is for unit->Stats->Var...
		//           and Initial is for unit->Type->Var... (no upgrade modification)
		const char *sloc[] = {"Unit", "Initial", "Type", nullptr};
		int i;

		std::string_view key = LuaToString(l, 3);
		for (i = 0; sloc[i] != nullptr; i++) {
			if (key == sloc[i]) {
				lua_pushnumber(l, i);
				break ;
			}
		}
		if (sloc[i] == nullptr) {
			LuaError(l, "Bad loc: '%s'", key.data());
		}
	} else {
		lua_pushnumber(l, 0);
	}
	lua_rawset(l, -3);

	lua_rawset(l, -3);
	return 1;
}

/**
**  Return equivalent lua table for .
**  {"Unit", {Unit = "Attacker", Variable = arg1, Component = "Value" or arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclUnitAttackerVar(lua_State *l)
{
	if (lua_gettop(l) == 0 || lua_gettop(l) > 3) {
		LuaError(l, "Bad number of arg for AttackerVar()\n");
	}
	return AliasUnitVar(l, "Attacker");
}

/**
**  Return equivalent lua table for .
**  {"Unit", {Unit = "Defender", Variable = arg1, Component = "Value" or arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclUnitDefenderVar(lua_State *l)
{
	if (lua_gettop(l) == 0 || lua_gettop(l) > 3) {
		LuaError(l, "Bad number of arg for DefenderVar()\n");
	}
	return AliasUnitVar(l, "Defender");
}

/**
**  Return equivalent lua table for .
**  {"Unit", {Unit = "Active", Variable = arg1, Component = "Value" or arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclActiveUnitVar(lua_State *l)
{
	if (lua_gettop(l) == 0 || lua_gettop(l) > 3) {
		LuaError(l, "Bad number of arg for ActiveUnitVar()\n");
	}
	return AliasUnitVar(l, "Active");
}

/**
**  Return equivalent lua table for .
**  {"Type", {Type = "Active", Variable = arg1, Component = "Value" or arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclActiveTypeVar(lua_State *l)
{
	if (lua_gettop(l) == 0 || lua_gettop(l) > 3) {
		LuaError(l, "Bad number of arg for ActiveTypeVar()\n");
	}
	return AliasTypeVar(l, "Type");
}


/**
**  Make alias for some function.
**
**  @param l  lua State.
**  @param s  FIXME: docu
**
**  @return the lua table {s, {arg1, arg2, ..., argn}} or {s, arg1}
*/
static int Alias(lua_State *l, const char *s)
{
	const int narg = lua_gettop(l);
	Assert(narg);
	lua_newtable(l);
	lua_pushnumber(l, 1);
	lua_pushstring(l, s);
	lua_rawset(l, -3);
	lua_pushnumber(l, 2);
	if (narg > 1) {
		lua_newtable(l);
		for (int i = 1; i <= narg; i++) {
			lua_pushnumber(l, i);
			lua_pushvalue(l, i);
			lua_rawset(l, -3);
		}
	} else {
		lua_pushvalue(l, 1);
	}
	lua_rawset(l, -3);
	return 1;
}

/**
**  Return equivalent lua table for add.
**  {"Add", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclAdd(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Add");
}

/**
**  Return equivalent lua table for add.
**  {"Div", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclSub(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Sub");
}
/**
**  Return equivalent lua table for add.
**  {"Mul", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclMul(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Mul");
}
/**
**  Return equivalent lua table for add.
**  {"Div", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclDiv(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Div");
}
/**
**  Return equivalent lua table for add.
**  {"Min", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclMin(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Min");
}
/**
**  Return equivalent lua table for add.
**  {"Max", {arg1, arg2, argn}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclMax(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Max");
}
/**
**  Return equivalent lua table for add.
**  {"Rand", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclRand(lua_State *l)
{
	LuaCheckArgs(l, 1);
	return Alias(l, "Rand");
}
/**
**  Return equivalent lua table for GreaterThan.
**  {"GreaterThan", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclGreaterThan(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "GreaterThan");
}
/**
**  Return equivalent lua table for GreaterThanOrEq.
**  {"GreaterThanOrEq", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclGreaterThanOrEq(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "GreaterThanOrEq");
}
/**
**  Return equivalent lua table for LessThan.
**  {"LessThan", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclLessThan(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "LessThan");
}
/**
**  Return equivalent lua table for LessThanOrEq.
**  {"LessThanOrEq", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclLessThanOrEq(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "LessThanOrEq");
}
/**
**  Return equivalent lua table for Equal.
**  {"Equal", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclEqual(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "Equal");
}
/**
**  Return equivalent lua table for NotEqual.
**  {"NotEqual", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclNotEqual(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "NotEqual");
}



/**
**  Return equivalent lua table for Concat.
**  {"Concat", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclConcat(lua_State *l)
{
	if (lua_gettop(l) < 1) { // FIXME do extra job for 1.
		LuaError(l, "Bad number of arg for Concat()\n");
	}
	return Alias(l, "Concat");
}

/**
**  Return equivalent lua table for String.
**  {"String", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclString(lua_State *l)
{
	LuaCheckArgs(l, 1);
	return Alias(l, "String");
}
/**
**  Return equivalent lua table for InverseVideo.
**  {"InverseVideo", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclInverseVideo(lua_State *l)
{
	LuaCheckArgs(l, 1);
	return Alias(l, "InverseVideo");
}
/**
**  Return equivalent lua table for UnitName.
**  {"UnitName", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
**
** Example:
**
** <div class="example"><code>u_data = <strong>UnitType</strong>("unit-footman")</code></div>
*/
static int CclUnitName(lua_State *l)
{
	LuaCheckArgs(l, 1);
	return Alias(l, "UnitName");
}
/**
**  Return equivalent lua table for If.
**  {"If", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclIf(lua_State *l)
{
	if (lua_gettop(l) != 2 && lua_gettop(l) != 3) {
		LuaError(l, "Bad number of arg for If()\n");
	}
	return Alias(l, "If");
}

/**
**  Return equivalent lua table for SubString.
**  {"SubString", {arg1, arg2, arg3}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclSubString(lua_State *l)
{
	if (lua_gettop(l) != 2 && lua_gettop(l) != 3) {
		LuaError(l, "Bad number of arg for SubString()\n");
	}
	return Alias(l, "SubString");
}

/**
**  Return equivalent lua table for Line.
**  {"Line", {arg1, arg2[, arg3]}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclLine(lua_State *l)
{
	if (lua_gettop(l) < 2 || lua_gettop(l) > 4) {
		LuaError(l, "Bad number of arg for Line()\n");
	}
	return Alias(l, "Line");
}

/**
**  Return equivalent lua table for Line.
**  {"Line", "arg1"}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclGameInfo(lua_State *l)
{
	LuaCheckArgs(l, 1);
	return Alias(l, "GameInfo");
}

/**
**  Return equivalent lua table for VideoTextLength.
**  {"VideoTextLength", {Text = arg1, Font = arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclVideoTextLength(lua_State *l)
{
	LuaCheckArgs(l, 2);
	lua_newtable(l);
	lua_pushnumber(l, 1);
	lua_pushstring(l, "VideoTextLength");
	lua_rawset(l, -3);
	lua_pushnumber(l, 2);

	lua_newtable(l);
	lua_pushstring(l, "Font");
	lua_pushvalue(l, 1);
	lua_rawset(l, -3);
	lua_pushstring(l, "Text");
	lua_pushvalue(l, 2);
	lua_rawset(l, -3);

	lua_rawset(l, -3);
	return 1;
}

/**
**  Return equivalent lua table for StringFind.
**  {"StringFind", {arg1, arg2}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclStringFind(lua_State *l)
{
	LuaCheckArgs(l, 2);
	return Alias(l, "StringFind");
}

/**
**  Return equivalent lua table for NumIf.
**  {"NumIf", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclNumIf(lua_State *l)
{
	if (lua_gettop(l) != 2 && lua_gettop(l) != 3) {
		LuaError(l, "Bad number of arg for NumIf()\n");
	}
	return Alias(l, "NumIf");
}

/**
**  Return equivalent lua table for PlayerData.
**  {"PlayerData", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclPlayerData(lua_State *l)
{
	if (lua_gettop(l) != 2 && lua_gettop(l) != 3) {
		LuaError(l, "Bad number of arg for PlayerData()\n");
	}
	return Alias(l, "PlayerData");
}

/**
**  Return equivalent lua table for PlayerName.
**  {"PlayerName", {arg1}}
**
**  @param l  Lua state.
**
**  @return   equivalent lua table.
*/
static int CclPlayerName(lua_State *l)
{
	LuaCheckArgs(l, 1);
	return Alias(l, "PlayerName");
}


static void AliasRegister()
{
	// Number.
	lua_register(Lua, "Add", CclAdd);
	lua_register(Lua, "Sub", CclSub);
	lua_register(Lua, "Mul", CclMul);
	lua_register(Lua, "Div", CclDiv);
	lua_register(Lua, "Min", CclMin);
	lua_register(Lua, "Max", CclMax);
	lua_register(Lua, "Rand", CclRand);

	lua_register(Lua, "GreaterThan", CclGreaterThan);
	lua_register(Lua, "LessThan", CclLessThan);
	lua_register(Lua, "Equal", CclEqual);
	lua_register(Lua, "GreaterThanOrEq", CclGreaterThanOrEq);
	lua_register(Lua, "LessThanOrEq", CclLessThanOrEq);
	lua_register(Lua, "NotEqual", CclNotEqual);
	lua_register(Lua, "VideoTextLength", CclVideoTextLength);
	lua_register(Lua, "StringFind", CclStringFind);


	// Unit
	lua_register(Lua, "AttackerVar", CclUnitAttackerVar);
	lua_register(Lua, "DefenderVar", CclUnitDefenderVar);
	lua_register(Lua, "ActiveUnitVar", CclActiveUnitVar);

	// Type
	lua_register(Lua, "TypeVar", CclActiveTypeVar);

	// String.
	lua_register(Lua, "Concat", CclConcat);
	lua_register(Lua, "String", CclString);
	lua_register(Lua, "InverseVideo", CclInverseVideo);
	lua_register(Lua, "UnitName", CclUnitName);
	lua_register(Lua, "SubString", CclSubString);
	lua_register(Lua, "Line", CclLine);
	lua_register(Lua, "GameInfo", CclGameInfo);
	lua_register(Lua, "PlayerName", CclPlayerName);
	lua_register(Lua, "PlayerData", CclPlayerData);

	lua_register(Lua, "If", CclIf);
	lua_register(Lua, "NumIf", CclNumIf);
}

/*............................................................................
..  Config
............................................................................*/

/**
**  Return the stratagus library path.
**
**  @param l  Lua state.
**
**  @return   Current libray path.
*/
static int CclStratagusLibraryPath(lua_State *l)
{
#ifdef STRATAGUS_PAX
	lua_pushstring(l, StratagusLibPath);
#else
	lua_pushstring(l, StratagusLibPath.c_str());
#endif
	return 1;
}

/**
**  Return a table with the filtered items found in the subdirectory.
*/
static int CclFilteredListDirectory(lua_State *l, int type, int mask)
{
	const int args = lua_gettop(l);
	if (args < 1 || args > 2) {
		LuaError(l, "incorrect argument");
	}
	std::string_view userdir = LuaToString(l, 1);
	const bool rel = args > 1 ? lua_toboolean(l, 2) : false;

	int pathtype = 0; // path relative to stratagus dir
	if (starts_with(userdir, "~")) {
		// path relative to user preferences directory
		pathtype = 1;
	}

	// security: disallow all special characters
	if (userdir.find_first_of(":*?\"<>|") != std::string_view::npos
	    || userdir.find("..") != std::string_view::npos) {
		LuaError(l, "Forbidden directory");
	}
	fs::path dir;

	if (pathtype == 1) {
		userdir = userdir.substr(1);
		dir = Parameters::Instance.GetUserDirectory();
		if (!GameName.empty()) {
			dir /= GameName;
		}
		dir /= userdir;
	} else if (rel) {
		dir = LibraryFileName(std::string(userdir));
	} else {
		dir = fs::path(StratagusLibPath) / userdir;
	}
	lua_newtable(l);
	int j = 0;
	for (const auto& flp : ReadDataDirectory(dir)) {
		if ((flp.type & mask) == type) {
			lua_pushnumber(l, j + 1);
			lua_pushstring(l, flp.name.string().c_str());
			lua_settable(l, -3);
			++j;
		}
	}
	return 1;
}

/**
**  Return a table with the files or directories found in the subdirectory.
*/
static int CclListDirectory(lua_State *l)
{
	return CclFilteredListDirectory(l, 0, 0);
}

/**
**  Return a table with the files found in the subdirectory.
*/
static int CclListFilesInDirectory(lua_State *l)
{
	return CclFilteredListDirectory(l, 0x1, 0x1);
}

/**
**  Return a table with the files found in the subdirectory.
*/
static int CclListDirsInDirectory(lua_State *l)
{
	return CclFilteredListDirectory(l, 0x0, 0x1);
}

/**
**  Set damage computation method.
**
**  @param l  Lua state.
*/
static int CclSetDamageFormula(lua_State *l)
{
	Assert(l);
	Damage = CclParseNumberDesc(l);
	return 0;
}

std::string getLuaLocation(lua_State* l)
{
	lua_Debug ar;
	lua_getstack(l, 1, &ar);
	lua_getinfo(l, "nSl", &ar);

	return std::string(ar.source) + ":" + std::to_string(ar.currentline) + ": ";
}

/**
**  Print debug message with info about current script name, line number and function.
**
**  @see DebugPrint
**
**  @param l  Lua state.
*/
static int CclDebugPrint(lua_State *l)
{
	LuaCheckArgs(l, 1);

#ifdef DEBUG
	fprintf(stdout, "%s%s\n", getLuaLocation(l).c_str(), LuaToString(l, 1).data());
#endif

	return 0;
}

/**
 * Restart the entire game. This function only returns when an error happens.
 */
static int CclRestartStratagus(lua_State *l)
{
	LuaCheckArgs(l, 0);
	const fs::path executable_path = GetExecutablePath();
	const bool insertRestartArgument = !ranges::contains(OriginalArgv, "-r");
	const std::vector<std::string> quotedArgs = QuoteArguments(OriginalArgv);

	const int newArgc = quotedArgs.size() + (insertRestartArgument ? 2 : 1);
	std::vector<char *> argv(newArgc);
	for (std::size_t i = 0; i < quotedArgs.size(); ++i) {
		argv[i] = const_cast<char *>(quotedArgs[i].c_str());
	}
	if (insertRestartArgument) {
		argv[newArgc - 2] = const_cast<char *>("-r");
	}
	argv[newArgc - 1] = nullptr;
#ifdef WIN32
	_execv(executable_path.string().c_str(), argv.data());
#else
	execvp(executable_path.c_str(), argv.data());
#endif

	return 0;
}

/*............................................................................
..  Commands
............................................................................*/

/**
**  Send command to ccl.
**
**  @param command  Zero terminated command string.
*/
int CclCommand(const std::string &command, bool exitOnError)
{
	#ifdef STRATAGUS_PAX
	const bool pax_map_loaded_command = command.find("MapLoaded") != std::string::npos;
	if (pax_map_loaded_command) {
		pax_stratagus_debug("MapLoaded command begin");
	}
	#endif
	const int status = luaL_loadbuffer(Lua, command.c_str(), command.size(), command.c_str());

	if (!status) {
		LuaCall(0, 1, exitOnError);
		#ifdef STRATAGUS_PAX
		if (pax_map_loaded_command) {
			pax_stratagus_debug("MapLoaded command complete");
		}
		#endif
	} else {
		report(status, exitOnError);
	}
	return status;
}

/*............................................................................
..  Setup
............................................................................*/

extern int tolua_stratagus_open(lua_State *tolua_S);

/**
**  Initialize Lua
*/
void InitLua()
{
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("InitLua: begin");
	#endif
	// For security we don't load all libs
#ifdef STRATAGUS_PAX
// The game scripts rely on Lua's standard base/table/string/math helpers.
// Keep host-facing libraries disabled, but do not start with an empty Lua
// environment: that causes valid Stargus scripts to fail later in CCL code.
static const luaL_Reg lualibs[] = {
	{"", luaopen_base},
	{LUA_TABLIBNAME, luaopen_table},
	{LUA_STRLIBNAME, luaopen_string},
	{LUA_MATHLIBNAME, luaopen_math},
	{nullptr, nullptr}
};
#else
	static const luaL_Reg lualibs[] = {
		{"", luaopen_base},
		{LUA_TABLIBNAME, luaopen_table},
#ifdef DEBUG
		{LUA_IOLIBNAME, luaopen_io},
		{LUA_LOADLIBNAME, luaopen_package},
#endif
#ifndef STRATAGUS_PAX
	{LUA_OSLIBNAME, luaopen_os},
#endif
#ifndef STRATAGUS_PAX
	{LUA_STRLIBNAME, luaopen_string},
#endif
#ifndef STRATAGUS_PAX
	{LUA_MATHLIBNAME, luaopen_math},
#endif
#ifndef STRATAGUS_PAX
	{LUA_DBLIBNAME, luaopen_debug},
#endif
		{nullptr, nullptr}
	};
#endif

	Lua = luaL_newstate();
	#ifdef STRATAGUS_PAX
	// The PAX build has no debugger/remote Lua hooks.  Keeping the hook
	// machinery disabled also avoids entering Lua's hook stack-growth path
	// while the app is running from its relocated image.
	if (Lua) {
		lua_sethook(Lua, nullptr, 0, 0);
	}
	#endif
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug(Lua ? "InitLua: state created" : "InitLua: state creation failed");
	#endif

	for (const luaL_Reg *lib = lualibs; lib->func; ++lib) {
		#ifdef STRATAGUS_PAX
		pax_stratagus_debug(lib->name[0] ? lib->name : "InitLua: base library");
		#endif
#if LUA_VERSION_NUM == 503 || LUA_VERSION_NUM == 502
		luaL_requiref(Lua, lib->name, lib->func, 1);
		lua_pop(Lua, 1);
#else
		lua_pushcfunction(Lua, lib->func);
		lua_pushstring(Lua, lib->name);
		lua_call(Lua, 1, 0);
#endif
	}
#if defined(DEBUG) && !defined(WIN32)
	static const char* mobdebug =
#include "./lua/mobdebug.luaheader"
;
	int status = luaL_loadbuffer(Lua, mobdebug, strlen(mobdebug), "mobdebug.lua");
	if (!status) {
		status = LuaCall(0, 0, false);
		if (!status) {
			ErrorPrint("mobdebug loaded and available via mobdebug.start()\n");
			lua_setglobal(Lua, "mobdebug");
		}
	}
	report(status, false);
#endif
#ifdef STRATAGUS_PAX
	pax_stratagus_debug("InitLua: registering tolua classes");
	tolua_stratagus_open(Lua);
	pax_stratagus_debug("InitLua: tolua classes registered");
#else
	tolua_stratagus_open(Lua);
#endif
	lua_settop(Lua, 0);  // discard any results
}

/*
static char *LuaEscape(const char *str)
{
	const unsigned char *src;
	char *dst;
	char *escapedString;
	int size = 0;

	for (src = (const unsigned char *)str; *src; ++src) {
		if (*src == '"' || *src == '\\') { // " -> \"
			size += 2;
		} else if (*src < 32 || *src > 127) { // 0xA -> \010
			size += 4;
		} else {
			++size;
		}
	}

	escapedString = new char[size + 1];
	for (src = (const unsigned char *)str, dst = escapedString; *src; ++src) {
		if (*src == '"' || *src == '\\') { // " -> \"
			*dst++ = '\\';
			*dst++ = *src;
		} else if (*src < 32 || *src > 127) { // 0xA -> \010
			*dst++ = '\\';
			snprintf(dst, (size + 1) - (dst - escapedString), "%03d", *src);
			dst += 3;
		} else {
			*dst++ = *src;
		}
	}
	*dst = '\0';

	return escapedString;
}
*/

static std::string ConcatTableString(const std::vector<std::string> &blockTableNames)
{
	if (blockTableNames.empty()) {
		return "";
	}
	std::string res(blockTableNames[0]);
	for (size_t i = 1; i != blockTableNames.size(); ++i) {
		if (blockTableNames[i][0] != '[') {
			res += ".";
		}
		res += blockTableNames[i];
	}
	return res;
}

static bool IsAValidTableName(std::string_view key)
{
	return key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") == std::string_view::npos;
}

static std::set<std::string, std::less<>> userForbiddenNames;

static int CclIgnoreVariableNameWhenSaving(lua_State *l)
{
	luaL_checkstring(l, -1);
	userForbiddenNames.insert(lua_tostring(l, -1));
	return 0;
}

static bool ShouldGlobalTableBeSaved(std::string_view key)
{
	if (IsAValidTableName(key) == false) {
		return false;
	}
	static const std::set<std::string_view> forbiddenNames = {
		"BasicContainer",
		"Button",
		"ButtonWidget",
		"CButtonPanel",
		"CChunkParticle",
		"CColor",
		"CEditor",
		"CFiller",
		"CFont",
		"CFontColor",
		"CGraphic",
		"CGraphicPtr",
		"CIcon",
		"CInfoPanel",
		"CMap",
		"CMapArea",
		"CMapInfo",
		"CMenuScreen",
		"CMinimap",
		"CNetworkHost",
		"CParticle",
		"CParticleManager",
		"CPieMenu",
		"CPlayer",
		"CPlayerColorGraphic",
		"CPlayerColorGraphicPtr",
		"CPosition",
		"CPreference",
		"CRadialParticle",
		"CResourceInfo",
		"CServerSetup",
		"CStatusLine",
		"CSmokeParticle",
		"CTileset",
		"CUIButton",
		"CUITimer",
		"CUIUserButton",
		"CUnit",
		"CUnitManager",
		"CUnitType",
		"CUserInterface",
		"CUpgrade",
		"CVideo",
		"CViewport",
		"CheckBox",
		"Color",
		"Container",
		"DownButton",
		"DropDown",
		"DropDownWidget",
		"EditorCommandLine",
		"EditorEditing",
		"EditorNotRunning",
		"EditorStarted",
		"FoodCost",
		"FontColors",
		"Fonts",
		"FreeWorkersCount",
		"GameDefeat",
		"GameDraw",
		"GameExit",
		"GameNoResult",
		"GameQuitToMenu",
		"GameRestart",
		"GameVictory",
		"Graphics",
		"GraphicAnimation",
		"Icons",
		"ImageButton",
		"ImageCheckBox",
		"ImageDropDownWidget",
		"ImageListBox",
		"ImageListBoxWidget",
		"ImageRadioButton",
		"ImageTextField",
		"ImageSlider",
		"ImageWidget",
		"InfiniteRepairRange",
		"Label",
		"LeftButton",
		"ListBox",
		"ListBoxWidget",
		"LuaActionListener",
		"ManaResCost",
		"MaxCosts",
		"MaxFontColors",
		"MaxResourceInfo",
		"MiddleButton",
		"Mng",
		"MngPtr",
		"Movie",
		"MultiLineLabel",
		"NoButton",
		"NumOfTypes",
		"OnlineService",
		"PlayerNeutral",
		"PlayerComputer",
		"PlayerMax",
		"PlayerNobody",
		"PlayerNumNeutral",
		"PlayerPerson",
		"PlayerRescuePassive",
		"PlayerRescueActive",
		"PlayerUnset",
		"RadioButton",
		"RightButton",
		"ScoreCost",
		"ScrollArea",
		"ScrollingWidget",
		"ServerSetupStateRacesArray",
		"Settings",
		"SettingsGameTypeFreeForAll",
		"SettingsGameTypeLeftVsRight",
		"SettingsGameTypeMachineVsMachine",
		"SettingsGameTypeMachineVsMachineTraining",
		"SettingsGameTypeManTeamVsMachine",
		"SettingsGameTypeManVsMachine",
		"SettingsGameTypeMapDefault",
		"SettingsGameTypeMelee",
		"SettingsGameTypeTopVsBottom",
		"SettingsMultiPlayerGame",
		"SettingsPresets",
		"SettingsPresetMapDefault",
		"SettingsSinglePlayerGame",
		"Slider",
		"SlotAvailable",
		"SlotClosed",
		"SlotComputer",
		"StatBoxWidget",
		"StaticParticle",
		"TextBox",
		"TextField",
		"UnitTypes",
		"Unset",
		"UpButton",
		"Upgrades",
		"VIEWPORT_SINGLE",
		"VIEWPORT_SPLIT_HORIZ",
		"VIEWPORT_SPLIT_HORIZ3",
		"VIEWPORT_SPLIT_VERT",
		"VIEWPORT_QUAD",
		"Vec2i",
		"Widget",
		"Window",
		"Windows",
		"arg",
		"assert",
		"cAllUnits",
		"cBuildingsOnly",
		"cExplored",
		"cKnown",
		"cHidden",
		"cNoRevelation",
		"cNumOfModes",
		"cShadowCasting",
		"cSimpleRadial",
		"collectgarbage",
		"coroutine",
		"debug",
		"dofile",
		"error",
		"expansion",
		"gcinfo",
		"getfenv",
		"getmetatable",
		"io",
		"ipairs",
		"loadfile",
		"loadlib",
		"loadstring",
		"math",
		"mt",
		"next",
		"newproxy",
		"os",
		"package",
		"pairs",
		"pcall",
		"print",
		"rawequal",
		"rawget",
		"rawset",
		"require",
		"setfenv",
		"setmetatable",
		"string",
		"table",
		"tolua",
		"tonumber",
		"tostring",
		"type",
		"unpack",
		"vector_CFiller_",
		"vector_CUIButton_",
		"vector_CUIUserButton_",
		"vector_string_",
		"xpcall",
		"_LOADED",
		"_TRACEBACK",
		"_VERSION",
		"_ai_scripts_",
		"_stringfunction_",
		"_triggers_",
		"_uiButtonCallbacks",
		"__file__",
		"__pow",
	}; // other string to protect?

	return !forbiddenNames.count(key) && !userForbiddenNames.count(key);
}

static bool ShouldLocalTableBeSaved(std::string_view key)
{
	if (IsAValidTableName(key) == false) {
		return false;
	}
	static const std::set<std::string_view> forbiddenNames = {"tolua_ubox", "_base", "__index"}; // other strings to protect?

	return !forbiddenNames.count(key);
}

static std::optional<std::string> LuaValueToString(lua_State *l)
{
	const int type_value = lua_type(l, -1);

	switch (type_value) {
		case LUA_TNIL:
			return "nil";
		case LUA_TNUMBER:
			return lua_tostring(l, -1); // let lua do the conversion
		case LUA_TBOOLEAN: {
			const bool b = lua_toboolean(l, -1);
			return b ? "true" : "false";
		}
		case LUA_TSTRING: {
			const std::string s = lua_tostring(l, -1);

			if ((s.find('\n') != std::string::npos)) {
				return std::string("[[") + s + "]]";
			} else {
				std::string res;
				for (char c : s) {
					if (c == '\"') {
						res.push_back('\\');
					}
					res.push_back(c);
				}
				res = "\"" + res + "\"";
				return res;
			}
		}
		case LUA_TTABLE:
			return std::nullopt;
		case LUA_TFUNCTION:
			// Could be done with string.dump(function)
			// and debug.getinfo(function).name (could be nil for anonymous function)
			// But not useful yet.
			return std::nullopt;
		case LUA_TUSERDATA:
		case LUA_TTHREAD:
		case LUA_TLIGHTUSERDATA:
		case LUA_TNONE:
		default : // no other cases
			return std::nullopt;
	}
}

/**
**  For saving lua state (table, number, string, bool, not function).
**
**  @param l        lua_State to save.
**  @param is_root  true for the main call, 0 for recursive call.
**
**  @return  "" if nothing could be saved.
**           else a string that could be executed in lua to restore lua state
**  @todo    do the output prettier (adjust indentation, newline)
*/
static std::string SaveGlobal(lua_State *l, bool is_root, std::vector<std::string> &blockTableNames)
{
	//Assert(!is_root || !lua_gettop(l));
	if (is_root) {
		lua_getglobal(l, "_G");// global table in lua.
	}
	std::string res;
	const std::string tablesName = ConcatTableString(blockTableNames);

	if (blockTableNames.empty() == false) {
		res = "if (" + tablesName + " == nil) then " + tablesName + " = {} end\n";
	}
	Assert(lua_istable(l, -1));

	lua_pushnil(l);
	while (lua_next(l, -2)) {
		const int type_key = lua_type(l, -2);
		std::string key = (type_key == LUA_TSTRING) ? lua_tostring(l, -2) : "";
		if ((key == "_G")
			|| (is_root && ShouldGlobalTableBeSaved(key) == false)
			|| (!is_root && ShouldLocalTableBeSaved(key) == false)) {
			lua_pop(l, 1); // pop the value
			continue;
		}
		std::string lhsLine;
		if (tablesName.empty() == false) {
			if (type_key == LUA_TSTRING) {
				lhsLine = tablesName + "." + key;
			} else if (type_key == LUA_TNUMBER) {
				lua_pushvalue(l, -2);
				lhsLine = tablesName + "[" + lua_tostring(l, -1) + "]";
				lua_pop(l, 1);
			}
		} else {
			lhsLine = key;
		}

		if (auto value = LuaValueToString(l)) {
			res += lhsLine + " = " + *value + "\n";
		} else {
			const int type_value = lua_type(l, -1);
			if (type_value == LUA_TTABLE) {
				if (key == "") {
					lua_pushvalue(l, -2);
					key = key + "[" + lua_tostring(l, -1) + "]";
					lua_pop(l, 1);
				}
				//res += "if (" + lhsLine + " == nil) then " + lhsLine + " = {} end\n";
				if (ranges::find(blockTableNames, key) == blockTableNames.end() && key != "[]")
				{
					lua_pushvalue(l, -1);
					blockTableNames.push_back(key);
					res += SaveGlobal(l, false, blockTableNames);
					blockTableNames.pop_back();
				}
			}
		}
		lua_pop(l, 1); /* pop the value */
	}
	lua_pop(l, 1); // pop the table
	//Assert(!is_root || !lua_gettop(l));
	return res;
}

std::string SaveGlobal(lua_State *l)
{
	std::vector<std::string> blockTableNames;

	return SaveGlobal(l, true, blockTableNames);
}

/**
**  Save user preferences
*/
void SavePreferences()
{
	std::vector<std::string> blockTableNames;

	if (!GameName.empty()) {
		lua_getglobal(Lua, GameName.c_str());
		if (lua_type(Lua, -1) == LUA_TTABLE) {
			blockTableNames.push_back(GameName);
			lua_pushstring(Lua, "preferences");
			lua_gettable(Lua, -2);
		} else {
			lua_getglobal(Lua, "preferences");
		}
	} else {
		lua_getglobal(Lua, "preferences");
	}
	blockTableNames.push_back("preferences");
	if (lua_type(Lua, -1) == LUA_TTABLE) {
		fs::path path = Parameters::Instance.GetUserDirectory();

		if (!GameName.empty()) {
			path /= GameName;
		}
		path /= "preferences.lua";

		FILE *fd = fopen(path.string().c_str(), "w");
		if (!fd) {
			ErrorPrint("Cannot open file '%s' for writing\n", path.u8string().c_str());
			return;
		}

		std::string s = SaveGlobal(Lua, false, blockTableNames);
		if (!GameName.empty()) {
			fprintf(fd, "if (%s == nil) then %s = {} end\n", GameName.c_str(), GameName.c_str());
		}
		fprintf(fd, "%s\n", s.c_str());
		fclose(fd);
	}
}

/**
**  Load stratagus config file.
*/
void LoadCcl(const fs::path &filename, const std::string &luaArgStr)
{
	//  Load and evaluate configuration file
	CclInConfigFile = true;
	#ifdef STRATAGUS_PAX
	pax_stratagus_debug("LoadCcl loading game scripts");
#ifdef PAX_STRATAGUS_WARCRAFT
	LuaLoadFile(LibraryFileName(filename.string()),luaArgStr);
#else
	LuaLoadPaxBootstrap(luaArgStr, true);
#endif
	pax_stratagus_debug("LoadCcl after LuaLoadPaxBootstrap");
	CclInConfigFile = false;
	LuaGarbageCollect();
	pax_stratagus_debug("LoadCcl game scripts complete");
	#else
	const fs::path name = LibraryFileName(filename.string());
	if (!fs::exists(name)) {
		ErrorPrint("Maybe you need to specify another gamepath with '-d /path/to/datadir'?\n");
		ExitFatal(-1);
	}

	ShowLoadProgress(_("Script %s\n"), name.u8string().c_str());
	LuaLoadFile(name, luaArgStr);
	CclInConfigFile = false;
	LuaGarbageCollect();
	#endif
}

#ifdef WIN32
fs::path GetAVolumePath(PWCHAR VolumeName)
{
	DWORD CharCount = MAX_PATH + 1;
	std::vector<WCHAR> Names(CharCount);

	for (;;) {
		const BOOL Success =
			GetVolumePathNamesForVolumeNameW(VolumeName, Names.data(), Names.size(), &CharCount);
		Names.resize(CharCount);
		if (Success || GetLastError() != ERROR_MORE_DATA) {
			break;
		}
	}
	return Names.data();
}

std::vector<fs::path> getVolumes()
{
	WCHAR VolumeName[MAX_PATH] = L"";

	//  Enumerate all volumes in the system.
	HANDLE FindHandle = FindFirstVolumeW(VolumeName, std::size(VolumeName));

	if (FindHandle == INVALID_HANDLE_VALUE) {
		const DWORD Error = GetLastError();
		wprintf(L"FindFirstVolumeW failed with error code %d\n", Error);
		ExitFatal(1);
	}

	std::vector<fs::path> result;
	for (;;) {
		size_t Index = wcslen(VolumeName) - 1;

		wprintf(L"Volume name: %s\n", VolumeName);
		if (!starts_with(VolumeName, LR"(\\?\)") || VolumeName[Index] != L'\\') {
			wprintf(L"FindFirstVolumeW/FindNextVolumeW returned a bad path: %s\n", VolumeName);
			ExitFatal(1);
		}

		//  Skip the \\?\ prefix and remove the trailing backslash.
		//  QueryDosDeviceW does not allow a trailing backslash,
		//  so temporarily remove it.
		VolumeName[Index] = L'\0';
		WCHAR DeviceName[MAX_PATH] = L"";
		DWORD CharCount = QueryDosDeviceW(&VolumeName[4], DeviceName, std::size(DeviceName));
		VolumeName[Index] = L'\\';

		if (CharCount == 0) {
			const DWORD Error = GetLastError();
			wprintf(L"QueryDosDeviceW failed with error code %d\n", Error);
			ExitFatal(1);
		}

		wprintf(L" Found a device: %s\n", DeviceName);
		fs::path r = GetAVolumePath(VolumeName);
		if (!r.empty()) {
			result.push_back(r);
 			wprintf(L"  Path: %s\n", r.wstring().c_str());
		}

		//  Move on to the next volume.
		const BOOL Success = FindNextVolumeW(FindHandle, VolumeName, std::size(VolumeName));

		if (!Success) {
			const DWORD Error = GetLastError();

			if (Error != ERROR_NO_MORE_FILES) {
				wprintf(L"FindNextVolumeW failed with error code %d\n", Error);
				ExitFatal(1);
			}

			//  Finished iterating through all the volumes.
			break;
		}
	}

	FindVolumeClose(FindHandle);

	return result;
}
#endif

static int CclListFilesystem(lua_State *l)
{
	LuaCheckArgs(l, 1);
	const std::string_view dir = LuaToString(l, 1);

#ifdef WIN32
	if (dir == "/") {
		std::vector<fs::path> vols = getVolumes();
		lua_newtable(l);
		int j = 0;
		for (auto const& vol: vols) {
			if (fs::exists(vol)) {
				lua_pushnumber(l, ++j);
				lua_pushstring(l, vol.generic_u8string().c_str());
				lua_settable(l, -3);
			}
		}
		return 1;
	}
#endif

	lua_newtable(l);
	int j = 0;
	for (auto const& dir_entry: fs::directory_iterator(fs::path(dir))) {
		if ((fs::is_regular_file(dir_entry.path()) || fs::is_directory(dir_entry.path())) && fs::exists(dir_entry.path())) {
			std::string name = dir_entry.path().generic_u8string();
			if (fs::is_directory(dir_entry.path())) {
				name += "/";
			}
			lua_pushnumber(l, ++j);
			lua_pushstring(l, name.c_str());
			lua_settable(l, -3);
		}
	}

	return 1;
}

void ScriptRegister()
{
	AliasRegister();
	#ifdef STRATAGUS_PAX
	lua_register(Lua, "PaxGameMenu", [](lua_State *) -> int {
		pax_stratagus_open_game_menu();
		return 0;
	});
	#endif

	lua_register(Lua, "LibraryPath", CclStratagusLibraryPath);
	lua_register(Lua, "ListDirectory", CclListDirectory);
	lua_register(Lua, "ListFilesInDirectory", CclListFilesInDirectory);
	lua_register(Lua, "ListDirsInDirectory", CclListDirsInDirectory);

	lua_register(Lua, "IgnoreVariableNameWhenSaving", CclIgnoreVariableNameWhenSaving);
	// TODO: Only allow this when extractor tool runs
	// lua_register(Lua, "ListFilesystem", CclListFilesystem);

	lua_register(Lua, "SetDamageFormula", CclSetDamageFormula);

	lua_register(Lua, "SavePreferences", CclSavePreferences);
	lua_register(Lua, "Load", CclLoad);
	lua_register(Lua, "LoadBuffer", CclLoadBuffer);

	lua_register(Lua, "DebugPrint", CclDebugPrint);

	lua_register(Lua, "RestartStratagus", CclRestartStratagus);
}

//@}
