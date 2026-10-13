#include "pax_editor.h"
#include <audio.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <input.hpp>
#include <miniz.h>
#include <os.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <unzip.hpp>
using namespace PaxEdit;
static void tap(Editor &e, int x, int y) {
  e.pointer(x, y, true);
  e.pointer(x, y, false);
  e.render();
}
static void drag(Editor &e, int x, int y, int dx, int dy) {
  e.pointer(x, y, true);
  e.pointer(dx, dy, true);
  e.render();
  e.pointer(dx, dy, false);
  e.render();
}
void testEditor() {
  std::string dir = OS::getScratchFolderLocation();
  std::filesystem::remove(dir + "Editor Test.sb3");
  std::filesystem::remove(dir + "Editor Import.sb3");
  Project p;
  auto flag = p.add("event_whenflagclicked", 30, 25);
  auto repeat = p.add("control_repeat", 30, 70);
  p.literal(repeat, {"TIMES", "3", false, false}, "3");
  auto move = p.add("motion_movesteps", 30, 160);
  p.literal(move, {"STEPS", "4", false, false}, "4");
  auto set = p.add("data_setvariableto", 30, 225);
  p.literal(set, {"VALUE", "0", false, false}, "0");
  auto change = p.add("data_changevariableby", 30, 300);
  assert(p.attach(set, flag));
  assert(p.attach(repeat, set));
  assert(p.attach(move, repeat, "SUBSTACK"));
  assert(p.attach(change, move));
  auto condition = p.add("control_if", 30, 330);
  auto gt = p.add("operator_gt", 330, 280);
  auto inside = p.add("motion_changeyby", 30, 390);
  assert(p.attach(condition, repeat));
  assert(p.attach(gt, condition, "CONDITION"));
  assert(p.attach(inside, condition, "SUBSTACK"));
  assert(!p.attach(flag, change));
  auto plus = p.add("operator_add", 330, 25);
  p.literal(plus, {"NUM1", "", false, false}, "2");
  p.literal(plus, {"NUM2", "", false, false}, "3");
  assert(p.attach(plus, move, "STEPS"));
  assert(p.blocks()[plus]["parent"] == move);
  p.checkpoint();
  p.detach(change);
  assert(p.blocks()[move]["next"].is_null());
  assert(p.undo());
  assert(p.blocks()[move]["next"] == change);
  assert(p.redo());
  assert(p.blocks()[move]["next"].is_null());
  assert(p.undo());
  p.checkpoint();
  auto copy = p.duplicate(repeat);
  assert(!copy.empty() &&
         p.descendants(copy).size() == p.descendants(repeat).size());
  p.erase(copy);
  assert(!p.blocks().contains(copy));
  assert(p.blocks().contains(repeat));
  assert(p.save(dir + "Editor Test.sb3"));
  assert(!p.save(dir + "Editor Test.sb3"));
  Project reloaded;
  assert(reloaded.load(dir + "Editor Test.sb3"));
  assert(reloaded.document() == p.document());
  auto bytes = reloaded.asset(
      reloaded.document()["targets"][1]["costumes"][0]["md5ext"]);
  assert(!bytes.empty());
  // The exported program is executed by the real Scratch interpreter.
  assert(Scratch::initializeRuntime());
  Unzip::filePath = dir + "Editor Test.sb3";
  assert(Unzip::load());
  Scratch::initializeScratchProject();
  ScriptThread monitor;
  for (int i = 0; i < 25; i++) {
    Scratch::stepScratchProject(monitor);
    pax_svc->delay_ms(5);
  }
  Sprite *sprite = nullptr;
  for (auto s : Scratch::sprites)
    if (s->name == "Sprite")
      sprite = s;
  assert(sprite);
  assert(sprite->xPosition == 15);
  assert(sprite->yPosition == 10);
  std::string varId = p.document()["targets"][1]["variables"].begin().key();
  assert(BlockExecutor::getVariableValue(varId, sprite).get<double>() == 3);
  Scratch::cleanupScratchProject();
  SoundPlayer::deinit();
  Render::deInit();
  OS::deinit();
  OS::toExit = false;
  // Imported unknown blocks, metadata, and all assets survive a copy/save.
  Project imported;
  assert(imported.load(dir + "PAX Demo.sb3"));
  auto original = imported.document();
  imported.blocks()["unknown"] = {{"opcode", "extension_opaque"},
                                  {"next", nullptr},
                                  {"parent", nullptr},
                                  {"inputs", Json::object()},
                                  {"fields", Json::object()},
                                  {"shadow", false},
                                  {"topLevel", true},
                                  {"x", 400},
                                  {"y", 200},
                                  {"mutation", {{"custom", "preserve"}}}};
  assert(imported.save(dir + "Editor Import.sb3"));
  Project again;
  assert(again.load(dir + "Editor Import.sb3"));
  assert(again.blocks()["unknown"] == imported.blocks()["unknown"]);
  assert(again.document()["targets"][0] == original["targets"][0]);
  for (auto &t : original["targets"])
    for (auto &c : t["costumes"]) {
      auto asset = c["md5ext"].get<std::string>();
      assert(imported.asset(asset) == again.asset(asset));
    }
  // Exercise actual palette drags and snapping through pointer events.
  assert(Render::Init());
  assert(pax_resize_surface(800, 480));
  Editor e;
  e.render();
  drag(e, 120, 105, 310, 100);
  assert(e.project().blocks().size() == 1);
  std::string root = e.project().blocks().begin().key();
  assert(e.project().blocks()[root]["opcode"] == "event_whenflagclicked");
  tap(e, 35, 76);
  auto dock = e.docks().front().rect;
  drag(e, 120, 105, dock.x + 30, dock.y + 9);
  std::string child = e.project().blocks()[root]["next"].get<std::string>();
  assert(e.project().blocks()[child]["opcode"] == "motion_movesteps");
  e.render();
  Hit input;
  for (auto &h : e.hits())
    if (h.id == child && h.input)
      input = h;
  tap(e, input.rect.x + 20, input.rect.y + 10);
  tap(e, 453, 350);
  e.keyboard("7");
  e.keyboard("enter");
  e.render();
  assert(e.project().blocks()[child]["inputs"]["STEPS"][1][1] == "7");
  tap(e, 35, 229); // Control category.
  auto block = e.project().add("control_repeat", 60, 130);
  assert(e.project().attach(block, child));
  e.render();
  Dock body;
  for (auto &d : e.docks())
    if (d.id == block && d.slot == "SUBSTACK")
      body = d;
  tap(e, 35, 76);
  drag(e, 120, 105, body.rect.x + 40, body.rect.y + 10);
  assert(e.project().blocks()[block]["inputs"]["SUBSTACK"][1].is_string());
  e.render();
  FILE *f = fopen("/tmp/pax-block-editor.ppm", "wb");
  fprintf(f, "P6\n%d %d\n255\n", PAX_W, PAX_H);
  for (int i = 0; i < PAX_W * PAX_H; i++) {
    uint16_t p = pax_pixels[i];
    unsigned char c[] = {
        static_cast<unsigned char>(((p >> 11) & 31) * 255 / 31),
        static_cast<unsigned char>(((p >> 5) & 63) * 255 / 63),
        static_cast<unsigned char>((p & 31) * 255 / 31)};
    fwrite(c, 1, 3, f);
  }
  fclose(f);

  // The actual Run button switches surfaces, runs the SB3, and returns on touch
  // STOP.
  auto previousTouch = pax_svc->touch_read;
  auto *mutableSvc = const_cast<app_services_t *>(pax_svc);
  mutableSvc->touch_read = [](int *x, int *y) {
    if (pax_editor_playing) {
      *x = 440;
      *y = 15;
      return 1;
    }
    return 0;
  };
  Editor runner(
      [](const std::string &file) { assert(pax_play_project(file)); });
  runner.project() = p;
  assert(SoundPlayer::init());
  runner.render();
  tap(runner, 380, 23);
  assert(PAX_W == 800 && PAX_H == 480);
  assert(!pax_editor_playing);
  mutableSvc->touch_read = previousTouch;
  // Full native canvas, nested reporters, and touch coordinates use all
  // 1024x600 pixels.
  auto getSize = mutableSvc->display_get_size;
  auto setCanvas = mutableSvc->display_set_canvas;
  mutableSvc->display_get_size = [](int *w, int *h) {
    *w = 1024;
    *h = 600;
  };
  mutableSvc->display_set_canvas = [](int w, int h) {
    assert(w == 1024 && h == 600);
    return 0;
  };
  pax_choose_canvas();
  assert(pax_editor_width == 1024 && pax_editor_height == 600);
  assert(pax_resize_surface(1024, 600));
  Editor full;
  assert(full.load(dir + "Editor Test.sb3"));
  full.render();
  assert(full.hits().size() > 8);
  mutableSvc->touch_read = [](int *x, int *y) {
    *x = 1000;
    *y = 580;
    return 1;
  };
  Input::getInput();
  assert(Input::mousePointer.x == 1000 && Input::mousePointer.y == 580);
  mutableSvc->touch_read = previousTouch;
  // The C-block's inner tab shares the nested stack's notch coordinates;
  // there is no artificial vertical gap and its width encloses the body.
  Hit repeatHeader, moveHeader, changeHeader;
  for (auto &hit : full.hits())
    if (!hit.input) {
      if (hit.id == repeat)
        repeatHeader = hit;
      if (hit.id == move)
        moveHeader = hit;
      if (hit.id == change)
        changeHeader = hit;
    }
  assert(moveHeader.rect.x == repeatHeader.rect.x + 18);
  assert(moveHeader.rect.y == repeatHeader.rect.y + repeatHeader.rect.h);
  assert(repeatHeader.rect.w >= moveHeader.rect.w + 18);
  auto orange = uint16_t(((255 & 248) << 8) | ((171 & 252) << 3) | (25 >> 3));
  assert(pax_pixels[(moveHeader.rect.y + 2) * PAX_W + moveHeader.rect.x + 30] ==
         orange);
  assert(pax_pixels[(changeHeader.rect.y + changeHeader.rect.h + 8) * PAX_W +
                    changeHeader.rect.x + 80] == orange);
  // Failed archive open keeps the previous project and makes Open/New
  // reachable.
  auto before = full.project().document();
  assert(!full.load(dir + "missing.sb3"));
  assert(full.project().document() == before);
  tap(full, 240, 23); // Open remains responsive after a failed load.
  tap(full, 112 + 600, 60 + 384); // Cancel the centered file picker.
  assert(full.load(dir + "Editor Test.sb3"));
  full.render();
  FILE *screen = fopen("/tmp/pax-block-editor-full.ppm", "wb");
  fprintf(screen, "P6\n%d %d\n255\n", PAX_W, PAX_H);
  for (int i = 0; i < PAX_W * PAX_H; i++) {
    uint16_t pixel = pax_pixels[i];
    unsigned char rgb[] = {
        static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
        static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63),
        static_cast<unsigned char>((pixel & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, screen);
  }
  fclose(screen);
  mutableSvc->display_get_size = getSize;
  mutableSvc->display_set_canvas = setCanvas;
  // Execute actual board opcodes via an exported project and the real
  // interpreter.
  float relay = -1;
  mutableSvc->hardware_read = [](const char *name, float *out) {
    if (std::string(name) != "temperature_c")
      return -1;
    *out = 23.5f;
    return 0;
  };
  static float hardwareWritten = -1;
  mutableSvc->hardware_write = [](const char *name, float value) {
    if (std::string(name) != "relay")
      return -1;
    hardwareWritten = value;
    return 0;
  };
  Project hardware;
  auto hflag = hardware.add("event_whenflagclicked", 30, 20);
  auto hset = hardware.add("data_setvariableto", 30, 80);
  auto reading = hardware.add("paxio_temperature_c", 30, 120);
  auto write = hardware.add("paxio_write", 30, 180);
  hardware.literal(write, {"CHANNEL", "", false, false}, "relay");
  hardware.literal(write, {"VALUE", "", false, false}, "1");
  assert(hardware.attach(hset, hflag));
  assert(hardware.attach(reading, hset, "VALUE"));
  assert(hardware.attach(write, hset));
  std::filesystem::remove(dir + "Hardware Test.sb3");
  assert(hardware.save(dir + "Hardware Test.sb3"));
  assert(pax_resize_surface(480, 360));
  assert(Scratch::initializeRuntime());
  Unzip::filePath = dir + "Hardware Test.sb3";
  assert(Unzip::load());
  Scratch::initializeScratchProject();
  ScriptThread hardwareMonitor;
  for (int i = 0; i < 12; i++)
    Scratch::stepScratchProject(hardwareMonitor);
  Sprite *hardwareSprite = nullptr;
  for (auto *sp : Scratch::sprites)
    if (sp->name == "Sprite")
      hardwareSprite = sp;
  assert(hardwareSprite);
  auto hid = hardware.document()["targets"][1]["variables"].begin().key();
  assert(BlockExecutor::getVariableValue(hid, hardwareSprite).get<double>() ==
         23.5);
  assert(hardwareWritten == 1);
  Scratch::cleanupScratchProject();
  SoundPlayer::deinit();
  Render::deInit();
  OS::deinit();
  OS::toExit = false;
  mutableSvc->hardware_read = nullptr;
  mutableSvc->hardware_write = nullptr;
  pax_editor_width = 800;
  pax_editor_height = 480;
  assert(Render::Init());
  assert(pax_resize_surface(800, 480));
  puts("PASS native 1024x600, loader-backed ZIP, failed-open recovery, real "
       "hardware opcode execution");
  // A long imported stack must render without recursive task-stack growth.
  Editor longStack;
  auto top = longStack.project().add("event_whenflagclicked", 30, 20);
  for (int i = 0; i < 500; i++) {
    auto node = longStack.project().add("motion_movesteps", 30, 20);
    assert(longStack.project().attach(node, top));
    top = node;
  }
  longStack.render();
  assert(longStack.hits().size() > 1000);
  // A C-block containing a long stack crops its vector raster to the display.
  Editor longContainer;
  auto container = longContainer.project().add("control_repeat", 30, 20);
  std::string tail;
  for (int i = 0; i < 500; i++) {
    auto node = longContainer.project().add("motion_movesteps", 30, 20);
    assert(tail.empty()
               ? longContainer.project().attach(node, container, "SUBSTACK")
               : longContainer.project().attach(node, tail));
    tail = node;
  }
  longContainer.render();
  assert(longContainer.hits().size() > 1000);
  SoundPlayer::deinit();
  Render::deInit();
  PAX_W = 480;
  PAX_H = 360;
  puts("PASS editor: touch dragging, snapping, inputs, nesting, undo/redo, SB3 "
       "round-trip, "
       "preserved assets, runtime execution");
}
