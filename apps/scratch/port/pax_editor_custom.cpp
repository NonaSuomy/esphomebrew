#include "pax_editor.h"
#include <algorithm>
namespace PaxEdit {
std::string
Project::createCustom(const std::string &name,
                      const std::vector<std::pair<std::string, bool>> &args,
                      bool warp) {
  error.clear();
  if (name.empty() || name.size() > 64 || name.find('%') != std::string::npos ||
      args.size() > 8) {
    error = "Use a block name and at most 8 inputs";
    return "";
  }
  std::set<std::string> names;
  std::string code = name;
  for (const auto &arg : args) {
    if (arg.first.empty() || arg.first.size() > 32 ||
        !names.insert(arg.first).second) {
      error = "Input names must be unique and nonempty";
      return "";
    }
    code += arg.second ? " %b" : " %s";
  }
  for (const auto &b : blocks())
    if (b.is_object() && b.value("opcode", "") == "procedures_prototype" &&
        b.contains("mutation") && b["mutation"].value("proccode", "") == code) {
      error = "That block already exists";
      return "";
    }
  auto unique = [&]() {
    std::string id;
    bool used;
    do {
      id = "pax_block_" + std::to_string(++serial_);
      used = false;
      for (const auto &t : doc_["targets"])
        used = used || t["blocks"].contains(id);
    } while (used);
    return id;
  };
  auto def = unique(), proto = unique();
  Json ids = Json::array(), argNames = Json::array(), defaults = Json::array(),
       inputs = Json::object();
  for (const auto &arg : args) {
    auto id = unique(), reporter = unique();
    ids.push_back(id);
    argNames.push_back(arg.first);
    defaults.push_back(arg.second ? Json(false) : Json(""));
    inputs[id] = Json::array({1, reporter});
    blocks()[reporter] = {
        {"opcode", arg.second ? "argument_reporter_boolean"
                              : "argument_reporter_string_number"},
        {"next", nullptr},
        {"parent", proto},
        {"inputs", Json::object()},
        {"fields", {{"VALUE", Json::array({arg.first, nullptr})}}},
        {"shadow", true},
        {"topLevel", false}};
  }
  Json mutation = {{"tagName", "mutation"},
                   {"children", Json::array()},
                   {"proccode", code},
                   {"argumentids", ids.dump()},
                   {"argumentnames", argNames.dump()},
                   {"argumentdefaults", defaults.dump()},
                   {"warp", warp ? "true" : "false"}};
  blocks()[proto] = {{"opcode", "procedures_prototype"},
                     {"next", nullptr},
                     {"parent", def},
                     {"inputs", inputs},
                     {"fields", Json::object()},
                     {"shadow", true},
                     {"topLevel", false},
                     {"mutation", mutation}};
  blocks()[def] = {{"opcode", "procedures_definition"},
                   {"next", nullptr},
                   {"parent", nullptr},
                   {"inputs", {{"custom_block", Json::array({1, proto})}}},
                   {"fields", Json::object()},
                   {"shadow", false},
                   {"topLevel", true},
                   {"x", 30},
                   {"y", 30}};
  return def;
}
bool Project::updateCustom(
    const std::string &id, const std::string &name,
    const std::vector<std::pair<std::string, bool>> &args, bool warp) {
  if (!blocks().contains(id) ||
      blocks()[id].value("opcode", "") != "procedures_definition") {
    error = "Select a custom definition";
    return false;
  }
  auto oldDef = blocks()[id];
  std::string oldProto = oldDef["inputs"]["custom_block"][1];
  auto old = blocks()[oldProto];
  auto mutation = old["mutation"];
  auto oldIds =
           Json::parse(mutation.value("argumentids", "[]"), nullptr, false),
       oldNames =
           Json::parse(mutation.value("argumentnames", "[]"), nullptr, false);
  if (!oldIds.is_array() || !oldNames.is_array() ||
      args.size() < oldNames.size()) {
    error = "Existing inputs must be retained";
    return false;
  }
  size_t typePos = 0;
  auto oldCode = mutation.value("proccode", "");
  for (size_t i = 0; i < oldNames.size(); i++) {
    typePos = oldCode.find('%', typePos);
    bool boolean = typePos != std::string::npos &&
                   typePos + 1 < oldCode.size() && oldCode[typePos + 1] == 'b';
    if (typePos != std::string::npos)
      typePos += 2;
    if (args[i].second != boolean) {
      error = "Existing input types must be retained";
      return false;
    }
  }
  auto bodyIds = descendants(id);
  blocks().erase(oldProto);
  auto created = createCustom(name, args, warp);
  if (created.empty()) {
    blocks()[oldProto] = old;
    return false;
  }
  auto newDef = blocks()[created];
  std::string newProto = newDef["inputs"]["custom_block"][1];
  auto newMutation = blocks()[newProto]["mutation"];
  auto newIds = Json::parse(newMutation["argumentids"].get<std::string>());
  newDef["next"] = oldDef["next"];
  newDef["x"] = oldDef.value("x", 30);
  newDef["y"] = oldDef.value("y", 30);
  blocks()[id] = newDef;
  blocks().erase(created);
  blocks()[newProto]["parent"] = id;
  for (const auto &input : old["inputs"])
    if (input.size() > 1 && input[1].is_string())
      blocks().erase(input[1].get<std::string>());
  for (auto &b : blocks())
    if (b.is_object() && b.value("opcode", "") == "procedures_call" &&
        b.contains("mutation") &&
        b["mutation"].value("proccode", "") == mutation.value("proccode", "")) {
      Json inputs = Json::object();
      for (size_t i = 0; i < newIds.size(); i++) {
        std::string nextId = newIds[i];
        if (i < oldIds.size() &&
            b["inputs"].contains(oldIds[i].get<std::string>()))
          inputs[nextId] = b["inputs"][oldIds[i].get<std::string>()];
        else
          inputs[nextId] = args[i].second
                               ? Json::array({2, nullptr})
                               : Json::array({1, Json::array({10, "0"})});
      }
      b["inputs"] = inputs;
      b["mutation"] = newMutation;
    }
  for (const auto &blockId : bodyIds)
    if (blocks().contains(blockId)) {
      auto &b = blocks()[blockId];
      if (b.value("opcode", "").rfind("argument_reporter_", 0) == 0) {
        auto value = b["fields"]["VALUE"][0];
        for (size_t i = 0; i < oldNames.size(); i++)
          if (value == oldNames[i]) {
            b["fields"]["VALUE"][0] = args[i].first;
            break;
          }
      }
    }
  error.clear();
  return true;
}
std::vector<std::string> Project::customEntries() const {
  std::vector<std::string> out;
  std::set<std::string> args;
  const auto &bs = doc_["targets"][target_]["blocks"];
  for (const auto &b : bs)
    if (b.is_object() && b.value("opcode", "") == "procedures_prototype" &&
        b.contains("mutation")) {
      const auto &m = b["mutation"];
      auto code = m.value("proccode", "");
      out.push_back("@call:" + code);
      auto names = Json::parse(m.value("argumentnames", "[]"), nullptr, false);
      if (!names.is_array())
        continue;
      size_t at = 0;
      for (const auto &name : names) {
        at = code.find('%', at);
        bool boolean = at != std::string::npos && at + 1 < code.size() &&
                       code[at + 1] == 'b';
        if (at != std::string::npos)
          at += 2;
        if (name.is_string())
          args.insert("@arg:" + name.get<std::string>() +
                      (boolean ? ":b" : ":s"));
      }
    }
  out.insert(out.end(), args.begin(), args.end());
  return out;
}
std::string Project::blockLabel(const std::string &id) const {
  const auto &bs = doc_["targets"][target_]["blocks"];
  if (!bs.contains(id))
    return "";
  const auto &b = bs[id];
  auto op = b.value("opcode", "");
  if (op == "procedures_definition") {
    auto in =
        b.value("inputs", Json::object()).value("custom_block", Json::array());
    if (in.size() > 1 && in[1].is_string() &&
        bs.contains(in[1].get<std::string>()))
      return "define " + blockLabel(in[1]);
  }
  if (op == "argument_reporter_boolean" ||
      op == "argument_reporter_string_number")
    return "{VALUE}";
  if (b.contains("mutation") && b["mutation"].contains("proccode")) {
    auto code = b["mutation"].value("proccode", "");
    auto ids =
        Json::parse(b["mutation"].value("argumentids", "[]"), nullptr, false);
    auto names =
        Json::parse(b["mutation"].value("argumentnames", "[]"), nullptr, false);
    size_t index = 0, pos = 0;
    while ((pos = code.find('%', pos)) != std::string::npos) {
      if (pos + 1 >= code.size() ||
          (code[pos + 1] != 's' && code[pos + 1] != 'b')) {
        pos++;
        continue;
      }
      std::string replacement = "input";
      if (op == "procedures_prototype" && names.is_array() &&
          index < names.size())
        replacement = names[index].get<std::string>();
      else if (ids.is_array() && index < ids.size())
        replacement = "{" + ids[index].get<std::string>() + "}";
      code.replace(pos, 2, replacement);
      pos += replacement.size();
      index++;
    }
    return code;
  }
  const auto *d = definition(op);
  return d ? d->label : op.substr(0, 40);
}
} // namespace PaxEdit
