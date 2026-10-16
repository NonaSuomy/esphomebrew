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
  int selected = 0, first = 0;
  const int top = 86, rowHeight = 38;
  const int rows = std::max(1, (PAX_H - top - 64) / rowHeight);
  const int bottom = top + rows * rowHeight;
  const int trackX = PAX_W - 30, trackWidth = 18;
  const int count = (int)files.size(), maxFirst = std::max(0, count - rows);
  bool wasA = true, wasUp = false, wasDown = false, wasTouch = true;
  bool dragging = false;
  int grabOffset = 0;
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
    bool navigated = false;
    if (count) {
      if ((up && !wasUp) || pressed("up arrow")) {
        selected = (selected + count - 1) % count;
        navigated = true;
      }
      if ((down && !wasDown) || pressed("down arrow")) {
        selected = (selected + 1) % count;
        navigated = true;
      }
      if (navigated) {
        if (selected < first) first = selected;
        if (selected >= first + rows) first = selected - rows + 1;
      }
    }
    first = std::clamp(first - Input::mouseScrollDelta * 3, 0, maxFirst);
    // Use canvas coordinates, including when returning from a stage preview.
    auto point = Input::getTouchPosition();
    int x = point[0], y = point[1];
    bool touch = Input::mousePointer.isPressed;
    bool newPress = touch && !wasTouch;
    int trackHeight = bottom - top;
    int thumbHeight = count > rows ? std::max(28, trackHeight * rows / count) : trackHeight;
    int thumbTop = top + (maxFirst ? first * (trackHeight - thumbHeight) / maxFirst : 0);
    if (!touch) dragging = false;
    // Pointer actions take precedence over a coincident A/Enter report.
    if (newPress && x >= trackX && x < trackX + trackWidth && y >= top && y < bottom) {
      if (maxFirst) {
        dragging = true;
        grabOffset = y >= thumbTop && y < thumbTop + thumbHeight ? y - thumbTop : thumbHeight / 2;
      }
    }
    if (dragging && touch) {
      first = std::clamp((y - top - grabOffset) * maxFirst /
                         std::max(1, trackHeight - thumbHeight), 0, maxFirst);
    } else if (newPress) {
      if (x >= 8 && x < 86 && y >= 8 && y < 46) return "@new";
      if (x >= PAX_W - 190 && x < PAX_W - 104 && y >= 8 && y < 46 && count)
        return files[selected];
      if (x >= PAX_W - 100 && x < PAX_W - 8 && y >= 8 && y < 46)
        return count ? "@edit:" + files[selected] : "@new";
      if (x >= 18 && x < trackX - 10 && y >= top && y < bottom) {
        int index = first + (y - top) / rowHeight;
        if (index < count) return "@edit:" + files[index];
      }
    }
    bool a = pad.values[PAX_INPUT_A] || pad.values[PAX_INPUT_START] || held("enter");
    if (!touch && ((a && !wasA) || pressed("enter")) && count)
      return files[selected];
    if (pressed("n")) return "@new";
    if (pressed("e")) return count ? "@edit:" + files[selected] : "@new";
    wasA = a;
    wasUp = up;
    wasDown = down;
    wasTouch = touch;
    Render::beginFrame(0, 20, 25, 40);
    pax_draw_text("Scratch PAX", PAX_W / 2, 22, 0xffffffff, 1.3, true);
    Render::drawBox(74, 30, 45, 25, 45, 95, 125);
    pax_draw_text("New", 45, 16, 0xffffffff, .9, true);
    Render::drawBox(82, 30, PAX_W - 147, 25, 45, 125, 90);
    pax_draw_text("Play", PAX_W - 147, 16, 0xffffffff, .8, true);
    Render::drawBox(84, 30, PAX_W - 54, 25, 75, 95, 140);
    pax_draw_text("Edit", PAX_W - 54, 16, 0xffffffff, .8, true);
    pax_draw_text("Click a project to edit; arrows select; Enter plays", PAX_W / 2, 60,
                  0xc8d4e8ff, 1, true);
    for (int i = first; i < count && i < first + rows; i++) {
      int y = top + (i - first) * rowHeight;
      if (i == selected)
        Render::drawBox(trackX - 28, rowHeight - 4, (trackX + 8) / 2, y + rowHeight / 2, 45, 70, 110);
      std::string name = files[i];
      float width = 0, height = 0;
      pax_font_measure(name, 1, width, height);
      while (width > trackX - 58 && name.size() > 4) {
        name.resize(name.size() - 4);
        name += "...";
        pax_font_measure(name, 1, width, height);
      }
      pax_draw_text(name, 25, y + 8, 0xffffffff, 1, false);
    }
    Render::drawBox(trackWidth, trackHeight, trackX + trackWidth / 2, top + trackHeight / 2, 35, 47, 66);
    thumbTop = top + (maxFirst ? first * (trackHeight - thumbHeight) / maxFirst : 0);
    Render::drawBox(trackWidth - 6, thumbHeight, trackX + trackWidth / 2,
                    thumbTop + thumbHeight / 2, maxFirst ? 149 : 75, maxFirst ? 173 : 92, maxFirst ? 211 : 119);
    if (files.empty()) {
      pax_draw_text("Copy .sb3 files to", PAX_W / 2, 130, 0xffffffff, 1, true);
      pax_draw_text("/sd/roms/scratch/projects/", PAX_W / 2, 160, 0xffffffff, 1, true);
    }
    std::string range = count ? std::to_string(first + 1) + "-" + std::to_string(std::min(count, first + rows)) : "0";
    pax_draw_text("Projects " + range + " of " + std::to_string(count), PAX_W / 2, PAX_H - 51, 0xc8d4e8ff, .85, true);
    pax_draw_text("Wheel or drag scrollbar   N: new   E: edit   Hold Menu: exit",
                  PAX_W / 2, PAX_H - 27, 0xc8d4e8ff, .8, true);
    Render::endFrame();
    pax_svc->delay_ms(15);
  }
  return "";
}
#ifdef PAX_HOST_TEST
std::string pax_host_choose_project() { return chooseProject(); }
#endif

bool pax_project_playing = false;
bool pax_play_project(const std::string &path) {
  if (PAX_W != pax_editor_width || PAX_H != pax_editor_height)
    if (!pax_resize_surface(pax_editor_width, pax_editor_height))
      return false;
  Unzip::filePath = path;
  Render::beginFrame(0, 20, 25, 40);
  pax_draw_text("Loading project", PAX_W / 2, PAX_H / 2 - 12, 0xffffffff, 1, true);
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
  pax_project_playing = true;
  Scratch::startScratchProject();
  pax_project_playing = false;
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
