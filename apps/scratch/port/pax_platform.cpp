#include "pax_port.h"
#include <filesystem.hpp>
#include <log.hpp>
#include <os.hpp>
#include <timer.hpp>
#include <blockExecutor.hpp>
#include <thread.hpp>
#include <algorithm>
#include <cstring>
#include <cstdio>
#ifdef PAX_HOST_TEST
#define SCRATCH_ROOT "/tmp/pax-scratch-test-sd/"
#else
#define SCRATCH_ROOT "/sd/roms/scratch/"
#endif
namespace OS {
bool toExit = false, loadedSettings = false;
std::string *customProjectsPath = nullptr;
bool init() { return true; }
void deinit() { delete customProjectsPath; customProjectsPath = nullptr; }
bool isEnhancedPlatform() { return false; }
std::string getFilesystemRootPrefix() { return "/sd"; }
std::string getConfigFolderLocation() { return SCRATCH_ROOT "settings/"; }
std::string getScratchFolderLocation() { return SCRATCH_ROOT "projects/"; }
std::string getRomFSLocation() { return SCRATCH_ROOT "romfs/"; }
std::string getPlatform() { return "PAX ESP32-P4"; }
bool isOnline() { return false; }
bool initWifi() { return false; }
void deInitWifi() {}
std::string getUsername() { return "Player"; }
}
namespace Log {
void log(std::string s) { pax_svc->log_printf("Scratch: %s\n", s.c_str()); }
void logWarning(std::string s) { log("Warning: " + s); }
void logError(std::string s) { log("Error: " + s); }
void logCritical(std::string s, bool fatal) { logError(s); if (fatal) pax_quit(1); }
void writeToFile(std::string) {}
void deleteLogFile() {}
}
namespace FileSystem {
bool fileExists(const std::string &path) { pax_file_stat_t st{}; return pax_svc->file_stat && pax_svc->file_stat(path.c_str(), &st) == 0; }
nonstd::expected<void,std::string> createDirectory(const std::string &path) {
 if (pax_svc->file_mkdir && pax_svc->file_mkdir(path.c_str()) == 0) return {};
 return nonstd::make_unexpected("Cannot create " + path);
}
void renameFile(const std::string &a,const std::string &b) { if(pax_svc->file_rename) pax_svc->file_rename(a.c_str(),b.c_str()); }
nonstd::expected<void,std::string> removeDirectory(const std::string &path) {
 // Only remove empty directories. Project loading does not require recursive deletion.
 if(pax_svc->file_remove && pax_svc->file_remove(path.c_str()) == 0) return {};
 return nonstd::make_unexpected("Cannot remove " + path);
}
std::string parentPath(const std::string &p) { auto n=p.find_last_of("/\\"); return n==std::string::npos ? "" : p.substr(0,n); }
nonstd::expected<std::vector<std::string>,std::string> listDirectory(const std::string &path) {
 std::vector<char> buf(65536,0); std::vector<std::string> out;
 if(!pax_svc->file_list_dir || pax_svc->file_list_dir(path.c_str(),buf.data(),buf.size())<0) return nonstd::make_unexpected("Cannot list " + path);
 size_t at=0; while(at<buf.size() && buf[at]) { size_t n=strnlen(buf.data()+at,buf.size()-at); if(n==buf.size()-at) return nonstd::make_unexpected("Directory listing truncated"); out.emplace_back(buf.data()+at,n); at+=n+1; }
 std::sort(out.begin(),out.end()); return out;
}
}
static long long projectClockUs;
static bool projectClockValid;
void pax_update_project_clock() { projectClockUs = pax_time_us() / 1000 * 1000; projectClockValid = true; }
static long long timerNow(const Timer *timer) {
 if (timer != &BlockExecutor::timer) return pax_time_us();
 if (!projectClockValid) pax_update_project_clock();
 return projectClockUs;
}
Timer::Timer(bool startNow):startTime(0) { if(startNow) start(); }
void Timer::start() { startTime=timerNow(this); }
uint64_t Timer::getTimeMs() { return (timerNow(this)-startTime)/1000; }
double Timer::getTimeMsDouble() { return (timerNow(this)-startTime)/1000.0; }
// The interpreter and mixer are pumped by one task; no concurrent SE operations.
struct SE_Mutex::Impl {};
SE_Mutex::SE_Mutex():impl(nullptr) {}
SE_Mutex::~SE_Mutex() {}
void SE_Mutex::init() {}
void SE_Mutex::lock() {}
void SE_Mutex::unlock() {}
bool SE_Mutex::tryLock() { return true; }
void SE_Thread::sleep(uint16_t ms) { pax_svc->delay_ms(ms); }
unsigned int SE_Thread::getCurrentThreadId() { return 1; }
