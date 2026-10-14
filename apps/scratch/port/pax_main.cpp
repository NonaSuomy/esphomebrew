#include "pax_editor.h"
#include "pax_port.h"
#include <atomic>
#include <audio.hpp>
#include <cstdlib>
#include <filesystem.hpp>
#include <input.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <setjmp.h>
#include <settings.hpp>
#include <unzip.hpp>
const app_services_t *pax_svc;
static jmp_buf quitJmp;
static std::atomic<bool> done(false);
static int result;
extern "C" long long pax_time_us() {
  return pax_svc ? pax_svc->get_time_us() : 0;
}
extern "C" void pax_quit(int code) {
  result = code;
  longjmp(quitJmp, 1);
}
int pax_editor_width = 800, pax_editor_height = 480;
void pax_choose_canvas() {
  int w = 800, h = 480;
  if (pax_svc->display_get_size && pax_svc->display_set_canvas) {
    pax_svc->display_get_size(&w, &h);
    // Old installed sidecars may still recommend 800x480. The user's full-
    // screen request should work when replacing only the binary as well.
    if (w == 800 && h == 480 && pax_svc->display_set_canvas(1024, 600) == 0) {
      w = 1024;
      h = 600;
    } else if (w < 800 || h < 480 || w > 1024 || h > 768 ||
               pax_svc->display_set_canvas(w, h) != 0) {
      w = 800;
      h = 480;
    }
  }
  pax_editor_width = w;
  pax_editor_height = h;
}
static std::string chooseProject() {
  if (!pax_resize_surface(pax_editor_width, pax_editor_height))
    return "";
  auto listed = FileSystem::listDirectory(OS::getScratchFolderLocation());
  std::vector<std::string> files;
  if (listed)
    for (auto f : *listed)
      if (f.size() > 4 && f.substr(f.size() - 4) == ".sb3")
        files.push_back(f);
  int selected = 0;
  bool wasA = true, wasUp = false, wasDown = false, wasTouch = true;
  while (!OS::toExit) {
    Input::getInput();
    pax_gamepad_state_t pad{};
    pax_svc->input_gamepad_read(&pad);
    auto held = [](const std::string &key) {
      return std::find(Input::inputKeys.begin(), Input::inputKeys.end(), key) != Input::inputKeys.end();
    };
    auto pressed = [](const std::string &key) {
      return std::find(Input::inputBuffer.begin(), Input::inputBuffer.end(), key) != Input::inputBuffer.end();
    };
    bool up = pad.values[PAX_INPUT_UP] || held("up arrow"),
         down = pad.values[PAX_INPUT_DOWN] || held("down arrow");
    if (!files.empty()) {
      if ((up && !wasUp) || pressed("up arrow"))
        selected = (selected + (int)files.size() - 1) % files.size();
      if ((down && !wasDown) || pressed("down arrow"))
        selected = (selected + 1) % files.size();
    }
    bool a = pad.values[PAX_INPUT_A] || pad.values[PAX_INPUT_START] ||
             std::find(Input::inputKeys.begin(), Input::inputKeys.end(),
                       "enter") != Input::inputKeys.end();
    if (((a && !wasA) || pressed("enter")) && !files.empty())
      return files[selected];
    if (pressed("n"))
      return "@new";
    if (pressed("e"))
      return files.empty() ? "@new" : "@edit:" + files[selected];
    bool touch = Input::mousePointer.isPressed;
    if (touch && !wasTouch) {
      if (Input::mousePointer.x < 86 && Input::mousePointer.y < 46)
        return "@new";
      if (Input::mousePointer.x > PAX_W - 100 && Input::mousePointer.y < 46)
        return files.empty() ? "@new" : "@edit:" + files[selected];
      int row = (Input::mousePointer.y - 85) / 30;
      int first = (selected / 7) * 7;
      if (Input::mousePointer.y >= 85 && row >= 0 && row < 7 &&
          first + row < (int)files.size())
        return "@edit:" + files[first + row];
    }
    wasA = a;
    wasUp = up;
    wasDown = down;
    wasTouch = touch;
    Render::beginFrame(0, 20, 25, 40);
    pax_draw_text("Scratch PAX", PAX_W / 2, 22, 0xffffffff, 1.3, true);
    Render::drawBox(74, 30, 45, 25, 45, 95, 125);
    pax_draw_text("New", 45, 16, 0xffffffff, .9, true);
    Render::drawBox(84, 30, PAX_W - 50, 25, 75, 95, 140);
    pax_draw_text("Edit", PAX_W - 50, 16, 0xffffffff, .8, true);
    pax_draw_text("Click a project to edit; arrows select; Enter plays", PAX_W / 2, 60,
                  0xc8d4e8ff, 1, true);
    int first = (selected / 7) * 7;
    for (int i = first; i < (int)files.size() && i < first + 7; i++) {
      int y = 85 + (i - first) * 30;
      if (i == selected)
        Render::drawBox(PAX_W - 40, 28, PAX_W / 2, y + 10, 45, 70, 110);
      pax_draw_text(files[i].substr(0, 35), 25, y, 0xffffffff, 1, false);
    }
    if (files.empty()) {
      pax_draw_text("Copy .sb3 files to", PAX_W / 2, 130, 0xffffffff, 1, true);
      pax_draw_text("/sd/roms/scratch/projects/", PAX_W / 2, 160, 0xffffffff, 1,
                    true);
    }
    pax_draw_text("N: new   E: edit selected   Hold Menu: exit",
                  PAX_W / 2, PAX_H - 35, 0xc8d4e8ff, .9, true);
    Render::endFrame();
    pax_svc->delay_ms(15);
  }
  return "";
}
bool pax_play_project(const std::string &path) {
  if (PAX_W != 480 || PAX_H != 360)
    if (!pax_resize_surface(480, 360))
      return false;
  Unzip::filePath = path;
  Render::beginFrame(0, 20, 25, 40);
  pax_draw_text("Loading project", 240, 150, 0xffffffff, 1, true);
  Render::endFrame();
  if (!Unzip::load()) {
    Scratch::cleanupScratchProject();
    return false;
  }
  std::string project = path.substr(path.find_last_of('/') + 1);
  if (!SettingsManager::getProjectSettings(
           project.substr(0, project.size() - 4))
           .contains("controls")) {
    Input::inputControls["dpadUp"] = "up arrow";
    Input::inputControls["dpadDown"] = "down arrow";
    Input::inputControls["dpadLeft"] = "left arrow";
    Input::inputControls["dpadRight"] = "right arrow";
    Input::inputControls["A"] = "space";
  }
  pax_update_project_clock();
  Scratch::startScratchProject();
  return true;
}
static int run() {
  pax_choose_canvas();
  FileSystem::createDirectory(OS::getScratchFolderLocation());
  FileSystem::createDirectory(OS::getConfigFolderLocation());
  if (!FileSystem::fileExists(OS::getRomFSLocation() +
                              "gfx/translations/languages.json")) {
    pax_svc->log_printf("Scratch: missing data; install scratch-data.zip to "
                        "/sd/roms/scratch/\n");
    return -2;
  }
  if (!Scratch::initializeRuntime())
    return -1;
  while (!OS::toExit) {
    auto project = chooseProject();
    if (project.empty())
      break;
    if (project == "@new") {
      PaxEdit::open("");
      continue;
    }
    if (project.rfind("@edit:", 0) == 0) {
      PaxEdit::open(OS::getScratchFolderLocation() + project.substr(6));
      continue;
    }
    pax_play_project(OS::getScratchFolderLocation() + project);
  }
  SoundPlayer::deinit();
  Render::deInit();
  OS::deinit();
  return 0;
}
#ifndef PAX_HOST_TEST
extern "C" {
typedef void (*init_fn)();
extern init_fn __init_array_start[], __init_array_end[];
}
static void appTask(void *) {
  if (setjmp(quitJmp) == 0) {
    for (auto fn = __init_array_start; fn < __init_array_end; fn++)
      (*fn)();
    result = run();
  }
  pax_close_all_files();
  pax_free_all_memory();
  done.store(true, std::memory_order_release);
  for (;;)
    pax_svc->delay_ms(1000);
}
extern "C" __attribute__((section(".text.entry"), used)) int
app_entry(const app_services_t *svc) {
  pax_svc = svc;
  if (!svc || svc->abi_version != PAX_ABI_VERSION || !svc->file_stat ||
      !svc->file_list_dir)
    return -1;
  done.store(false);
  result = 0;
  void *handle = nullptr;
  svc->log_printf("Scratch Everywhere! PAX starting\n");
  if (svc->task_create(appTask, "scratch", 256 * 1024, nullptr, 3, &handle,
                       0) != 0)
    return -1;
  while (!done.load(std::memory_order_acquire))
    svc->delay_ms(50);
  svc->task_delete(handle);
  return result;
}
#else
int pax_host_run() {
  if (setjmp(quitJmp) == 0)
    result = run();
  return result;
}
#endif
