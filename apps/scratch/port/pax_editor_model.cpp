#include "pax_editor.h"
#include <algorithm>
#include <cstring>
#include <filesystem.hpp>
#include <fstream>
#include <miniz.h>
#include <os.hpp>
namespace PaxEdit {
static Slot num(const char *k, const char *v) { return {k, v, false, false}; }
static Slot str(const char *k, const char *v) { return {k, v, false, false}; }
static Slot field(const char *k, const char *v) { return {k, v, true, false}; }
static Slot boolean(const char *k) { return {k, "", false, true}; }
const std::vector<Definition> &definitions() {
    static const std::vector<Definition> defs = {
        {"motion_movesteps", "move steps", 0, Stack, {num("STEPS", "10")}},
        {"motion_turnright", "turn clockwise", 0, Stack, {num("DEGREES", "15")}},
        {"motion_turnleft", "turn counterclockwise", 0, Stack, {num("DEGREES", "15")}},
        {"motion_gotoxy", "go to x / y", 0, Stack, {num("X", "0"), num("Y", "0")}},
        {"motion_changexby", "change x by", 0, Stack, {num("DX", "10")}},
        {"motion_changeyby", "change y by", 0, Stack, {num("DY", "10")}},
        {"motion_pointindirection", "point in direction", 0, Stack, {num("DIRECTION", "90")}},
        {"motion_ifonedgebounce", "if on edge, bounce", 0, Stack, {}},
        {"motion_xposition", "x position", 0, Reporter, {}},
        {"motion_yposition", "y position", 0, Reporter, {}},
        {"looks_say", "say", 1, Stack, {str("MESSAGE", "Hello!")}},
        {"looks_sayforsecs",
         "say for seconds",
         1,
         Stack,
         {str("MESSAGE", "Hello!"), num("SECS", "2")}},
        {"looks_show", "show", 1, Stack, {}},
        {"looks_hide", "hide", 1, Stack, {}},
        {"looks_changesizeby", "change size by", 1, Stack, {num("CHANGE", "10")}},
        {"looks_setsizeto", "set size to %", 1, Stack, {num("SIZE", "100")}},
        {"looks_nextcostume", "next costume", 1, Stack, {}},
        {"looks_switchcostumeto", "switch costume to", 1, Stack, {str("COSTUME", "Sprite")}},
        {"sound_play", "start sound", 2, Stack, {str("SOUND_MENU", "beep")}},
        {"sound_playuntildone", "play sound until done", 2, Stack, {str("SOUND_MENU", "beep")}},
        {"sound_stopallsounds", "stop all sounds", 2, Stack, {}},
        {"sound_setvolumeto", "set volume to %", 2, Stack, {num("VOLUME", "100")}},
        {"event_whenflagclicked", "when green flag clicked", 3, Hat, {}},
        {"event_whenkeypressed", "when key pressed", 3, Hat, {field("KEY_OPTION", "space")}},
        {"event_whenthisspriteclicked", "when sprite clicked", 3, Hat, {}},
        {"event_whenbroadcastreceived",
         "when I receive",
         3,
         Hat,
         {field("BROADCAST_OPTION", "message1")}},
        {"event_broadcast", "broadcast", 3, Stack, {str("BROADCAST_INPUT", "message1")}},
        {"control_wait", "wait seconds", 4, Stack, {num("DURATION", "1")}},
        {"control_repeat", "repeat times", 4, Container, {num("TIMES", "10")}, 1},
        {"control_forever", "forever", 4, Container, {}, 1},
        {"control_if", "if", 4, Container, {boolean("CONDITION")}, 1},
        {"control_if_else", "if / else", 4, Container, {boolean("CONDITION")}, 2},
        {"control_wait_until", "wait until", 4, Stack, {boolean("CONDITION")}},
        {"control_stop", "stop", 4, Cap, {field("STOP_OPTION", "all")}},
        {"sensing_keypressed", "key pressed?", 5, Boolean, {str("KEY_OPTION", "space")}},
        {"sensing_mousedown", "mouse down?", 5, Boolean, {}},
        {"sensing_mousex", "mouse x", 5, Reporter, {}},
        {"sensing_mousey", "mouse y", 5, Reporter, {}},
        {"sensing_timer", "timer", 5, Reporter, {}},
        {"sensing_resettimer", "reset timer", 5, Stack, {}},
        {"sensing_askandwait", "ask and wait", 5, Stack, {str("QUESTION", "What is your name?")}},
        {"sensing_answer", "answer", 5, Reporter, {}},
        {"operator_add", "+", 6, Reporter, {num("NUM1", "1"), num("NUM2", "2")}},
        {"operator_subtract", "-", 6, Reporter, {num("NUM1", "1"), num("NUM2", "2")}},
        {"operator_multiply", "*", 6, Reporter, {num("NUM1", "2"), num("NUM2", "3")}},
        {"operator_divide", "/", 6, Reporter, {num("NUM1", "6"), num("NUM2", "2")}},
        {"operator_random", "pick random", 6, Reporter, {num("FROM", "1"), num("TO", "10")}},
        {"operator_gt", ">", 6, Boolean, {num("OPERAND1", "1"), num("OPERAND2", "0")}},
        {"operator_lt", "<", 6, Boolean, {num("OPERAND1", "0"), num("OPERAND2", "1")}},
        {"operator_equals", "=", 6, Boolean, {str("OPERAND1", "1"), str("OPERAND2", "1")}},
        {"operator_and", "and", 6, Boolean, {boolean("OPERAND1"), boolean("OPERAND2")}},
        {"operator_or", "or", 6, Boolean, {boolean("OPERAND1"), boolean("OPERAND2")}},
        {"operator_not", "not", 6, Boolean, {boolean("OPERAND")}},
        {"operator_join", "join", 6, Reporter, {str("STRING1", "hello "), str("STRING2", "world")}},
        {"data_setvariableto",
         "set variable to",
         7,
         Stack,
         {field("VARIABLE", "score"), num("VALUE", "0")}},
        {"data_changevariableby",
         "change variable by",
         7,
         Stack,
         {field("VARIABLE", "score"), num("VALUE", "1")}},
        {"data_variable", "variable", 7, Reporter, {field("VARIABLE", "score")}},
        {"data_showvariable", "show variable", 7, Stack, {field("VARIABLE", "score")}},
        {"data_hidevariable", "hide variable", 7, Stack, {field("VARIABLE", "score")}},
        {"pen_clear", "erase all pen", 8, Stack, {}},
        {"pen_penDown", "pen down", 8, Stack, {}},
        {"pen_penUp", "pen up", 8, Stack, {}},
        {"pen_stamp", "stamp", 8, Stack, {}},
        {"pen_setPenSizeTo", "set pen size", 8, Stack, {num("SIZE", "3")}},
        {"pen_setPenColorToColor", "set pen color", 8, Stack, {str("COLOR", "#4080ff")}}};
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
        if (target.contains(key) && target[key].is_object() && target[key].contains(id))
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
    doc_ = {{"targets", Json::array()},
            {"monitors", Json::array()},
            {"extensions", Json::array()},
            {"meta", {{"semver", "3.0.0"}, {"vm", "0.2.0"}, {"agent", "PAX block editor"}}}};
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
    std::string bg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"480\" height=\"360\"><rect "
                     "width=\"480\" height=\"360\" fill=\"#edf4fb\"/></svg>";
    assets_["c20aaa6229d855198c0c9d0e0ef2c860.svg"] = bg;
    doc_["targets"][0]["costumes"].push_back({{"assetId", "c20aaa6229d855198c0c9d0e0ef2c860"},
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
    assets_[key + ".svg"] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\"><rect x=\"4\" "
        "y=\"4\" width=\"56\" height=\"56\" rx=\"12\" fill=\"#ffb020\"/><circle cx=\"23\" "
        "cy=\"26\" r=\"5\"/><circle cx=\"41\" cy=\"26\" r=\"5\"/><path d=\"M22 44H42\" "
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
         {"spriteName", target().value("isStage", false) ? Json(nullptr) : target()["name"]},
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
bool Project::load(const std::string &path) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        error = "Could not open project archive";
        return false;
    }
    size_t size = 0;
    void *data = mz_zip_reader_extract_file_to_heap(&zip, "project.json", &size, 0);
    if (!data || size > 8 * 1024 * 1024) {
        if (data)
            mz_free(data);
        mz_zip_reader_end(&zip);
        error = "Missing or oversized project.json";
        return false;
    }
    Json candidate = Json::parse((char *)data, (char *)data + size, nullptr, false);
    mz_free(data);
    mz_zip_reader_end(&zip);
    if (candidate.is_discarded() || !candidate.is_object() || !candidate.contains("targets") ||
        !candidate["targets"].is_array() || candidate["targets"].empty()) {
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
    bool reading = false;
    if (!source_.empty()) {
        reading = mz_zip_reader_init_file(&reader, source_.c_str(), 0);
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
    bool ok =
        mz_zip_writer_add_mem(&writer, "project.json", json.data(), json.size(), MZ_BEST_SPEED);
    if (reading)
        for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&reader); i++) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&reader, i, &st)) {
                ok = false;
                break;
            }
            std::string name = st.m_filename;
            if (name == "project.json" || assets_.count(name))
                continue;
            ok = mz_zip_writer_add_from_zip_reader(&writer, &reader, i);
        }
    for (const auto &item : assets_)
        if (ok)
            ok = mz_zip_writer_add_mem(&writer, item.first.c_str(), item.second.data(),
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
    if (source_.empty() || !mz_zip_reader_init_file(&z, source_.c_str(), 0))
        return {};
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
    const auto *d = definition(op);
    if (!d)
        return "";
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
        blocks()[id]["inputs"][i == 0 ? "SUBSTACK" : "SUBSTACK2"] = Json::array({2, nullptr});
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
bool Project::attach(const std::string &id, const std::string &parent, const std::string &slot) {
    if (!blocks().contains(id) || !blocks().contains(parent) || descendants(id).count(parent))
        return false;
    const auto *d = definition(blocks()[id].value("opcode", ""));
    if (d && d->shape == Hat)
        return false;
    bool reporter = d && (d->shape == Reporter || d->shape == Boolean);
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
                            : (previous.is_array() && previous.size() > 1 && previous[1].is_array()
                                   ? previous[1]
                                   : Json());
        p["inputs"][slot] =
            fallback.is_null() ? Json::array({2, id}) : Json::array({3, id, fallback});
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
    auto newId = map[id];
    blocks()[newId]["parent"] = nullptr;
    blocks()[newId]["topLevel"] = true;
    blocks()[newId]["x"] = blocks()[id].value("x", 30) + 30;
    blocks()[newId]["y"] = blocks()[id].value("y", 30) + 30;
    return newId;
}
void Project::literal(const std::string &id, const Slot &slot, const std::string &value) {
    if (!blocks().contains(id))
        return;
    auto &b = blocks()[id];
    if (slot.field) {
        Json identifier = nullptr;
        if (slot.key == "VARIABLE") {
            addVariable(value);
            for (auto it = target()["variables"].begin(); it != target()["variables"].end(); ++it)
                if (it.value()[0] == value) {
                    identifier = it.key();
                    break;
                }
        }
        if (slot.key == "BROADCAST_OPTION") {
            auto &messages = doc_["targets"][0]["broadcasts"];
            std::string key = "pax_message_" + value;
            messages[key] = value;
            identifier = key;
        }
        b["fields"][slot.key] = Json::array({value, identifier});
        if (slot.key == "STOP_OPTION")
            b["mutation"] = {{"tagName", "mutation"},
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
    bool numeric = !value.empty() && std::strspn(value.c_str(), "0123456789.-+") == value.size();
    b["inputs"][slot.key] = Json::array({1, Json::array({numeric ? 4 : 10, value})});
}
std::vector<Slot> Project::slots(const std::string &id) const {
    const auto &bs = doc_["targets"][target_]["blocks"];
    if (!bs.contains(id))
        return {};
    const auto &b = bs[id];
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
