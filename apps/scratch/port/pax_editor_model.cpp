#include "pax_editor.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <filesystem.hpp>
#include <fstream>
#include <miniz.h>
#include <os.hpp>
namespace PaxEdit {
// Use loader file services for random-access ZIP reads on the device. The
// Scratch player already does this; avoiding a second newlib FILE adapter
// also gives the editor identical SD/USB behavior.
class ArchiveFile {
  void *file_ = nullptr;

public:
  ~ArchiveFile() {
    if (file_)
      pax_svc->file_close(file_);
  }
  bool open(mz_zip_archive &zip, const std::string &path) {
    if (!pax_svc->file_open)
      return mz_zip_reader_init_file(&zip, path.c_str(), 0);
    pax_file_stat_t st{};
    if (!pax_svc->file_stat || pax_svc->file_stat(path.c_str(), &st) != 0 ||
        st.is_dir || st.size < 22)
      return false;
    file_ = pax_svc->file_open(path.c_str(), "rb");
    if (!file_)
      return false;
    zip.m_pIO_opaque = this;
    zip.m_pRead = [](void *context, mz_uint64 offset, void *buf,
                     size_t size) -> size_t {
      auto *self = static_cast<ArchiveFile *>(context);
      if (offset > 0x7fffffff ||
          pax_svc->file_seek(self->file_, (long)offset, SEEK_SET) != 0)
        return 0;
      return pax_svc->file_read(buf, 1, size, self->file_);
    };
    return mz_zip_reader_init(&zip, st.size, 0);
  }
};

static Slot num(const char *k, const char *v) { return {k, v, false, false}; }
static Slot str(const char *k, const char *v) { return {k, v, false, false}; }
static Slot field(const char *k, const char *v) { return {k, v, true, false}; }
static Slot boolean(const char *k) { return {k, "", false, true}; }
const std::vector<Definition> &definitions() {
  static const std::vector<Definition> defs = {
      {"motion_movesteps",
       "move {STEPS} steps",
       0,
       Stack,
       {num("STEPS", "10")}},
      {"motion_turnright",
       "turn right {DEGREES} degrees",
       0,
       Stack,
       {num("DEGREES", "15")}},
      {"motion_turnleft",
       "turn left {DEGREES} degrees",
       0,
       Stack,
       {num("DEGREES", "15")}},
      {"motion_gotoxy",
       "go to x: {X} y: {Y}",
       0,
       Stack,
       {num("X", "0"), num("Y", "0")}},
      {"motion_changexby", "change x by {DX}", 0, Stack, {num("DX", "10")}},
      {"motion_changeyby", "change y by {DY}", 0, Stack, {num("DY", "10")}},
      {"motion_pointindirection",
       "point in direction {DIRECTION}",
       0,
       Stack,
       {num("DIRECTION", "90")}},
      {"motion_ifonedgebounce", "if on edge, bounce", 0, Stack, {}},
      {"motion_xposition", "x position", 0, Reporter, {}},
      {"motion_yposition", "y position", 0, Reporter, {}},
      {"looks_say", "say {MESSAGE}", 1, Stack, {str("MESSAGE", "Hello!")}},
      {"looks_sayforsecs",
       "say {MESSAGE} for {SECS} seconds",
       1,
       Stack,
       {str("MESSAGE", "Hello!"), num("SECS", "2")}},
      {"looks_show", "show", 1, Stack, {}},
      {"looks_hide", "hide", 1, Stack, {}},
      {"looks_changesizeby",
       "change size by {CHANGE}",
       1,
       Stack,
       {num("CHANGE", "10")}},
      {"looks_setsizeto",
       "set size to {SIZE} %",
       1,
       Stack,
       {num("SIZE", "100")}},
      {"looks_nextcostume", "next costume", 1, Stack, {}},
      {"looks_switchcostumeto",
       "switch costume to {COSTUME}",
       1,
       Stack,
       {str("COSTUME", "Sprite")}},
      {"sound_play",
       "start sound {SOUND_MENU}",
       2,
       Stack,
       {str("SOUND_MENU", "beep")}},
      {"sound_playuntildone",
       "play sound {SOUND_MENU} until done",
       2,
       Stack,
       {str("SOUND_MENU", "beep")}},
      {"sound_stopallsounds", "stop all sounds", 2, Stack, {}},
      {"sound_setvolumeto",
       "set volume to {VOLUME} %",
       2,
       Stack,
       {num("VOLUME", "100")}},
      {"event_whenflagclicked", "when green flag clicked", 3, Hat, {}},
      {"event_whenkeypressed",
       "when {KEY_OPTION} key pressed",
       3,
       Hat,
       {field("KEY_OPTION", "space")}},
      {"event_whenthisspriteclicked", "when sprite clicked", 3, Hat, {}},
      {"event_whenbroadcastreceived",
       "when I receive {BROADCAST_OPTION}",
       3,
       Hat,
       {field("BROADCAST_OPTION", "message1")}},
      {"event_broadcast",
       "broadcast {BROADCAST_INPUT}",
       3,
       Stack,
       {str("BROADCAST_INPUT", "message1")}},
      {"control_wait",
       "wait {DURATION} seconds",
       4,
       Stack,
       {num("DURATION", "1")}},
      {"control_repeat",
       "repeat {TIMES}",
       4,
       Container,
       {num("TIMES", "10")},
       1},
      {"control_forever", "forever", 4, Container, {}, 1},
      {"control_if",
       "if {CONDITION} then",
       4,
       Container,
       {boolean("CONDITION")},
       1},
      {"control_if_else",
       "if {CONDITION} then",
       4,
       Container,
       {boolean("CONDITION")},
       2},
      {"control_wait_until",
       "wait until {CONDITION}",
       4,
       Stack,
       {boolean("CONDITION")}},
      {"control_stop",
       "stop {STOP_OPTION}",
       4,
       Cap,
       {field("STOP_OPTION", "all")}},
      {"sensing_keypressed",
       "key {KEY_OPTION} pressed?",
       5,
       Boolean,
       {str("KEY_OPTION", "space")}},
      {"sensing_mousedown", "mouse down?", 5, Boolean, {}},
      {"sensing_mousex", "mouse x", 5, Reporter, {}},
      {"sensing_mousey", "mouse y", 5, Reporter, {}},
      {"sensing_timer", "timer", 5, Reporter, {}},
      {"sensing_resettimer", "reset timer", 5, Stack, {}},
      {"sensing_askandwait",
       "ask {QUESTION} and wait",
       5,
       Stack,
       {str("QUESTION", "What is your name?")}},
      {"sensing_answer", "answer", 5, Reporter, {}},
      {"operator_add",
       "{NUM1} + {NUM2}",
       6,
       Reporter,
       {num("NUM1", "1"), num("NUM2", "2")}},
      {"operator_subtract",
       "{NUM1} - {NUM2}",
       6,
       Reporter,
       {num("NUM1", "1"), num("NUM2", "2")}},
      {"operator_multiply",
       "{NUM1} * {NUM2}",
       6,
       Reporter,
       {num("NUM1", "2"), num("NUM2", "3")}},
      {"operator_divide",
       "{NUM1} / {NUM2}",
       6,
       Reporter,
       {num("NUM1", "6"), num("NUM2", "2")}},
      {"operator_random",
       "pick random {FROM} to {TO}",
       6,
       Reporter,
       {num("FROM", "1"), num("TO", "10")}},
      {"operator_gt",
       "{OPERAND1} > {OPERAND2}",
       6,
       Boolean,
       {num("OPERAND1", "1"), num("OPERAND2", "0")}},
      {"operator_lt",
       "{OPERAND1} < {OPERAND2}",
       6,
       Boolean,
       {num("OPERAND1", "0"), num("OPERAND2", "1")}},
      {"operator_equals",
       "{OPERAND1} = {OPERAND2}",
       6,
       Boolean,
       {str("OPERAND1", "1"), str("OPERAND2", "1")}},
      {"operator_and",
       "{OPERAND1} and {OPERAND2}",
       6,
       Boolean,
       {boolean("OPERAND1"), boolean("OPERAND2")}},
      {"operator_or",
       "{OPERAND1} or {OPERAND2}",
       6,
       Boolean,
       {boolean("OPERAND1"), boolean("OPERAND2")}},
      {"operator_not", "not {OPERAND}", 6, Boolean, {boolean("OPERAND")}},
      {"operator_join",
       "join {STRING1} {STRING2}",
       6,
       Reporter,
       {str("STRING1", "hello "), str("STRING2", "world")}},
      {"data_setvariableto",
       "set {VARIABLE} to {VALUE}",
       7,
       Stack,
       {field("VARIABLE", "score"), num("VALUE", "0")}},
      {"data_changevariableby",
       "change {VARIABLE} by {VALUE}",
       7,
       Stack,
       {field("VARIABLE", "score"), num("VALUE", "1")}},
      {"data_variable",
       "{VARIABLE}",
       7,
       Reporter,
       {field("VARIABLE", "score")}},
      {"data_showvariable",
       "show variable {VARIABLE}",
       7,
       Stack,
       {field("VARIABLE", "score")}},
      {"data_hidevariable",
       "hide variable {VARIABLE}",
       7,
       Stack,
       {field("VARIABLE", "score")}},
      {"data_addtolist", "add {ITEM} to {LIST}", 7, Stack, {str("ITEM", "thing"), field("LIST", "my list")}},
      {"data_deleteoflist", "delete {INDEX} of {LIST}", 7, Stack, {str("INDEX", "1"), field("LIST", "my list")}},
      {"data_deletealloflist", "delete all of {LIST}", 7, Stack, {field("LIST", "my list")}},
      {"data_insertatlist", "insert {ITEM} at {INDEX} of {LIST}", 7, Stack, {str("ITEM", "thing"), str("INDEX", "1"), field("LIST", "my list")}},
      {"data_replaceitemoflist", "replace item {INDEX} of {LIST} with {ITEM}", 7, Stack, {str("INDEX", "1"), field("LIST", "my list"), str("ITEM", "thing")}},
      {"data_itemoflist", "item {INDEX} of {LIST}", 7, Reporter, {str("INDEX", "1"), field("LIST", "my list")}},
      {"data_itemnumoflist", "item # of {ITEM} in {LIST}", 7, Reporter, {str("ITEM", "thing"), field("LIST", "my list")}},
      {"data_lengthoflist", "length of {LIST}", 7, Reporter, {field("LIST", "my list")}},
      {"data_listcontainsitem", "{LIST} contains {ITEM}?", 7, Boolean, {field("LIST", "my list"), str("ITEM", "thing")}},
      {"data_listcontents", "{LIST}", 7, Reporter, {field("LIST", "my list")}},
      {"data_showlist", "show list {LIST}", 7, Stack, {field("LIST", "my list")}},
      {"data_hidelist", "hide list {LIST}", 7, Stack, {field("LIST", "my list")}},
      {"pen_clear", "erase all pen", 8, Stack, {}},
      {"pen_penDown", "pen down", 8, Stack, {}},
      {"pen_penUp", "pen up", 8, Stack, {}},
      {"pen_stamp", "stamp", 8, Stack, {}},
      {"pen_setPenSizeTo", "set pen size {SIZE}", 8, Stack, {num("SIZE", "3")}},
      {"pen_setPenColorToColor",
       "set pen color {COLOR}",
       8,
       Stack,
       {str("COLOR", "#4080ff")}},
      {"paxio_read",
       "sensor {CHANNEL}",
       9,
       Reporter,
       {str("CHANNEL", "temperature_c")}},
      {"paxio_available",
       "{CHANNEL} available?",
       9,
       Boolean,
       {str("CHANNEL", "temperature_c")}},
      {"paxio_write",
       "set {CHANNEL} to {VALUE}",
       9,
       Stack,
       {str("CHANNEL", "relay"), num("VALUE", "0")}},
      {"paxio_status", "last IO status", 9, Reporter, {}},
      {"paxio_relay", "set relay {VALUE} (0/1)", 9, Stack, {num("VALUE", "0")}},
      {"paxio_servo", "servo {VALUE} %", 9, Stack, {num("VALUE", "0")}},
      {"paxio_rgbw",
       "LED R {R} G {G} B {B} W {W}",
       9,
       Stack,
       {num("R", "100"), num("G", "0"), num("B", "0"), num("W", "0")}},
      {"paxio_microphone",
       "microphone {VALUE} (0/1)",
       9,
       Stack,
       {num("VALUE", "1")}},
      {"paxio_camera", "capture camera snapshot", 9, Stack, {}},
      {"paxio_button",
       "button {BUTTON} pressed?",
       9,
       Boolean,
       {field("BUTTON", "dpadUp")}},
      {"paxio_distance_cm", "distance (cm)", 9, Reporter, {}},
      {"paxio_light_lux", "light (lux)", 9, Reporter, {}},
      {"paxio_temperature_c", "temperature (C)", 9, Reporter, {}},
      {"paxio_humidity_pct", "humidity (%)", 9, Reporter, {}},
      {"paxio_accel_x", "acceleration X (g)", 9, Reporter, {}},
      {"paxio_accel_y", "acceleration Y (g)", 9, Reporter, {}},
      {"paxio_accel_z", "acceleration Z (g)", 9, Reporter, {}},
      {"paxio_gyro_x", "gyro X (deg/s)", 9, Reporter, {}},
      {"paxio_gyro_y", "gyro Y (deg/s)", 9, Reporter, {}},
      {"paxio_gyro_z", "gyro Z (deg/s)", 9, Reporter, {}},
      {"paxio_motion", "PIR motion", 9, Reporter, {}},
      {"paxio_hall", "magnet detected", 9, Reporter, {}},
      {"paxio_touch_pad", "touch pad", 9, Reporter, {}},
      {"paxio_gas_voltage", "gas sensor (V)", 9, Reporter, {}},
      {"paxio_buttons_voltage", "buttons ADC (V)", 9, Reporter, {}},
      {"paxio_microphone_level", "microphone level", 9, Reporter, {}},
      {"paxio_camera_ready", "camera ready", 9, Reporter, {}},
  };
  return defs;
}
const Definition *definition(const std::string &op) {
  for (const auto &d : definitions())
    if (op == d.opcode)
      return &d;
  return nullptr;
}
static std::string blockReference(const Json &j) {
  return j.is_string() ? j.get<std::string>() : "";
}
static bool usedId(const Json &doc, const std::string &id, const char *key) {
  for (const auto &target : doc["targets"])
    if (target.contains(key) && target[key].is_object() &&
        target[key].contains(id))
      return true;
  return false;
}
static std::string literalText(const Json &j) {
  if (j.is_string())
    return j.get<std::string>();
  return j.is_null() ? "" : j.dump();
}
static Json targetBase(const std::string &name, bool stage) {
  return {{"isStage", stage},
          {"name", name},
          {"variables", Json::object()},
          {"lists", Json::object()},
          {"broadcasts", Json::object()},
          {"blocks", Json::object()},
          {"comments", Json::object()},
          {"currentCostume", 0},
          {"costumes", Json::array()},
          {"sounds", Json::array()},
          {"volume", 100},
          {"layerOrder", stage ? 0 : 1},
          {"visible", true},
          {"x", 0},
          {"y", 0},
          {"size", 100},
          {"direction", 90},
          {"draggable", false},
          {"rotationStyle", "all around"}};
}
Project::Project() { fresh(); }
void Project::fresh() {
  doc_ = {
      {"targets", Json::array()},
      {"monitors", Json::array()},
      {"extensions", Json::array()},
      {"meta",
       {{"semver", "3.0.0"}, {"vm", "0.2.0"}, {"agent", "PAX block editor"}}}};
  source_.clear();
  assets_.clear();
  undo_.clear();
  redo_.clear();
  undoBytes_ = 0;
  serial_ = 0;
  target_ = 0;
  auto stage = targetBase("Stage", true);
  stage["tempo"] = 60;
  stage["videoTransparency"] = 50;
  stage["videoState"] = "on";
  doc_["targets"].push_back(stage);
  std::string bg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"480\" "
                   "height=\"360\"><rect "
                   "width=\"480\" height=\"360\" fill=\"#edf4fb\"/></svg>";
  assets_["c20aaa6229d855198c0c9d0e0ef2c860.svg"] = bg;
  doc_["targets"][0]["costumes"].push_back(
      {{"assetId", "c20aaa6229d855198c0c9d0e0ef2c860"},
       {"md5ext", "c20aaa6229d855198c0c9d0e0ef2c860.svg"},
       {"name", "Backdrop"},
       {"dataFormat", "svg"},
       {"bitmapResolution", 1},
       {"rotationCenterX", 240},
       {"rotationCenterY", 180}});
  addSprite("Sprite");
  addVariable("score");
  error.clear();
}
void Project::selectTarget(int index) {
  if (index >= 0 && index < (int)doc_["targets"].size())
    target_ = index;
}
void Project::addSprite(const std::string &name) {
  auto t = targetBase(name.empty() ? "Sprite" : name, false);
  std::string key = "d5fcaa4b94b90df121af16e911a9645c";
  assets_[key + ".svg"] = "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                          "width=\"64\" height=\"64\"><rect x=\"4\" "
                          "y=\"4\" width=\"56\" height=\"56\" rx=\"12\" "
                          "fill=\"#ffb020\"/><circle cx=\"23\" "
                          "cy=\"26\" r=\"5\"/><circle cx=\"41\" cy=\"26\" "
                          "r=\"5\"/><path d=\"M22 44H42\" "
                          "stroke=\"#203040\" stroke-width=\"4\"/></svg>";
  t["costumes"].push_back({{"assetId", key},
                           {"md5ext", key + ".svg"},
                           {"name", "Sprite"},
                           {"dataFormat", "svg"},
                           {"bitmapResolution", 1},
                           {"rotationCenterX", 32},
                           {"rotationCenterY", 32}});
  std::string tone;
  auto u16 = [&](unsigned v) {
    tone += (char)(v & 255);
    tone += (char)(v >> 8);
  };
  auto u32 = [&](unsigned v) {
    u16(v & 65535);
    u16(v >> 16);
  };
  tone += "RIFF";
  u32(36 + 8820);
  tone += "WAVEfmt ";
  u32(16);
  u16(1);
  u16(1);
  u32(22050);
  u32(44100);
  u16(2);
  u16(16);
  tone += "data";
  u32(8820);
  for (int i = 0; i < 4410; i++)
    u16((unsigned short)((i / 25) % 2 ? 6000 : -6000));
  assets_["9ee8cafe0fef0cf90c82fdaef8d88dee.wav"] = std::move(tone);
  t["sounds"].push_back({{"assetId", "9ee8cafe0fef0cf90c82fdaef8d88dee"},
                         {"md5ext", "9ee8cafe0fef0cf90c82fdaef8d88dee.wav"},
                         {"name", "beep"},
                         {"dataFormat", "wav"},
                         {"rate", 22050},
                         {"sampleCount", 4410}});
  t["layerOrder"] = doc_["targets"].size();
  doc_["targets"].push_back(t);
  target_ = doc_["targets"].size() - 1;
}
void Project::addVariable(const std::string &name) {
  if (name.empty())
    return;
  auto &vars = target()["variables"];
  for (auto &v : vars)
    if (v.is_array() && !v.empty() && v[0] == name)
      return;
  std::string id = "pax_var_" + std::to_string(++serial_);
  while (usedId(doc_, id, "variables"))
    id = "pax_var_" + std::to_string(++serial_);
  vars[id] = Json::array({name, 0});
  doc_["monitors"].push_back(
      {{"id", id},
       {"mode", "default"},
       {"opcode", "data_variable"},
       {"params", {{"VARIABLE", name}}},
       {"spriteName",
        target().value("isStage", false) ? Json(nullptr) : target()["name"]},
       {"value", 0},
       {"x", 10},
       {"y", 10 + (int)doc_["monitors"].size() * 26},
       {"width", 0},
       {"height", 0},
       {"visible", false},
       {"sliderMin", 0},
       {"sliderMax", 100},
       {"isDiscrete", true}});
}
void Project::addList(const std::string &name) {
  if (name.empty()) return;
  auto &lists = target()["lists"];
  for (const auto &v : lists) if (v.is_array() && v.size() >= 2 && v[0] == name) return;
  std::string id;
  do { id = "pax_list_" + std::to_string(++serial_); } while (usedId(doc_, id, "lists"));
  lists[id] = Json::array({name, Json::array()});
  doc_["monitors"].push_back({{"id",id},{"mode","list"},{"opcode","data_listcontents"},
    {"params",{{"LIST",name}}},{"spriteName",target().value("isStage",false)?Json(nullptr):target()["name"]},
    {"value",Json::array()},{"x",10},{"y",10},{"width",180},{"height",140},{"visible",false}});
}
std::vector<std::string> Project::choices(const std::string &key) const {
  std::vector<std::string> out;
  auto add = [&](const std::string &name) {if(std::find(out.begin(),out.end(),name)==out.end())out.push_back(name);};
  const auto &t=doc_["targets"][target_];
  if(key=="VARIABLE" || key=="LIST") {
    std::string collection=key=="VARIABLE"?"variables":"lists";
    for(int index:{target_,0})for(const auto &v:doc_["targets"][index][collection])if(v.is_array()&&!v.empty()&&v[0].is_string())add(v[0]);
  } else if(key=="COSTUME" || key=="BACKDROP" || key=="SOUND_MENU" || key=="currentCostume") {
    const auto &owner=key=="BACKDROP"?doc_["targets"][0]:t;
    for(const auto &v:owner[key=="SOUND_MENU"?"sounds":"costumes"])add(v.value("name",""));
  } else if(key=="BROADCAST_OPTION" || key=="BROADCAST_INPUT") {
    for(const auto &v:doc_["targets"][0]["broadcasts"])if(v.is_string())add(v);
  } else if(key=="KEY_OPTION") {
    out={"space","up arrow","down arrow","right arrow","left arrow","any"};
    for(char c='a';c<='z';c++)add(std::string(1,c));for(char c='0';c<='9';c++)add(std::string(1,c));
  } else if(key=="STOP_OPTION") out={"all","this script","other scripts in sprite"};
  else if(key=="rotationStyle" || key=="STYLE") out={"all around","left-right","don't rotate"};
  else if(key=="visible") out={"true","false"};
  else if(key=="TO" || key=="TOWARDS" || key=="TOUCHINGOBJECTMENU") {
    out={"_mouse_"};if(key=="TO")add("_random_");if(key=="TOUCHINGOBJECTMENU")add("_edge_");
    for(const auto &sprite:doc_["targets"])if(!sprite.value("isStage",false)&&sprite.value("name","")!=t.value("name",""))add(sprite.value("name",""));
  }
  return out;
}
bool Project::setTargetProperty(const std::string &key,const std::string &value) {
  auto &t=target();error.clear();
  if(key=="name") {
    if(value.empty()||value.size()>64){error="Use a name of 1 to 64 characters";return false;}
    for(int i=0;i<(int)doc_["targets"].size();i++)if(i!=target_&&doc_["targets"][i].value("name","")==value){error="That sprite name already exists";return false;}
    std::string old=t.value("name","");t[key]=value;
    for(auto &monitor:doc_["monitors"])if(monitor.value("spriteName",Json())==old)monitor["spriteName"]=value;
    // Rename only sprite selectors, never arbitrary text literals.
    auto selector=[](const std::string &k){return k=="TO"||k=="TOWARDS"||k=="TOUCHINGOBJECTMENU"||k=="OBJECT";};
    for(auto &owner:doc_["targets"])for(auto &block:owner["blocks"])if(block.is_object()) {
      if(block.contains("fields"))for(auto &entry:block["fields"].items())if(selector(entry.key())&&entry.value().is_array()&&!entry.value().empty()&&entry.value()[0]==old)entry.value()[0]=value;
      if(block.contains("inputs"))for(auto &entry:block["inputs"].items())if(selector(entry.key())&&entry.value().is_array())for(size_t i=1;i<entry.value().size();i++) {
        auto &literal=entry.value()[i];if(literal.is_array()&&literal.size()==2&&literal[0]==10&&literal[1]==old)literal[1]=value;
      }
    }
    return true;
  }
  if(key=="currentCostume") {for(size_t i=0;i<t["costumes"].size();i++)if(t["costumes"][i].value("name","")==value){t[key]=i;return true;}error="Choose an existing costume";return false;}
  if(key=="visible"){if(value!="true"&&value!="false"){error="Use true or false";return false;}t[key]=value=="true";return true;}
  if(key=="rotationStyle"){auto opts=choices(key);if(std::find(opts.begin(),opts.end(),value)==opts.end()){error="Choose a rotation style";return false;}t[key]=value;return true;}
  if(key!="x"&&key!="y"&&key!="size"&&key!="direction"){error="Unknown sprite property";return false;}
  if(t.value("isStage",false)){error="This property belongs to sprites";return false;}
  char *end=nullptr;double number=std::strtod(value.c_str(),&end);
  if(value.empty()||!end||*end||!std::isfinite(number)||std::abs(number)>1000000||(key=="size"&&(number<0||number>1000))){error="Enter a valid number (size 0 to 1000)";return false;}
  if(key=="direction"){number=std::fmod(number+180,360);if(number<0)number+=360;number-=180;}
  t[key]=number;return true;
}
bool Project::load(const std::string &path) {
  mz_zip_archive zip{};
  ArchiveFile file;
  if (!file.open(zip, path)) {
    error = "Could not open SB3: check SD path and ZIP format";
    return false;
  }
  mz_zip_archive_file_stat jsonStat{};
  int jsonIndex = mz_zip_reader_locate_file(&zip, "project.json", nullptr, 0);
  if (jsonIndex < 0 || !mz_zip_reader_file_stat(&zip, jsonIndex, &jsonStat) ||
      jsonStat.m_uncomp_size > 8 * 1024 * 1024) {
    mz_zip_reader_end(&zip);
    error = "Missing or oversized project.json";
    return false;
  }
  size_t size = 0;
  void *data =
      mz_zip_reader_extract_file_to_heap(&zip, "project.json", &size, 0);
  if (!data || size > 8 * 1024 * 1024) {
    if (data)
      mz_free(data);
    mz_zip_reader_end(&zip);
    error = "Missing or oversized project.json";
    return false;
  }
  Json candidate =
      Json::parse((char *)data, (char *)data + size, nullptr, false);
  mz_free(data);
  mz_zip_reader_end(&zip);
  if (candidate.is_discarded() || !candidate.is_object() ||
      !candidate.contains("targets") || !candidate["targets"].is_array() ||
      candidate["targets"].empty()) {
    error = "Invalid Scratch 3 project";
    return false;
  }
  for (const auto &t : candidate["targets"])
    if (!t.is_object() || !t.contains("blocks") || !t["blocks"].is_object() ||
        !t.contains("costumes") || !t["costumes"].is_array()) {
      error = "Invalid sprite or block data";
      return false;
    }
  doc_ = std::move(candidate);
  if (!doc_.contains("monitors"))
    doc_["monitors"] = Json::array();
  if (!doc_.contains("extensions"))
    doc_["extensions"] = Json::array();
  source_ = path;
  assets_.clear();
  undo_.clear();
  redo_.clear();
  undoBytes_ = 0;
  serial_ = 0;
  target_ = doc_["targets"].size() > 1 ? 1 : 0;
  error.clear();
  return true;
}
bool Project::save(const std::string &path) {
  if (FileSystem::fileExists(path)) {
    error = "File exists; choose a new name";
    return false;
  }
  mz_zip_archive reader{}, writer{};
  ArchiveFile sourceFile;
  bool reading = false;
  if (!source_.empty()) {
    reading = sourceFile.open(reader, source_);
    if (!reading) {
      error = "Source assets could not be opened";
      return false;
    }
  }
  std::string temp = path + ".tmp-" + std::to_string(pax_time_us());
  if (!mz_zip_writer_init_file(&writer, temp.c_str(), 0)) {
    if (reading)
      mz_zip_reader_end(&reader);
    error = "Could not create saved project";
    return false;
  }
  std::string json = doc_.dump();
  bool ok = mz_zip_writer_add_mem(&writer, "project.json", json.data(),
                                  json.size(), MZ_BEST_SPEED);
  std::set<std::string> referencedAssets;
  for (const auto &owner : doc_["targets"])
    for (const auto &kind : {"costumes", "sounds"})
      if (owner.contains(kind))
        for (const auto &asset : owner[kind])
          referencedAssets.insert(asset.value("md5ext", ""));
  if (reading)
    for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&reader); i++) {
      mz_zip_archive_file_stat st{};
      if (!mz_zip_reader_file_stat(&reader, i, &st)) {
        ok = false;
        break;
      }
      std::string name = st.m_filename;
      if (name == "project.json" ||
          (assets_.count(name) && referencedAssets.count(name)))
        continue;
      ok = mz_zip_writer_add_from_zip_reader(&writer, &reader, i);
    }
  for (const auto &item : assets_)
    if (ok && referencedAssets.count(item.first))
      ok =
          mz_zip_writer_add_mem(&writer, item.first.c_str(), item.second.data(),
                                item.second.size(), MZ_BEST_SPEED);
  if (ok)
    ok = mz_zip_writer_finalize_archive(&writer);
  if (!mz_zip_writer_end(&writer))
    ok = false;
  if (reading)
    mz_zip_reader_end(&reader);
  if (ok && pax_svc->file_rename)
    ok = pax_svc->file_rename(temp.c_str(), path.c_str()) == 0;
  else if (ok)
    ok = std::rename(temp.c_str(), path.c_str()) == 0;
  if (!ok) {
    if (pax_svc->file_remove)
      pax_svc->file_remove(temp.c_str());
    else
      std::remove(temp.c_str());
    error = "Save failed; existing files are unchanged";
    return false;
  }

  std::string config = R"({"settings":{"sb3InRam":false}})";
  if (!source_.empty()) {
    std::ifstream in(source_ + ".json");
    if (in) {
      std::string contents((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
      if (contents.size() < 65536) {
        auto parsed = Json::parse(contents, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
          parsed["settings"]["sb3InRam"] = false;
          config = parsed.dump();
        }
      }
    }
  }
  if (!FileSystem::fileExists(path + ".json")) {
    std::ofstream out(path + ".json");
    out << config;
  }
  error.clear();
  return true;
}
std::vector<unsigned char> Project::asset(const std::string &name) const {
  auto own = assets_.find(name);
  if (own != assets_.end())
    return {own->second.begin(), own->second.end()};
  mz_zip_archive z{};
  ArchiveFile file;
  if (source_.empty() || !file.open(z, source_))
    return {};
  mz_zip_archive_file_stat st{};
  int index = mz_zip_reader_locate_file(&z, name.c_str(), nullptr, 0);
  if (index < 0 || !mz_zip_reader_file_stat(&z, index, &st) ||
      st.m_uncomp_size > 8 * 1024 * 1024) {
    mz_zip_reader_end(&z);
    return {};
  }
  size_t size;
  void *p = mz_zip_reader_extract_file_to_heap(&z, name.c_str(), &size, 0);
  std::vector<unsigned char> bytes;
  if (p) {
    if (size < 8 * 1024 * 1024)
      bytes.assign((unsigned char *)p, (unsigned char *)p + size);
    mz_free(p);
  }
  mz_zip_reader_end(&z);
  return bytes;
}
std::string Project::add(const std::string &op, int x, int y) {
  if (op.rfind("@call:", 0) == 0 || op.rfind("@arg:", 0) == 0) {
    std::string id;
    bool used;
    do {
      id = "pax_block_" + std::to_string(++serial_);
      used = false;
      for (const auto &t : doc_["targets"])
        used = used || t["blocks"].contains(id);
    } while (used);
    Json node = {{"next", nullptr},
                 {"parent", nullptr},
                 {"inputs", Json::object()},
                 {"fields", Json::object()},
                 {"shadow", false},
                 {"topLevel", true},
                 {"x", x},
                 {"y", y}};
    if (op.rfind("@arg:", 0) == 0) {
      auto end = op.rfind(':');
      node["opcode"] = op.substr(end) == ":b"
                           ? "argument_reporter_boolean"
                           : "argument_reporter_string_number";
      node["fields"]["VALUE"] = Json::array({op.substr(5, end - 5), nullptr});
    } else {
      std::string code = op.substr(6);
      bool found = false;
      for (const auto &b : blocks())
        if (b.value("opcode", "") == "procedures_prototype" &&
            b.contains("mutation") &&
            b["mutation"].value("proccode", "") == code) {
          node["opcode"] = "procedures_call";
          node["mutation"] = b["mutation"];
          auto ids = Json::parse(b["mutation"].value("argumentids", "[]"));
          size_t at = 0;
          for (const auto &arg : ids) {
            at = code.find('%', at);
            bool boolean = at != std::string::npos && code[at + 1] == 'b';
            if (at != std::string::npos)
              at += 2;
            node["inputs"][arg.get<std::string>()] =
                boolean ? Json::array({2, nullptr})
                        : Json::array({1, Json::array({10, ""})});
          }
          found = true;
          break;
        }
      if (!found) {
        error = "Unknown custom block";
        return "";
      }
    }
    blocks()[id] = node;
    return id;
  }
  const auto *d = definition(op);
  if (!d)
    return "";
  if (op.rfind("paxio_", 0) == 0) {
    auto &extensions = doc_["extensions"];
    if (std::find(extensions.begin(), extensions.end(), "paxio") ==
        extensions.end())
      extensions.push_back("paxio");
  }
  std::string id;
  do {
    id = "pax_block_" + std::to_string(++serial_);
  } while (usedId(doc_, id, "blocks"));
  Json b = {{"opcode", op},
            {"next", nullptr},
            {"parent", nullptr},
            {"inputs", Json::object()},
            {"fields", Json::object()},
            {"shadow", false},
            {"topLevel", true},
            {"x", x},
            {"y", y}};
  blocks()[id] = b;
  for (const auto &s : d->slots)
    literal(id, s, s.value);
  for (int i = 0; i < d->branches; i++)
    blocks()[id]["inputs"][i == 0 ? "SUBSTACK" : "SUBSTACK2"] =
        Json::array({2, nullptr});
  if (d->category == 8) {
    auto &e = doc_["extensions"];
    if (std::find(e.begin(), e.end(), "pen") == e.end())
      e.push_back("pen");
  }
  return id;
}
std::set<std::string> Project::descendants(const std::string &id) const {
  const auto &bs = doc_["targets"][target_]["blocks"];
  std::set<std::string> out;
  std::vector<std::string> todo{id};
  while (!todo.empty() && out.size() < 10000) {
    auto at = todo.back();
    todo.pop_back();
    if (at.empty() || !bs.contains(at) || !out.insert(at).second)
      continue;
    const auto &b = bs[at];
    if (!b.is_object())
      continue;
    if (b.contains("next"))
      todo.push_back(blockReference(b["next"]));
    if (b.contains("inputs") && b["inputs"].is_object())
      for (const auto &s : b["inputs"]) {
        if (s.is_array())
          for (size_t i = 1; i < s.size(); i++)
            if (s[i].is_string())
              todo.push_back(blockReference(s[i]));
      }
  }
  return out;
}
void Project::detach(const std::string &id) {
  if (!blocks().contains(id))
    return;
  auto &b = blocks()[id];
  std::string parent = b.contains("parent") ? blockReference(b["parent"]) : "";
  if (blocks().contains(parent)) {
    auto &p = blocks()[parent];
    if (p.value("next", Json()) == id)
      p["next"] = nullptr;
    if (p.contains("inputs") && p["inputs"].is_object())
      for (auto &s : p["inputs"]) {
        if (s.is_array() && s.size() > 1 && s[1] == id) {
          if (s.size() > 2) {
            s = Json::array({1, s[2]});
          } else
            s = Json::array({2, nullptr});
        }
      }
  }
  b["parent"] = nullptr;
  b["topLevel"] = true;
}
bool Project::attach(const std::string &id, const std::string &parent,
                     const std::string &slot) {
  if (!blocks().contains(id) || !blocks().contains(parent) ||
      descendants(id).count(parent))
    return false;
  const auto *d = definition(blocks()[id].value("opcode", ""));
  if ((d && d->shape == Hat) ||
      blocks()[id].value("opcode", "") == "procedures_definition")
    return false;
  bool reporter =
      (d && (d->shape == Reporter || d->shape == Boolean)) ||
      blocks()[id].value("opcode", "") == "argument_reporter_boolean" ||
      blocks()[id].value("opcode", "") == "argument_reporter_string_number";
  bool sub = slot == "SUBSTACK" || slot == "SUBSTACK2";
  if (reporter && (slot.empty() || sub))
    return false;
  if (!reporter && !slot.empty() && !sub)
    return false;
  detach(id);
  auto &p = blocks()[parent];
  if (reporter) {
    Json previous = p["inputs"].value(slot, Json::array({2, nullptr}));
    if (previous.is_array() && previous.size() > 1 && previous[1].is_string()) {
      auto old = blockReference(previous[1]);
      if (blocks().contains(old) && !blocks()[old].value("shadow", false)) {
        detach(old);
        blocks()[old]["x"] = blocks()[parent].value("x", 30) + 260;
        blocks()[old]["y"] = blocks()[parent].value("y", 30);
      }
    }
    Json fallback = previous.is_array() && previous.size() > 2
                        ? previous[2]
                        : (previous.is_array() && previous.size() > 1 &&
                                   previous[1].is_array()
                               ? previous[1]
                               : Json());
    p["inputs"][slot] = fallback.is_null() ? Json::array({2, id})
                                           : Json::array({3, id, fallback});
  } else {
    std::string old;
    if (slot.empty())
      old = blockReference(p.value("next", Json()));
    else {
      auto input = p["inputs"].value(slot, Json());
      if (input.is_array() && input.size() > 1)
        old = blockReference(input[1]);
    }
    std::string tail = id;
    std::set<std::string> seen;
    while (blocks().contains(tail) && seen.insert(tail).second) {
      auto next = blockReference(blocks()[tail].value("next", Json()));
      if (next.empty() || !blocks().contains(next))
        break;
      tail = next;
    }
    if (slot.empty())
      p["next"] = id;
    else
      p["inputs"][slot] = Json::array({2, id});
    if (!old.empty() && old != id) {
      blocks()[tail]["next"] = old;
      blocks()[old]["parent"] = tail;
      blocks()[old]["topLevel"] = false;
    }
  }
  blocks()[id]["parent"] = parent;
  blocks()[id]["topLevel"] = false;
  return true;
}
void Project::erase(const std::string &id) {
  auto ids = descendants(id);
  detach(id);
  for (auto &s : ids)
    blocks().erase(s);
}
std::string Project::duplicate(const std::string &id) {
  auto ids = descendants(id);
  if (ids.empty())
    return "";
  std::map<std::string, std::string> map;
  for (const auto &s : ids) {
    std::string key;
    do {
      key = "pax_block_" + std::to_string(++serial_);
    } while (usedId(doc_, key, "blocks"));
    map[s] = key;
  }
  for (const auto &s : ids) {
    Json copy = blocks()[s];
    if (copy.contains("next") && copy["next"].is_string() &&
        map.count(blockReference(copy["next"])))
      copy["next"] = map[blockReference(copy["next"])];
    if (copy.contains("parent") && copy["parent"].is_string() &&
        map.count(blockReference(copy["parent"])))
      copy["parent"] = map[blockReference(copy["parent"])];
    if (copy.contains("inputs"))
      for (auto &in : copy["inputs"])
        if (in.is_array())
          for (size_t k = 1; k < in.size(); k++)
            if (in[k].is_string() && map.count(blockReference(in[k])))
              in[k] = map[blockReference(in[k])];
    blocks()[map[s]] = copy;
  }
  if (blocks()[id].value("opcode", "") == "procedures_definition") {
    std::string copiedProto = blocks()[map[id]]["inputs"]["custom_block"][1];
    auto oldCode = blocks()[copiedProto]["mutation"].value("proccode", "");
    std::string code = "copy " + oldCode;
    bool collision;
    do {
      collision = false;
      for (const auto &block : blocks())
        if (block.value("opcode", "") == "procedures_prototype" &&
            block.contains("mutation") &&
            block["mutation"].value("proccode", "") == code)
          collision = true;
      if (collision)
        code = "copy " + code;
    } while (collision);
    blocks()[copiedProto]["mutation"]["proccode"] = code;
    for (const auto &entry : map) {
      auto &block = blocks()[entry.second];
      if (block.value("opcode", "") == "procedures_call" &&
          block.contains("mutation") &&
          block["mutation"].value("proccode", "") == oldCode)
        block["mutation"]["proccode"] = code;
    }
  }
  auto newId = map[id];
  blocks()[newId]["parent"] = nullptr;
  blocks()[newId]["topLevel"] = true;
  blocks()[newId]["x"] = blocks()[id].value("x", 30) + 30;
  blocks()[newId]["y"] = blocks()[id].value("y", 30) + 30;
  return newId;
}
void Project::literal(const std::string &id, const Slot &slot,
                      const std::string &value) {
  if (!blocks().contains(id))
    return;
  auto &b = blocks()[id];
  if (slot.field) {
    Json identifier = nullptr;
    if (slot.key == "VARIABLE" || slot.key == "LIST") {
      const char *collection=slot.key=="VARIABLE"?"variables":"lists";
      auto findId=[&](){for(int index:{target_,0})for(auto it=doc_["targets"][index][collection].begin();it!=doc_["targets"][index][collection].end();++it)
        if(it.value().is_array()&&it.value().size()>=2&&it.value()[0]==value){identifier=it.key();return;}};
      findId();
      if(identifier.is_null()){if(slot.key=="VARIABLE")addVariable(value);else addList(value);findId();}
    }
    if (slot.key == "BROADCAST_OPTION") {
      auto &messages = doc_["targets"][0]["broadcasts"];
      for(auto it=messages.begin();it!=messages.end();++it)if(it.value()==value){identifier=it.key();break;}
      if(identifier.is_null()){std::string key="pax_message_"+value;messages[key]=value;identifier=key;}
    }
    b["fields"][slot.key] = Json::array({value, identifier});
    if (slot.key == "STOP_OPTION")
      b["mutation"] = {
          {"tagName", "mutation"},
          {"children", Json::array()},
          {"hasnext", value == "other scripts in sprite" ? "true" : "false"}};
    return;
  }
  if (b["inputs"].contains(slot.key)) {
    auto old = b["inputs"][slot.key];
    if (old.is_array() && old.size() > 1 && old[1].is_string()) {
      auto child = blockReference(old[1]);
      if (blocks().contains(child) && blocks()[child].value("shadow", false))
        erase(child);
      else if (blocks().contains(child))
        detach(child);
    }
  }
  if (slot.boolean) {
    b["inputs"][slot.key] = Json::array({2, nullptr});
    return;
  }
  bool numeric = !value.empty() &&
                 std::strspn(value.c_str(), "0123456789.-+") == value.size();
  b["inputs"][slot.key] =
      Json::array({1, Json::array({numeric ? 4 : 10, value})});
}
std::vector<Slot> Project::slots(const std::string &id) const {
  const auto &bs = doc_["targets"][target_]["blocks"];
  if (!bs.contains(id))
    return {};
  const auto &b = bs[id];
  if (b.value("opcode", "") == "procedures_definition")
    return {};
  auto *d = definition(b.value("opcode", ""));
  std::vector<Slot> result = d ? d->slots : std::vector<Slot>{};
  if (!d) {
    if (b.contains("inputs") && b["inputs"].is_object())
      for (auto it = b["inputs"].begin(); it != b["inputs"].end(); ++it)
        if (it.key() != "SUBSTACK" && it.key() != "SUBSTACK2")
          result.push_back({it.key(), "", false, it.key() == "CONDITION"});
    if (b.contains("fields") && b["fields"].is_object())
      for (auto it = b["fields"].begin(); it != b["fields"].end(); ++it)
        result.push_back({it.key(), "", true, false});
  }
  if (b.value("opcode", "") == "procedures_call" && b.contains("mutation")) {
    auto code = b["mutation"].value("proccode", "");
    auto ids =
        Json::parse(b["mutation"].value("argumentids", "[]"), nullptr, false);
    size_t at = 0;
    if (ids.is_array())
      for (const auto &arg : ids) {
        at = code.find('%', at);
        bool boolean = at != std::string::npos && at + 1 < code.size() &&
                       code[at + 1] == 'b';
        if (at != std::string::npos)
          at += 2;
        for (auto &slot : result)
          if (slot.key == arg)
            slot.boolean = boolean;
      }
  }
  for (auto &s : result) {
    Json value;
    if (s.field) {
      if (b.contains("fields") && b["fields"].contains(s.key)) {
        value = b["fields"][s.key];
        if (value.is_array() && !value.empty())
          s.value = literalText(value[0]);
      }
    } else if (b.contains("inputs") && b["inputs"].contains(s.key)) {
      value = b["inputs"][s.key];
      if (value.is_array() && value.size() > 1) {
        auto in = value[1];
        if (in.is_array() && in.size() > 1)
          s.value = literalText(in[1]);
        else if (in.is_string()) {
          auto child = blockReference(in);
          if (bs.contains(child)) {
            const auto &shadow = bs[child];
            if (shadow.value("shadow", false) && shadow.contains("fields") &&
                !shadow["fields"].empty()) {
              auto f = shadow["fields"].begin().value();
              if (f.is_array() && !f.empty())
                s.value = literalText(f[0]);
            } else
              s.value = "[block]";
          }
        } else
          s.value = "<>";
      }
    }
  }
  if (b.value("opcode", "") == "procedures_call")
    for (auto &slot : result)
      if (slot.value.empty() && !slot.boolean)
        slot.value = "0";
  return result;
}
void Project::checkpoint() {
  auto s = doc_.dump();
  redo_.clear();
  if (s.size() > 1024 * 1024) {
    undo_.clear();
    undoBytes_ = 0;
    return;
  }
  undoBytes_ += s.size();
  undo_.push_back(std::move(s));
  while (undo_.size() > 20 || undoBytes_ > 1024 * 1024) {
    undoBytes_ -= undo_.front().size();
    undo_.pop_front();
  }
}
bool Project::undo() {
  if (undo_.empty())
    return false;
  redo_.push_back(doc_.dump());
  doc_ = Json::parse(undo_.back());
  undoBytes_ -= undo_.back().size();
  undo_.pop_back();
  target_ = std::min(target_, (int)doc_["targets"].size() - 1);
  return true;
}
bool Project::redo() {
  if (redo_.empty())
    return false;
  auto s = doc_.dump();
  undoBytes_ += s.size();
  undo_.push_back(std::move(s));
  doc_ = Json::parse(redo_.back());
  redo_.pop_back();
  target_ = std::min(target_, (int)doc_["targets"].size() - 1);
  return true;
}
} // namespace PaxEdit
