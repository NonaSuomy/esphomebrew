#define PAX_APP_SIDE 1

#include "pax_platform.h"
#include "pax_render_profile.h"
#include <algorithm>
#include <SDL.h>
#include "network.h"
#include "netconnect.h"
#include "online_service.h"
#include "video.h"
#include "game.h"
#include "interface.h"
#include "cursor.h"
#include "commands.h"
#include "unit.h"
#include "unittype.h"
#include "ui.h"
#include "widgets.h"
#include "movie.h"
#include "pax_menu.h"
#include "pax_savegame.h"
#include "pax_results.h"
#include "player.h"
#include "iolib.h"
#include "results.h"
#include "viewport.h"
#include "replay.h"
#include "map.h"
#include "script.h"
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
#include "sound.h"
#endif
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
#include "player.h"
#include "unitptr.h"
#endif

#include <cstring>
#include <cstdio>
#ifdef PAX_STRATAGUS_MIDI_WAR1
extern "C" int pax_stratagus_midi_soundfont(const char *);
#endif

void pax_stratagus_results_capture()
{
	pax_results = {};
	pax_results.outcome = GameResult == GameVictory ? 1 : GameResult == GameDefeat ? 2 : GameResult == GameDraw ? 3 : 0;
	if (!pax_results.outcome) return;
	pax_results.seconds = GameCycle / CyclesPerSecond;
	for (int i=0; i<8; ++i) {
		const auto &p=Players[i];
		if (!p.TotalUnits) continue;
		auto &r=pax_results.rows[pax_results.count++];
		snprintf(r.name,sizeof(r.name),"%.20s%s",p.Name.c_str(), &p==ThisPlayer ? " (You)" : "");
		r.units=p.TotalUnits; r.buildings=p.TotalBuildings;
		r.minerals=p.TotalResources[1]; r.gas=p.TotalResources[2];
		r.kills=p.TotalKills; r.razings=p.TotalRazings;
	}
}

#ifdef PAX_STRATAGUS_WARCRAFT
static_assert(static_cast<decltype(CVariable::Increase)>(-1)<0,
	"Spell/HP decay must remain signed on the ESP32 toolchain");
void pax_warcraft_health_trace(const CUnit &unit, const CUnit *attacker, int damage, const void *caller)
{
	static unsigned count;
	if (count++ >= 32 || !_pax_svc || !_pax_svc->log_printf) return;
	_pax_svc->log_printf("Warcraft: health cycle=%lu unit=%s player=%d tile=%d,%d HP=%d/%d increase=%d TTL=%lu poison=%d damage=%d attacker=%s caller=%p\n",
		GameCycle,unit.Type->Ident.c_str(),unit.Player->Index,unit.tilePos.x,unit.tilePos.y,
		unit.Variable[HP_INDEX].Value,unit.Variable[HP_INDEX].Max,unit.Variable[HP_INDEX].Increase,
		unit.TTL,unit.Variable[POISON_INDEX].Value,damage,attacker ? attacker->Type->Ident.c_str():"none",caller);
}
#endif

class CUDPSocket_Impl {};
class CTCPSocket_Impl {};

CHost::CHost(const std::string &, int) : ip(0), port(0) {}
std::string CHost::toString() const { return {}; }
bool CHost::isValid() const { return false; }

CUDPSocket::CUDPSocket() = default;
CUDPSocket::~CUDPSocket() = default;
bool CUDPSocket::Open(const CHost &) { return false; }
void CUDPSocket::Close() {}
void CUDPSocket::Send(const CHost &, const void *, unsigned int) {}
int CUDPSocket::Recv(void *, int, CHost *) { return -1; }
void CUDPSocket::SetNonBlocking() {}
int CUDPSocket::HasDataToRead(int) { return 0; }
bool CUDPSocket::IsValid() const { return false; }
std::vector<unsigned long> CUDPSocket::GetSocketAddresses() { return {}; }

CTCPSocket::CTCPSocket() = default;
CTCPSocket::~CTCPSocket() = default;
bool CTCPSocket::Open(const CHost &) { return false; }
void CTCPSocket::Close() {}
bool CTCPSocket::Connect(const CHost &) { return false; }
int CTCPSocket::Send(const void *, unsigned int) { return -1; }
int CTCPSocket::Recv(void *, int) { return -1; }
void CTCPSocket::SetNonBlocking() {}
int CTCPSocket::HasDataToRead(int) { return 0; }
bool CTCPSocket::IsValid() const { return false; }

CNetworkParameter::CNetworkParameter() : localPort(defaultPort), gameCyclesPerUpdate(0), NetworkLag(0), timeoutInS(0) {}
CNetworkParameter CNetworkParameter::Instance;
CUDPSocket NetworkFildes;
bool NetworkInSync = true;
// Zero means local single-player in Stratagus. A nonzero value assigns
// ThisPlayer from NetLocalPlayerNumber, which otherwise selects empty slot 0
// instead of this campaign's human slot 1 and immediately triggers defeat.
int NetPlayers = 0;
CNetworkHost Hosts[PlayerMax];
int NetConnectRunning = 0;
int NetConnectType = 0;
int NetLocalHostsSlot = 0;
int NetLocalPlayerNumber = 0;
std::string NetworkMapName;
std::string NetworkMapFragmentName;
CServerSetup ServerSetupState;
CServerSetup LocalSetupState;
bool NoRandomPlacementMultiplayer = false;

void CServerSetup::Clear()
{
	ServerGameSettings.Init();
	for (int i = 0; i < PlayerMax; ++i) {
		CompOpt[i] = SlotOption::Available;
		Ready[i] = 0;
	}
}

void CServerSetup::Save(const std::function<void(std::string)> &) {}
void InitNetwork1() {}
void ExitNetwork1() {}
void NetworkOnStartGame() { CommandLogDisabled = true; }
void NetworkEvent() {}
void NetworkSync() {}
void NetworkQuitGame() {}
void NetworkRecover() {}
void NetworkCommands() {}
void NetworkSendChatMessage(const std::string &) {}
void NetworkSendCommand(int command, const CUnit &unit, int x, int y,
                        const CUnit *dest, const CUnitType *type, EFlushMode flush)
{
	// Even local play routes UI orders through the network command transport.
	// With multiplayer excluded, execute its native decoder locally instead
	// of silently discarding every movement/attack/button command.
	const UnitRef destination = dest ? UnitNumber(*dest) : type ? type->Slot : 0xFFFF;
	CommandLogDisabled = true;
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: local command=%d unit=%u destination=%d,%d begin\n", command, (unsigned)UnitNumber(unit), x, y);
#endif
	ExecCommand(command | (flush == EFlushMode::On ? 0x80 : 0), UnitNumber(unit), x, y, destination);
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: local command complete\n");
#endif
}
void NetworkSendExtendedCommand(int command, int arg1, int arg2, int arg3, int arg4, int status)
{ ExecExtendedCommand(command, status, arg1, arg2, arg3, arg4); }
void NetworkSendSelection(CUnit **, int) {}
#ifdef PAX_STRATAGUS_WARCRAFT
static int CclPaxIsNetworkGame(lua_State *state)
{
	LuaCheckArgs(state,0);
	lua_pushboolean(state,IsNetworkGame());
	return 1;
}
#endif
void NetworkCclRegister()
{
#ifdef PAX_STRATAGUS_MIDI_WAR1
	lua_register(Lua,"SetMidiSoundFont",[](lua_State *state)->int {
		LuaCheckArgs(state,1);
		const std::string name(LuaToString(state,1));
		lua_pushboolean(state,pax_stratagus_midi_soundfont(name.c_str()));
		return 1;
	});
#endif
#ifdef PAX_STRATAGUS_WARCRAFT
	// The transport bindings are excluded, but stock single-player Lua still
	// queries this when setting fog, choosing unit rules, and opening menus.
	lua_register(Lua,"IsNetworkGame",CclPaxIsNetworkGame);
#endif
}
void NetworkServerStartGame() {}
void NetworkGamePrepareGameSettings() {}
int GetNetworkState() { return 0; }
void NetworkInitClientConnect() {}
void NetworkInitServerConnect(int) {}
void NetworkProcessClientRequest() {}
void NetworkProcessServerRequest() {}
void NetworkServerResyncClients() {}
void NetworkDetachFromServer() {}
int NetworkSetupServerAddress(const std::string &, int) { return -1; }
void OnlineServiceCclRegister() {}
void InitOnlineService() {}
void DeInitOnlineService() {}
std::unique_ptr<OnlineContext> OnlineContextHandler;

SDL_Window *TheWindow = nullptr;
SDL_Renderer *TheRenderer = nullptr;
SDL_Surface *TheScreen = nullptr;
SDL_Texture *TheTexture = nullptr;
int RefreshRate = 60;
uint8_t SizeChangeCounter = 0;
static const EventCallback *s_callbacks = nullptr;
extern "C" SDL_Surface *pax_stratagus_screen_create(int w, int h);

void SetCallbacks(const EventCallback *callbacks) { s_callbacks = callbacks; }
const EventCallback *GetCallbacks() { return s_callbacks; }

void InitVideoSdl()
{
	// Native line/fog callbacks and the screen must use the panel's RGB565.
	Video.Depth = 16;
	if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("Stargus: InitVideoSdl begin\n");
	TheWindow = SDL_CreateWindow("Stratagus PAX", 0, 0, 800, 480, SDL_WINDOW_SHOWN);
	if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("Stargus: window=%p\n", (void *)TheWindow);
	TheRenderer = SDL_CreateRenderer(TheWindow, -1, SDL_RENDERER_SOFTWARE);
	/* Stratagus allocates the correctly sized texture in ResizeScreen().
	 * Avoid a second large PSRAM allocation during the initial window setup. */
	TheTexture = nullptr;
	TheScreen = pax_stratagus_screen_create(320,200);
	ColorBlack = Video.MapRGB(TheScreen->format, 0, 0, 0);
	ColorDarkGreen = Video.MapRGB(TheScreen->format, 48, 100, 4);
	ColorLightBlue = Video.MapRGB(TheScreen->format, 52, 113, 166);
	ColorBlue = Video.MapRGB(TheScreen->format, 0, 0, 252);
	ColorOrange = Video.MapRGB(TheScreen->format, 248, 140, 20);
	ColorWhite = Video.MapRGB(TheScreen->format, 252, 248, 240);
	ColorLightGray = Video.MapRGB(TheScreen->format, 192, 192, 192);
	ColorGray = Video.MapRGB(TheScreen->format, 128, 128, 128);
	ColorDarkGray = Video.MapRGB(TheScreen->format, 64, 64, 64);
	ColorRed = Video.MapRGB(TheScreen->format, 252, 0, 0);
	ColorGreen = Video.MapRGB(TheScreen->format, 0, 252, 0);
	ColorYellow = Video.MapRGB(TheScreen->format, 252, 252, 0);
	if (_pax_svc && _pax_svc->log_printf) _pax_svc->log_printf("Stargus: renderer=%p texture=%p\n", (void *)TheRenderer, (void *)TheTexture);
}
void RealizeVideoMemory()
{
	PaxRenderScope profile(PAX_PRESENT);
	if (GameRunning && UI.SelectedViewport) {
		const auto &v=*UI.SelectedViewport;
		pax_stratagus_audio_view(v.MapPos.x*PixelTileSize.x+v.Offset.x,v.MapPos.y*PixelTileSize.y+v.Offset.y,
			v.BottomRightPos.x-v.TopLeftPos.x+1,v.BottomRightPos.y-v.TopLeftPos.y+1);
	} else pax_stratagus_audio_view(0,0,0,0);
	if (TheScreen && TheScreen->pixels) {
		++FrameCounter;
		pax_stratagus_video_present565(TheScreen->pixels, Video.Width, Video.Height);
#ifdef PAX_STRATAGUS_WARCRAFT
		static unsigned menuFrames;
		if (!GameRunning && _pax_svc->log_printf && (++menuFrames==1 || menuFrames%120==0))
			_pax_svc->log_printf("Warcraft: native menu frame=%u size=%dx%d\n",menuFrames,Video.Width,Video.Height);
#endif
		static unsigned game_frames;
		if (GameRunning && _pax_svc && _pax_svc->log_printf) {
			++game_frames;
			if (game_frames == 1)
				_pax_svc->log_printf("Stargus: first game frame presented %dx%d\n", Video.Width, Video.Height);
			if (game_frames == 1) {
				_pax_svc->log_printf("Stargus: native RGB565 screen, target simulation=%d cycles/s\n", CyclesPerSecond);
				pax_stratagus_audio_start();
				pax_stratagus_pointer_start();
			}
			if (game_frames == 2 || game_frames % 30 == 0)
				_pax_svc->log_printf("Stargus: game video frame=%u cycle=%lu\n", game_frames, GameCycle);
			if (game_frames % 300 == 0 && Lua)
				_pax_svc->log_printf("Stargus: Lua runtime heap=%d KiB stack=%d\n",
					lua_gc(Lua,LUA_GCCOUNT,0),lua_gettop(Lua));
		}
	}
}
void pax_stratagus_video_shutdown(void)
{
	pax_stratagus_portrait_close();
	// The process normally releases these on desktop exit. A PAX returns
	// to a persistent launcher instead, so retain no 800x480 screen allocation.
	SDL_DestroyTexture(TheTexture); TheTexture = nullptr;
	SDL_FreeSurface(TheScreen); TheScreen = nullptr;
	SDL_DestroyRenderer(TheRenderer); TheRenderer = nullptr;
	SDL_DestroyWindow(TheWindow); TheWindow = nullptr;
	SDL_Quit();
}
void SetVideoSync() {}
static unsigned s_modal_generation;
std::vector<FileList> PaxReadDataDirectory(const fs::path &directory)
{
	std::vector<FileList> files;
	if (!_pax_svc || !_pax_svc->file_list_dir || !_pax_svc->file_stat) return files;
	char entries[8192]{};
	const std::string path=directory.string();
	const int count=_pax_svc->file_list_dir(path.c_str(),entries,sizeof(entries));
	size_t offset=0;
	for (int i=0;i<count && offset<sizeof(entries);++i) {
		const char *name=entries+offset;
		const size_t remaining=sizeof(entries)-offset;
		size_t length=0; while (length<remaining && name[length]) ++length;
		if (length==remaining) break;
		offset+=length+1;
		if (!length || !strcmp(name,".") || !strcmp(name,"..")) continue;
		pax_file_stat_t info{};
		const std::string full=(directory/name).string();
		if (_pax_svc->file_stat(full.c_str(),&info)!=0) continue;
		files.emplace_back(); files.back().name=name; files.back().type=info.is_dir?0:1;
	}
	std::sort(files.begin(),files.end());
	return files;
}
unsigned pax_stratagus_modal_generation() { return s_modal_generation; }
void pax_stratagus_open_game_menu()
{
	// Run a modal overlay without advancing the simulation. Always unpause
	// on Return/Esc, including the native HUD button's pre-paused callback.
	GamePaused=true;
	pax_stratagus_menu_set_tutorial(strstr(CurrentMapPath,"/tutorial/")!=nullptr);
	const int end=pax_stratagus_game_menu_run(TheScreen);
	GamePaused=false;
	++s_modal_generation;
	UI.StatusLine.Clear();
	if (end==40) {
		const bool okay=pax_stratagus_save_named(pax_stratagus_menu_save_name())==0;
		SetMessage("%s",okay?"Game saved to SD card.":"Save failed; previous save kept.");
	} else if (end==50) {
		if (pax_stratagus_load_named(pax_stratagus_menu_save_name())==0) StopGame(GameQuitToMenu);
		else SetMessage("%s","Load failed: missing, incomplete or corrupt save.");
	} else if (end>=10 && end<13) {
		const bool okay=pax_stratagus_save_slot(end-10)==0;
		SetMessage("%s",okay?"Game saved to SD card.":"Save failed; previous save kept.");
	} else if (end>=20 && end<23) {
		if (pax_stratagus_request_load_slot(end-20)==0) StopGame(GameQuitToMenu);
		else SetMessage("%s","Load failed: missing, incomplete or corrupt save.");
	} else if (end==1) StopGame(GameQuitToMenu);
	else if (end==2) StopGame(GameRestart);
	else if (end==3) { pax_exit_requested=1; StopGame(GameExit); }
}
void Invalidate() {}
void InvalidateArea(int, int, int, int) {}
int pax_stratagus_touch_button(int held)
{
	// Touch-only RTS controls: selecting a unit then tapping empty ground
	// issues its default order. Buttons and explicit target modes retain
	// left-click semantics even when held. A map hold can order a unit
	// target (attack/gather/etc), but must not cancel placement or a submenu.
	if (GameRunning && CursorState == CursorStates::Point &&
		!CursorBuilding && CurrentButtonLevel==0 &&
		UI.MouseViewport && UI.MouseViewport->IsInsideMapArea(CursorScreenPos) &&
		(held || (!Selected.empty() && !UnitUnderCursor))) return SDL_BUTTON_RIGHT;
	return SDL_BUTTON_LEFT;
}
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
static void InputSmokeTest()
{
	if (!GameRunning || !ThisPlayer || !UI.SelectedViewport || GameCycle < 2) return;
	static unsigned frame;
	static CUnit *marine;
	static PixelPos initial, point, topLeft, bottomRight;
	static int firstAnimationFrame;
	static bool animated;
	static PixelPos previous;
	static unsigned backwards;
	static CUnit *center;
	static unsigned initialScvs;
	static CUnit *builder, *constructed;
	static unsigned constructionStage, nextActionFrame, buildingIndex;
	static int initialSupply, savedBuildSpeed, savedTrainSpeed;
	static bool sawConstruction, sawStages;
	static int lastConstructionFrame;
	static unsigned initialMarines;
	static CUnitPtr combatEnemy;
	static int combatInitialHp;
	++frame;
	auto move = [](const PixelPos &pos) {
		SDL_Event event{}; event.type = SDL_MOUSEMOTION;
		event.motion.x = pos.x; event.motion.y = pos.y; SDL_PushEvent(&event);
	};
	auto button = [](int which, bool down) {
		SDL_Event event{}; event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		event.button.button = which; SDL_PushEvent(&event);
	};
	if (frame == 3) {
		for (CUnit *unit : ThisPlayer->GetUnits()) {
			if (unit->Type->Ident != "unit-terran-marine") continue;
			PixelPos pos = UI.SelectedViewport->MapToScreenPixelPos(unit->GetMapPixelPosCenter());
			if (!UI.SelectedViewport->IsInsideMapArea(pos)) continue;
			if (!marine) {
				marine = unit; point = pos; initial = unit->GetMapPixelPosCenter();
				topLeft = bottomRight = pos; firstAnimationFrame = unit->Frame;
			}
			topLeft.x = std::min(topLeft.x, pos.x-14); topLeft.y = std::min(topLeft.y, pos.y-14);
			bottomRight.x = std::max(bottomRight.x, pos.x+14); bottomRight.y = std::max(bottomRight.y, pos.y+14);
		}
		if (marine) {
			_pax_svc->log_printf("Stargus: input self-test tapping Marine at %d,%d\n", point.x, point.y);
			move(point); button(SDL_BUTTON_LEFT, true); button(SDL_BUTTON_LEFT, false);
		}
	} else if (marine && frame == 6) {
		_pax_svc->log_printf("Stargus: input self-test single selected=%u\n", (unsigned)Selected.size());
		move(topLeft); button(SDL_BUTTON_LEFT, true);
	} else if (marine && frame == 7) {
		move(bottomRight); button(SDL_BUTTON_LEFT, false);
	} else if (marine && frame == 10) {
		_pax_svc->log_printf("Stargus: input self-test rectangle selected=%u\n", (unsigned)Selected.size());
		move(PixelPos(point.x+64, point.y+32)); button(SDL_BUTTON_RIGHT, true); button(SDL_BUTTON_RIGHT, false);
		previous=marine->GetMapPixelPosCenter();
	} else if (marine && frame > 10 && frame <= 60) {
		animated |= marine->Frame != firstAnimationFrame;
		const PixelPos current=marine->GetMapPixelPosCenter();
		if ((current.x-previous.x)*2+(current.y-previous.y)<0) ++backwards;
		previous=current;
		if (frame == 60) {
			const PixelPos now = marine->GetMapPixelPosCenter();
			_pax_svc->log_printf("Stargus: input self-test %s: movement=%d,%d animation=%d selected=%u\n",
				(now != initial && animated && !Selected.empty()) ? "PASS" : "FAIL",
				now.x-initial.x, now.y-initial.y, animated, (unsigned)Selected.size());
			_pax_svc->log_printf("Stargus: movement regression backwards=%u\n",backwards);
		}
	} else if (frame==65) {
		// Exercise the native HUD menu callback and its Esc return path.
		move(PixelPos(UI.MenuButton.X+30,UI.MenuButton.Y+10));
		button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false);
		SDL_Event escape{}; escape.type=SDL_KEYDOWN; escape.key.keysym.sym=SDLK_ESCAPE; SDL_PushEvent(&escape);
		escape.type=SDL_KEYUP; SDL_PushEvent(&escape);
	} else if (frame==70) {
		for (CUnit *unit:ThisPlayer->GetUnits()) {
			if (unit->Type->Ident=="unit-terran-command-center") center=unit;
			if (unit->Type->Ident=="unit-terran-scv") ++initialScvs;
		}
		if (center) {
			UI.SelectedViewport->Center(center->GetMapPixelPosCenter());
			move(UI.SelectedViewport->MapToScreenPixelPos(center->GetMapPixelPosCenter()));
			button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false);
		}
	} else if (frame==73 && center) {
		_pax_svc->log_printf("Stargus: HUD regression panels=%u selected=%u center=%d\n",
			(unsigned)UI.InfoPanelContents.size(),(unsigned)Selected.size(),
			!Selected.empty() && Selected[0]==center);
		// Test-only funds, not a gameplay cheat in normal builds.
		ThisPlayer->Resources[1]=300;
		_pax_svc->log_printf("Stargus: production settings limits=%d/%d/%d allowed=%d supply=%d/%d\n",
			ThisPlayer->UnitLimit,ThisPlayer->BuildingLimit,ThisPlayer->TotalUnitLimit,
			ThisPlayer->Allow.Units[UnitTypeByIdent("unit-terran-scv").Slot],ThisPlayer->Demand,ThisPlayer->Supply);
		const auto &b=UI.ButtonPanel.Buttons[0]; move(PixelPos(b.X+18,b.Y+17));
		for (int i=0;i<6;++i) { button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false); }
	} else if (frame==76 && center) {
		unsigned queued=0; for (const auto &order:center->Orders) if (order->Action==UnitAction::Train) ++queued;
		_pax_svc->log_printf("Stargus: training regression queued=%u (expected 5)\n",queued);
	} else if (frame==80 && marine && STRATAGUS_PAX_TEST_MISSION==2) {
		// Diagnostic only: place one enemy near a stopped Marine. No attack
		// command is issued, so damage proves native automatic retaliation.
		CommandStopUnit(*marine);
		const auto &type=UnitTypeByIdent("unit-zerg-zergling");
		for (int dy=-8;dy<=8 && !combatEnemy;++dy) for (int dx=-8;dx<=8 && !combatEnemy;++dx) {
			const Vec2i pos=marine->tilePos+Vec2i(dx,dy);
			if (!Map.Info.IsPointOnMap(pos) || !UnitTypeCanBeAt(type,pos)) continue;
			combatEnemy=CUnitPtr(MakeUnitAndPlace(pos,type,&Players[4]));
			if (combatEnemy) { combatInitialHp=combatEnemy->Variable[HP_INDEX].Value; break; }
		}
		_pax_svc->log_printf("Stargus: combat regression started enemy=%d aggressive=%d can-attack=%d range=%d diplomacy=%d\n",
			bool(combatEnemy),marine->IsAggressive(),marine->Type->CanAttack,
			marine->Variable[ATTACKRANGE_INDEX].Value,ThisPlayer->IsEnemy(Players[4]));
	} else if (frame==180 && combatEnemy) {
		const int hp=combatEnemy->Variable[HP_INDEX].Value;
		_pax_svc->log_printf("Stargus: combat regression %s enemy-HP=%d->%d (no attack command)\n",
			hp<combatInitialHp?"PASS":"FAIL",combatInitialHp,hp);
		combatEnemy.Reset();
	} else if (frame==200) {
		CUnit *geyser=nullptr;
		for (CUnit *unit:Players[15].GetUnits())
			if (unit->Type->Ident=="unit-resource-vespene-geyser") { geyser=unit; break; }
		const bool valid=geyser && geyser->Type->Sprite && geyser->Type->Sprite->getSurface();
		if (valid) {
			UI.SelectedViewport->Center(geyser->GetMapPixelPosCenter());
			// Exercise the exact renderer that faulted, even if fog hides this
			// distant neutral unit. Also test a missing sprite safely returning.
			DrawUnitType(*geyser->Type,nullptr,0,0,PixelPos(160,80));
			DrawUnitType(*geyser->Type,geyser->Type->Sprite.get(),0,0,PixelPos(160,80));
		}
		if (geyser) _pax_svc->log_printf("Stargus: geyser scrolling regression %s sprite=%d\n",valid?"PASS":"FAIL",valid);
		else _pax_svc->log_printf("Stargus: geyser scrolling regression SKIP map has no geyser (use Boot Camp)\n");
	} else if (frame==230 && center) {
		UI.SelectedViewport->Center(center->GetMapPixelPosCenter());
	} else if (frame>=250 && frame<=775 && (frame-250)%75==0) {
		const int race=ThisPlayer->Race;
		CSound *sound=nullptr;
		switch((frame-250)/75) {
			case 0:sound=GameSounds.NotEnoughRes[race][1].Sound.get();break;
			case 1:sound=GameSounds.NotEnoughRes[race][2].Sound.get();break;
			case 2:sound=GameSounds.NotEnoughFood[race].Sound.get();break;
			case 3:sound=GameSounds.ResearchComplete[race].Sound.get();break;
			case 4:sound=GameSounds.PlacementError[race].Sound.get();break;
			case 5:sound=GameSounds.PlacementSuccess[race].Sound.get();break;
			case 6:sound=GameSounds.Click.Sound.get();break;
			case 7:sound=SoundForName("explosion-medium").get();break;
		}
		_pax_svc->log_printf("Stargus: audio native game-event regression step=%u mapped=%d\n",(frame-250)/75,sound && sound->PaxId);
		PlayGameSound(sound,255);
	} else if (frame==1000 && center) {
		unsigned scvs=0; for (CUnit *unit:ThisPlayer->GetUnits()) if (unit->Type->Ident=="unit-terran-scv") ++scvs;
		_pax_svc->log_printf("Stargus: training regression %s new-SCVs=%u\n",scvs>initialScvs?"PASS":"FAIL",scvs-initialScvs);
	}
	if (frame<1010) return;
	// Exercise the actual SCV submenu, native placement and completed
	// construction before asking the Barracks to produce a Marine. Speeds
	// and money below are diagnostic-only; the final build has none of them.
	auto click = [&](int index) {
		const auto &b=UI.ButtonPanel.Buttons[index];
		move(PixelPos(b.X+18,b.Y+17));
		button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false);
	};
	if (constructionStage==0) {
		for (CUnit *unit:ThisPlayer->GetUnits()) {
			if (!builder && unit->Type->Ident=="unit-terran-scv") builder=unit;
			if (unit->Type->Ident=="unit-terran-marine") ++initialMarines;
		}
		if (!builder) return;
		initialSupply=ThisPlayer->Supply;
		savedBuildSpeed=ThisPlayer->SpeedBuild; savedTrainSpeed=ThisPlayer->SpeedTrain;
		ThisPlayer->SpeedBuild=500; ThisPlayer->SpeedTrain=500; ThisPlayer->Resources[1]=1000;
		constructionStage=1; nextActionFrame=frame;
	}
	if (frame<nextActionFrame) return;
	if (constructionStage==1) {
		UI.SelectedViewport->Center(builder->GetMapPixelPosCenter());
		move(UI.SelectedViewport->MapToScreenPixelPos(builder->GetMapPixelPosCenter()));
		button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false);
		constructionStage=2; nextActionFrame=frame+3;
	} else if (constructionStage==2) {
		click(6); constructionStage=3; nextActionFrame=frame+3;
	} else if (constructionStage==3) {
		_pax_svc->log_printf("Stargus: construction submenu level=%d SCV=%d\n",
			CurrentButtonLevel,!Selected.empty() && Selected[0]==builder);
		click(buildingIndex?3:1); constructionStage=4; nextActionFrame=frame+3;
	} else if (constructionStage==4) {
		const char *id=buildingIndex?"unit-terran-barracks":"unit-terran-supply-depot";
		CUnitType &type=UnitTypeByIdent(id);
		if (CursorBuilding!=&type) {
			_pax_svc->log_printf("Stargus: construction regression FAIL missing placement cursor for %s\n",id);
			constructionStage=99; return;
		}
		bool found=false;
		for (int radius=4;radius<40 && !found;radius+=2) {
			for (int dy=-radius;dy<=radius && !found;dy+=2) {
				for (int dx=-radius;dx<=radius && !found;dx+=2) {
					const Vec2i pos=builder->tilePos+Vec2i(dx,dy);
					if (!Map.Info.IsPointOnMap(pos) || !Map.Info.IsPointOnMap(pos+Vec2i(type.TileWidth-1,type.TileHeight-1))) continue;
					bool explored=true;
					for (int y=0;y<type.TileHeight;++y) for (int x=0;x<type.TileWidth;++x)
						explored &= Map.Field(pos+Vec2i(x,y))->playerInfo.IsExplored(*ThisPlayer);
					const PixelPos point=UI.SelectedViewport->MapToScreenPixelPos(Map.TilePosToMapPixelPos_Center(pos));
					if (!explored || !UI.SelectedViewport->IsInsideMapArea(point) || !CanBuildUnitType(builder,type,pos,0)) continue;
					// Keep the click and whole footprint away from edge scrolling.
					if (!UI.SelectedViewport->IsInsideMapArea(PixelPos(point.x-24,point.y-24)) ||
						!UI.SelectedViewport->IsInsideMapArea(PixelPos(point.x+type.TileWidth*8+16,point.y+type.TileHeight*8+16))) continue;
					move(point); button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false);
					_pax_svc->log_printf("Stargus: construction placement %s tile=%d,%d\n",id,pos.x,pos.y);
					found=true;
				}
			}
		}
		if (!found) {
			_pax_svc->log_printf("Stargus: construction regression FAIL no visible free footprint for %s\n",id);
			constructionStage=99; return;
		}
		constructed=nullptr; sawConstruction=sawStages=false; lastConstructionFrame=-1;
		constructionStage=5; nextActionFrame=frame+3;
	} else if (constructionStage==5) {
		const char *id=buildingIndex?"unit-terran-barracks":"unit-terran-supply-depot";
		if (!constructed) for (CUnit *unit:ThisPlayer->GetUnits())
			if (unit->Type->Ident==id && unit->CurrentAction()==UnitAction::Built) { constructed=unit; break; }
		if (!constructed) return;
		if (constructed->CurrentAction()==UnitAction::Built) {
			sawConstruction=true;
			if (lastConstructionFrame>=0 && lastConstructionFrame!=constructed->Frame) sawStages=true;
			lastConstructionFrame=constructed->Frame;
			return;
		}
		const bool healthy=constructed->Variable[HP_INDEX].Value==constructed->Variable[HP_INDEX].Max;
		const bool supply=buildingIndex || ThisPlayer->Supply==initialSupply+8;
		_pax_svc->log_printf("Stargus: construction regression %s %s staged=%d HP=%d/%d supply=%d\n",
			healthy && supply && sawConstruction && sawStages?"PASS":"FAIL",id,sawStages,
			constructed->Variable[HP_INDEX].Value,constructed->Variable[HP_INDEX].Max,ThisPlayer->Supply);
		if (!buildingIndex) {
			buildingIndex=1; constructionStage=1; nextActionFrame=frame+3;
		} else {
			UI.SelectedViewport->Center(constructed->GetMapPixelPosCenter());
			move(UI.SelectedViewport->MapToScreenPixelPos(constructed->GetMapPixelPosCenter()));
			button(SDL_BUTTON_LEFT,true); button(SDL_BUTTON_LEFT,false);
			constructionStage=6; nextActionFrame=frame+3;
		}
	} else if (constructionStage==6) {
		click(0); constructionStage=7; nextActionFrame=frame+3;
	} else if (constructionStage==7) {
		unsigned marines=0; for (CUnit *unit:ThisPlayer->GetUnits())
			if (unit->Type->Ident=="unit-terran-marine") ++marines;
		if (marines<=initialMarines) return;
		ThisPlayer->SpeedBuild=savedBuildSpeed; ThisPlayer->SpeedTrain=savedTrainSpeed;
		_pax_svc->log_printf("Stargus: barracks production regression PASS new-Marines=%u\n",marines-initialMarines);
		constructionStage=99;
		pax_stratagus_save_selftest();
	}
}
#endif
#ifdef STRATAGUS_PAX_WARCRAFT_SMOKE_TEST
#include "pax_warcraft_test.h"
#endif
void WaitEventsOneFrame() {
	PaxRenderScope profile(PAX_INPUT_WAIT);
#ifdef STRATAGUS_PAX_WARCRAFT_SMOKE_TEST
	WarcraftInputSmokeTest();
#endif
#ifdef STRATAGUS_PAX_RESULTS_TEST
	static bool resultsTested;
	if (GameRunning && GameCycle>=2400 && !resultsTested) {
		resultsTested=true;
		_pax_svc->log_printf("Stargus: results regression ending diagnostic mission cycle=%lu\n",GameCycle);
		StopGame(GameVictory);
	}
#endif
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	InputSmokeTest();
#endif
	SDL_Event event;
	for (unsigned n = 0; n < 32 && !pax_exit_requested && SDL_PollEvent(&event); ++n) {
		if (!s_callbacks) continue;
#ifdef PAX_STRATAGUS_WARCRAFT
		const EventCallback *callbacksBeforeEvent=s_callbacks;
#endif
		const Uint32 ticks = event.common.timestamp ? event.common.timestamp : SDL_GetTicks();
		switch (event.type) {
			case SDL_MOUSEMOTION:
				InputMouseMove(*s_callbacks, ticks, event.motion.x, event.motion.y); break;
			case SDL_MOUSEBUTTONDOWN:
				InputMouseButtonPress(*s_callbacks, ticks, event.button.button); break;
			case SDL_MOUSEBUTTONUP:
				InputMouseButtonRelease(*s_callbacks, ticks, event.button.button); break;
			case SDL_KEYDOWN:
#ifndef PAX_STRATAGUS_WARCRAFT
				if (GameRunning && event.key.keysym.sym==SDLK_ESCAPE && CursorState==CursorStates::Point &&
					!CursorBuilding && CurrentButtonLevel==0) {
					pax_stratagus_open_game_menu(); break;
				}
#endif
				InputKeyButtonPress(*s_callbacks, ticks, event.key.keysym.sym,
					event.key.keysym.sym < 128 ? event.key.keysym.sym : 0); break;
			case SDL_KEYUP:
				InputKeyButtonRelease(*s_callbacks, ticks, event.key.keysym.sym,
					event.key.keysym.sym < 128 ? event.key.keysym.sym : 0); break;
			case SDL_USEREVENT:
				if (event.user.data1) ((void (*)())event.user.data1)(); break;
		}
#ifdef PAX_STRATAGUS_WARCRAFT
		// Match the desktop SDL pump: menu button callbacks are intentionally
		// empty; Guichan needs the raw event and one logic pass each frame.
		if (callbacksBeforeEvent==s_callbacks) handleInput(&event);
#endif
	}
	if (s_callbacks) {
		InputMouseTimeout(*s_callbacks, SDL_GetTicks());
		InputKeyTimeout(*s_callbacks, SDL_GetTicks());
		CursorAnimate(SDL_GetTicks());
	}
#ifdef PAX_STRATAGUS_WARCRAFT
	if (!pax_exit_requested) handleInput(nullptr);
#endif
	if (pax_exit_requested) GameRunning = false;
	// The fixed-rate game clock waits only for the remaining tick budget.
	// Menu loops still need a bounded yield, but a slow game frame must not
	// incur another unconditional 33 ms after drawing.
	if (!GameRunning) SDL_Delay(10);
}
void SdlLockScreen() {}
void SdlUnlockScreen() {}
const char *SdlKey2Str(int) { return "unknown"; }
int Str2SdlKey(const char *s) {
	if (!s || !*s) return SDLK_UNKNOWN;
	if (!s[1]) return (unsigned char)*s;
	if (!strcmp(s, "escape")) return SDLK_ESCAPE;
	if (!strcmp(s, "return")) return SDLK_RETURN;
	return SDLK_UNKNOWN;
}
void ToggleGrabMouse(int) {}
bool SdlGetGrabMouse() { return false; }
void ToggleFullScreen() {}
int PlayMovie(const std::string &name) {
	// Match the engine's no-Theora contract: images return nonzero for
	// ShowTitleImage; unsupported movies return zero, never PNG decoding.
	if (name.find(".ogg")!=std::string::npos || name.find(".ogv")!=std::string::npos ||
		name.find(".smk")!=std::string::npos) {
#ifndef STRATAGUS_PAX_WARCRAFT_SMOKE_TEST
		pax_stratagus_movie_play(TheScreen,name.c_str());
#endif
		return 0;
	}
	return -1;
}
