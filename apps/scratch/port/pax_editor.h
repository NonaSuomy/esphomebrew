#pragma once
#include "pax_port.h"
#include <deque>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
namespace PaxEdit {
using Json = nlohmann::json;
enum Shape { Stack, Hat, Container, Reporter, Boolean, Cap };
struct Slot {
  std::string key, value;
  bool field = false, boolean = false;
};
struct Definition {
  const char *opcode, *label;
  int category;
  Shape shape;
  std::vector<Slot> slots;
  int branches = 0;
};
const std::vector<Definition> &definitions();
const Definition *definition(const std::string &opcode);
class Project {
  Json doc_;
  int target_ = 1;
  unsigned serial_ = 0;
  std::string source_;
  std::map<std::string, std::string> assets_;
  std::deque<std::string> undo_, redo_;
  size_t undoBytes_ = 0;

public:
  std::string error;
  Project();
  void fresh();
  bool load(const std::string &path);
  bool save(const std::string &path);
  const Json &document() const { return doc_; }
  Json &document() { return doc_; }
  Json &target() { return doc_["targets"][target_]; }
  Json &blocks() { return target()["blocks"]; }
  int targetIndex() const { return target_; }
  void selectTarget(int index);
  std::string source() const { return source_; }
  std::string add(const std::string &opcode, int x, int y);
  void detach(const std::string &id);
  bool attach(const std::string &id, const std::string &parent,
              const std::string &slot = "");
  void erase(const std::string &id);
  std::string duplicate(const std::string &id);
  std::set<std::string> descendants(const std::string &id) const;
  void literal(const std::string &id, const Slot &slot,
               const std::string &value);
  std::vector<Slot> slots(const std::string &id) const;
  void checkpoint();
  bool undo();
  bool redo();
  void addSprite(const std::string &name);
  void addVariable(const std::string &name);
  std::vector<unsigned char> asset(const std::string &name) const;
};
struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};
struct Hit {
  Rect rect;
  std::string id;
  Slot slot;
  bool input = false;
};
struct Dock {
  Rect rect;
  std::string id, slot;
  bool input = false, boolean = false;
};
class Editor {
  Project project_;
  int width_ = 800, height_ = 480;
  int category_ = 3, palettePage_ = 0;
  int viewX_ = 20, viewY_ = 25;
  std::string selected_, status_ = "Drag blocks into the workspace; F5: play, F6: stop";
  std::vector<Hit> hits_;
  std::vector<Dock> docks_;
  std::vector<std::pair<Rect, std::string>> palette_;
  bool down_ = false, moved_ = false, dragging_ = false, pan_ = false,
       panMode_ = false, dirty_ = false, closed_ = false;
  int pressX_ = 0, pressY_ = 0, oldX_ = 0, oldY_ = 0, offsetX_ = 0,
      offsetY_ = 0;
  std::string pending_, palettePending_;
  bool pendingInput_ = false;
  Slot pendingSlot_;
  enum Modal {
    None,
    InputValue,
    ProjectName,
    SpriteName,
    VariableName,
    ConfirmBack,
    ConfirmNew,
    OpenFile,
    HardwareChannels,
    ReporterInputs
  };
  Modal modal_ = None;
  std::string text_, editId_;
  Slot editSlot_;
  bool shift_ = false;
  std::vector<std::pair<Rect, std::string>> keys_;
  std::vector<std::string> files_;
  int filePage_ = 0;
  std::vector<uint16_t> preview_;
  std::string previewLabel_ = "Stage";
  std::function<void(const std::string &)> player_;
  Rect drawHeader(const std::string &id, int x, int y,
                  std::set<std::string> &ancestors, bool paint,
                  int forcedWidth = 0, bool background = true);
  Rect measureStack(const std::string &id, std::set<std::string> &visited,
                    int depth);
  int drawStack(const std::string &id, int x, int y,
                std::set<std::string> &visited, bool dragged = false);
  void drawModal();
  void handleModal(int x, int y);
  void commitText();
  void refreshPreview();
  void home();
  void run();
  void save();
  bool drop(const std::string &id, int x, int y);

public:
  explicit Editor(std::function<void(const std::string &)> player = {});
  Project &project() { return project_; }
  const std::vector<Hit> &hits() const { return hits_; }
  const std::vector<std::pair<Rect, std::string>> &palette() const {
    return palette_;
  }
  const std::vector<Dock> &docks() const { return docks_; }
  bool load(const std::string &path);
  void render();
  void pointer(int x, int y, bool down);
  void keyboard(const std::string &text);
  void scroll(int amount, int x);
  bool recoverDraft();
  bool closed() const { return closed_; }
};
void open(const std::string &path);
} // namespace PaxEdit
bool pax_play_project(const std::string &path);
