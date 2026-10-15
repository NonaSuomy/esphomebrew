#include "pax_editor.h"
#include "pax_icons.h"
#include <algorithm>
#include <audiostack.hpp>
#include <cmath>
#include <filesystem.hpp>
#include <font_atlas.hpp>
#include <fstream>
#include <input.hpp>
#include <iomanip>
#include <lunasvg.h>
#include <os.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <sstream>
#include <stb_image.h>
namespace PaxEdit {
static const char *categories[] = {
    "Motion",    "Looks",     "Sound", "Events",   "Control",  "Sensing",
    "Operators", "Variables", "Pen",   "Board IO", "My Blocks"};
static const int colors[] = {0x4c97ff, 0x9966ff, 0xcf63cf, 0xffbf00,
                             0xffab19, 0x5cb1d6, 0x59c059, 0xff8c1a,
                             0x0fbd8c, 0x0f9f96, 0xff6680};
static int panelWidth() { return 177 + std::max(0, PAX_W - 800) / 3; }
static int previewHeight() { return (panelWidth() - 17) * 3 / 4; }
static int paletteWidth() {
  return std::clamp(174 + (PAX_W - 800) / 4, 174, 220);
}
static int paletteRight() { return 75 + paletteWidth(); }
static Rect workspace() {
  int left = paletteRight() + 5;
  return {left, 58, PAX_W - panelWidth() - 21 - left, PAX_H - 121};
}
// Extra panel pixels belong to the script workspace; stage and footer stay
// anchored.
static Rect R(Rect r) {
  const bool footer = r.y >= 382;
  if (r.x >= 623) {
    r.x = PAX_W - panelWidth() + (r.x - 623) * panelWidth() / 177;
    r.w = r.w * panelWidth() / 177;
    if (r.y >= 210 && r.y < 382)
      r.y += previewHeight() - 120;
  }
  if (footer)
    r.y += PAX_H - 480;
  return r;
}
static int categoryStep() { return std::min(39, (PAX_H - 105) / 11); }
static Rect categoryRect(int i) {
  return {4, 60 + i * categoryStep(), 65, categoryStep() - 4};
}

static std::string blockReference(const Json &j) {
  return j.is_string() ? j.get<std::string>() : "";
}
static bool modalDrawing = false;
static void box(Rect r, int color) {
  if (modalDrawing) {
    r.x += (PAX_W - 800) / 2;
    r.y += (PAX_H - 480) / 2;
  }
  Render::drawBox(r.w, r.h, r.x + r.w / 2, r.y + r.h / 2, (color >> 16) & 255,
                  (color >> 8) & 255, color & 255);
}
// Cached antialiased font and vector contours keep redraws allocation-light.
static std::shared_ptr<FontAtlas> uiFont;
static FontGeneration *font(int size) {
  if (!uiFont) {
    uiFont = std::make_shared<FontAtlas>();
    std::ifstream file(OS::getRomFSLocation() +
                           "gfx/ingame/fonts/NotoSans-Medium.ttf",
                       std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), {});
    if (!data.empty())
      uiFont->loadFromMemory(std::move(data));
  }
  if (!uiFont->isValid())
    return nullptr;
  static std::vector<uint32_t> ascii;
  if (ascii.empty())
    for (int c = 32; c < 127; c++)
      ascii.push_back(c);
  return &uiFont->ensureGeneration(size, ascii);
}
static int textWidth(const std::string &s, int size = 14) {
  auto *g = font(size);
  if (!g)
    return s.size() * size / 2;
  float px = 0, py = 0;
  GlyphQuad q;
  for (unsigned char c : s)
    uiFont->getGlyphQuad(*g, c < 128 ? c : '?', px, py, q);
  return (int)std::ceil(px);
}
static void text(const std::string &s, int x, int y, int color = 0x575e75ff,
                 float scale = .75f, bool centered = false) {
  if (modalDrawing) {
    x += (PAX_W - 800) / 2;
    y += (PAX_H - 480) / 2;
  }
  int size = std::max(10, (int)std::lround(scale * 20));
  auto *g = font(size);
  if (!g) {
    pax_draw_text(s, x, y, color, scale, centered);
    return;
  }
  float px = centered ? x - textWidth(s, size) / 2 : x, py = y + g->ascent;
  for (unsigned char c : s) {
    GlyphQuad q;
    if (!uiFont->getGlyphQuad(*g, c < 128 ? c : '?', px, py, q) || !q.valid)
      continue;
    int w = std::max(1, (int)(q.x1 - q.x0)),
        h = std::max(1, (int)(q.y1 - q.y0));
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++) {
        int sx =
            std::clamp((int)((q.s0 + (q.s1 - q.s0) * xx / w) * g->atlasWidth),
                       0, g->atlasWidth - 1);
        int sy =
            std::clamp((int)((q.t0 + (q.t1 - q.t0) * yy / h) * g->atlasHeight),
                       0, g->atlasHeight - 1);
        pax_blend((int)q.x0 + xx, (int)q.y0 + yy, (color >> 24) & 255,
                  (color >> 16) & 255, (color >> 8) & 255,
                  g->pixels[sy * g->atlasWidth + sx] * (color & 255) / 255);
      }
  }
}
struct Contour {
  int w, h, color, type, top, bitmapHeight;
  std::string variant;
  std::vector<unsigned char> pixels;
};
static std::deque<Contour> contours;
static void contour(Rect r, int color, int type, bool selected = false,
                    const std::vector<std::pair<int, int>> &mouths = {},
                    bool bottomTab = true) {
  if (r.w <= 0 || r.h <= 0 || r.x >= PAX_W || r.y >= PAX_H || r.x + r.w < 0 ||
      r.y + r.h < 0)
    return;
  int pad = type == Hat ? 9 : 1;
  int top = std::max(0, pad - r.y),
      bitmapHeight =
          std::min(r.h + pad + 7, (int)(PAX_H / pax_ui_zoom) - r.y + pad) - top;
  if (bitmapHeight <= 0)
    return;
  std::string variant;
  for (auto mouth : mouths)
    variant +=
        std::to_string(mouth.first) + ":" + std::to_string(mouth.second) + ";";
  variant += bottomTab ? "tab" : "cap";
  auto it =
      std::find_if(contours.begin(), contours.end(), [&](const Contour &c) {
        return c.w == r.w && c.h == r.h && c.color == color && c.type == type &&
               c.top == top && c.bitmapHeight == bitmapHeight &&
               c.variant == variant;
      });
  if (it == contours.end()) {
    size_t bytes = (size_t)(r.w + 2) * bitmapHeight * 4;
    size_t cached = 0;
    for (auto &c : contours)
      cached += c.pixels.size();
    while (!contours.empty() &&
           (contours.size() >= 64 || cached + bytes > 4 * 1024 * 1024)) {
      cached -= contours.front().pixels.size();
      contours.pop_front();
    }
    std::ostringstream svg;
    int h = r.h, w = r.w;
    std::string path;
    if (type == Container && !mouths.empty()) {
      // One continuous C contour: inner tabs use the child's 18px indent,
      // while outer tabs stay aligned with the surrounding script stack.
      path = "M 4 0 H 18 L 24 5 H 38 L 44 0 H " + std::to_string(w - 4) +
             " Q " + std::to_string(w) + " 0 " + std::to_string(w) + " 4 ";
      for (auto mouth : mouths) {
        int a = mouth.first, b = mouth.second;
        path += "V " + std::to_string(a - 4) + " Q " + std::to_string(w) + " " +
                std::to_string(a) + " " + std::to_string(w - 4) + " " +
                std::to_string(a) + " H 62 L 56 " + std::to_string(a + 5) +
                " H 42 L 36 " + std::to_string(a) + " H 22 Q 18 " +
                std::to_string(a) + " 18 " + std::to_string(a + 4) + " V " +
                std::to_string(b - 4) + " Q 18 " + std::to_string(b) + " 22 " +
                std::to_string(b) + " H 36 L 42 " + std::to_string(b + 5) +
                " H 56 L 62 " + std::to_string(b) + " H " +
                std::to_string(w - 4) + " Q " + std::to_string(w) + " " +
                std::to_string(b) + " " + std::to_string(w) + " " +
                std::to_string(b + 4) + " ";
      }
      path += "V " + std::to_string(h - 4) + " Q " + std::to_string(w) + " " +
              std::to_string(h) + " " + std::to_string(w - 4) + " " +
              std::to_string(h);
      if (bottomTab)
        path += " H 44 L 38 " + std::to_string(h + 5) + " H 24 L 18 " +
                std::to_string(h);
      path += " H 4 Q 0 " + std::to_string(h) + " 0 " + std::to_string(h - 4) +
              " V 4 Q 0 0 4 0 Z";
    } else if (type == Boolean)
      path = "M 12 0 H " + std::to_string(w - 12) + " L " + std::to_string(w) +
             " " + std::to_string(h / 2) + " L " + std::to_string(w - 12) +
             " " + std::to_string(h) + " H 12 L 0 " + std::to_string(h / 2) +
             " Z";
    else if (type == Reporter || type == 7)
      path = "M " + std::to_string(h / 2) + " 0 H " +
             std::to_string(w - h / 2) + " A " + std::to_string(h / 2) + " " +
             std::to_string(h / 2) + " 0 0 1 " + std::to_string(w - h / 2) +
             " " + std::to_string(h) + " H " + std::to_string(h / 2) + " A " +
             std::to_string(h / 2) + " " + std::to_string(h / 2) + " 0 0 1 " +
             std::to_string(h / 2) + " 0 Z";
    else {
      path = "M 4 0 ";
      if (type == Hat)
        path += "Q 30 -16 70 0 ";
      else if (type != 7 && type != 8)
        path += "H 18 L 24 5 H 38 L 44 0 ";
      path += "H " + std::to_string(w - 4) + " Q " + std::to_string(w) + " 0 " +
              std::to_string(w) + " 4 V " + std::to_string(h - 4) + " Q " +
              std::to_string(w) + " " + std::to_string(h) + " " +
              std::to_string(w - 4) + " " + std::to_string(h) + " ";
      if (type != Cap && type != 8)
        path += "H 44 L 38 " + std::to_string(h + 5) + " H 24 L 18 " +
                std::to_string(h) + " ";
      path += "H 4 Q 0 " + std::to_string(h) + " 0 " + std::to_string(h - 4) +
              " V 4 Q 0 0 4 0 Z";
    }
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << w + 2
        << "' height='" << bitmapHeight << "' viewBox='0 " << top << " "
        << w + 2 << " " << bitmapHeight << "'><path transform='translate(1 "
        << pad << ")' d='" << path << "' fill='#" << std::hex << std::setw(6)
        << std::setfill('0') << color << "' stroke='#" << std::setw(6)
        << (((color & 0xfefefe) >> 1) + ((color & 0xfcfcfc) >> 2))
        << "' stroke-width='1'/></svg>";
    auto doc = lunasvg::Document::loadFromData(svg.str());
    if (!doc) {
      box(r, color);
      return;
    }
    auto bm = doc->renderToBitmap();
    bm.convertToRGBA();
    contours.push_back({r.w,
                        r.h,
                        color,
                        type,
                        top,
                        bitmapHeight,
                        variant,
                        {bm.data(), bm.data() + bm.width() * bm.height() * 4}});
    it = std::prev(contours.end());
  }
  int w = r.w + 2, h = it->bitmapHeight;
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) {
      auto *c = &it->pixels[(yy * w + xx) * 4];
      pax_blend(r.x + xx - 1, r.y + yy - pad + it->top, c[0], c[1], c[2], c[3]);
      if (selected && c[3] > 0 && c[3] < 200)
        pax_blend(r.x + xx - 1, r.y + yy - pad + it->top, 255, 230, 60, 255);
    }
}
static void button(Rect r, const std::string &s, int color = 0xe9f1fc) {
  Rect draw = r;
  if (modalDrawing) {
    draw.x += (PAX_W - 800) / 2;
    draw.y += (PAX_H - 480) / 2;
  }
  contour(draw, color, 8);
  text(s, r.x + r.w / 2, r.y + 7, 0x575e75ff, .70f, true);
}
static void toolbarButton(Rect r, PaxIcon icon, const std::string &label,
                          int color = 0xe9f1fc) {
  contour(r, color, 8);
  if (label.empty()) {
    pax_draw_icon(icon, r.x + (r.w - 24) / 2, r.y + (r.h - 24) / 2, 24);
    return;
  }
  // Keep the label and icon together with a visible gap at both canvas sizes.
  const int glyph = 15, gap = 5;
  const int labelWidth = static_cast<int>(label.size()) * 6;
  const int left = r.x + (r.w - glyph - gap - labelWidth) / 2;
  pax_draw_icon(icon, left, r.y + (r.h - glyph) / 2, glyph);
  text(label, left + glyph + gap, r.y + 7, 0x575e75ff, .70f);
}
static std::string fieldLabel(const std::string &key,bool stage=false) {
  if(key=="name")return "Name";if(key=="x")return "X";if(key=="y")return "Y";
  if(key=="size")return "Size (%)";if(key=="direction")return "Direction";if(key=="visible")return "Visible";
  if(key=="currentCostume")return stage?"Backdrop":"Costume";
  if(key=="rotationStyle"||key=="STYLE")return "Rotation";
  if(key=="VARIABLE")return "Variable";if(key=="LIST")return "List";
  if(key=="COSTUME")return "Costume";if(key=="BACKDROP")return "Backdrop";if(key=="SOUND_MENU")return "Sound";
  if(key=="BROADCAST_OPTION"||key=="BROADCAST_INPUT")return "Message";
  if(key=="KEY_OPTION")return "Key";if(key=="STOP_OPTION")return "Stop";
  if(key=="TO"||key=="TOWARDS"||key=="TOUCHINGOBJECTMENU")return "Target";
  return key;
}
static std::string choiceLabel(const std::string &value) {
  if(value=="_mouse_")return "mouse-pointer";if(value=="_random_")return "random position";if(value=="_edge_")return "edge";return value;
}
static std::vector<std::string> propertyKeys(Project &p) {
  return p.target().value("isStage",false)?std::vector<std::string>{"name","currentCostume"}:
    std::vector<std::string>{"name","x","y","size","direction","visible","currentCostume","rotationStyle"};
}
static std::string propertyValue(Project &p,const std::string &key) {
  auto &t=p.target();
  if(key=="currentCostume") {int i=t.value("currentCostume",0);return i>=0&&i<(int)t["costumes"].size()?t["costumes"][i].value("name",""):"";}
  if(!t.contains(key))return "";return t[key].is_string()?t[key].get<std::string>():t[key].dump();
}
static Shape shape(const Json &b) {
  auto op = b.value("opcode", "");
  if (op == "procedures_definition")
    return Hat;
  if (op == "argument_reporter_boolean")
    return Boolean;
  if (op == "argument_reporter_string_number")
    return Reporter;
  const auto *d = definition(b.value("opcode", ""));
  if (d)
    return d->shape;
  if (b.contains("inputs") && b["inputs"].contains("SUBSTACK"))
    return Container;
  return Stack;
}
Editor::Editor(std::function<void(const std::string &)> player)
    : player_(std::move(player)) {
  width_ = PAX_W;
  height_ = PAX_H;
  home();
  refreshPreview();
}
bool Editor::load(const std::string &path) {
  if (!project_.load(path)) {
    status_ = project_.error;
    return false;
  }
  dirty_ = false;
  home();
  refreshPreview();
  status_ = "Opened " + path.substr(path.find_last_of('/') + 1);
  return true;
}
void Editor::home() {
  viewX_ = 20;
  viewY_ = 25;
  int mx = 100000, my = 100000;
  for (const auto &b : project_.blocks())
    if (b.is_object() && b.value("topLevel", false) &&
        !b.value("shadow", false)) {
      mx = std::min(mx, b.value("x", 30));
      my = std::min(my, b.value("y", 30));
    }
  if (mx < 100000) {
    viewX_ = 20 - mx;
    viewY_ = 25 - my;
  }
}
struct Piece {
  std::string text;
  int slot = -1;
};
static std::vector<Piece> pieces(const std::string &label,
                                 const std::vector<Slot> &slots) {
  std::vector<Piece> out;
  std::set<int> used;
  size_t pos = 0;
  while (pos < label.size()) {
    size_t a = label.find('{', pos),
           b = a == std::string::npos ? a : label.find('}', a);
    if (a == std::string::npos || b == std::string::npos) {
      out.push_back({label.substr(pos), -1});
      break;
    }
    if (a > pos)
      out.push_back({label.substr(pos, a - pos), -1});
    auto key = label.substr(a + 1, b - a - 1);
    for (size_t i = 0; i < slots.size(); i++)
      if (slots[i].key == key) {
        out.push_back({"", (int)i});
        used.insert(i);
        break;
      }
    pos = b + 1;
  }
  for (size_t i = 0; i < slots.size(); i++)
    if (!used.count(i)) {
      out.push_back({" ", -1});
      out.push_back({"", (int)i});
    }
  // Whitespace is layout, not part of a token's measured glyph string.
  for (auto &piece : out)
    if (piece.slot < 0) {
      auto first = piece.text.find_first_not_of(' '),
           last = piece.text.find_last_not_of(' ');
      piece.text = first == std::string::npos
                       ? ""
                       : piece.text.substr(first, last - first + 1);
    }
  out.erase(std::remove_if(
                out.begin(), out.end(),
                [](const Piece &p) { return p.slot < 0 && p.text.empty(); }),
            out.end());
  return out;
}
static std::string inputText(const Slot &s) {
  return s.value.empty() || s.value == "<>" ? (s.boolean ? "" : s.key)
                                            : s.value.substr(0, 24);
}
static int fieldColor(int color) {
  return (((color >> 16) & 255) * 9 / 10 << 16) |
         (((color >> 8) & 255) * 9 / 10 << 8) | ((color & 255) * 9 / 10);
}
// Reporters are drawn recursively inside sockets, so nested arithmetic remains
// visible and editable.
Rect Editor::drawHeader(const std::string &id, int x, int y,
                        std::set<std::string> &ancestors, bool paint,
                        int forcedWidth, bool background) {
  auto &bs = project_.blocks();
  if (!bs.contains(id) || !bs[id].is_object() || ancestors.size() >= 16 ||
      !ancestors.insert(id).second)
    return {x, y, 48, 32};
  auto &b = bs[id];
  auto op = b.value("opcode", "");
  auto *d = definition(op);
  auto type = shape(b);
  auto slots = project_.slots(id);
  auto ps = pieces(project_.blockLabel(id), slots);
  int color =
      d ? colors[d->category]
        : (op.rfind("procedures_", 0) == 0 || op.rfind("argument_", 0) == 0
               ? 0xff6680
               : 0x71849a);
  std::vector<Rect> cells(slots.size());
  std::vector<std::string> children(slots.size());
  int padding = type == Boolean ? 18 : 16;
  int width = padding * 2 + std::max(0, (int)ps.size() - 1) * 10,
      height = type == Reporter || type == Boolean ? 32 : 36;
  for (auto &part : ps) {
    if (part.slot < 0) {
      width += textWidth(part.text);
      continue;
    }
    auto &slot = slots[part.slot];
    Rect cell{0, 0, std::max(32, textWidth(inputText(slot)) + 18), 26};
    if (slot.boolean)
      cell.w = 44;
    if (slot.value == "[block]" && b.contains("inputs")) {
      auto in = b["inputs"].value(slot.key, Json());
      if (in.is_array() && in.size() > 1 && in[1].is_string()) {
        auto child = blockReference(in[1]);
        if (bs.contains(child) && bs[child].is_object() &&
            (shape(bs[child]) == Reporter || shape(bs[child]) == Boolean)) {
          children[part.slot] = child;
          cell = drawHeader(child, 0, 0, ancestors, false);
        }
      }
    }
    cells[part.slot] = cell;
    width += cell.w;
    height = std::max(height, cell.h + 10);
  }
  width = std::max(forcedWidth, std::clamp(width, 56, 1200));
  Rect head{x, y, width, height};
  if (paint) {
    if (background)
      contour(head, color, type, selected_ == id);
    hits_.push_back({head, id, {}, false});
    int xx = x + padding;
    for (auto &part : ps) {
      if (part.slot < 0) {
        text(part.text, xx, y + (height - 14) / 2, 0xffffffff, .7f);
        xx += textWidth(part.text) + 10;
        continue;
      }
      auto slot = slots[part.slot];
      auto cell = cells[part.slot];
      cell.x = xx;
      cell.y = y + (height - cell.h) / 2;
      if (!children[part.slot].empty())
        drawHeader(children[part.slot], cell.x, cell.y, ancestors, true);
      else {
        contour(cell,
                slot.boolean ? 0x3e963e
                             : (slot.field ? fieldColor(color) : 0xffffff),
                slot.boolean ? Boolean : (slot.field ? 8 : Reporter));
        text(inputText(slot), cell.x + cell.w / 2, cell.y + (cell.h - 14) / 2,
             slot.boolean || slot.field ? 0xffffffff : 0x575e75ff, .7f, true);
        hits_.push_back({cell, id, slot, true});
      }
      if (!slot.field)
        docks_.push_back({{cell.x - 4, cell.y - 4, cell.w + 8, cell.h + 8},
                          id,
                          slot.key,
                          true,
                          slot.boolean});
      xx += cell.w + 10;
    }
  }
  ancestors.erase(id);
  return head;
}
struct PalettePart {
  Piece piece;
  Rect rect;
};
struct PaletteLayout {
  int width = 0, bodyHeight = 0, headerHeight = 0, top = 0, bottom = 0;
  std::vector<PalettePart> parts;
  int height() const { return top + bodyHeight + bottom + 2; }
};
static PaletteLayout paletteLayout(const Definition &d) {
  PaletteLayout result;
  int limit = paletteWidth() - 16;
  int padding = d.shape == Boolean ? 16 : 12, x = padding, row = 0, right = 0;
  auto parts = pieces(d.label, d.slots);
  for (auto part : parts) {
    // Long literal phrases wrap at words; inputs remain intact.
    std::vector<Piece> segments{part};
    if (part.slot < 0 && textWidth(part.text, 12) > limit - 2 * padding) {
      segments.clear();
      std::istringstream words(part.text);
      std::string word, line;
      while (words >> word) {
        std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && textWidth(next, 12) > limit - 2 * padding) {
          segments.push_back({line, -1});
          line = word;
        } else
          line = next;
      }
      if (!line.empty())
        segments.push_back({line, -1});
    }
    for (auto segment : segments) {
      int w =
          segment.slot < 0
              ? textWidth(segment.text, 12)
              : std::max(28,
                         textWidth(inputText(d.slots[segment.slot]), 12) + 18);
      if (segment.slot >= 0 && d.slots[segment.slot].boolean)
        w = 40;
      w = std::min(w, limit - 2 * padding);
      if (x > padding && x + w > limit - padding) {
        row++;
        x = padding;
      }
      result.parts.push_back({segment, {x, 5 + row * 28, w, 24}});
      right = std::max(right, x + w);
      x += w + 6;
    }
  }
  result.width = std::max(d.shape == Container ? 88 : 56, right + padding);
  result.headerHeight = 34 + row * 28;
  result.bodyHeight = result.headerHeight;
  if (d.shape == Container)
    result.bodyHeight +=
        d.branches * 24 + std::max(0, d.branches - 1) * 24 + 16;
  result.top = d.shape == Hat ? 9 : 0;
  result.bottom =
      d.shape == Hat || d.shape == Stack || d.shape == Container ? 5 : 0;
  return result;
}
static std::vector<const Definition *> categoryDefinitions(int category) {
  std::vector<const Definition *> defs;
  for (auto &d : definitions())
    if (d.category == category)
      defs.push_back(&d);
  return defs;
}
static std::vector<int> paletteStarts(int category) {
  auto defs = categoryDefinitions(category);
  std::vector<int> starts{0};
  int y = 89;
  for (size_t i = 0; i < defs.size(); i++) {
    int height = paletteLayout(*defs[i]).height();
    if (i && y + height > PAX_H - (category==7?156:106)) {
      starts.push_back(i);
      y = 89;
    }
    y += height + 12;
  }
  return starts;
}
static void paletteBlock(Rect r, const Definition &d, int color,
                         const PaletteLayout &layout) {
  int y = r.y + layout.top;
  std::vector<std::pair<int, int>> mouths;
  int h = layout.headerHeight;
  for (int i = 0; i < d.branches; i++) {
    mouths.push_back({h, h + 24});
    h += 24 + (i + 1 < d.branches ? 24 : 16);
  }
  contour({r.x, y, layout.width, layout.bodyHeight}, color, d.shape, false,
          mouths, std::string(d.opcode) != "control_forever");
  for (auto &part : layout.parts) {
    auto cell = part.rect;
    cell.x += r.x;
    cell.y += y;
    if (part.piece.slot < 0)
      text(part.piece.text, cell.x, cell.y + 6, 0xffffffff, .6f);
    else {
      auto &slot = d.slots[part.piece.slot];
      contour(cell,
              slot.boolean ? 0x3e963e
                           : (slot.field ? fieldColor(color) : 0xffffff),
              slot.boolean ? Boolean : (slot.field ? 8 : Reporter));
      text(inputText(slot), cell.x + cell.w / 2, cell.y + 6,
           slot.boolean || slot.field ? 0xffffffff : 0x575e75ff, .6f, true);
    }
  }
  for (size_t i = 1; i < mouths.size(); i++)
    text("else", r.x + 12, y + mouths[i - 1].second + 6, 0xffffffff, .6f);
}
static std::vector<std::string> branchesOf(const Json &block) {
  auto *d = definition(block.value("opcode", ""));
  int count = d ? d->branches : 0;
  if (block.contains("inputs") && block["inputs"].contains("SUBSTACK"))
    count = std::max(count, 1);
  if (block.contains("inputs") && block["inputs"].contains("SUBSTACK2"))
    count = 2;
  std::vector<std::string> out;
  for (int i = 0; i < count; i++) {
    auto in = block["inputs"].value(i ? "SUBSTACK2" : "SUBSTACK", Json());
    out.push_back(in.is_array() && in.size() > 1 ? blockReference(in[1]) : "");
  }
  return out;
}
Rect Editor::measureStack(const std::string &id, std::set<std::string> &visited,
                          int depth) {
  int width = 0, height = 0;
  std::string current = id;
  auto &blocks = project_.blocks();
  if (depth >= 64)
    return {0, 0, 0, 0};
  while (blocks.contains(current) && blocks[current].is_object() &&
         visited.size() < 10000 && visited.insert(current).second) {
    auto &block = blocks[current];
    std::set<std::string> ancestors;
    auto head = drawHeader(current, 0, 0, ancestors, false);
    int w = head.w, h = head.h;
    auto children = branchesOf(block);
    for (size_t i = 0; i < children.size(); i++) {
      auto child = measureStack(children[i], visited, depth + 1);
      w = std::max(w, child.w + 26);
      h += std::max(32, child.h) + (i + 1 < children.size() ? 24 : 16);
    }
    width = std::max(width, w);
    height += h;
    current = block.contains("next") ? blockReference(block["next"]) : "";
  }
  return {0, 0, width, height};
}
int Editor::drawStack(const std::string &id, int x, int y,
                      std::set<std::string> &visited, bool dragged) {
  auto &bs = project_.blocks();
  static int nesting = 0;
  if (nesting >= 64)
    return 0;
  struct Depth {
    int &level;
    Depth(int &n) : level(n) { ++level; }
    ~Depth() { --level; }
  } depth(nesting);
  int initialY = y;
  std::string current = id;
  // Ordinary stacks are iterative: a long imported script must not consume
  // one ESP task-stack frame per block. Only C-block bodies recurse.
  while (bs.contains(current) && bs[current].is_object() &&
         visited.size() < 10000 && visited.insert(current).second) {
    auto &b = bs[current];
    auto op = b.value("opcode", "");
    const auto *d = definition(op);
    auto type = shape(b);
    int color =
        d ? colors[d->category]
          : (op.rfind("procedures_", 0) == 0 || op.rfind("argument_", 0) == 0
                 ? 0xff6680
                 : 0x71849a);
    std::set<std::string> ancestors;
    auto head = drawHeader(current, x, y, ancestors, false);
    int w = head.w, header = head.h, height = header;
    auto children = branchesOf(b);
    std::vector<std::pair<int, int>> mouths;
    for (size_t branch = 0; branch < children.size(); branch++) {
      std::set<std::string> measured;
      auto child = measureStack(children[branch], measured, nesting);
      w = std::max(w, child.w + 26);
      int nested = std::max(32, child.h);
      mouths.push_back({height, height + nested});
      height += nested + (branch + 1 < children.size() ? 24 : 16);
    }
    if (!children.empty()) {
      contour({x, y, w, height}, color, Container, selected_ == current, mouths,
              op != "control_forever");
      hits_.push_back({{x, y, w, height}, current, {}, false});
    }
    drawHeader(current, x, y, ancestors, true, w, children.empty());
    for (size_t branch = 0; branch < children.size(); branch++) {
      auto mouth = mouths[branch];
      if (!children[branch].empty())
        drawStack(children[branch], x + 18, y + mouth.first, visited, dragged);
      else
        text("drop blocks inside", x + 25, y + mouth.first + 10, 0x8494a8ff,
             .62f);
      docks_.push_back({{x + 18, y + mouth.first - 5, w - 18, 30},
                        current,
                        branch ? "SUBSTACK2" : "SUBSTACK",
                        false,
                        false});
      if (branch + 1 < children.size())
        text("else", x + 12, y + mouth.second + 6, 0xffffffff, .7f);
    }
    if (type != Reporter && type != Boolean && type != Cap &&
        op != "control_forever")
      docks_.push_back({{x, y + height - 4, w, 19}, current, "", false, false});
    y += height;
    std::string next = b.contains("next") ? blockReference(b["next"]) : "";
    if (next.empty())
      break;
    // Scratch puzzle tabs overlap the next block's notch.

    current = std::move(next);
  }
  return y - initialY;
}

void Editor::refreshPreview() {
  preview_.assign(160 * 120, 0xffff);
  auto drawCostume = [&](const Json &t) {
    if (!t.contains("costumes") || t["costumes"].empty())
      return;
    int ci = t.value("currentCostume", 0);
    if (ci < 0 || ci >= (int)t["costumes"].size())
      ci = 0;
    auto c = t["costumes"][ci];
    auto bytes = project_.asset(c.value("md5ext", ""));
    if (bytes.empty())
      return;
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (c.value("dataFormat", "") == "svg") {
      auto svg = lunasvg::Document::loadFromData(
          std::string((char *)bytes.data(), bytes.size()));
      if (!svg)
        return;
      w = std::max(1, std::min(512, (int)svg->width()));
      h = std::max(1, std::min(512, (int)svg->height()));
      auto bitmap = svg->renderToBitmap(w, h);
      bitmap.convertToRGBA();
      if (!bitmap.data())
        return;
      rgba.assign(bitmap.data(), bitmap.data() + w * h * 4);
    } else {
      int channels;
      auto *p = stbi_load_from_memory(bytes.data(), bytes.size(), &w, &h,
                                      &channels, 4);
      if (!p)
        return;
      if (w > 4096 || h > 4096) {
        stbi_image_free(p);
        return;
      }
      rgba.assign(p, p + w * h * 4);
      stbi_image_free(p);
    }
    bool stage = t.value("isStage", false);
    float scale = (stage ? 1.f : t.value("size", 100.f) / 100.f) / 3 /
                  c.value("bitmapResolution", 1.f);
    int dw = std::max(1, (int)(w * scale)), dh = std::max(1, (int)(h * scale));
    if (stage) {
      dw = 160;
      dh = 120;
    }
    int cx = 80 + (int)(t.value("x", 0.f) / 3),
        cy = 60 - (int)(t.value("y", 0.f) / 3);
    if (stage) {
      cx = 80;
      cy = 60;
    }
    for (int yy = std::max(0, cy - dh / 2);
         yy < std::min(120, cy + (dh + 1) / 2); yy++)
      for (int xx = std::max(0, cx - dw / 2);
           xx < std::min(160, cx + (dw + 1) / 2); xx++) {
        int sx = std::clamp((xx - (cx - dw / 2)) * w / dw, 0, w - 1),
            sy = std::clamp((yy - (cy - dh / 2)) * h / dh, 0, h - 1);
        auto *p = &rgba[(sy * w + sx) * 4];
        int a = p[3];
        uint16_t old = preview_[yy * 160 + xx];
        int r = (p[0] * a + ((old >> 11) & 31) * 255 / 31 * (255 - a)) / 255,
            g = (p[1] * a + ((old >> 5) & 63) * 255 / 63 * (255 - a)) / 255,
            b = (p[2] * a + (old & 31) * 255 / 31 * (255 - a)) / 255;
        preview_[yy * 160 + xx] =
            ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
      }
  };
  for (const auto &t : project_.document()["targets"])
    if (t.value("isStage", false) || t.value("visible", true))
      drawCostume(t);
  previewLabel_ = "Stage (initial state)";
}
void Editor::render() {
  hits_.clear();
  docks_.clear();
  palette_.clear();
  keys_.clear();
  pax_set_clip(0, 0, PAX_W, PAX_H);
  Render::beginFrame(0, 245, 248, 252);
  box(R({0, 0, PAX_W, 49}), 0x855cd6);
  text("Scratch PAX", 12, 15, 0xffffffff, .9f);
  toolbarButton(R({140, 9, 62, 31}), PaxIcon::New, "New");
  toolbarButton(R({209, 9, 62, 31}), PaxIcon::Open, "Open");
  toolbarButton(R({278, 9, 62, 31}), PaxIcon::Save, "Save", 0xbbe8d3);
  toolbarButton(R({347, 9, 62, 31}), PaxIcon::Flag, "", 0xe9f1fc);
  toolbarButton(R({417, 9, 62, 31}), PaxIcon::Undo, "Undo");
  toolbarButton(R({486, 9, 62, 31}), PaxIcon::Redo, "Redo");
  toolbarButton(R({555, 9, 62, 31}), PaxIcon::Home, "Home");
  toolbarButton(R({624, 9, 75, 31}), panMode_ ? PaxIcon::Blocks : PaxIcon::Pan, panMode_ ? "Blocks" : "Pan");
  toolbarButton(R({713, 9, 74, 31}), PaxIcon::Back, "Back");
  box(R({0, 49, 72, PAX_H - 96}), 0xe6edf6);
  for (int i = 0; i < 11; i++) {
    Rect r = categoryRect(i);
    if (category_ == i)
      box(r, 0xffffff);
    contour({r.x + 23, r.y + 1, 18, 18}, colors[i], Reporter);
    text(categories[i], r.x + r.w / 2, r.y + 21, 0x575e75ff, .5f, true);
  }
  box({75, 49, paletteWidth(), PAX_H - 96}, 0xf0f3f8);
  text(categories[category_], 86, 60, 0x26374cff, .8f);
  auto defs = categoryDefinitions(category_);
  auto starts = paletteStarts(category_);
  if (category_ != 10)
    palettePage_ = std::clamp(palettePage_, 0, (int)starts.size() - 1);
  int first = category_ == 10 ? 0 : starts[palettePage_],
      last = category_ == 10                         ? 0
             : palettePage_ + 1 < (int)starts.size() ? starts[palettePage_ + 1]
                                                     : defs.size();
  int paletteY = 89;
  for (int i = first; i < last; i++) {
    auto layout = paletteLayout(*defs[i]);
    Rect r{84, paletteY, layout.width, layout.height()};
    pax_set_clip(75, 80, paletteWidth(), PAX_H - (category_==7?231:181));
    paletteBlock(r, *defs[i], colors[category_], layout);
    pax_set_clip(0, 0, PAX_W, PAX_H);
    palette_.push_back({r, defs[i]->opcode});
    paletteY += r.h + 12;
  }
  if (category_ == 10) {
    button({84, 89, paletteWidth() - 18, 32}, "Make a Block");
    auto entries = project_.customEntries();
    int count = std::max(1, (PAX_H - 220) / 48);
    palettePage_ = std::clamp(palettePage_, 0,
                              std::max(0, ((int)entries.size() - 1) / count));
    for (int i = palettePage_ * count;
         i < (int)entries.size() && i < (palettePage_ + 1) * count; i++) {
      Rect r{84, 140 + (i - palettePage_ * count) * 48, paletteWidth() - 18,
             36};
      auto label = entries[i].substr(6);
      if (entries[i].rfind("@arg:", 0) == 0)
        label = entries[i].substr(5, entries[i].size() - 7);
      else
        for (auto &entry : project_.blocks().items())
          if (entry.value().value("opcode", "") == "procedures_prototype" &&
              entry.value().contains("mutation") &&
              entry.value()["mutation"].value("proccode", "") ==
                  entries[i].substr(6)) {
            label = project_.blockLabel(entry.key());
            break;
          }
      contour(r, 0xff6680,
              entries[i].rfind("@arg:", 0) == 0
                  ? (entries[i].substr(entries[i].size() - 2) == ":b"
                         ? Boolean
                         : Reporter)
                  : Stack);
      text(label.substr(0, 23), r.x + 9, r.y + 10, 0xffffffff, .65f);
      palette_.push_back({r, entries[i]});
    }
  }
  button(R({84, 382, 70, 29}), "Prev");
  button(R({164, 382, 76, 29}), "Next");
  box(workspace(), 0xffffff);
  pax_set_clip(workspace().x, workspace().y, workspace().w, workspace().h);
  for (int y = workspace().y + 10; y < workspace().y + workspace().h; y += 20)
    for (int x = workspace().x + 10; x < workspace().x + workspace().w; x += 20)
      box({x, y, 1, 1}, 0xdbe3ed);
  pax_set_ui_zoom(zoom_, workspace().x, workspace().y);
  std::set<std::string> visited;
  for (auto it = project_.blocks().begin(); it != project_.blocks().end(); ++it)
    if (it.value().is_object() && it.value().value("topLevel", false) &&
        !it.value().value("shadow", false) && it.key() != selected_) {
      drawStack(it.key(), workspace().x + viewX_ + it.value().value("x", 30),
                workspace().y + viewY_ + it.value().value("y", 30), visited);
    }
  if (!selected_.empty() && project_.blocks().contains(selected_) &&
      project_.blocks()[selected_].value("topLevel", false))
    drawStack(
        selected_,
        workspace().x + viewX_ + project_.blocks()[selected_].value("x", 30),
        workspace().y + viewY_ + project_.blocks()[selected_].value("y", 30),
        visited, true);
  if (dragging_ && !selected_.empty()) {
    auto *d = definition(project_.blocks()[selected_].value("opcode", ""));
    auto ids = project_.descendants(selected_);
    for (const auto &dock : docks_)
      if (!ids.count(dock.id) && dock.rect.contains(oldX_, oldY_) &&
          ((dock.input &&
            (shape(project_.blocks()[selected_]) == Reporter ||
             shape(project_.blocks()[selected_]) == Boolean) &&
            (!dock.boolean ||
             shape(project_.blocks()[selected_]) == Boolean)) ||
           (!dock.input && shape(project_.blocks()[selected_]) != Reporter &&
            shape(project_.blocks()[selected_]) != Boolean &&
            shape(project_.blocks()[selected_]) != Hat))) {
        box({dock.rect.x, dock.rect.y, dock.rect.w, 3}, 0xff495f);
        break;
      }
  }
  pax_set_ui_zoom(1, 0, 0);
  pax_set_clip(0, 0, PAX_W, PAX_H);
  drawScrollbars();
  box(R({623, 49, 177, PAX_H - 96}), 0xe6edf6);
  text(previewLabel_, PAX_W - panelWidth() + 10, 61, 0x26374cff, .6f);
  if (preview_.size() == 160 * 120)
    for (int y = 0; y < previewHeight(); y++)
      for (int x = 0; x < panelWidth() - 17; x++)
        pax_pixels[(82 + y) * PAX_W + PAX_W - panelWidth() + 9 + x] =
            preview_[(y * 120 / previewHeight()) * 160 +
                     x * 160 / (panelWidth() - 17)];
  text("Sprites", PAX_W - panelWidth() + 12, 215 + previewHeight() - 120,
       0x26374cff, .8f);
  int n = project_.document()["targets"].size(),
      spriteFirst = project_.targetIndex() / 4 * 4;
  button(R({721, 210, 30, 24}), "<");
  button(R({758, 210, 30, 24}), ">");
  for (int i = spriteFirst; i < n && i < spriteFirst + 4; i++) {
    Rect r = R({633, 241 + (i - spriteFirst) * 29, 155, 26});
    button(
        r,
        project_.document()["targets"][i].value("name", "Sprite").substr(0, 17),
        i == project_.targetIndex() ? 0xbacef4 : 0xffffff);
  }
  button({PAX_W-panelWidth()+10,PAX_H-122,panelWidth()-22,28}, "Properties");
  if(category_==7)button({84,PAX_H-146,70,26},"List +");
  button(R({633, 394, 73, 29}), "Sprite +");
  button(R({714, 394, 73, 29}), "Var +");
  box(R({0, 436, PAX_W, 44}), 0xe1e9f3);
  text((dirty_ ? "* " : "") + status_.substr(0, 50), 12, PAX_H - 32, 0x26374cff,
       .65f);
  int zx = PAX_W - panelWidth() - 135;
  button({zx, PAX_H - 37, 34, 28}, "-");
  button({zx + 38, PAX_H - 37, 34, 28}, "+");
  button({zx + 76, PAX_H - 37, 53, 28},
         std::to_string((int)std::lround(zoom_ * 100)) + "%");
  button(R({625, 443, 76, 28}), "Copy");
  button(R({711, 443, 77, 28}), "Trash", 0xf4bcc4);
  if (modal_ != None) {
    drawModal();
    modalDrawing = false;
  }
  Render::endFrame();
}
bool Editor::drop(const std::string &id, int x, int y) {
  Shape type = shape(project_.blocks()[id]);
  auto ids = project_.descendants(id);
  for (const auto &dock : docks_)
    if (!ids.count(dock.id) && dock.rect.contains(x, y)) {
      bool reporter = type == Reporter || type == Boolean;
      if (dock.input && !reporter)
        continue;
      if (!dock.input && (reporter || type == Hat))
        continue;
      if (dock.input && dock.boolean && type != Boolean)
        continue;
      if (project_.attach(id, dock.id, dock.slot)) {
        status_ = dock.input ? "Reporter connected" : "Blocks snapped together";
        return true;
      }
    }
  return false;
}
void Editor::drawScrollbars() {
  const auto area = workspace();
  const int visibleX = std::max(1, (int)(area.w / zoom_));
  const int visibleY = std::max(1, (int)(area.h / zoom_));
  int left = std::min(-viewX_, -24), top = std::min(-viewY_, -24);
  int right = std::max(-viewX_ + visibleX, visibleX), bottom = std::max(-viewY_ + visibleY, visibleY);
  for (auto it = project_.blocks().begin(); it != project_.blocks().end(); ++it) {
    if (!it.value().is_object() || !it.value().value("topLevel", false) || it.value().value("shadow", false)) continue;
    std::set<std::string> visited;
    auto size = measureStack(it.key(), visited, 0);
    int x = it.value().value("x", 30), y = it.value().value("y", 30);
    left = std::min(left, x - 24); top = std::min(top, y - 24);
    right = std::max(right, x + size.w + 24); bottom = std::max(bottom, y + size.h + 24);
  }
  if (!scrollDrag_) {
    scrollMinX_ = left; scrollMaxX_ = std::max(left, right - visibleX);
    scrollMinY_ = top; scrollMaxY_ = std::max(top, bottom - visibleY);
  }
  scrollTrackX_ = {area.x, area.y + area.h + 2, area.w, 12};
  scrollTrackY_ = {area.x + area.w + 2, area.y, 12, area.h};
  auto thumb = [](Rect track, bool vertical, int pos, int low, int high, int visible) {
    int length = vertical ? track.h : track.w;
    int size = high > low ? std::clamp((int)((double)length * visible / (high - low + visible)), 22, length) : length;
    int offset = high > low ? (int)((double)(length - size) * std::clamp(pos - low, 0, high - low) / (high - low)) : 0;
    return vertical ? Rect{track.x, track.y + offset, track.w, size} : Rect{track.x + offset, track.y, size, track.h};
  };
  scrollThumbX_ = thumb(scrollTrackX_, false, -viewX_, scrollMinX_, scrollMaxX_, visibleX);
  scrollThumbY_ = thumb(scrollTrackY_, true, -viewY_, scrollMinY_, scrollMaxY_, visibleY);
  box(scrollTrackX_, 0xe9edf3); box(scrollTrackY_, 0xe9edf3);
  box(scrollThumbX_, scrollDrag_ == 1 ? 0x855cd6 : 0xaab5c5);
  box(scrollThumbY_, scrollDrag_ == 2 ? 0x855cd6 : 0xaab5c5);
}
void Editor::setZoom(float value) {
  value = std::clamp(value, .5f, 2.f);
  float cx = workspace().w / 2.f, cy = workspace().h / 2.f;
  viewX_ += (int)(cx / value - cx / zoom_);
  viewY_ += (int)(cy / value - cy / zoom_);
  zoom_ = value;
}
void Editor::pointer(int x, int y, bool down) {
  // Scrollbars stay in screen coordinates, independent of block zoom.
  if (modal_ == None && (scrollDrag_ || (down && !down_ &&
      (scrollTrackX_.contains(x, y) || scrollTrackY_.contains(x, y))))) {
    if (!scrollDrag_) {
      scrollDrag_ = scrollTrackX_.contains(x, y) ? 1 : 2;
      const auto thumb = scrollDrag_ == 1 ? scrollThumbX_ : scrollThumbY_;
      scrollGrab_ = thumb.contains(x, y) ? (scrollDrag_ == 1 ? x - thumb.x : y - thumb.y)
                                       : (scrollDrag_ == 1 ? thumb.w : thumb.h) / 2;
    }
    auto track = scrollDrag_ == 1 ? scrollTrackX_ : scrollTrackY_;
    auto thumb = scrollDrag_ == 1 ? scrollThumbX_ : scrollThumbY_;
    int travel = (scrollDrag_ == 1 ? track.w - thumb.w : track.h - thumb.h);
    int offset = (scrollDrag_ == 1 ? x - track.x : y - track.y) - scrollGrab_;
    int low = scrollDrag_ == 1 ? scrollMinX_ : scrollMinY_;
    int high = scrollDrag_ == 1 ? scrollMaxX_ : scrollMaxY_;
    int value = low + (travel > 0 ? (int)((double)std::clamp(offset, 0, travel) * (high - low) / travel) : 0);
    if (scrollDrag_ == 1) viewX_ = -value; else viewY_ = -value;
    if (!down) scrollDrag_ = 0;
    down_ = down; oldX_ = x; oldY_ = y; oldWorkspace_ = false;
    return;
  }

  bool inWorkspace = modal_ == None && workspace().contains(x, y);
  bool finishedPaintStroke = false;
  if (modal_ == CostumePaint) {
    int px = x - (PAX_W - 800) / 2 - 90, py = y - (PAX_H - 480) / 2 - 100;
    if (down && !down_)
      paintStroke_ = px >= 0 && py >= 0 && px < 256 && py < 256;
    if (paintStroke_ && (down || down_)) {
      if (!down_) {
        paintUndo_.push_back(paintPixels_);
        paintRedo_.clear();
        if (paintUndo_.size() > 16)
          paintUndo_.erase(paintUndo_.begin());
      }
      int gx = std::clamp(px, 0, 255) / 8, gy = std::clamp(py, 0, 255) / 8;
      if (paintLastX_ < 0) {
        paintLastX_ = gx;
        paintLastY_ = gy;
      }
      int steps = std::max(abs(gx - paintLastX_), abs(gy - paintLastY_));
      for (int i = 0; i <= steps; i++) {
        int xx = steps ? paintLastX_ + (gx - paintLastX_) * i / steps : gx,
            yy = steps ? paintLastY_ + (gy - paintLastY_) * i / steps : gy;
        paintPixels_[yy * 32 + xx] = paintColor_;
      }
      paintLastX_ = gx;
      paintLastY_ = gy;
    }
    finishedPaintStroke = !down && paintStroke_;
  }
  if (!down)
    paintLastX_ = -1;
  if (inWorkspace) {
    x = workspace().x + (int)((x - workspace().x) / zoom_);
    y = workspace().y + (int)((y - workspace().y) / zoom_);
  }

  if (down && !down_) {
    pressX_ = oldX_ = x;
    pressY_ = oldY_ = y;
    moved_ = false;
    dragging_ = false;
    pan_ = false;
    pending_.clear();
    palettePending_.clear();
    pendingInput_ = false;
    if (modal_ == None) {
      if (inWorkspace) {
        if (panMode_)
          pan_ = true;
        else
          for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (it->rect.contains(x, y)) {
              pending_ = it->id;
              pendingInput_ = it->input;
              pendingSlot_ = it->slot;
              offsetX_ = x - it->rect.x;
              offsetY_ = y - it->rect.y;
              if (it->input && it->slot.value != "[block]")
                for (const auto &head : hits_)
                  if (head.id == it->id && !head.input) {
                    offsetX_ = x - head.rect.x;
                    offsetY_ = y - head.rect.y;
                    break;
                  }
              break;
            }
        if (pending_.empty())
          pan_ = true;
      } else
        for (const auto &p : palette_)
          if (p.first.contains(x, y)) {
            palettePending_ = p.second;
            offsetX_ = 10;
            offsetY_ = 10;
            break;
          }
    }
  }
  if (down_ && modal_ == None &&
      (down || abs(x - pressX_) + abs(y - pressY_) > 8)) {
    if (abs(x - pressX_) + abs(y - pressY_) > 8)
      moved_ = true;
    if (moved_ && !dragging_ &&
        (!pending_.empty() || !palettePending_.empty())) {
      project_.checkpoint();
      if (!palettePending_.empty())
        selected_ =
            project_.add(palettePending_, x - workspace().x - viewX_ - offsetX_,
                         y - workspace().y - viewY_ - offsetY_);
      else {
        selected_ = pending_;
        if (pendingInput_) {
          auto in = project_.blocks()[selected_]["inputs"].value(
              pendingSlot_.key, Json());
          if (in.is_array() && in.size() > 1 && in[1].is_string() &&
              project_.blocks().contains(blockReference(in[1])) &&
              !project_.blocks()[blockReference(in[1])].value("shadow", false))
            selected_ = blockReference(in[1]);
        }
        project_.detach(selected_);
      }
      dragging_ = !selected_.empty();
      dirty_ = true;
    }
    if (dragging_) {
      project_.blocks()[selected_]["x"] = x - workspace().x - viewX_ - offsetX_;
      project_.blocks()[selected_]["y"] = y - workspace().y - viewY_ - offsetY_;
    } else if (pan_ && moved_) {
      viewX_ += x - oldX_;
      viewY_ += y - oldY_;
    }
  }
  if (!down && down_) {
    if (modal_ != None) {
      if (!finishedPaintStroke)
        handleModal(x, y);
    } else if (dragging_) {
      if (R({711, 443, 77, 28}).contains(x, y) || x < paletteRight()) {
        project_.erase(selected_);
        selected_.clear();
        status_ = "Stack deleted (Undo restores it)";
      } else if (inWorkspace)
        drop(selected_, x, y);
      else {
        project_.undo();
        selected_.clear();
        status_ = "Drop blocks in the workspace";
      }
      dragging_ = false;
    } else if (!moved_) {
      if (!palettePending_.empty()) {
        project_.checkpoint();
        selected_ = project_.add(palettePending_, 30,
                                 30 + (int)project_.blocks().size() % 6 * 45);
        dirty_ = true;
        status_ = "Added block; drag it to connect";
      } else if (!pending_.empty()) {
        selected_ = pending_;
        if (!pendingInput_ && project_.blocks()[pending_].value("opcode", "") ==
                                  "procedures_definition") {
          customEditing_ = pending_;
          auto proto = project_.blocks()[pending_]["inputs"]["custom_block"][1]
                           .get<std::string>();
          auto m = project_.blocks()[proto]["mutation"];
          auto code = m.value("proccode", "");
          customName_ = code;
          size_t pos = 0;
          while ((pos = customName_.find('%', pos)) != std::string::npos) {
            if (pos + 1 < customName_.size() &&
                (customName_[pos + 1] == 's' || customName_[pos + 1] == 'b'))
              customName_.erase(pos, 2);
            else
              pos++;
          }
          while (!customName_.empty() && customName_.back() == ' ')
            customName_.pop_back();
          customArgs_.clear();
          auto names =
              Json::parse(m.value("argumentnames", "[]"), nullptr, false);
          pos = 0;
          if (names.is_array())
            for (const auto &name : names) {
              pos = code.find('%', pos);
              bool boolean = pos != std::string::npos &&
                             pos + 1 < code.size() && code[pos + 1] == 'b';
              if (pos != std::string::npos)
                pos += 2;
              customArgs_.push_back({name.get<std::string>(), boolean});
            }
          customWarp_ = m.value("warp", "false") == "true";
          modal_ = CustomWizard;
        }
        if (pendingInput_) {
          editId_ = pending_;
          editSlot_ = pendingSlot_;
          text_ = pendingSlot_.value;
          chooseInput();
          if (editSlot_.key == "CHANNEL") {
            files_.clear();
            filePage_ = 0;
            if (pax_svc->hardware_count && pax_svc->hardware_info) {
              bool output = project_.blocks()[editId_].value("opcode", "") ==
                            "paxio_write";
              for (int i = 0; i < pax_svc->hardware_count(); i++) {
                pax_hardware_info_t info{};
                if (pax_svc->hardware_info(i, &info) == 0 &&
                    (!output || (info.flags & PAX_HW_WRITE)))
                  files_.push_back(info.name);
              }
            }
            modal_ = HardwareChannels;
          }
          if (pendingSlot_.value == "[block]") {
            auto child =
                project_.blocks()[pending_]["inputs"][pendingSlot_.key];
            editId_ = blockReference(child[1]);
            modal_ = ReporterInputs;
          }
        }
      } else if (y >= 60 && y < 60 + categoryStep() * 11 && x < 72) {
        category_ = std::clamp((y - 60) / categoryStep(), 0, 10);
        palettePage_ = 0;
      } else if (R({84, 382, 70, 29}).contains(x, y))
        palettePage_ = std::max(0, palettePage_ - 1);
      else if (R({164, 382, 76, 29}).contains(x, y)) {
        if (category_ == 10 ||
            palettePage_ + 1 < (int)paletteStarts(category_).size())
          palettePage_++;
      } else if (R({140, 9, 62, 31}).contains(x, y)) {
        if (dirty_)
          modal_ = ConfirmNew;
        else {
          project_.fresh();
          status_ = "New project; drag blocks into the workspace";
          home();
          refreshPreview();
          selected_.clear();
        }
      } else if (R({209, 9, 62, 31}).contains(x, y)) {
        auto dir = FileSystem::listDirectory(OS::getScratchFolderLocation());
        files_.clear();
        if (dir)
          for (auto &f : *dir)
            if (f.size() > 4 && f.substr(f.size() - 4) == ".sb3" && f[0] != '.')
              files_.push_back(f);
        filePage_ = 0;
        modal_ = OpenFile;
      } else if (R({278, 9, 62, 31}).contains(x, y)) {
        modal_ = ProjectName;
        auto s = project_.source();
        text_ = s.empty() ? "My Project" : s.substr(s.find_last_of('/') + 1);
        if (text_.size() > 4 && text_.substr(text_.size() - 4) == ".sb3")
          text_.resize(text_.size() - 4);
        text_ += "-edited";
      } else if (R({347, 9, 62, 31}).contains(x, y))
        run();
      else if (R({417, 9, 62, 31}).contains(x, y)) {
        if (project_.undo()) {
          dirty_ = true;
          selected_.clear();
          home();
          refreshPreview();
          status_ = "Undone";
        }
      } else if (R({486, 9, 62, 31}).contains(x, y)) {
        if (project_.redo()) {
          dirty_ = true;
          selected_.clear();
          home();
          refreshPreview();
          status_ = "Redone";
        }
      } else if (R({555, 9, 62, 31}).contains(x, y))
        home();
      else if (R({624, 9, 75, 31}).contains(x, y)) {
        panMode_ = !panMode_;
        status_ =
            panMode_ ? "Drag the workspace to pan" : "Drag blocks to edit";
      } else if (R({713, 9, 74, 31}).contains(x, y)) {
        if (dirty_)
          modal_ = ConfirmBack;
        else
          closed_ = true;
      } else if (R({625, 443, 76, 28}).contains(x, y) && !selected_.empty()) {
        project_.checkpoint();
        selected_ = project_.duplicate(selected_);
        dirty_ = true;
        status_ = "Duplicated stack";
      } else if (R({711, 443, 77, 28}).contains(x, y) && !selected_.empty()) {
        project_.checkpoint();
        project_.erase(selected_);
        selected_.clear();
        dirty_ = true;
        status_ = "Deleted stack";
      } else if (Rect{PAX_W - panelWidth() - 135, PAX_H - 37, 34, 28}.contains(
                     x, y))
        setZoom(zoom_ - .25f);
      else if (Rect{PAX_W - panelWidth() - 97, PAX_H - 37, 34, 28}.contains(x,
                                                                            y))
        setZoom(zoom_ + .25f);
      else if (Rect{PAX_W - panelWidth() - 59, PAX_H - 37, 53, 28}.contains(x,
                                                                            y))
        setZoom(1);
      else if (category_ == 10 &&
               Rect{84, 89, paletteWidth() - 18, 32}.contains(x, y)) {
        customEditing_.clear();
        customName_ = "my block";
        customArgs_.clear();
        customWarp_ = false;
        modal_ = CustomWizard;
      } else if (Rect{PAX_W - panelWidth() + 10, PAX_H - 122, panelWidth() - 22,
                      28}
                     .contains(x, y)) {
        editId_.clear();modal_=SpriteProperties;
      } else if (category_ == 7 &&
                 Rect{84, PAX_H - 146, 70, 26}.contains(x, y)) {
        modal_=ListName;text_="my list";
      } else if (R({633, 394, 73, 29}).contains(x, y)) {
        modal_ = SpriteName;
        text_ =
            "Sprite " + std::to_string(project_.document()["targets"].size());
      } else if (R({714, 394, 73, 29}).contains(x, y)) {
        modal_ = VariableName;
        text_ = "score";
      } else if (R({721, 210, 30, 24}).contains(x, y)) {
        project_.selectTarget(std::max(0, project_.targetIndex() - 4));
        selected_.clear();
        home();
      } else if (R({758, 210, 30, 24}).contains(x, y)) {
        project_.selectTarget(
            std::min((int)project_.document()["targets"].size() - 1,
                     project_.targetIndex() + 4));
        selected_.clear();
        home();
      } else if (x >= PAX_W - panelWidth() + 10 && x < PAX_W - 12 &&
                 y >= 241 + previewHeight() - 120 &&
                 y < 357 + previewHeight() - 120) {
        int index = project_.targetIndex() / 4 * 4 +
                    (y - 241 - previewHeight() + 120) / 29;
        project_.selectTarget(index);
        selected_.clear();
        home();
        refreshPreview();
      }
    }
  }
  if (!down)
    paintStroke_ = false;
  down_ = down;
  oldWorkspace_ = inWorkspace;
  oldX_ = x;
  oldY_ = y;
}
void Editor::scroll(int amount, int x) {
  if (modal_ == None) {
    if (x < paletteRight())
      palettePage_ = std::clamp(palettePage_ + (amount < 0 ? 1 : -1), 0,
                                (int)paletteStarts(category_).size() - 1);
    else
      viewY_ += amount * 30;
  }
}
void Editor::openAssets(bool sound) {
  pax_stop_editor_sound();
  assetSound_ = sound;
  assetPage_ = 0;
  assetIndex_ = 0;
  modal_ = Assets;
}
void Editor::openPaint(bool fresh) {
  paintName_ =
      fresh ? "painted costume"
            : project_.target()["costumes"][assetIndex_].value("name", "") +
                  " painted";
  paintWidth_ = project_.target().value("isStage", false) ? 480 : 96;
  paintHeight_ = project_.target().value("isStage", false) ? 360 : 96;
  paintPixels_.assign(1024, 0);
  paintUndo_.clear();
  paintRedo_.clear();
  if (!fresh && !project_.costumePixels(assetIndex_, paintPixels_, paintWidth_,
                                        paintHeight_)) {
    status_ = "Could not rasterize costume";
    return;
  }
  paintCenterX_ = paintWidth_ / 2.;
  paintCenterY_ = paintHeight_ / 2.;
  if (!fresh) {
    const auto &costume = project_.target()["costumes"][assetIndex_];
    double resolution = costume.value("bitmapResolution", 1.);
    if (!std::isfinite(resolution) || resolution <= 0)
      resolution = 1;
    paintCenterX_ =
        costume.value("rotationCenterX", paintWidth_ / 2.) / resolution;
    paintCenterY_ =
        costume.value("rotationCenterY", paintHeight_ / 2.) / resolution;
    paintWidth_ = std::max(1, (int)std::ceil(paintWidth_ / resolution));
    paintHeight_ = std::max(1, (int)std::ceil(paintHeight_ / resolution));
  }
  paintLastX_ = -1;
  paintStroke_ = false;
  modal_ = CostumePaint;
}
void Editor::openSound() {
  pax_stop_editor_sound();
  if (!project_.soundPcm(assetIndex_, soundSamples_, soundRate_)) {
    status_ = "Edit WAV/MP3 audio with at most 1 million frames";
    return;
  }
  trimFrom_ = 0;
  trimTo_ = 100;
  modal_ = SoundEdit;
}
void Editor::drawAssetModal() {
  auto &assets = project_.target()[assetSound_ ? "sounds" : "costumes"];
  if (modal_ == Assets) {
    text(assetSound_                                 ? "Sounds"
         : project_.target().value("isStage", false) ? "Backdrops"
                                                     : "Costumes",
         400, 76, 0x26374cff, .9f, true);
    if (assets.empty())
      text("No assets yet. Add a tone or import a file.", 400, 180, 0x575e75ff,
           .8f, true);
    for (int i = assetPage_ * 7;
         i < (int)assets.size() && i < assetPage_ * 7 + 7; i++)
      button({90, 110 + (i - assetPage_ * 7) * 34, 620, 30},
             assets[i].value("name", "").substr(0, 45));
    button({90, 368, 105, 32}, "New");
    button({210, 368, 105, 32}, "Import");
    button({330, 368, 105, 32}, "Prev");
    button({450, 368, 105, 32}, "Next");
    button({570, 368, 140, 32}, "Done");
    return;
  }
  if (modal_ == ImportAsset) {
    text(assetSound_ ? "Import WAV / MP3" : "Import SVG / PNG", 400, 76,
         0x26374cff, .9f, true);
    text("Put files in /sd/roms/scratch/assets/", 400, 97, 0x575e75ff, .65f,
         true);
    for (int i = filePage_ * 7; i < (int)files_.size() && i < filePage_ * 7 + 7;
         i++)
      button({90, 125 + (i - filePage_ * 7) * 31, 620, 28},
             files_[i].substr(0, 50));
    button({100, 368, 140, 32}, "Prev");
    button({265, 368, 140, 32}, "Next");
    button({555, 368, 140, 32}, "Cancel");
    return;
  }
  if (modal_ == AssetActions) {
    if (assetIndex_ >= (int)assets.size()) {
      modal_ = Assets;
      return;
    }
    text(assets[assetIndex_].value("name", "").substr(0, 45), 400, 85,
         0x26374cff, .9f, true);
    button({100, 150, 280, 44}, assetSound_ ? "Edit sound" : "Paint a copy");
    button({420, 150, 280, 44}, "Rename");
    button({100, 220, 280, 44}, assetSound_ ? "Preview" : "Use this costume");
    button({420, 220, 280, 44}, "Delete", 0xf4bcc4);
    text("Edits can be undone with Undo.", 400, 310, 0x575e75ff, .8f, true);
    button({555, 368, 140, 32}, "Back");
    return;
  }
  if (modal_ == CostumePaint) {
    text("Paint a copy - original costume stays intact", 400, 76, 0x26374cff,
         .85f, true);
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 32; x++) {
        box({90 + x * 8, 100 + y * 8, 8, 8},
            (x / 2 + y / 2) % 2 ? 0xffffff : 0xd9dfe8);
        auto color = paintPixels_[y * 32 + x];
        for (int yy = 0; yy < 8; yy++)
          for (int xx = 0; xx < 8; xx++)
            pax_blend(90 + x * 8 + xx + (PAX_W - 800) / 2,
                      100 + y * 8 + yy + (PAX_H - 480) / 2, color >> 24,
                      (color >> 16) & 255, (color >> 8) & 255, color & 255);
      }
    static const uint32_t colors[] = {0x000000ff, 0xffffffff, 0xff6680ff,
                                      0xffab19ff, 0xffbf00ff, 0x59c059ff,
                                      0x4c97ffff, 0x9966ffff};
    for (int i = 0; i < 8; i++) {
      box({400 + (i % 4) * 70, 120 + (i / 4) * 55, 55, 40}, colors[i] >> 8);
      if (paintColor_ == colors[i])
        text("*", 425 + (i % 4) * 70, 128 + (i / 4) * 55,
             colors[i] == 0x000000ff ? 0xffffffff : 0x26374cff, .9f, true);
    }
    button({400, 235, 135, 32}, "Eraser");
    button({555, 235, 135, 32}, "Fill all");
    button({400, 280, 135, 32}, "Mirror");
    button({555, 280, 135, 32}, "Clear");
    button({400, 315, 135, 20}, "Undo");
    button({555, 315, 135, 20}, "Redo");
    button({400, 340, 140, 38}, "Apply copy", 0xbbe8d3);
    button({555, 340, 140, 38}, "Cancel");
    return;
  }
  if (modal_ == SoundEdit) {
    text(assets[assetIndex_].value("name", "") + " - " +
             std::to_string(soundSamples_.size() / (double)soundRate_)
                 .substr(0, 5) +
             " seconds",
         400, 76, 0x26374cff, .8f, true);
    box({90, 110, 620, 105}, 0xffffff);
    for (int x = 0; x < 620; x++) {
      size_t from = soundSamples_.size() * x / 620,
             to = std::max(from + 1, soundSamples_.size() * (x + 1) / 620);
      int peak = 0;
      for (size_t i = from; i < std::min(to, soundSamples_.size()); i++)
        peak = std::max(peak, std::abs((int)soundSamples_[i]));
      int h = std::max(1, peak * 95 / 32768);
      box({90 + x, 162 - h / 2, 1, h},
          x >= trimFrom_ * 6.2 && x < trimTo_ * 6.2 ? 0x855cd6 : 0xbac4d4);
    }
    button({90, 230, 190, 32},
           "Start: " + std::to_string((int)trimFrom_) + "%");
    button({290, 230, 190, 32}, "End: " + std::to_string((int)trimTo_) + "%");
    button({490, 230, 100, 32}, "Trim");
    button({600, 230, 110, 32}, "Reverse");
    button({90, 275, 140, 32}, "Louder");
    button({250, 275, 140, 32}, "Quieter");
    button({410, 275, 140, 32}, "Fade in");
    button({570, 275, 140, 32}, "Fade out");
    button({90, 335, 190, 38}, "Play / Stop");
    button({300, 335, 190, 38}, "Undo edit");
    button({510, 335, 200, 38}, "Done");
    return;
  }
  if (modal_ == CustomWizard) {
    text(customEditing_.empty() ? "Make a Block" : "Edit custom block", 400, 76,
         0x26374cff, .9f, true);
    button({90, 105, 620, 32}, "Name: " + customName_);
    for (size_t i = 0; i < customArgs_.size(); i++)
      button({90 + (int)(i % 2) * 310, 150 + (int)(i / 2) * 36, 295, 30},
             (customArgs_[i].second ? "Boolean: " : "Text/number: ") +
                 customArgs_[i].first);
    button({90, 303, 190, 30}, "Add text/number");
    button({290, 303, 190, 30}, "Add Boolean");
    button({490, 303, 220, 30},
           customWarp_ ? "Fast mode: on" : "Fast mode: off");
    button({90, 350, 190, 40}, "Cancel");
    button({510, 350, 200, 40}, customEditing_.empty() ? "Create" : "Apply",
           0xbbe8d3);
    return;
  }
}
void Editor::handleAssetModal(int x, int y) {
  auto &assets = project_.target()[assetSound_ ? "sounds" : "costumes"];
  auto applySound = [&](double from, double to, double gain, bool reverse,
                        bool fadeIn = false, bool fadeOut = false) {
    pax_stop_editor_sound();
    project_.checkpoint();
    if (project_.editSound(assetIndex_, from, to, gain, reverse, fadeIn,
                           fadeOut)) {
      dirty_ = true;
      status_ = "Sound updated";
      openSound();
    } else
      status_ = project_.error;
  };
  if (modal_ == Assets) {
    if (Rect{90, 368, 105, 32}.contains(x, y)) {
      if (assetSound_) {
        project_.checkpoint();
        if (project_.addTone("tone")) {
          dirty_ = true;
          assetIndex_ = assets.size() - 1;
          openSound();
        } else
          status_ = project_.error;
      } else
        openPaint(true);
    } else if (Rect{210, 368, 105, 32}.contains(x, y)) {
      files_.clear();
      filePage_ = 0;
      auto made =
          FileSystem::createDirectory(std::string("/sd/roms/scratch/assets/"));
      if (!made) {
        status_ = made.error();
        return;
      }
      auto entries =
          FileSystem::listDirectory(std::string("/sd/roms/scratch/assets/"));
      if (entries)
        for (auto name : *entries) {
          auto dot = name.find_last_of('.');
          auto ext = dot == std::string::npos ? "" : name.substr(dot + 1);
          std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (assetSound_ ? (ext == "wav" || ext == "mp3")
                          : (ext == "png" || ext == "svg"))
            files_.push_back(name);
        }
      modal_ = ImportAsset;
    } else if (Rect{330, 368, 105, 32}.contains(x, y))
      assetPage_ = std::max(0, assetPage_ - 1);
    else if (Rect{450, 368, 105, 32}.contains(x, y)) {
      if ((assetPage_ + 1) * 7 < (int)assets.size())
        assetPage_++;
    } else if (Rect{570, 368, 140, 32}.contains(x, y)) {
      pax_stop_editor_sound();
      modal_ = SpriteProperties;
    } else if (x >= 90 && x < 710 && y >= 110 && y < 348 &&
               (y - 110) % 34 < 30) {
      int index = assetPage_ * 7 + (y - 110) / 34;
      if (index < (int)assets.size()) {
        assetIndex_ = index;
        modal_ = AssetActions;
      }
    }
    return;
  }
  if (modal_ == ImportAsset) {
    if (Rect{100, 368, 140, 32}.contains(x, y))
      filePage_ = std::max(0, filePage_ - 1);
    else if (Rect{265, 368, 140, 32}.contains(x, y)) {
      if ((filePage_ + 1) * 7 < (int)files_.size())
        filePage_++;
    } else if (Rect{555, 368, 140, 32}.contains(x, y))
      modal_ = Assets;
    else if (x >= 90 && x < 710 && y >= 125 && y < 342 && (y - 125) % 31 < 28) {
      int index = filePage_ * 7 + (y - 125) / 31;
      if (index < (int)files_.size()) {
        project_.checkpoint();
        if (project_.importAsset(std::string("/sd/roms/scratch/assets/") +
                                     files_[index],
                                 assetSound_)) {
          dirty_ = true;
          status_ = "Asset imported";
          assetIndex_ = assets.size() - 1;
          modal_ = AssetActions;
          refreshPreview();
        } else
          status_ = project_.error;
      }
    }
    return;
  }
  if (modal_ == AssetActions) {
    if (Rect{100, 150, 280, 44}.contains(x, y)) {
      if (assetSound_)
        openSound();
      else
        openPaint(false);
    } else if (Rect{420, 150, 280, 44}.contains(x, y)) {
      text_ = assets[assetIndex_].value("name", "");
      modal_ = AssetRename;
    } else if (Rect{100, 220, 280, 44}.contains(x, y)) {
      if (assetSound_) {
        std::vector<int16_t> samples;
        int rate;
        if (project_.soundPcm(assetIndex_, samples, rate))
          pax_preview_editor_sound(samples, rate);
        else
          status_ = "Could not decode sound";
      } else {
        project_.checkpoint();
        project_.target()["currentCostume"] = assetIndex_;
        dirty_ = true;
        refreshPreview();
      }
    } else if (Rect{420, 220, 280, 44}.contains(x, y)) {
      project_.checkpoint();
      if (project_.deleteAsset(assetSound_, assetIndex_)) {
        dirty_ = true;
        modal_ = Assets;
        refreshPreview();
      } else
        status_ = project_.error;
    } else if (Rect{555, 368, 140, 32}.contains(x, y)) {
      pax_stop_editor_sound();
      modal_ = Assets;
    }
    return;
  }
  if (modal_ == CostumePaint) {
    static const uint32_t colors[] = {0x000000ff, 0xffffffff, 0xff6680ff,
                                      0xffab19ff, 0xffbf00ff, 0x59c059ff,
                                      0x4c97ffff, 0x9966ffff};
    for (int i = 0; i < 8; i++)
      if (Rect{400 + (i % 4) * 70, 120 + (i / 4) * 55, 55, 40}.contains(x, y)) {
        paintColor_ = colors[i];
        return;
      }
    bool changesCanvas = Rect{555, 235, 135, 32}.contains(x, y) ||
                         Rect{400, 280, 135, 32}.contains(x, y) ||
                         Rect{555, 280, 135, 32}.contains(x, y);
    if (changesCanvas) {
      paintUndo_.push_back(paintPixels_);
      paintRedo_.clear();
      if (paintUndo_.size() > 16)
        paintUndo_.erase(paintUndo_.begin());
    }
    if (Rect{400, 235, 135, 32}.contains(x, y))
      paintColor_ = 0;
    else if (Rect{555, 235, 135, 32}.contains(x, y))
      std::fill(paintPixels_.begin(), paintPixels_.end(), paintColor_);
    else if (Rect{400, 280, 135, 32}.contains(x, y))
      for (int yy = 0; yy < 32; yy++)
        for (int xx = 0; xx < 16; xx++)
          std::swap(paintPixels_[yy * 32 + xx],
                    paintPixels_[yy * 32 + 31 - xx]);
    else if (Rect{555, 280, 135, 32}.contains(x, y))
      std::fill(paintPixels_.begin(), paintPixels_.end(), 0);
    else if (Rect{400, 315, 135, 20}.contains(x, y)) {
      if (!paintUndo_.empty()) {
        paintRedo_.push_back(paintPixels_);
        paintPixels_ = paintUndo_.back();
        paintUndo_.pop_back();
      }
    } else if (Rect{555, 315, 135, 20}.contains(x, y)) {
      if (!paintRedo_.empty()) {
        paintUndo_.push_back(paintPixels_);
        paintPixels_ = paintRedo_.back();
        paintRedo_.pop_back();
      }
    } else if (Rect{400, 340, 140, 38}.contains(x, y)) {
      project_.checkpoint();
      if (project_.paintCostume(paintName_, paintPixels_, paintWidth_,
                                paintHeight_, paintCenterX_, paintCenterY_)) {
        dirty_ = true;
        status_ = "Painted copy added";
        modal_ = Assets;
        refreshPreview();
      } else
        status_ = project_.error;
    } else if (Rect{555, 340, 140, 38}.contains(x, y))
      modal_ = Assets;
    return;
  }
  if (modal_ == SoundEdit) {
    if (Rect{90, 230, 190, 32}.contains(x, y) ||
        Rect{290, 230, 190, 32}.contains(x, y)) {
      trimKey_ = x < 280 ? "Start (%)" : "End (%)";
      text_ = std::to_string((int)(x < 280 ? trimFrom_ : trimTo_));
      modal_ = SoundTrim;
    } else if (Rect{490, 230, 100, 32}.contains(x, y))
      applySound(trimFrom_, trimTo_, 1, false);
    else if (Rect{600, 230, 110, 32}.contains(x, y))
      applySound(0, 100, 1, true);
    else if (Rect{90, 275, 140, 32}.contains(x, y))
      applySound(0, 100, 1.25, false);
    else if (Rect{250, 275, 140, 32}.contains(x, y))
      applySound(0, 100, .8, false);
    else if (Rect{410, 275, 140, 32}.contains(x, y))
      applySound(0, 100, 1, false, true);
    else if (Rect{570, 275, 140, 32}.contains(x, y))
      applySound(0, 100, 1, false, false, true);
    else if (Rect{90, 335, 190, 38}.contains(x, y)) {
      if (Mixer::isSoundPlaying("pax_editor_preview"))
        pax_stop_editor_sound();
      else
        pax_preview_editor_sound(soundSamples_, soundRate_);
    } else if (Rect{300, 335, 190, 38}.contains(x, y)) {
      pax_stop_editor_sound();
      if (project_.undo()) {
        dirty_ = true;
        if (assetIndex_ < (int)project_.target()["sounds"].size())
          openSound();
        else
          modal_ = Assets;
      }
    } else if (Rect{510, 335, 200, 38}.contains(x, y)) {
      pax_stop_editor_sound();
      std::vector<int16_t>().swap(soundSamples_);
      modal_ = Assets;
    }
    return;
  }
  if (modal_ == CustomWizard) {
    if (Rect{90, 105, 620, 32}.contains(x, y)) {
      text_ = customName_;
      modal_ = CustomName;
    } else if (Rect{90, 303, 190, 30}.contains(x, y) ||
               Rect{290, 303, 190, 30}.contains(x, y)) {
      if (customArgs_.size() < 8)
        customArgs_.push_back(
            {"input" + std::to_string(customArgs_.size() + 1), x >= 290});
      else
        status_ = "A custom block can have up to 8 inputs";
    } else if (Rect{490, 303, 220, 30}.contains(x, y))
      customWarp_ = !customWarp_;
    else if (Rect{90, 350, 190, 40}.contains(x, y))
      modal_ = None;
    else if (Rect{510, 350, 200, 40}.contains(x, y)) {
      project_.checkpoint();
      auto id =
          customEditing_.empty()
              ? project_.createCustom(customName_, customArgs_, customWarp_)
              : (project_.updateCustom(customEditing_, customName_, customArgs_,
                                       customWarp_)
                     ? customEditing_
                     : std::string());
      if (!id.empty()) {
        selected_ = id;
        dirty_ = true;
        modal_ = None;
        home();
        status_ = "Custom block created; drag blocks below its definition";
      } else
        status_ = project_.error;
    } else
      for (size_t i = 0; i < customArgs_.size(); i++)
        if (Rect{90 + (int)(i % 2) * 310, 150 + (int)(i / 2) * 36, 295, 30}
                .contains(x, y)) {
          argEdit_ = i;
          text_ = customArgs_[i].first;
          modal_ = CustomArgName;
          break;
        }
    return;
  }
}

void Editor::chooseInput() {
  files_=project_.choices(editSlot_.key);filePage_=0;
  modal_=files_.empty()?(editId_.empty()?SpritePropertyValue:InputValue):Choices;
}
void Editor::drawModal() {
  modalDrawing = true;
  box({65, 55, 670, 367}, 0x26374c);
  box({69, 59, 662, 359}, 0xf2f6fc);
  if (modal_ == ConfirmBack || modal_ == ConfirmNew) {
    text("There are unsaved edits.", 400, 120, 0x26374cff, 1, true);
    text("Save before leaving, or discard this workspace.", 400, 164,
         0x26374cff, .75f, true);
    button({110, 245, 165, 45}, "Keep editing");
    button({313, 245, 165, 45}, "Save a copy", 0xbbe8d3);
    button({515, 245, 165, 45}, "Discard", 0xf4bcc4);
    return;
  }
  if (modal_ == Assets || modal_ == AssetActions || modal_ == ImportAsset ||
      modal_ == CostumePaint || modal_ == SoundEdit || modal_ == CustomWizard) {
    drawAssetModal();
    return;
  }
  if(modal_==SpriteProperties) {
    text(project_.target().value("isStage",false)?"Stage properties":"Sprite properties",400,76,0x26374cff,.9f,true);
    auto fields=propertyKeys(project_);
    for(size_t i=0;i<fields.size();i++)button({90+(int)(i%2)*310,120+(int)(i/2)*52,295,40},fieldLabel(fields[i],project_.target().value("isStage",false))+": "+propertyValue(project_,fields[i]).substr(0,22));
    button({90, 368, 140, 32}, project_.target().value("isStage", false)
                                   ? "Backdrops"
                                   : "Costumes");
    button({255, 368, 140, 32}, "Sounds");
    button({555, 368, 140, 32}, "Done");
    return;
  }
  if(modal_==Choices) {
    text("Choose "+fieldLabel(editSlot_.key,project_.target().value("isStage",false)),400,76,0x26374cff,.9f,true);
    for(int i=filePage_*7;i<(int)files_.size()&&i<filePage_*7+7;i++)button({90,110+(i-filePage_*7)*34,620,30},choiceLabel(files_[i]).substr(0,55));
    button({90,368,115,32},"Prev");button({220,368,115,32},"Next");button({350,368,145,32},"Keyboard");button({515,368,180,32},"Cancel");return;
  }
  if (modal_ == ReporterInputs) {
    text("Reporter inputs", 400, 80, 0x26374cff, .9f, true);
    auto slots = project_.slots(editId_);
    for (size_t i = 0; i < slots.size() && i < 6; i++)
      button({100, 125 + (int)i * 36, 600, 31},
             slots[i].key + ": " + slots[i].value);
    button({545, 368, 140, 32}, "Back");
    return;
  }
  if (modal_ == HardwareChannels) {
    text("Board channels", 400, 76, 0x575e75ff, .9f, true);
    if (files_.empty())
      text("No channels configured. Install the I/O firmware.", 400, 145,
           0x575e75ff, .8f, true);
    for (int i = filePage_ * 7; i < (int)files_.size() && i < filePage_ * 7 + 7;
         i++)
      button({90, 110 + (i - filePage_ * 7) * 34, 620, 30}, files_[i]);
    button({100, 368, 140, 32}, "Prev");
    button({265, 368, 140, 32}, "Next");
    button({555, 368, 140, 32}, "Keyboard");
    return;
  }
  if (modal_ == OpenFile) {
    text(dirty_ ? "Open a project (current edits saved as a recovered draft)"
                : "Open a Scratch project",
         400, 76, 0x26374cff, .85f, true);
    for (int i = filePage_ * 7; i < (int)files_.size() && i < filePage_ * 7 + 7;
         i++)
      button({90, 110 + (i - filePage_ * 7) * 34, 620, 30},
             files_[i].substr(0, 55));
    button({100, 368, 140, 32}, "Prev");
    button({265, 368, 140, 32}, "Next");
    button({555, 368, 140, 32}, "Cancel");
    return;
  }
  std::string title =
      (modal_ == InputValue || modal_ == SpritePropertyValue)
          ? fieldLabel(editSlot_.key, project_.target().value("isStage", false))
      : modal_ == ProjectName   ? "Save project as"
      : modal_ == SpriteName    ? "Sprite name"
      : modal_ == ListName      ? "List name"
      : modal_ == AssetRename   ? "Asset name"
      : modal_ == CustomName    ? "Custom block name"
      : modal_ == CustomArgName ? "Input name"
      : modal_ == SoundTrim     ? trimKey_
                                : "Variable name";
  text(title, 400, 72, 0x26374cff, .9f, true);
  box({88, 106, 620, 36}, 0xffffff);
  text(text_.substr(0, 60) + "_", 98, 116, 0x26374cff, .85f);
  const std::vector<std::string> rows = {shift_ ? "!@#$%^&*()" : "1234567890",
                                         "qwertyuiop", "asdfghjkl",
                                         "zxcvbnm#-."};
  for (int row = 0; row < 4; row++)
    for (size_t i = 0; i < rows[row].size(); i++) {
      Rect r{90 + (int)i * 61, 155 + row * 42, 55, 36};
      std::string label(1, shift_ ? (char)toupper(rows[row][i]) : rows[row][i]);
      button(r, label);
      keys_.push_back({r, label});
    }
  button({90, 335, 93, 35}, shift_ ? "lower" : "SHIFT");
  button({190, 335, 120, 35}, "Space");
  button({317, 335, 95, 35}, "Backspace");
  button({419, 335, 90, 35}, "Clear");
  button({516, 335, 89, 35}, "Cancel");
  button({612, 335, 95, 35}, "OK", 0xbbe8d3);
  if (modal_ == InputValue && editSlot_.boolean)
    text("Drag a green condition into this socket.", 400, 389, 0x26374cff, .7f,
         true);
}
void Editor::handleModal(int x, int y) {
  x -= (PAX_W - 800) / 2;
  y -= (PAX_H - 480) / 2;
  if (modal_ == ConfirmBack || modal_ == ConfirmNew) {
    auto action = modal_;
    if (Rect{110, 245, 165, 45}.contains(x, y))
      modal_ = None;
    else if (Rect{313, 245, 165, 45}.contains(x, y)) {
      modal_ = ProjectName;
      text_ = "My Project-edited";
    } else if (Rect{515, 245, 165, 45}.contains(x, y)) {
      modal_ = None;
      if (action == ConfirmBack)
        closed_ = true;
      else {
        project_.fresh();
        status_ = "New project; drag blocks into the workspace";
        dirty_ = false;
        selected_.clear();
        home();
        refreshPreview();
      }
    }
    return;
  }
  if (modal_ == Assets || modal_ == AssetActions || modal_ == ImportAsset ||
      modal_ == CostumePaint || modal_ == SoundEdit || modal_ == CustomWizard) {
    handleAssetModal(x, y);
    return;
  }
  if(modal_==SpriteProperties) {
    if (Rect{90, 368, 140, 32}.contains(x, y)) {
      openAssets(false);
      return;
    }
    if (Rect{255, 368, 140, 32}.contains(x, y)) {
      openAssets(true);
      return;
    }
    if(Rect{555,368,140,32}.contains(x,y)){modal_=None;return;}
    auto fields=propertyKeys(project_);
    for(size_t i=0;i<fields.size();i++)if(Rect{90+(int)(i%2)*310,120+(int)(i/2)*52,295,40}.contains(x,y)) {
      editId_.clear();editSlot_={fields[i],propertyValue(project_,fields[i]),false,false};text_=editSlot_.value;chooseInput();break;
    }
    return;
  }
  if(modal_==Choices) {
    if(Rect{515,368,180,32}.contains(x,y))modal_=editId_.empty()?SpriteProperties:None;
    else if(Rect{350,368,145,32}.contains(x,y))modal_=editId_.empty()?SpritePropertyValue:InputValue;
    else if(Rect{90,368,115,32}.contains(x,y))filePage_=std::max(0,filePage_-1);
    else if(Rect{220,368,115,32}.contains(x,y)){if((filePage_+1)*7<(int)files_.size())filePage_++;}
    else if(x>=90&&x<710&&y>=110&&y<348){int index=filePage_*7+(y-110)/34;if((y-110)%34<30&&index<(int)files_.size()){text_=files_[index];modal_=editId_.empty()?SpritePropertyValue:InputValue;commitText();}}
    return;
  }
  if (modal_ == ReporterInputs) {
    if (Rect{545, 368, 140, 32}.contains(x, y)) {
      modal_ = None;
      return;
    }
    auto slots = project_.slots(editId_);
    if (x >= 100 && x < 700 && y >= 125) {
      int row = (y - 125) / 36;
      if (row < (int)slots.size() && row < 6) {
        editSlot_ = slots[row];
        text_ = editSlot_.value;
        if (text_ == "[block]") {
          auto child = project_.blocks()[editId_]["inputs"][editSlot_.key];
          editId_ = blockReference(child[1]);
        } else
          chooseInput();
      }
    }
    return;
  }
  if (modal_ == HardwareChannels) {
    if (Rect{555, 368, 140, 32}.contains(x, y))
      modal_ = InputValue;
    else if (Rect{100, 368, 140, 32}.contains(x, y))
      filePage_ = std::max(0, filePage_ - 1);
    else if (Rect{265, 368, 140, 32}.contains(x, y)) {
      if ((filePage_ + 1) * 7 < (int)files_.size())
        filePage_++;
    } else if (x >= 90 && x < 710 && y >= 110 && y < 348) {
      int index = filePage_ * 7 + (y - 110) / 34;
      if (index < (int)files_.size()) {
        text_ = files_[index];
        modal_ = InputValue;
        commitText();
      }
    }
    return;
  }
  if (modal_ == OpenFile) {
    if (Rect{555, 368, 140, 32}.contains(x, y))
      modal_ = None;
    else if (Rect{100, 368, 140, 32}.contains(x, y))
      filePage_ = std::max(0, filePage_ - 1);
    else if (Rect{265, 368, 140, 32}.contains(x, y)) {
      if ((filePage_ + 1) * 7 < (int)files_.size())
        filePage_++;
    } else if (x >= 90 && x < 710 && y >= 110 && y < 348) {
      int index = filePage_ * 7 + (y - 110) / 34;
      if (index < (int)files_.size()) {
        if (dirty_ && !recoverDraft()) {
          status_ = "Could not recover edits; save first";
          return;
        }
        if (load(OS::getScratchFolderLocation() + files_[index]))
          modal_ = None;
      }
    }
    return;
  }
  for (auto &k : keys_)
    if (k.first.contains(x, y)) {
      if (text_.size() < 128)
        text_ += k.second;
      return;
    }
  if (Rect{90, 335, 93, 35}.contains(x, y))
    shift_ = !shift_;
  else if (Rect{190, 335, 120, 35}.contains(x, y)) {
    if (text_.size() < 128)
      text_ += ' ';
  } else if (Rect{317, 335, 95, 35}.contains(x, y)) {
    if (!text_.empty())
      text_.pop_back();
  } else if (Rect{419, 335, 90, 35}.contains(x, y))
    text_.clear();
  else if (Rect{516, 335, 89, 35}.contains(x, y))
    modal_ = modal_ == AssetRename                               ? AssetActions
             : (modal_ == CustomName || modal_ == CustomArgName) ? CustomWizard
             : modal_ == SoundTrim                               ? SoundEdit
             : modal_ == SpritePropertyValue ? SpriteProperties
                                             : None;
  else if (Rect{612, 335, 95, 35}.contains(x, y))
    commitText();
}
void Editor::keyboard(const std::string &key) {
  if (modal_ == None && key == "f5") {
    run();
    return;
  }
  if (modal_ == None || modal_ == OpenFile || modal_ == HardwareChannels ||
      modal_ == ReporterInputs || modal_ == Choices ||
      modal_ == SpriteProperties || modal_ == Assets ||
      modal_ == AssetActions || modal_ == ImportAsset ||
      modal_ == CostumePaint || modal_ == SoundEdit || modal_ == CustomWizard ||
      modal_ == ConfirmBack || modal_ == ConfirmNew)
    return;
  if (key == "enter")
    commitText();
  else if (key == "backspace") {
    if (!text_.empty())
      text_.pop_back();
  } else if (key == "space") {
    if (text_.size() < 128)
      text_ += ' ';
  } else if (key.size() == 1 && text_.size() < 128)
    text_ += key;
}
void Editor::commitText() {
  if (modal_ == AssetRename) {
    project_.checkpoint();
    if (!project_.renameAsset(assetSound_, assetIndex_, text_)) {
      status_ = project_.error;
      return;
    }
    dirty_ = true;
    modal_ = AssetActions;
    refreshPreview();
    return;
  }
  if (modal_ == CustomName) {
    customName_ = text_;
    modal_ = CustomWizard;
    return;
  }
  if (modal_ == CustomArgName) {
    if (argEdit_ >= 0 && argEdit_ < (int)customArgs_.size())
      customArgs_[argEdit_].first = text_;
    modal_ = CustomWizard;
    return;
  }
  if (modal_ == SoundTrim) {
    char *end;
    double v = std::strtod(text_.c_str(), &end);
    if (text_.empty() || *end || !std::isfinite(v) || v < 0 || v > 100) {
      status_ = "Enter a percent from 0 to 100";
      return;
    }
    if (trimKey_ == "Start (%)")
      trimFrom_ = v;
    else
      trimTo_ = v;
    modal_ = SoundEdit;
    return;
  }
  if(modal_==SpritePropertyValue) {
    project_.checkpoint();
    if(!project_.setTargetProperty(editSlot_.key,text_)){status_=project_.error;return;}
    dirty_=true;status_="Sprite property updated";refreshPreview();modal_=SpriteProperties;return;
  }
  if (modal_ == InputValue) {
    if (editSlot_.boolean) {
      modal_ = None;
      return;
    }
    project_.checkpoint();
    project_.literal(editId_, editSlot_, text_);
    dirty_ = true;
    status_ = "Input updated";
  } else if (modal_ == ProjectName) {
    save();
    return;
  } else if (modal_ == SpriteName) {
    project_.checkpoint();
    project_.addSprite(text_);
    dirty_ = true;
    selected_.clear();
    home();
    refreshPreview();
  } else if(modal_==ListName) {
    if(text_.empty()){status_="Enter a list name";return;}
    project_.checkpoint();project_.addList(text_);dirty_=true;category_=7;palettePage_=0;status_="List created";
  } else if (modal_ == VariableName) {
    project_.checkpoint();
    project_.addVariable(text_);
    dirty_ = true;
    status_ = "Variable created";
  }
  modal_ = None;
}
void Editor::save() {
  std::string name = text_;
  if (name.empty() || name.size() > 64 ||
      name.find_first_of("/\\:") != std::string::npos || name == "." ||
      name == "..") {
    status_ = "Choose a short filename without slashes";
    return;
  }
  if (name.size() < 4 || name.substr(name.size() - 4) != ".sb3")
    name += ".sb3";
  if (project_.save(OS::getScratchFolderLocation() + name)) {
    dirty_ = false;
    modal_ = None;
    status_ = "Saved " + name;
  } else
    status_ = project_.error;
}
void Editor::run() {
  if (!player_) {
    status_ = "Player callback unavailable";
    return;
  }
  std::string path = OS::getScratchFolderLocation() + ".pax-preview-" +
                     std::to_string(pax_time_us()) + ".sb3";
  if (!project_.save(path)) {
    status_ = project_.error;
    return;
  }
  pax_editor_playing = true;
  player_(path);
  pax_editor_playing = false;
  if (pax_pixels && PAX_W == 480 && PAX_H == 360) {
    preview_.resize(160 * 120);
    for (int y = 0; y < 120; y++)
      for (int x = 0; x < 160; x++)
        preview_[y * 160 + x] = pax_pixels[(y * 3) * 480 + x * 3];
    previewLabel_ = "Last run";
  }
  if (pax_svc->file_remove) {
    pax_svc->file_remove(path.c_str());
    pax_svc->file_remove((path + ".json").c_str());
  } else {
    std::remove(path.c_str());
    std::remove((path + ".json").c_str());
  }
  if (!pax_resize_surface(width_, height_)) {
    closed_ = true;
    return;
  }
  status_ = "Stopped. Continue editing or save your project";
}
bool Editor::recoverDraft() {
  if (dirty_) {
    std::string file = OS::getScratchFolderLocation() + "Recovered-" +
                       std::to_string(pax_time_us()) + ".sb3";
    if (project_.save(file)) {
      pax_svc->log_printf("Editor: saved unsaved work to %s\n", file.c_str());
      return true;
    } else {
      pax_svc->log_printf("Editor: recovery save failed: %s\n",
                          project_.error.c_str());
      return false;
    }
  }
  return true;
}
void open(const std::string &path) {
  if (!pax_resize_surface(pax_editor_width, pax_editor_height))
    return;
  Editor editor([](const std::string &file) { pax_play_project(file); });
  if (!path.empty() && !editor.load(path)) {
    pax_svc->log_printf("Editor: %s\n", editor.project().error.c_str());
    // Leave the error visible in the editor; New/Open remain usable.
  }
  // The browser opens on press. Consume that same contact before enabling
  // editing.
  while (!OS::toExit) {
    Input::getInput();
    if (!Input::mousePointer.isPressed)
      break;
    pax_svc->delay_ms(12);
  }
  editor.render();
  bool previousDown = false;
  int previousX = Input::mousePointer.x, previousY = Input::mousePointer.y;
  while (!OS::toExit && !editor.closed()) {
    Input::getInput();
    editor.pointer(Input::mousePointer.x, Input::mousePointer.y,
                   Input::mousePointer.isPressed);
    // Playing calls Input::getInput again; keep this iteration independent.
    auto keyboardEvents = Input::inputBuffer;
    for (const auto &key : keyboardEvents)
      editor.keyboard(key);
    if (Input::mouseScrollDelta)
      editor.scroll(Input::mouseScrollDelta, Input::mousePointer.x);
    const bool down = Input::mousePointer.isPressed;
    bool changed = down != previousDown || (down &&
        (Input::mousePointer.x != previousX || Input::mousePointer.y != previousY)) ||
        !keyboardEvents.empty() || Input::mouseScrollDelta;
    if (changed) editor.render();
    else pax_refresh_pointer();
    previousDown = down;
    previousX = Input::mousePointer.x; previousY = Input::mousePointer.y;
    pax_audio_pump();
    pax_svc->delay_ms(4);
  }
  pax_stop_editor_sound();
  if (OS::toExit)
    editor.recoverDraft();
  pax_resize_surface(480, 360);
}
} // namespace PaxEdit
