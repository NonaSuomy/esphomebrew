#define PAX_APP_SIDE 1
#include "pax_platform.h"
#include "pax_savegame.h"
#include "game.h"
#include "iolib.h"
#include "player.h"
#include "unit.h"
#include "unit_manager.h"
#include "map.h"
#include "results.h"
#include <cstdio>
#include <cstring>
#include <array>

static char loadPath[192];
static bool loadRequested;
static constexpr char complete[]="-- PAX native save complete v2\n";
static bool slot_name(int slot,char *name,size_t size) {
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	if (slot==3) { snprintf(name,size,"pax-regression.sav"); return true; }
#endif
	if (slot<0 || slot>=3) return false;
	snprintf(name,size,"pax-slot-%d.sav",slot+1); return true;
}
static bool slot_path(int slot,char *path,size_t size,const char *suffix=".gz") {
	char name[64]; if (!slot_name(slot,name,sizeof(name))) return false;
	snprintf(path,size,"%s/%s%s",PAX_STRATAGUS_DATA_ROOT,name,suffix); return true;
}
static bool file_exists(const char *path) {
	pax_file_stat_t info{};
	return _pax_svc && _pax_svc->file_stat &&
		_pax_svc->file_stat(path,&info)==0 && !info.is_dir && info.size>20;
}
static bool save_name(const char *input,char *name,size_t size) {
	if (!input || !*input || strlen(input)>=size-7 || input[0]=='.' || strstr(input,"..")) return false;
	for (const unsigned char *p=(const unsigned char *)input;*p;++p)
		if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') ||
			*p==' ' || *p=='_' || *p=='-' || *p=='.')) return false;
	strcpy(name,input); size_t length=strlen(name);
	if (length>3 && !strcmp(name+length-3,".gz")) { name[length-3]=0; length-=3; }
	if (length<4 || strcmp(name+length-4,".sav")) {
		if (length+4>=size) return false; strcat(name,".sav");
	}
	return !strstr(name,".pending") && !strstr(name,".backup") && !strstr(name,".deleted");
}
static void named_path(const char *name,char *path,size_t size,const char *suffix=".gz") {
	snprintf(path,size,"%s/%s%s",PAX_STRATAGUS_DATA_ROOT,name,suffix);
}
int pax_stratagus_save_named_exists(const char *input) {
	char name[64],path[192],backup[192]; if (!save_name(input,name,sizeof(name))) return false;
	named_path(name,path,sizeof(path)); named_path(name,backup,sizeof(backup),".backup.gz");
	return file_exists(path) || file_exists(backup);
}
int pax_stratagus_save_slot_exists(int slot) {
	char path[192],backup[192];
	return slot_path(slot,path,sizeof(path)) && slot_path(slot,backup,sizeof(backup),".backup.gz") &&
		(file_exists(path) || file_exists(backup));
}
static bool validate(const char *path) {
	// Streaming gzip reads verify its CRC without a second whole-save buffer.
	// A final marker rejects truncated or unrelated files before ending play.
	CFile file; if (file.open(path,CL_OPEN_READ)==-1) return false;
	char block[4096],tail[sizeof(complete)-1]{}; size_t total=0;
	int count; bool okay=true;
	while ((count=file.read(block,sizeof(block)))>0) {
		total+=count; if (total>16*1024*1024) { okay=false; break; }
		if ((size_t)count>=sizeof(tail)) memcpy(tail,block+count-sizeof(tail),sizeof(tail));
		else { memmove(tail,tail+count,sizeof(tail)-count); memcpy(tail+sizeof(tail)-count,block,count); }
	}
	return file.close()==0 && okay && count==0 && total>sizeof(tail) && !memcmp(tail,complete,sizeof(tail));
}
int pax_stratagus_save_named(const char *input) {
	char name[64],path[192],pending[192],backup[192];
	if (!save_name(input,name,sizeof(name)) || !_pax_svc || !_pax_svc->file_stat || !_pax_svc->file_rename || !_pax_svc->file_remove) return -1;
	named_path(name,path,sizeof(path)); named_path(name,pending,sizeof(pending),".pending.gz");
	named_path(name,backup,sizeof(backup),".backup.gz");
	// Save to a separate file; never truncate the current successful save.
	char temporary[80]; snprintf(temporary,sizeof(temporary),"%s.pending",name);
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: native save begin name=%s cycle=%lu\n",name,GameCycle);
	if (SaveGame(temporary)!=0 || !validate(pending)) return -1;
	const bool exists=file_exists(path);
	if (exists) {
		pax_file_stat_t info{};
		if (_pax_svc->file_stat(backup,&info)==0 && _pax_svc->file_remove(backup)!=0) return -1;
		if (_pax_svc->file_rename(path,backup)!=0) return -1;
	}
	if (_pax_svc->file_rename(pending,path)!=0) {
		if (exists) _pax_svc->file_rename(backup,path);
		return -1;
	}
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: native save complete path=%s\n",path);
	return 0;
}
int pax_stratagus_save_slot(int slot) {
	char name[64]; return slot_name(slot,name,sizeof(name))?pax_stratagus_save_named(name):-1;
}
int pax_stratagus_load_named(const char *input) {
	char name[64],path[192]; if (!save_name(input,name,sizeof(name))) return -1;
	named_path(name,path,sizeof(path));
	if (!validate(path)) {
		// Recover the last completed save if power failed during promotion,
		// or if the active file has subsequently become truncated/corrupt.
		named_path(name,path,sizeof(path),".backup.gz"); if (!validate(path)) return -1;
	}
	strcpy(loadPath,path); loadRequested=true;
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: native load requested name=%s\n",name);
	return 0;
}
int pax_stratagus_request_load_slot(int slot) {
	char name[64]; return slot_name(slot,name,sizeof(name))?pax_stratagus_load_named(name):-1;
}
int pax_stratagus_delete_named(const char *input) {
	char name[64],path[192];
	if (!save_name(input,name,sizeof(name)) || !_pax_svc || !_pax_svc->file_stat || !_pax_svc->file_remove) return -1;
	bool removed=false;
	for (const char *suffix:{".gz",".backup.gz",".pending.gz"}) {
		named_path(name,path,sizeof(path),suffix);
		pax_file_stat_t info{};
		if (_pax_svc->file_stat(path,&info)==0) {
			if (info.is_dir || _pax_svc->file_remove(path)!=0) return -1; removed=true;
		}
	}
	return removed?0:-1;
}
int pax_stratagus_list_saves(char names[][64],int capacity) {
	if (!_pax_svc || !_pax_svc->file_list_dir || capacity<=0) return 0;
	char entries[8192]{}; const int count=_pax_svc->file_list_dir(PAX_STRATAGUS_DATA_ROOT,entries,sizeof(entries));
	int found=0; size_t offset=0;
	for (int i=0;i<count && found<capacity && offset<sizeof(entries);++i) {
		const char *entry=entries+offset; size_t length=0;
		while (length<sizeof(entries)-offset && entry[length]) ++length;
		if (length==sizeof(entries)-offset) break; offset+=length+1;
		if (length<7 || strcmp(entry+length-7,".sav.gz") || !strcmp(entry,"pax-regression.sav.gz")) continue;
		char name[64]; if (!save_name(entry,name,sizeof(name))) continue;
		snprintf(names[found++],64,"%s.gz",name);
	}
	for (int i=0;i<found;++i) for (int j=i+1;j<found;++j) if (strcmp(names[i],names[j])>0) {
		char swap[64]; strcpy(swap,names[i]); strcpy(names[i],names[j]); strcpy(names[j],swap);
	}
	return found;
}
const char *pax_stratagus_take_load_path() {
	if (!loadRequested) return nullptr; loadRequested=false; return loadPath;
}
bool pax_stratagus_reload_pending() { return loadRequested; }
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
static bool regressionPending;
static unsigned long savedCycle;
static int savedMinerals,savedGas,savedSupply,savedDemand;
static unsigned long long savedState;
static unsigned long long savedMap;
static std::array<unsigned long long,7> savedMapComponents;
static std::array<unsigned long long,7> map_components() {
	std::array<unsigned long long,7> hashes; hashes.fill(1469598103934665603ULL);
	for (size_t i=0;i<Map.Fields.size();++i) {
		const CMapField &tile=Map.Fields[i];
		const unsigned fields[]={unsigned(tile.getGraphicTile()),unsigned(tile.playerInfo.SeenTile),unsigned(tile.Value),
			unsigned(tile.getMoveCost()),unsigned(tile.getTileIndex()),unsigned(tile.getElevation()),unsigned(tile.Flags)};
		for (int i=0;i<7;++i) hashes[i]=(hashes[i]^fields[i])*1099511628211ULL;
	}
	return hashes;
}
static unsigned long long map_state() {
	unsigned long long value=1469598103934665603ULL;
	for (size_t i=0;i<Map.Fields.size();++i) {
		const CMapField &tile=Map.Fields[i];
		const unsigned fields[]={unsigned(tile.getGraphicTile()),unsigned(tile.playerInfo.SeenTile),unsigned(tile.Value),
			unsigned(tile.getMoveCost()),unsigned(tile.getTileIndex()),unsigned(tile.getElevation()),unsigned(tile.Flags)};
		for (unsigned field:fields) value=(value^field)*1099511628211ULL;
	}
	return value;
}
static unsigned long long unit_state() {
	unsigned long long value=1469598103934665603ULL;
	for (const CUnit *unit:UnitManager->GetUnits()) {
		const int fields[]={int(UnitNumber(*unit)),unit->Player->Index,unit->tilePos.x,unit->tilePos.y,
			unit->IX,unit->IY,unit->Variable[HP_INDEX].Value,int(unit->Orders.size())};
		for (int field:fields) value=(value^(unsigned)field)*1099511628211ULL;
	}
	return value;
}
#endif
void pax_stratagus_save_selftest() {
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	static bool tested; if (tested) return; tested=true;
	savedCycle=GameCycle; savedMinerals=ThisPlayer->Resources[1]; savedGas=ThisPlayer->Resources[2];
	savedSupply=ThisPlayer->Supply; savedDemand=ThisPlayer->Demand; savedState=unit_state();
	savedMap=map_state();
	savedMapComponents=map_components();
	if (pax_stratagus_save_slot(3)!=0) {
		_pax_svc->log_printf("Stargus: native save/load regression FAIL saving\n"); return;
	}
	ThisPlayer->Resources[1]+=321;
	regressionPending=pax_stratagus_request_load_slot(3)==0;
	if (regressionPending) StopGame(GameQuitToMenu);
#endif
}
void pax_stratagus_save_reload_completed() {
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: native load complete cycle=%lu\n",GameCycle);
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
	if (regressionPending) {
		const bool okay=GameCycle==savedCycle && ThisPlayer->Resources[1]==savedMinerals &&
			ThisPlayer->Resources[2]==savedGas && ThisPlayer->Supply==savedSupply &&
			ThisPlayer->Demand==savedDemand && unit_state()==savedState && map_state()==savedMap;
		_pax_svc->log_printf("Stargus: restored state cycle=%d resources=%d supply=%d units=%d terrain=%d\n",
			GameCycle==savedCycle,ThisPlayer->Resources[1]==savedMinerals && ThisPlayer->Resources[2]==savedGas,
			ThisPlayer->Supply==savedSupply && ThisPlayer->Demand==savedDemand,unit_state()==savedState,map_state()==savedMap);
		const auto map=map_components();
		_pax_svc->log_printf("Stargus: restored terrain graphic=%d seen=%d value=%d move=%d index=%d elevation=%d flags=%d\n",
			map[0]==savedMapComponents[0],map[1]==savedMapComponents[1],map[2]==savedMapComponents[2],
			map[3]==savedMapComponents[3],map[4]==savedMapComponents[4],map[5]==savedMapComponents[5],map[6]==savedMapComponents[6]);
		_pax_svc->log_printf("Stargus: native save/load regression %s cycle=%lu minerals=%d supply=%d/%d\n",
			okay?"PASS":"FAIL",GameCycle,ThisPlayer->Resources[1],ThisPlayer->Demand,ThisPlayer->Supply);
		regressionPending=false;
	}
#endif
}
