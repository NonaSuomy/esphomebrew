#include "pax_editor.h"
#include <audio.hpp>
#include <image.hpp>
#include <cassert>
#include <cmath>
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
static void captureEditor(const std::string &name) {
  std::error_code error;
  std::filesystem::copy_file("/tmp/pax-scratch-test.ppm","/tmp/scratch-native-"+name+".ppm",std::filesystem::copy_options::overwrite_existing,error);
  assert(!error);
}
void testEditor() {
  for (auto &d : definitions())
    assert(BlockExecutor::getHandlers().count(d.opcode));
  int shortcutRuns = 0;
  Editor shortcuts([&](const std::string &file) { assert(std::filesystem::exists(file));shortcutRuns++; });
  shortcuts.keyboard("f5");assert(shortcutRuns == 1);
  // Function keys never run a project while editing a filename/input.
  shortcuts.render();tap(shortcuts, 310, 25);
  shortcuts.keyboard("f5");assert(shortcutRuns == 1);
  puts("PASS F5 preview and modal shortcut guard");
  std::string dir = OS::getScratchFolderLocation();
  std::filesystem::remove(dir + "Editor Test.sb3");
  std::filesystem::remove(dir + "Editor Import.sb3");
  Project features;features.selectTarget(0);features.addVariable("global score");features.addList("shared items");
  const auto globalVariable=features.target()["variables"].begin().key();
  const auto globalList=features.target()["lists"].begin().key();
  features.target()["broadcasts"]["originalMessageId"]="ready";
  features.selectTarget(1);auto listAdd=features.add("data_addtolist",30,30);
  features.literal(listAdd,{"LIST","",true,false},"shared items");
  assert(features.blocks()[listAdd]["fields"]["LIST"][1]==globalList);
  auto setGlobal=features.add("data_setvariableto",30,90);features.literal(setGlobal,{"VARIABLE","",true,false},"global score");
  assert(features.blocks()[setGlobal]["fields"]["VARIABLE"][1]==globalVariable);
  auto message=features.add("event_whenbroadcastreceived",30,150);features.literal(message,{"BROADCAST_OPTION","",true,false},"ready");
  assert(features.blocks()[message]["fields"]["BROADCAST_OPTION"][1]=="originalMessageId");
  assert(features.choices("LIST").size()==2);assert(!features.choices("COSTUME").empty());assert(!features.choices("SOUND_MENU").empty());
  assert(features.setTargetProperty("x","123.5"));assert(!features.setTargetProperty("size","-1"));
  assert(!features.setTargetProperty("direction","nan"));assert(features.setTargetProperty("visible","false"));
  std::string oldName=features.target().value("name","");auto say=features.add("looks_say",30,210);features.literal(say,{"MESSAGE","",false,false},oldName);
  assert(features.setTargetProperty("name","Renamed sprite"));assert(features.blocks()[say]["inputs"]["MESSAGE"][1][1]==oldName);
  features.checkpoint();features.addList("temporary");assert(features.undo());
  std::filesystem::remove(dir+"UI Features.sb3");assert(features.save(dir+"UI Features.sb3"));Project reopened;assert(reopened.load(dir+"UI Features.sb3"));
  assert(reopened.target()["x"]==123.5&&!reopened.target()["visible"].get<bool>());assert(reopened.blocks()[listAdd]["fields"]["LIST"][1]==globalList);
  puts("PASS list authoring, shared selectors, preserved broadcast IDs, sprite properties and SB3 round trip");
  Project advanced;
  assert(advanced.storeAsset("abc", "txt") ==
         "900150983cd24fb0d6963f7d28e17f72");
  assert(advanced.storeAsset(std::string(1000000, 'a'), "txt") ==
         "7707d6ae4e027c70eea2a935c2296f21");
  std::vector<uint32_t> painted(1024, 0);
  painted[0] = 0x112233ff;
  painted[33] = 0xff6680ff;
  auto originalCostume = advanced.target()["costumes"][0];
  advanced.checkpoint();
  assert(advanced.paintCostume("pixel art", painted));
  assert(advanced.target()["costumes"][0] == originalCostume);
  std::vector<uint32_t> raster;
  int pw, ph;
  assert(advanced.costumePixels(1, raster, pw, ph));
  assert(raster[0] == painted[0] && raster[33] == painted[33]);
  auto paintFile =
      advanced.target()["costumes"][1]["md5ext"].get<std::string>();
  assert(advanced.undo());
  assert(advanced.target()["costumes"].size() == 1);
  assert(!advanced.asset(paintFile).empty());
  assert(advanced.redo());
  assert(advanced.addTone("tone", 440, 1));
  int toneIndex = advanced.target()["sounds"].size() - 1, rate = 0;
  std::vector<int16_t> samples, edited;
  assert(advanced.soundPcm(toneIndex, samples, rate) &&
         samples.size() == 22050 && rate == 22050);
  advanced.checkpoint();
  assert(advanced.editSound(toneIndex, 50, 100, .5, true));
  assert(advanced.soundPcm(toneIndex, edited, rate) && edited.size() == 11025);
  assert(edited[0] == (int16_t)std::lround(samples.back() * .5));
  assert(!advanced.editSound(toneIndex, 70, 20, 1, false));
  assert(advanced.undo());
  assert(advanced.soundPcm(toneIndex, edited, rate) && edited == samples);
  {
    std::ofstream f(dir + "import.svg");
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'><rect "
         "width='20' height='10' fill='red'/></svg>";
  }
  assert(advanced.importAsset(dir + "import.svg", false));
  assert(advanced.target()["costumes"].size() == 3);
  auto fn = advanced.createCustom(
      "advance", {{"distance", false}, {"enabled", true}}, false);
  assert(!fn.empty());
  assert(advanced
             .createCustom("advance", {{"distance", false}, {"enabled", true}},
                           false)
             .empty());
  auto customBody = advanced.add("control_if", 30, 90),
       customMove = advanced.add("motion_movesteps", 30, 160);
  auto distanceArg = advanced.add("@arg:distance:s", 320, 40),
       enabledArg = advanced.add("@arg:enabled:b", 320, 100);
  assert(advanced.attach(customBody, fn));
  assert(advanced.attach(customMove, customBody, "SUBSTACK"));
  assert(advanced.attach(distanceArg, customMove, "STEPS"));
  assert(advanced.attach(enabledArg, customBody, "CONDITION"));
  auto customFlag = advanced.add("event_whenflagclicked", 380, 30),
       customCall = advanced.add("@call:advance %s %b", 380, 90);
  assert(advanced.attach(customCall, customFlag));
  auto customSlots = advanced.slots(customCall);
  assert(customSlots.size() == 2);
  Slot numberSlot, booleanSlot;
  for (auto slot : customSlots)
    (slot.boolean ? booleanSlot : numberSlot) = slot;
  assert(booleanSlot.boolean);
  advanced.literal(customCall, numberSlot, "12");
  auto trueCondition = advanced.add("operator_gt", 500, 150);
  assert(advanced.attach(trueCondition, customCall, booleanSlot.key));
  assert(advanced.updateCustom(
      fn, "forward", {{"steps", false}, {"go", true}, {"note", false}}, true));
  assert(advanced.blocks()[distanceArg]["fields"]["VALUE"][0] == "steps");
  assert(advanced.blocks()[customCall]["mutation"]["proccode"] ==
         "forward %s %b %s");
  auto copiedDefinition = advanced.duplicate(fn);
  assert(!copiedDefinition.empty());
  auto originalPrototype =
      advanced.blocks()[fn]["inputs"]["custom_block"][1].get<std::string>();
  auto copiedPrototype =
      advanced.blocks()[copiedDefinition]["inputs"]["custom_block"][1]
          .get<std::string>();
  assert(advanced.blocks()[originalPrototype]["mutation"]["proccode"] !=
         advanced.blocks()[copiedPrototype]["mutation"]["proccode"]);
  std::filesystem::remove(dir + "Advanced UI.sb3");
  assert(advanced.save(dir + "Advanced UI.sb3"));
  Project advancedCopy;
  assert(advancedCopy.load(dir + "Advanced UI.sb3"));
  assert(advancedCopy.asset(paintFile) == advanced.asset(paintFile));
  assert(advancedCopy.soundPcm(toneIndex, edited, rate) && edited == samples);
  assert(advancedCopy.blocks()[customCall]["mutation"] ==
         advanced.blocks()[customCall]["mutation"]);
  puts("PASS MD5 assets, non-destructive paint, audio trim/reverse/undo/import "
       "and typed custom-block SB3 round trip");

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
  assert(Scratch::initializeRuntime());
  Unzip::filePath = dir + "Advanced UI.sb3";
  assert(Unzip::load());
  Scratch::initializeScratchProject();
  ScriptThread customMonitor;
  for (int i = 0; i < 30; i++)
    assert(Scratch::stepScratchProject(customMonitor).first);
  Sprite *customSprite = nullptr;
  for (auto target : Scratch::sprites)
    if (target->name == "Sprite")
      customSprite = target;
  assert(customSprite && customSprite->xPosition == 12);
  Scratch::cleanupScratchProject();
  SoundPlayer::deinit();
  Render::deInit();
  OS::deinit();
  OS::toExit = false;
  puts("PASS authored custom number and Boolean arguments execute in the real "
       "Scratch runtime");
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
  // Exercise the actual touch dialogs at 800x480, including list creation.
  Editor ui;ui.render();tap(ui,640,370);tap(ui,450,135);
  for(int i=0;i<10;i++)ui.keyboard("backspace");ui.keyboard("4");ui.keyboard("2");ui.keyboard("enter");assert(ui.project().target()["x"]==42);
  tap(ui,600,382);tap(ui,34,330);tap(ui,110,347);
  for(int i=0;i<10;i++)ui.keyboard("backspace");ui.keyboard("q");ui.keyboard("enter");
  bool madeList=false;for(const auto &list:ui.project().target()["lists"])if(list[0]=="q")madeList=true;assert(madeList);
  auto listBlock=ui.project().add("data_addtolist",40,60);ui.render();
  Rect listHit{};for(const auto &hit:ui.hits())if(hit.id==listBlock&&hit.input&&hit.slot.key=="LIST")listHit=hit.rect;
  assert(listHit.w>0);tap(ui,listHit.x+listHit.w/2,listHit.y+listHit.h/2);tap(ui,200,125);
  assert(ui.project().blocks()[listBlock]["fields"]["LIST"][0]=="q");
  puts("PASS touch property dialog and list creation dialog");
  Editor tools;
  tools.project().target()["costumes"][0]["bitmapResolution"] = 2;
  tools.project().target()["costumes"][0]["rotationCenterX"] = 24;
  tools.project().target()["costumes"][0]["rotationCenterY"] = 48;
  tools.render();
  tap(tools, 640, 370);
  tap(tools, 120, 382);
  tap(tools, 200, 125);
  tap(tools, 200, 170);
  tools.pointer(124, 132, true);
  tools.pointer(300, 308, false);
  tools.render();
  tap(tools, 440, 325);
  tap(tools, 600, 325);
  captureEditor("paint");
  tap(tools, 450, 355);
  assert(tools.project().target()["costumes"].size() == 2);
  std::vector<uint32_t> fastStroke;
  int fsw, fsh;
  assert(tools.project().costumePixels(1, fastStroke, fsw, fsh));
  assert(std::count_if(fastStroke.begin(), fastStroke.end(),
                       [](uint32_t c) { return c == 0x4c97ffff; }) > 20);
  assert(tools.project().target()["costumes"][1]["rotationCenterX"] == 12);
  assert(tools.project().target()["costumes"][1]["rotationCenterY"] == 24);
  assert(fsw == 32 && fsh == 32);
  tap(tools, 650, 382);
  tap(tools, 650, 382);
  tap(tools, 440, 25);
  assert(tools.project().target()["costumes"].size() == 1);
  tap(tools, 640, 370);
  tap(tools, 300, 382);
  tap(tools, 120, 382);
  assert(tools.project().target()["sounds"].size() == 2);
  captureEditor("sound");
  tap(tools, 150, 246);
  for (int i = 0; i < 5; i++)
    tools.keyboard("backspace");
  tools.keyboard("5");
  tools.keyboard("0");
  tools.keyboard("enter");
  tap(tools, 530, 246);
  assert(tools.project().target()["sounds"][1]["sampleCount"] == 11025);
  tap(tools, 180, 350);
  tap(tools, 610, 350);
  tap(tools, 650, 382);
  tap(tools, 650, 382);
  tap(tools, 35, 410);
  tap(tools, 125, 105);
  tap(tools, 160, 318);
  tap(tools, 360, 318);
  captureEditor("custom");
  tap(tools, 610, 370);
  assert(tools.project().customEntries().size() == 3);
  auto zoomBlock = tools.project().add("motion_movesteps", 20, 160);
  tap(tools, 543, 455);
  assert(tools.zoom() == 1.25f);
  tools.render();
  Rect zh{};
  for (auto &hit : tools.hits())
    if (hit.id == zoomBlock && !hit.input)
      zh = hit.rect;
  int zx = 254 + (int)((zh.x + 60 - 254) * tools.zoom()),
      zy = 58 + (int)((zh.y + zh.h - 2 - 58) * tools.zoom());
  int beforeX = tools.project().blocks()[zoomBlock]["x"],
      beforeY = tools.project().blocks()[zoomBlock]["y"];
  tools.pointer(zx, zy, true);
  tools.pointer(zx + 50, zy + 50, false);
  tools.render();
  assert(tools.project().blocks()[zoomBlock]["x"] == beforeX + 40 &&
         tools.project().blocks()[zoomBlock]["y"] == beforeY + 40);
  tap(tools, 588, 455);
  assert(tools.zoom() == 1);
  std::vector<std::pair<std::string, bool>> manyArgs;
  for (int i = 0; i < 8; i++)
    manyArgs.push_back({"a" + std::to_string(i), i % 2 != 0});
  assert(!tools.project().createCustom("many inputs", manyArgs, false).empty());
  tools.render();
  auto pageOne = tools.palette().front().second;
  tap(tools, 200, 395);
  assert(!tools.palette().empty() && tools.palette().front().second != pageOne);
  puts("PASS paint strokes, waveform trim, custom block wizard and dragging at "
       "125 percent zoom");
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
      int cw, ch;pax_svc->display_get_size(&cw,&ch);
      float fit=std::min((float)cw/PAX_W,(float)ch/PAX_H);
      *x=(cw-(int)(PAX_W*fit))/2+(int)((PAX_W-40)*fit);
      *y=(ch-(int)(PAX_H*fit))/2+(int)(17*fit);
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
  mutableSvc->display_get_size = [](int *w, int *h) {
    *w = 800;
    *h = 480;
  };
  pax_choose_canvas();
  assert(pax_editor_width == 1024 && pax_editor_height == 600);
  mutableSvc->display_get_size = [](int *w, int *h) {
    *w = 1024;
    *h = 600;
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
  // Palette hit bounds include hat rises and puzzle tabs, with 12px clear
  // space.
  for (size_t i = 1; i < full.palette().size(); i++)
    assert(full.palette()[i].first.y >=
           full.palette()[i - 1].first.y + full.palette()[i - 1].first.h + 12);
  Hit ifHeader, booleanHeader;
  std::vector<Hit> compareInputs;
  for (auto &hit : full.hits()) {
    if (hit.id == condition && !hit.input)
      ifHeader = hit;
    if (hit.id == gt && !hit.input)
      booleanHeader = hit;
    if (hit.id == gt && hit.input)
      compareInputs.push_back(hit);
  }
  assert(booleanHeader.rect.x - ifHeader.rect.x >= 32);
  assert(ifHeader.rect.x + ifHeader.rect.w -
             (booleanHeader.rect.x + booleanHeader.rect.w) >=
         40);
  assert(compareInputs.size() == 2);
  assert(compareInputs[1].rect.x -
             (compareInputs[0].rect.x + compareInputs[0].rect.w) >=
         18);
  tap(full, 35, 230); // Control palette includes visibly taller C blocks.
  for (size_t i = 1; i < full.palette().size(); i++)
    assert(full.palette()[i].first.y >=
           full.palette()[i - 1].first.y + full.palette()[i - 1].first.h + 12);
  bool sawContainer = false;
  for (auto &item : full.palette())
    if (item.second == "control_repeat")
      sawContainer = item.first.h > 70;
  assert(sawContainer);
  tap(full, 35, 195); // Restore Events for the reference preview.
  // Failed archive open keeps the previous project and makes Open/New
  // reachable.
  auto before = full.project().document();
  assert(!full.load(dir + "missing.sb3"));
  assert(full.project().document() == before);
  tap(full, 170,
      23); // New recovers from the failed open without keeping the old file.
  assert(full.project().blocks().empty() && full.project().source().empty());
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
  for (auto dimensions : {std::pair<int,int>{800,480}, {1024,600}}) {
    assert(pax_resize_surface(dimensions.first, dimensions.second));
    Editor navigation;
    auto near = navigation.project().add("motion_movesteps", 0, 0);
    auto far = navigation.project().add("motion_movesteps", 1400, 1600);
    navigation.render();
    auto hit = [&](const std::string &id) { for (const auto &h : navigation.hits()) if(h.id==id&&!h.input)return h.rect; assert(false); return Rect{}; };
    auto before = hit(far);
    int panel = 177 + std::max(0, PAX_W-800)/3;
    int left = 80 + std::clamp(174+(PAX_W-800)/4,174,220);
    int trackX = PAX_W-panel-19, trackY=PAX_H-61;
    tap(navigation, trackX-40, trackY);
    auto horizontal=hit(far);printf("SCROLL %dx%d before=%d,%d after=%d,%d track=%d,%d\n",PAX_W,PAX_H,before.x,before.y,horizontal.x,horizontal.y,left+100,trackY);assert(horizontal.x<before.x&&horizontal.y==before.y);
    drag(navigation, trackX, 100, trackX, PAX_H-90);
    auto vertical=hit(far);assert(vertical.y<horizontal.y&&vertical.x==horizontal.x);
    navigation.render();captureEditor("scrollbars-"+std::to_string(PAX_W));
  }
  assert(pax_resize_surface(1024,600));
  Editor hardwareLayout;
  auto demo = std::filesystem::path(__FILE__).parent_path().parent_path()/"data/PAX-Fun-Demo.sb3";
  assert(hardwareLayout.load(demo.string()));hardwareLayout.render();captureEditor("properties-scrollbars-1024");
  assert(pax_resize_surface(800,480));
  puts("PASS both canvas scrollbars navigate off-screen blocks at 800x480 and 1024x600");
  {
    auto *svc=const_cast<app_services_t *>(pax_svc);
    auto oldTouch=svc->touch_read;auto oldMouse=svc->input_mouse_read;auto oldSize=svc->display_get_size;
    svc->touch_read=[](int*,int*){return 0;};
    svc->display_get_size=[](int*w,int*h){*w=PAX_W;*h=PAX_H;};
    static int deltaX=0,deltaY=0;
    svc->input_mouse_read=[](int*x,int*y,int*b){*x=deltaX;*y=deltaY;*b=0;deltaX=deltaY=0;return 1;};
    pax_native_cursor=false;
    Input::getInput();auto pos=Input::getTouchPosition();deltaX=50-pos[0];deltaY=60-pos[1];Input::getInput();
    Render::beginFrame(0,0,0,0);Render::endFrame();assert(pax_pixels[60*PAX_W+50]==0xffff);
    deltaX=25;deltaY=30;Input::getInput();pax_refresh_pointer();
    assert(pax_pixels[60*PAX_W+50]==0);assert(pax_pixels[90*PAX_W+75]==0xffff);
    deltaX=PAX_W;deltaY=PAX_H;Input::getInput();pax_refresh_pointer();
    assert(pax_pixels[90*PAX_W+75]==0);assert(pax_pixels[PAX_W*PAX_H-1]==0xffff);
    pax_native_cursor=true;pax_refresh_pointer();assert(pax_pixels[PAX_W*PAX_H-1]==0);
    svc->touch_read=oldTouch;svc->input_mouse_read=oldMouse;svc->display_get_size=oldSize;
    pax_mouse_present=false;pax_native_cursor=false;
    puts("PASS cursor-only updates restore the old background, clip at screen edges, and suppress duplicate desktop cursor");
  }
  {
    std::vector<uint8_t> rgba(7*5*4);
    for(int y=0;y<5;++y)for(int x=0;x<7;++x){auto i=(y*7+x)*4;rgba[i]=x*31;rgba[i+1]=y*47;rgba[i+2]=(x+y)*17;rgba[i+3]=(x+y)%3==0?0:(x+y)%3==1?128:255;}
    ImageData image;image.width=7;image.height=5;image.pitch=28;image.pixels=rgba.data();
    ImageSubrect sub{1,1,5,3};
    for(float scale:{.5f,1.f,1.67f,70.f})for(bool flip:{false,true})for(bool cropped:{false,true}){
      ImageRenderParams params;params.x=160.25f;params.y=120.75f;params.scale=scale;params.flip=flip;if(cropped)params.subrect=&sub;
      auto render=[&](bool fast){Render::hasFrameBegan=false;pax_set_ui_zoom(1,0,0);pax_set_clip(0,0,PAX_W,PAX_H);Render::beginFrame(0,31,65,93);pax_set_clip(5,9,PAX_W-17,PAX_H-25);pax_set_ui_zoom(fast?1.f:std::nextafter(1.f,2.f),0,0);pax_draw_image(image,params);return std::vector<uint16_t>(pax_pixels,pax_pixels+PAX_W*PAX_H);};
      auto fast=render(true);auto reference=render(false);assert(fast==reference);
    }
    pax_set_ui_zoom(1,0,0);pax_set_clip(0,0,PAX_W,PAX_H);Render::hasFrameBegan=false;
    puts("PASS row-based image renderer matches reference pixels for clipping, scaling, horizontal flip, cropped images and alpha blending");
  }
  {
    auto *svc=const_cast<app_services_t *>(pax_svc);auto oldClock=svc->get_time_us;
    static int64_t fakeClock;fakeClock=1000;
    svc->get_time_us=[]()->int64_t{return fakeClock;};
    bool oldWarp=Scratch::warpTimer,oldStop=Scratch::shouldStop;
    Scratch::warpTimer=true;Scratch::shouldStop=false;
    Block loop;loop.nextBlock=&loop;int blocks=0;
    loop.blockFunction=BlockFunc(BlockFuncValue([&](Block*,ScriptThread*,Sprite*,Value*){++blocks;fakeClock+=100;return BlockResult::CONTINUE_IMMEDIATELY;}));
    ScriptThread thread;Sprite sprite;thread.finished=false;thread.nextBlock=&loop;thread.withoutScreenRefresh=true;
    BlockExecutor::runThread(thread,sprite);
    assert(blocks==64&&!thread.finished&&thread.nextBlock==&loop);
    BlockExecutor::runThread(thread,sprite);assert(blocks==128&&!thread.finished);
    svc->get_time_us=oldClock;Scratch::warpTimer=oldWarp;Scratch::shouldStop=oldStop;
    puts("PASS long warp chains yield a bounded slice and resume without losing the next block");
  }
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
