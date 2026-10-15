#pragma once
#include "pax_port.h"
#include <deque>
#include <functional>
#include <limits>
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
  void addList(const std::string &name);
  std::vector<std::string> choices(const std::string &key) const;
  bool setTargetProperty(const std::string &key, const std::string &value);
  std::string
  createCustom(const std::string &name,
               const std::vector<std::pair<std::string, bool>> &args,
               bool warp);
  bool updateCustom(const std::string &id, const std::string &name,
                    const std::vector<std::pair<std::string, bool>> &args,
                    bool warp);
  std::vector<std::string> customEntries() const;
  std::string blockLabel(const std::string &id) const;
  bool paintCostume(const std::string &name,
                    const std::vector<uint32_t> &pixels, int width = 96,
                    int height = 96,
                    double centerX = std::numeric_limits<double>::quiet_NaN(),
                    double centerY = std::numeric_limits<double>::quiet_NaN());
  bool costumePixels(int index, std::vector<uint32_t> &pixels, int &width,
                     int &height) const;
  bool importAsset(const std::string &path, bool sound);
  bool renameAsset(bool sound, int index, const std::string &name);
  bool deleteAsset(bool sound, int index);
  bool soundPcm(int index, std::vector<int16_t> &samples, int &rate) const;
  bool editSound(int index, double from, double to, double gain, bool reverse,
                 bool fadeIn = false, bool fadeOut = false);
  bool addTone(const std::string &name, double frequency = 440,
               double seconds = 1);
  std::string storeAsset(const std::string &data, const std::string &extension);
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
  std::string selected_,
      status_ = "Drag blocks into the workspace; F5: play, F6: stop";
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
    ListName,
    Choices,
    SpriteProperties,
    SpritePropertyValue,
    Assets,
    AssetActions,
    AssetRename,
    ImportAsset,
    CostumePaint,
    SoundEdit,
    SoundTrim,
    CustomWizard,
    CustomName,
    CustomArgName,
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
  void chooseInput();
  void setZoom(float value);
  void drawScrollbars();
  Rect scrollTrackX_{}, scrollTrackY_{}, scrollThumbX_{}, scrollThumbY_{};
  int scrollMinX_=0, scrollMaxX_=0, scrollMinY_=0, scrollMaxY_=0, scrollDrag_=0, scrollGrab_=0;
  void openAssets(bool sound);
  void openPaint(bool fresh);
  void openSound();
  void drawAssetModal();
  void handleAssetModal(int x, int y);
  float zoom_ = 1;
  bool oldWorkspace_ = false, assetSound_ = false, customWarp_ = false,
       paintStroke_ = false;
  int assetIndex_ = 0, assetPage_ = 0, paintWidth_ = 96, paintHeight_ = 96,
      paintLastX_ = -1, paintLastY_ = -1, argEdit_ = -1;
  double paintCenterX_ = 48, paintCenterY_ = 48;
  uint32_t paintColor_ = 0x4c97ffff;
  std::vector<uint32_t> paintPixels_;
  std::vector<std::vector<uint32_t>> paintUndo_, paintRedo_;
  std::vector<int16_t> soundSamples_;
  int soundRate_ = 22050;
  double trimFrom_ = 0, trimTo_ = 100;
  std::string customEditing_;
  std::string paintName_, customName_ = "my block", trimKey_;
  std::vector<std::pair<std::string, bool>> customArgs_;

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
  float zoom() const { return zoom_; }
  bool closed() const { return closed_; }
};
void open(const std::string &path);
} // namespace PaxEdit
bool pax_play_project(const std::string &path);
