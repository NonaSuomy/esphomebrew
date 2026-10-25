#pragma once
// Diagnostic builds only: drive stock SDL input, never call button callbacks
// directly. Normal PAXs contain neither automatic clicks nor test resources.
extern std::unique_ptr<gcn::Gui> Gui;
static SDL_Event warcraftRelease;
static Uint32 warcraftReleaseAt;
static bool warcraftReleasePending;

static void warcraft_click(const PixelPos &pos,int button=SDL_BUTTON_LEFT)
{
	SDL_Event event{}; event.type=SDL_MOUSEMOTION;
	event.motion.x=pos.x; event.motion.y=pos.y; SDL_PushEvent(&event);
	event={}; event.type=SDL_MOUSEBUTTONDOWN; event.button.button=button;
	event.button.state=SDL_PRESSED; event.button.x=pos.x; event.button.y=pos.y; SDL_PushEvent(&event);
	// Exercise a human-duration click across separate logic/render passes,
	// not just DOWN+UP in one batch as in the original smoke test.
	warcraftRelease=event; warcraftRelease.type=SDL_MOUSEBUTTONUP;
	warcraftRelease.button.state=SDL_RELEASED;
	warcraftReleaseAt=SDL_GetTicks()+120; warcraftReleasePending=true;
}
static gcn::Button *warcraft_find_button(gcn::Widget *widget,const char *caption,PixelPos &pos,int x=0,int y=0,unsigned *skip=nullptr)
{
	if (!widget) return nullptr;
	x+=widget->getX(); y+=widget->getY();
	if (auto *button=dynamic_cast<gcn::Button *>(widget)) {
		std::string text=button->getCaption();
		for (size_t i=0;(i=text.find("~!",i))!=std::string::npos;) text.erase(i,2);
		if (text==caption) {
			if (skip && *skip) --*skip;
			else { pos={x+button->getWidth()/2,y+button->getHeight()/2}; return button; }
		}
	}
	if (auto *container=dynamic_cast<gcn::Container *>(widget))
		for (auto *child:container->getChildren())
			if (auto *button=warcraft_find_button(child,caption,pos,x,y,skip)) return button;
	return nullptr;
}
static bool warcraft_has_briefing_overlay(gcn::Widget *widget)
{
	if (!widget) return false;
	// Stock Briefing() places a transparent full-screen ImageButton on top.
	if (dynamic_cast<CImageButton *>(widget) && widget->getWidth()==Video.Width &&
		widget->getHeight()==Video.Height) return true;
	if (auto *container=dynamic_cast<gcn::Container *>(widget))
		for (auto *child:container->getChildren())
			if (warcraft_has_briefing_overlay(child)) return true;
	return false;
}
static void WarcraftInputSmokeTest()
{
	const bool war2=strcmp(PAX_STRATAGUS_BANNER,"Wargus")==0;
	const char *farmName=war2 ? "unit-farm":"unit-human-farm";
	if (warcraftReleasePending && Sint32(SDL_GetTicks()-warcraftReleaseAt)>=0) {
		warcraftRelease.button.timestamp=SDL_GetTicks();
		SDL_PushEvent(&warcraftRelease); warcraftReleasePending=false;
	}
	static unsigned menuStage, gameStage;
	static Uint32 nextMenu;
	static CUnit *worker, *hall;
	static CUnitPtr farm;
	static PixelPos initial, target;
	static int initialFrame;
	static bool animated;
	static unsigned peasants;
	static unsigned long due;
	static unsigned long constructionDeadline;
	// Dismiss the stock startup Tips modal through SDL, just as a player does.
	// Otherwise the native game intentionally remains paused at cycle zero.
	if (war2 && GameRunning && GamePaused && Gui && SDL_GetTicks()>=nextMenu) {
		PixelPos close, next;
		if (warcraft_find_button(Gui->getTop(),"Next Tip",next) &&
			warcraft_find_button(Gui->getTop(),"Close (~<Esc~>)",close)) {
			_pax_svc->log_printf("Warcraft: input test closing startup Tips\n");
			warcraft_click(close); nextMenu=SDL_GetTicks()+2000; return;
		}
	}
	if (!GameRunning) {
		if (!Gui || !Gui->getTop() || SDL_GetTicks()<nextMenu) return;
#ifdef STRATAGUS_PAX_WARCRAFT_SOUND_TEST
#ifdef STRATAGUS_PAX_WARCRAFT_MULTIPLAYER_TEST
		const char *captions[]={"Multi Player Game","OK","Multi Player Game","",""};
		if (menuStage==2) {
			PixelPos returned;
			if (warcraft_find_button(Gui->getTop(),"Multi Player Game",returned)) {
				_pax_svc->log_printf("Warcraft: multiplayer notice test PASS opened, closed and returned to main menu\n");
				menuStage=5;
			}
			return;
		}
#else
		const char *captions[]={"Options","Sound (~<F7~>)","OK",
			strcmp(PAX_STRATAGUS_BANNER,"Wargus")==0 ? "Previous (~<Esc~>)":"Previous Menu","Options"};
		if (menuStage==4) {
			PixelPos returned;
			if (warcraft_find_button(Gui->getTop(),"Options",returned)) {
				_pax_svc->log_printf("Warcraft: sound menu test PASS opened, closed and returned to main menu\n");
				menuStage=5;
			}
			return;
		}
#endif
		if (menuStage==5) return;
#else
		const char *captions[]={"Single Player Game","Campaign Game",war2 ? "Alliance":"Human campaign",war2 ? "Start mission":"I. Regent","Continue"};
#endif
		const char *caption=captions[std::min(menuStage,4u)];
		PixelPos pos;
		unsigned skip=0;
#ifndef STRATAGUS_PAX_WARCRAFT_SOUND_TEST
		// Stock campaign list sorts Beyond the Dark Portal before Tides.
		// Choose the second Alliance button: Tides of Darkness, mission one.
		if (war2 && menuStage==2) skip=1;
#endif
		if (warcraft_find_button(Gui->getTop(),caption,pos,0,0,&skip)) {
			if (pos.x<0 || pos.y<0 || pos.x>=Video.Width || pos.y>=Video.Height) {
				_pax_svc->log_printf("Warcraft: input test BLOCKED offscreen button=%s at %d,%d canvas=%dx%d\n",caption,pos.x,pos.y,Video.Width,Video.Height);
				nextMenu=SDL_GetTicks()+10000; return;
			}
			_pax_svc->log_printf("Warcraft: input test SDL click stage=%u button=%s at %d,%d\n",menuStage,caption,pos.x,pos.y);
			if (menuStage<4) ++menuStage;
#ifdef STRATAGUS_PAX_WARCRAFT_SOUND_TEST
			if (menuStage==4) _pax_svc->log_printf("Warcraft: sound menu test clicked return\n");
#endif
			warcraft_click(pos); nextMenu=SDL_GetTicks()+2000;
		} else if (menuStage==4 && warcraft_has_briefing_overlay(Gui->getTop())) {
			_pax_svc->log_printf("Warcraft: input test SDL briefing screen click\n");
			warcraft_click({Video.Width/2,Video.Height/2}); nextMenu=SDL_GetTicks()+2000;
		}
		return;
	}
	if (worker && gameStage==2) animated|=worker->Frame!=initialFrame;
#ifdef STRATAGUS_PAX_WARCRAFT_EXIT_TEST
	// Reproduce Exit Program through the native HUD and Lua confirmation,
	// not the launcher's remote-close operation or direct callback calls.
	if (GameCycle<60 || SDL_GetTicks()<nextMenu) return;
	if (gameStage==0) {
		warcraft_click({UI.MenuButton.X+8,UI.MenuButton.Y+8});
		_pax_svc->log_printf("Warcraft: exit regression clicked in-game Menu\n");
		gameStage=1; nextMenu=SDL_GetTicks()+1500; return;
	}
	if (gameStage<=3 && Gui) {
		const char *caption=gameStage==1 ? "End Scenario":"Exit Program";
		PixelPos pos;
		if (warcraft_find_button(Gui->getTop(),caption,pos)) {
			warcraft_click(pos);
			_pax_svc->log_printf("Warcraft: exit regression stage=%u clicked %s\n",gameStage,caption);
			++gameStage; nextMenu=SDL_GetTicks()+1500;
		}
	}
	return;
#endif
	if (!ThisPlayer || !UI.SelectedViewport || GameCycle<due) return;
	auto select=[](CUnit *unit) {
		UI.SelectedViewport->Center(unit->GetMapPixelPosCenter());
		warcraft_click(UI.SelectedViewport->MapToScreenPixelPos(unit->GetMapPixelPosCenter()));
	};
	if (gameStage==0 && GameCycle>=10) {
		if (!war2) {
			unsigned poisoned=0;
			for (CUnit *unit:ThisPlayer->GetUnits()) poisoned+=unit->Variable[POISON_INDEX].Value!=0;
			_pax_svc->log_printf("Warcraft: input test initial poison %s poisoned=%u\n",poisoned ? "FAIL":"PASS",poisoned);
		}
		for (CUnit *unit:ThisPlayer->GetUnits()) {
			if (unit->Type->Ident=="unit-peasant") { ++peasants; if (!worker) worker=unit; }
			if (unit->Type->Ident==(war2 ? "unit-town-hall":"unit-human-town-hall")) hall=unit;
		}
		if (!worker) { _pax_svc->log_printf("Warcraft: input test FAIL no player peasant\n"); gameStage=99; return; }
		initial=worker->GetMapPixelPosCenter(); initialFrame=worker->Frame;
		select(worker); due=GameCycle+10; ++gameStage;
	} else if (gameStage==1) {
		_pax_svc->log_printf("Warcraft: input test selection %s count=%u\n",!Selected.empty() && Selected[0]==worker ? "PASS":"FAIL",unsigned(Selected.size()));
		bool found=false;
		for (int dy=-3;dy<=3 && !found;++dy) for (int dx=3;dx<=5 && !found;++dx) {
			Vec2i tile=worker->tilePos+Vec2i(dx,dy);
			if (Map.Info.IsPointOnMap(tile) && UnitTypeCanBeAt(*worker->Type,tile)) {
				target={tile.x*PixelTileSize.x+PixelTileSize.x/2,tile.y*PixelTileSize.y+PixelTileSize.y/2}; found=true;
			}
		}
		if (!found) { _pax_svc->log_printf("Warcraft: input test FAIL no move target\n"); gameStage=99; return; }
		warcraft_click(UI.SelectedViewport->MapToScreenPixelPos(target),SDL_BUTTON_RIGHT);
		due=GameCycle+180; ++gameStage;
	} else if (gameStage==2) {
		animated|=worker->Frame!=initialFrame;
		const PixelPos now=worker->GetMapPixelPosCenter();
		_pax_svc->log_printf("Warcraft: input test movement %s delta=%d,%d animation=%d\n",now!=initial ? "PASS":"FAIL",now.x-initial.x,now.y-initial.y,animated);
		if (!hall) { _pax_svc->log_printf("Warcraft: input test BLOCKED mission has no town hall\n"); gameStage=99; return; }
		select(hall); due=GameCycle+10; ++gameStage;
	} else if (gameStage==3) {
		// Diagnostic funds only; production still uses the native HUD button.
		ThisPlayer->Resources[1]=2000; ThisPlayer->Resources[2]=2000;
		const auto &button=UI.ButtonPanel.Buttons[0];
		warcraft_click({button.X+button.Style->Width/2,button.Y+button.Style->Height/2});
		due=GameCycle+10; ++gameStage;
	} else if (gameStage==4) {
		unsigned queued=0;
		for (const auto &order:hall->Orders) if (order->Action==UnitAction::Train) ++queued;
		_pax_svc->log_printf("Warcraft: input test production %s queued=%u initial-peasants=%u\n",queued ? "PASS":"FAIL",queued,peasants);
		select(worker); due=GameCycle+10; ++gameStage;
	} else if (gameStage==5 || gameStage==6) {
		const auto &button=UI.ButtonPanel.Buttons[gameStage==5 ? (war2 ? 6 : 4) : 0];
		warcraft_click({button.X+button.Style->Width/2,button.Y+button.Style->Height/2});
		due=GameCycle+10; ++gameStage;
	} else if (gameStage==7) {
		CUnitType &type=UnitTypeByIdent(farmName);
		if (CursorBuilding!=&type) {
			_pax_svc->log_printf("Warcraft: input test construction FAIL no farm placement cursor level=%d\n",CurrentButtonLevel);
			gameStage=99; return;
		}
		bool found=false;
		for (int radius=2;radius<=8 && !found;++radius)
		for (int dy=-radius;dy<=radius && !found;++dy) for (int dx=-radius;dx<=radius && !found;++dx) {
			if (std::abs(dx)!=radius && std::abs(dy)!=radius) continue;
			const Vec2i tile=worker->tilePos+Vec2i(dx,dy);
			if (!Map.Info.IsPointOnMap(tile) || !Map.Info.IsPointOnMap(tile+Vec2i(type.TileWidth-1,type.TileHeight-1))) continue;
			bool explored=true;
			for (int y=0;y<type.TileHeight;++y) for (int x=0;x<type.TileWidth;++x)
				explored&=Map.Field(tile+Vec2i(x,y))->playerInfo.IsExplored(*ThisPlayer);
			if (!explored) continue;
			const PixelPos pos=UI.SelectedViewport->MapToScreenPixelPos(Map.TilePosToMapPixelPos_Center(tile));
			if (!UI.SelectedViewport->IsInsideMapArea(pos) || !CanBuildUnitType(worker,type,tile,0)) continue;
			warcraft_click(pos); found=true;
			_pax_svc->log_printf("Warcraft: input test farm placement tile=%d,%d\n",tile.x,tile.y);
		}
		if (!found) { _pax_svc->log_printf("Warcraft: input test construction BLOCKED no valid visible site\n"); gameStage=99; return; }
			due=GameCycle+10; constructionDeadline=GameCycle+900; ++gameStage;
	} else if (gameStage==8) {
		for (CUnit *unit:ThisPlayer->GetUnits())
			if (unit->Type->Ident==farmName && unit->CurrentAction()==UnitAction::Built) { farm=unit; break; }
		// Placement orders first move the worker to the footprint. A one-second
		// check falsely fails valid distant sites before construction begins.
		if (!farm && GameCycle<constructionDeadline) { due=GameCycle+10; return; }
		_pax_svc->log_printf("Warcraft: input test construction %s\n",farm ? "PASS started farm":"FAIL no farm under construction");
		if (!farm) { gameStage=99; return; }
		constructionDeadline=GameCycle+3600;
		due=GameCycle+30; ++gameStage;
	} else if (gameStage==9) {
		const bool complete=farm->CurrentAction()!=UnitAction::Built &&
			farm->Variable[HP_INDEX].Value==farm->Variable[HP_INDEX].Max;
		if (!complete && GameCycle<constructionDeadline) { due=GameCycle+30; return; }
		_pax_svc->log_printf("Warcraft: input test farm completion %s HP=%d/%d supply=%d\n",
			complete ? "PASS":"FAIL",farm->Variable[HP_INDEX].Value,
			farm->Variable[HP_INDEX].Max,ThisPlayer->Supply);
		if (!war2) {
			unsigned alive=0,poisoned=0;
			for (CUnit *unit:ThisPlayer->GetUnits()) if (unit->Type->Ident=="unit-footman") {
				alive+=unit->IsAlive(); poisoned+=unit->Variable[POISON_INDEX].Value!=0;
			}
			_pax_svc->log_printf("Warcraft: input test footman survival cycle=%lu alive=%u poison=%u\n",GameCycle,alive,poisoned);
		}
		farm.Reset(); gameStage=99;
	}
}
