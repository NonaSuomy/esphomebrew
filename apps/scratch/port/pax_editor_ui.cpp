#include "pax_editor.h"
#include <algorithm>
#include <cmath>
#include <filesystem.hpp>
#include <input.hpp>
#include <lunasvg.h>
#include <os.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <stb_image.h>
namespace PaxEdit {
static const char *categories[] = {"Motion",  "Looks",     "Sound",     "Events", "Control",
                                   "Sensing", "Operators", "Variables", "Pen"};
static const int colors[] = {0x4c97ff, 0x9966ff, 0xcf63cf, 0xffbf00, 0xffab19,
                             0x5cb1d6, 0x59c059, 0xff8c1a, 0x0fbd8c};
static const Rect workspace{254, 58, 364, 375};
static std::string blockReference(const Json &j) {
    return j.is_string() ? j.get<std::string>() : "";
}
static void box(Rect r, int color) {
    Render::drawBox(r.w, r.h, r.x + r.w / 2, r.y + r.h / 2, (color >> 16) & 255, (color >> 8) & 255,
                    color & 255);
}
static void text(const std::string &s, int x, int y, int color = 0x26374cff, float scale = .75f,
                 bool centered = false) {
    pax_draw_text(s, x, y, color, scale, centered);
}
static void button(Rect r, const std::string &s, int color = 0xe1e8f2) {
    box(r, color);
    text(s, r.x + r.w / 2, r.y + 8, 0x26374cff, .75f, true);
}
static Shape shape(const Json &b) {
    const auto *d = definition(b.value("opcode", ""));
    if (d)
        return d->shape;
    if (b.contains("inputs") && b["inputs"].contains("SUBSTACK"))
        return Container;
    return Stack;
}
Editor::Editor(std::function<void(const std::string &)> player) : player_(std::move(player)) {
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
        if (b.is_object() && b.value("topLevel", false) && !b.value("shadow", false)) {
            mx = std::min(mx, b.value("x", 30));
            my = std::min(my, b.value("y", 30));
        }
    if (mx < 100000) {
        viewX_ = 20 - mx;
        viewY_ = 25 - my;
    }
}
int Editor::drawStack(const std::string &id, int x, int y, std::set<std::string> &visited,
                      bool dragged) {
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
    while (bs.contains(current) && bs[current].is_object() && visited.size() < 10000 &&
           visited.insert(current).second) {
        auto &b = bs[current];
        auto op = b.value("opcode", "");
        const auto *d = definition(op);
        auto type = shape(b);
        int color = d ? colors[d->category] : 0x71849a;
        auto slots = project_.slots(current);
        int rows = (slots.size() + 1) / 2;
        int header = slots.empty() ? 32 : 29 + rows * 25;
        int w = 238;
        if (type == Reporter || type == Boolean)
            w = 218;
        Rect head{x, y, w, header};
        box(head, color);
        if (selected_ == current) {
            box({x, y, w, 2}, 0xffffff);
            box({x, y, 2, header}, 0xffffff);
        }
        if (type == Hat)
            box({x + 12, y - 7, 90, 7}, color);
        if (type == Stack || type == Hat || type == Container) {
            box({x + 20, y + header, 32, 4}, color);
            box({x + 20, y, 32, 3}, 0x425168);
        }
        if (type == Reporter) {
            box({x, y, 5, 3}, 0xf5f8fc);
            box({x + w - 5, y, 5, 3}, 0xf5f8fc);
        }
        text(d ? d->label : op.substr(0, 26), x + 10, y + 7, 0xffffffff, .67f);
        hits_.push_back({head, current, {}, false});
        for (size_t i = 0; i < slots.size(); i++) {
            auto s = slots[i];
            Rect cell{x + 8 + (int)(i % 2) * 115, y + 27 + (int)(i / 2) * 25, 107, 22};
            box(cell, s.boolean ? 0x345d38 : 0xfafcfe);
            std::string label = s.value;
            if (label.empty() || label == "<>")
                label = s.boolean ? "condition" : s.key;
            if (label == "[block]") {
                auto input = b["inputs"][s.key];
                auto child = blockReference(input[1]);
                if (bs.contains(child)) {
                    const auto *cd = definition(bs[child].value("opcode", ""));
                    label = cd ? cd->label : bs[child].value("opcode", "");
                }
            }
            text(label.substr(0, 15), cell.x + 4, cell.y + 4, s.boolean ? 0xffffffff : 0x26374cff,
                 .58f);
            hits_.push_back({cell, current, s, true});
            if (!s.field)
                docks_.push_back({{cell.x - 4, cell.y - 4, cell.w + 8, cell.h + 8},
                                  current,
                                  s.key,
                                  true,
                                  s.boolean});
        }
        int height = header;
        int branches =
            d ? d->branches : ((b.contains("inputs") && b["inputs"].contains("SUBSTACK")) ? 1 : 0);
        if (b.contains("inputs") && b["inputs"].contains("SUBSTACK2"))
            branches = 2;
        for (int branch = 0; branch < branches; branch++) {
            std::string key = branch ? "SUBSTACK2" : "SUBSTACK";
            if (branch) {
                box({x, y + height, w, 22}, color);
                text("else", x + 10, y + height + 3, 0xffffffff, .7f);
                height += 22;
            }
            Json in = b["inputs"].value(key, Json());
            std::string child = in.is_array() && in.size() > 1 ? blockReference(in[1]) : "";
            int nested =
                child.empty() ? 34 : drawStack(child, x + 18, y + height + 4, visited, dragged);
            nested = std::max(34, nested + 4);
            box({x, y + height, 14, nested}, color);
            if (child.empty())
                text("drop blocks inside", x + 23, y + height + 10, 0x8494a8ff, .62f);
            docks_.push_back({{x + 16, y + height - 5, w - 16, 30}, current, key, false, false});
            height += nested;
            box({x, y + height, w, 12}, color);
            height += 12;
        }
        if (type != Reporter && type != Boolean && type != Cap && op != "control_forever")
            docks_.push_back({{x, y + height - 4, w, 19}, current, "", false, false});
        y += height;
        std::string next = b.contains("next") ? blockReference(b["next"]) : "";
        if (next.empty())
            break;
        y += 3;
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
            auto svg =
                lunasvg::Document::loadFromData(std::string((char *)bytes.data(), bytes.size()));
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
            auto *p = stbi_load_from_memory(bytes.data(), bytes.size(), &w, &h, &channels, 4);
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
        float scale =
            (stage ? 1.f : t.value("size", 100.f) / 100.f) / 3 / c.value("bitmapResolution", 1.f);
        int dw = std::max(1, (int)(w * scale)), dh = std::max(1, (int)(h * scale));
        if (stage) {
            dw = 160;
            dh = 120;
        }
        int cx = 80 + (int)(t.value("x", 0.f) / 3), cy = 60 - (int)(t.value("y", 0.f) / 3);
        if (stage) {
            cx = 80;
            cy = 60;
        }
        for (int yy = std::max(0, cy - dh / 2); yy < std::min(120, cy + (dh + 1) / 2); yy++)
            for (int xx = std::max(0, cx - dw / 2); xx < std::min(160, cx + (dw + 1) / 2); xx++) {
                int sx = std::clamp((xx - (cx - dw / 2)) * w / dw, 0, w - 1),
                    sy = std::clamp((yy - (cy - dh / 2)) * h / dh, 0, h - 1);
                auto *p = &rgba[(sy * w + sx) * 4];
                int a = p[3];
                uint16_t old = preview_[yy * 160 + xx];
                int r = (p[0] * a + ((old >> 11) & 31) * 255 / 31 * (255 - a)) / 255,
                    g = (p[1] * a + ((old >> 5) & 63) * 255 / 63 * (255 - a)) / 255,
                    b = (p[2] * a + (old & 31) * 255 / 31 * (255 - a)) / 255;
                preview_[yy * 160 + xx] = ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
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
    box({0, 0, 800, 49}, 0x4d65a8);
    text("PAX Blocks", 12, 15, 0xffffffff, .9f);
    button({140, 9, 62, 31}, "New");
    button({209, 9, 62, 31}, "Open");
    button({278, 9, 62, 31}, "Save", 0xbbe8d3);
    button({347, 9, 62, 31}, "Run", 0x72dfa3);
    button({417, 9, 62, 31}, "Undo");
    button({486, 9, 62, 31}, "Redo");
    button({555, 9, 62, 31}, "Home");
    button({624, 9, 75, 31}, panMode_ ? "Blocks" : "Pan");
    button({713, 9, 74, 31}, "Back");
    box({0, 49, 72, 384}, 0xe6edf6);
    for (int i = 0; i < 9; i++) {
        Rect r{4, 60 + i * 39, 65, 35};
        if (category_ == i)
            box(r, 0xffffff);
        box({r.x + 3, r.y + 5, 8, 23}, colors[i]);
        text(categories[i], r.x + 13, r.y + 12, 0x26374cff, .5f);
    }
    box({75, 49, 174, 384}, 0xf0f3f8);
    text(categories[category_], 86, 60, 0x26374cff, .8f);
    std::vector<const Definition *> defs;
    for (const auto &d : definitions())
        if (d.category == category_)
            defs.push_back(&d);
    int first = palettePage_ * 6;
    for (int i = first; i < (int)defs.size() && i < first + 6; i++) {
        Rect r{84, 89 + (i - first) * 46, 157, 38};
        box(r, colors[category_]);
        text(std::string(defs[i]->label).substr(0, 21), r.x + 7, r.y + 10, 0xffffffff, .58f);
        palette_.push_back({r, defs[i]->opcode});
    }
    button({84, 382, 70, 29}, "Prev");
    button({164, 382, 76, 29}, "Next");
    box(workspace, 0xffffff);
    pax_set_clip(workspace.x, workspace.y, workspace.w, workspace.h);
    for (int y = workspace.y + 10; y < workspace.y + workspace.h; y += 20)
        for (int x = workspace.x + 10; x < workspace.x + workspace.w; x += 20)
            box({x, y, 1, 1}, 0xdbe3ed);
    std::set<std::string> visited;
    for (auto it = project_.blocks().begin(); it != project_.blocks().end(); ++it)
        if (it.value().is_object() && it.value().value("topLevel", false) &&
            !it.value().value("shadow", false) && it.key() != selected_) {
            drawStack(it.key(), workspace.x + viewX_ + it.value().value("x", 30),
                      workspace.y + viewY_ + it.value().value("y", 30), visited);
        }
    if (!selected_.empty() && project_.blocks().contains(selected_) &&
        project_.blocks()[selected_].value("topLevel", false))
        drawStack(selected_, workspace.x + viewX_ + project_.blocks()[selected_].value("x", 30),
                  workspace.y + viewY_ + project_.blocks()[selected_].value("y", 30), visited,
                  true);
    if (dragging_ && !selected_.empty()) {
        auto *d = definition(project_.blocks()[selected_].value("opcode", ""));
        auto ids = project_.descendants(selected_);
        for (const auto &dock : docks_)
            if (!ids.count(dock.id) && dock.rect.contains(oldX_, oldY_) && d &&
                ((dock.input && (d->shape == Reporter || d->shape == Boolean) &&
                  (!dock.boolean || d->shape == Boolean)) ||
                 (!dock.input && d->shape != Reporter && d->shape != Boolean && d->shape != Hat))) {
                box({dock.rect.x, dock.rect.y, dock.rect.w, 3}, 0xff495f);
                break;
            }
    }
    pax_set_clip(0, 0, PAX_W, PAX_H);
    box({623, 49, 177, 384}, 0xe6edf6);
    text(previewLabel_, 633, 61, 0x26374cff, .6f);
    if (preview_.size() == 160 * 120)
        for (int y = 0; y < 120; y++)
            std::copy_n(&preview_[y * 160], 160, &pax_pixels[(82 + y) * PAX_W + 632]);
    text("Sprites", 635, 215, 0x26374cff, .8f);
    int n = project_.document()["targets"].size(), spriteFirst = project_.targetIndex() / 5 * 5;
    button({721, 210, 30, 24}, "<");
    button({758, 210, 30, 24}, ">");
    for (int i = spriteFirst; i < n && i < spriteFirst + 5; i++) {
        Rect r{633, 241 + (i - spriteFirst) * 29, 155, 26};
        button(r, project_.document()["targets"][i].value("name", "Sprite").substr(0, 17),
               i == project_.targetIndex() ? 0xbacef4 : 0xffffff);
    }
    button({633, 394, 73, 29}, "Sprite +");
    button({714, 394, 73, 29}, "Var +");
    box({0, 436, 800, 44}, 0xe1e9f3);
    text((dirty_ ? "* " : "") + status_.substr(0, 76), 12, 448, 0x26374cff, .65f);
    button({625, 443, 76, 28}, "Copy");
    button({711, 443, 77, 28}, "Trash", 0xf4bcc4);
    if (modal_ != None)
        drawModal();
    // The player leaves cursor drawing to projects; the editor needs its own
    // pointer so USB mouse users can see blocks and sockets under the cursor.
    if (oldX_ > 0 && oldY_ > 0) {
        for (int yy = 0; yy < 18; yy++)
            for (int xx = 0; xx <= yy / 2; xx++) {
                bool edge = xx == 0 || xx == yy / 2 || yy == 17;
                int c = edge ? 255 : 25;
                pax_blend(oldX_ + xx, oldY_ + yy, c, c, c, 255);
            }
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
void Editor::pointer(int x, int y, bool down) {
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
            if (workspace.contains(x, y)) {
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
    if (down && down_ && modal_ == None) {
        if (abs(x - pressX_) + abs(y - pressY_) > 8)
            moved_ = true;
        if (moved_ && !dragging_ && (!pending_.empty() || !palettePending_.empty())) {
            project_.checkpoint();
            if (!palettePending_.empty())
                selected_ = project_.add(palettePending_, x - workspace.x - viewX_ - offsetX_,
                                         y - workspace.y - viewY_ - offsetY_);
            else {
                selected_ = pending_;
                if (pendingInput_) {
                    auto in =
                        project_.blocks()[selected_]["inputs"].value(pendingSlot_.key, Json());
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
            project_.blocks()[selected_]["x"] = x - workspace.x - viewX_ - offsetX_;
            project_.blocks()[selected_]["y"] = y - workspace.y - viewY_ - offsetY_;
        } else if (pan_ && moved_) {
            viewX_ += x - oldX_;
            viewY_ += y - oldY_;
        }
    }
    if (!down && down_) {
        if (modal_ != None) {
            handleModal(x, y);
        } else if (dragging_) {
            if (Rect{711, 443, 77, 28}.contains(x, y) || x < 249) {
                project_.erase(selected_);
                selected_.clear();
                status_ = "Stack deleted (Undo restores it)";
            } else if (workspace.contains(x, y))
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
                selected_ =
                    project_.add(palettePending_, 30, 30 + (int)project_.blocks().size() % 6 * 45);
                dirty_ = true;
                status_ = "Added block; drag it to connect";
            } else if (!pending_.empty()) {
                selected_ = pending_;
                if (pendingInput_) {
                    editId_ = pending_;
                    editSlot_ = pendingSlot_;
                    text_ = pendingSlot_.value;
                    modal_ = InputValue;
                    if (pendingSlot_.value == "[block]") {
                        auto child = project_.blocks()[pending_]["inputs"][pendingSlot_.key];
                        editId_ = blockReference(child[1]);
                        modal_ = ReporterInputs;
                    }
                }
            } else if (y >= 60 && y < 411 && x < 72) {
                category_ = std::clamp((y - 60) / 39, 0, 8);
                palettePage_ = 0;
            } else if (Rect{84, 382, 70, 29}.contains(x, y))
                palettePage_ = std::max(0, palettePage_ - 1);
            else if (Rect{164, 382, 76, 29}.contains(x, y)) {
                int count = 0;
                for (auto &d : definitions())
                    if (d.category == category_)
                        count++;
                if ((palettePage_ + 1) * 6 < count)
                    palettePage_++;
            } else if (Rect{140, 9, 62, 31}.contains(x, y)) {
                if (dirty_)
                    modal_ = ConfirmNew;
                else {
                    project_.fresh();
                    home();
                    refreshPreview();
                    selected_.clear();
                }
            } else if (Rect{209, 9, 62, 31}.contains(x, y)) {
                auto dir = FileSystem::listDirectory(OS::getScratchFolderLocation());
                files_.clear();
                if (dir)
                    for (auto &f : *dir)
                        if (f.size() > 4 && f.substr(f.size() - 4) == ".sb3" && f[0] != '.')
                            files_.push_back(f);
                filePage_ = 0;
                modal_ = OpenFile;
            } else if (Rect{278, 9, 62, 31}.contains(x, y)) {
                modal_ = ProjectName;
                auto s = project_.source();
                text_ = s.empty() ? "My Project" : s.substr(s.find_last_of('/') + 1);
                if (text_.size() > 4 && text_.substr(text_.size() - 4) == ".sb3")
                    text_.resize(text_.size() - 4);
                text_ += "-edited";
            } else if (Rect{347, 9, 62, 31}.contains(x, y))
                run();
            else if (Rect{417, 9, 62, 31}.contains(x, y)) {
                if (project_.undo()) {
                    dirty_ = true;
                    selected_.clear();
                    home();
                    refreshPreview();
                    status_ = "Undone";
                }
            } else if (Rect{486, 9, 62, 31}.contains(x, y)) {
                if (project_.redo()) {
                    dirty_ = true;
                    selected_.clear();
                    home();
                    refreshPreview();
                    status_ = "Redone";
                }
            } else if (Rect{555, 9, 62, 31}.contains(x, y))
                home();
            else if (Rect{624, 9, 75, 31}.contains(x, y)) {
                panMode_ = !panMode_;
                status_ = panMode_ ? "Drag the workspace to pan" : "Drag blocks to edit";
            } else if (Rect{713, 9, 74, 31}.contains(x, y)) {
                if (dirty_)
                    modal_ = ConfirmBack;
                else
                    closed_ = true;
            } else if (Rect{625, 443, 76, 28}.contains(x, y) && !selected_.empty()) {
                project_.checkpoint();
                selected_ = project_.duplicate(selected_);
                dirty_ = true;
                status_ = "Duplicated stack";
            } else if (Rect{711, 443, 77, 28}.contains(x, y) && !selected_.empty()) {
                project_.checkpoint();
                project_.erase(selected_);
                selected_.clear();
                dirty_ = true;
                status_ = "Deleted stack";
            } else if (Rect{633, 394, 73, 29}.contains(x, y)) {
                modal_ = SpriteName;
                text_ = "Sprite " + std::to_string(project_.document()["targets"].size());
            } else if (Rect{714, 394, 73, 29}.contains(x, y)) {
                modal_ = VariableName;
                text_ = "score";
            } else if (Rect{721, 210, 30, 24}.contains(x, y)) {
                project_.selectTarget(std::max(0, project_.targetIndex() - 5));
                selected_.clear();
                home();
            } else if (Rect{758, 210, 30, 24}.contains(x, y)) {
                project_.selectTarget(std::min((int)project_.document()["targets"].size() - 1,
                                               project_.targetIndex() + 5));
                selected_.clear();
                home();
            } else if (x >= 633 && x < 788 && y >= 241 && y < 386) {
                int index = project_.targetIndex() / 5 * 5 + (y - 241) / 29;
                project_.selectTarget(index);
                selected_.clear();
                home();
                refreshPreview();
            }
        }
    }
    down_ = down;
    oldX_ = x;
    oldY_ = y;
}
void Editor::scroll(int amount, int x) {
    if (modal_ == None) {
        if (x < 249)
            palettePage_ = std::max(0, palettePage_ + (amount < 0 ? 1 : -1));
        else
            viewY_ += amount * 30;
    }
}
void Editor::drawModal() {
    box({65, 55, 670, 367}, 0x26374c);
    box({69, 59, 662, 359}, 0xf2f6fc);
    if (modal_ == ConfirmBack || modal_ == ConfirmNew) {
        text("There are unsaved edits.", 400, 120, 0x26374cff, 1, true);
        text("Save before leaving, or discard this workspace.", 400, 164, 0x26374cff, .75f, true);
        button({110, 245, 165, 45}, "Keep editing");
        button({313, 245, 165, 45}, "Save a copy", 0xbbe8d3);
        button({515, 245, 165, 45}, "Discard", 0xf4bcc4);
        return;
    }
    if (modal_ == ReporterInputs) {
        text("Reporter inputs", 400, 80, 0x26374cff, .9f, true);
        auto slots = project_.slots(editId_);
        for (size_t i = 0; i < slots.size() && i < 6; i++)
            button({100, 125 + (int)i * 36, 600, 31}, slots[i].key + ": " + slots[i].value);
        button({545, 368, 140, 32}, "Back");
        return;
    }
    if (modal_ == OpenFile) {
        text(dirty_ ? "Save first to keep your current edits" : "Open a Scratch project", 400, 76,
             0x26374cff, .85f, true);
        for (int i = filePage_ * 7; i < (int)files_.size() && i < filePage_ * 7 + 7; i++)
            button({90, 110 + (i - filePage_ * 7) * 34, 620, 30}, files_[i].substr(0, 55));
        button({100, 368, 140, 32}, "Prev");
        button({265, 368, 140, 32}, "Next");
        button({555, 368, 140, 32}, "Cancel");
        return;
    }
    std::string title = modal_ == InputValue    ? editSlot_.key
                        : modal_ == ProjectName ? "Save project as"
                        : modal_ == SpriteName  ? "Sprite name"
                                                : "Variable name";
    text(title, 400, 72, 0x26374cff, .9f, true);
    box({88, 106, 620, 36}, 0xffffff);
    text(text_.substr(0, 60) + "_", 98, 116, 0x26374cff, .85f);
    const std::vector<std::string> rows = {shift_ ? "!@#$%^&*()" : "1234567890", "qwertyuiop",
                                           "asdfghjkl", "zxcvbnm#-."};
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
        text("Drag a green condition into this socket.", 400, 389, 0x26374cff, .7f, true);
}
void Editor::handleModal(int x, int y) {
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
                dirty_ = false;
                selected_.clear();
                home();
                refreshPreview();
            }
        }
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
                    modal_ = InputValue;
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
        } else if (!dirty_ && x >= 90 && x < 710 && y >= 110 && y < 348) {
            int index = filePage_ * 7 + (y - 110) / 34;
            if (index < (int)files_.size() && load(OS::getScratchFolderLocation() + files_[index]))
                modal_ = None;
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
        modal_ = None;
    else if (Rect{612, 335, 95, 35}.contains(x, y))
        commitText();
}
void Editor::keyboard(const std::string &key) {
    if (modal_ == None || modal_ == OpenFile || modal_ == ReporterInputs || modal_ == ConfirmBack ||
        modal_ == ConfirmNew)
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
    if (name.empty() || name.size() > 64 || name.find_first_of("/\\:") != std::string::npos ||
        name == "." || name == "..") {
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
    std::string path =
        OS::getScratchFolderLocation() + ".pax-preview-" + std::to_string(pax_time_us()) + ".sb3";
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
    if (!pax_resize_surface(800, 480)) {
        closed_ = true;
        return;
    }
    status_ = "Stopped. Continue editing or save your project";
}
void Editor::recoverDraft() {
    if (dirty_) {
        std::string file =
            OS::getScratchFolderLocation() + "Recovered-" + std::to_string(pax_time_us()) + ".sb3";
        if (project_.save(file))
            pax_svc->log_printf("Editor: saved unsaved work to %s\n", file.c_str());
        else
            pax_svc->log_printf("Editor: recovery save failed: %s\n", project_.error.c_str());
    }
}
void open(const std::string &path) {
    if (!pax_resize_surface(800, 480))
        return;
    Editor editor([](const std::string &file) { pax_play_project(file); });
    if (!path.empty() && !editor.load(path)) {
        pax_svc->log_printf("Editor: %s\n", editor.project().error.c_str());
        pax_resize_surface(480, 360);
        return;
    }
    editor.render();
    while (!OS::toExit && !editor.closed()) {
        Input::getInput();
        editor.pointer(Input::mousePointer.x, Input::mousePointer.y, Input::mousePointer.isPressed);
        for (const auto &key : Input::inputBuffer)
            editor.keyboard(key);
        if (Input::mouseScrollDelta)
            editor.scroll(Input::mouseScrollDelta, Input::mousePointer.x);
        editor.render();
        pax_svc->delay_ms(12);
    }
    if (OS::toExit)
        editor.recoverDraft();
    pax_resize_surface(480, 360);
}
} // namespace PaxEdit
