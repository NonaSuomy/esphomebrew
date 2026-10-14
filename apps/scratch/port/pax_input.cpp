#include "pax_port.h"
#include <input.hpp>
#include <render.hpp>
#include <set>
#include <climits>
bool pax_editor_playing = false;
bool pax_native_cursor = false;
bool pax_mouse_present = false;
static std::set<std::string> keys;
static int tx = PAX_W / 2, ty = PAX_H / 2;
static int mouseButtons;
static float mouseX = PAX_W / 2, mouseY = PAX_H / 2;
static std::string keyName(int key) {
    switch (key) {
    case PAX_KEY_UP:
        return "up arrow";
    case PAX_KEY_DOWN:
        return "down arrow";
    case PAX_KEY_LEFT:
        return "left arrow";
    case PAX_KEY_RIGHT:
        return "right arrow";
    case 139:
        return "f5";
    case 140:
        return "f6";
    case 141:
        return "f7";
    case 32:
        return "space";
    case 13:
        return "enter";
    case 27:
        return "escape";
    case 8:
    case 127:
        return "backspace";
    }
    return key >= 33 && key <= 126 ? std::string(1, (char)tolower(key)) : "";
}
std::array<int, 2> Input::getTouchPosition() { return {tx, ty}; }
void Input::getInput() {
    inputButtons.clear();
    inputKeys.clear();
    inputBuffer.clear();
    std::set<std::string> pressedKeys, freshKeys;
    pax_gamepad_state_t pad{};
    pax_svc->input_gamepad_read(&pad);
    const int buttons[] = {PAX_INPUT_UP, PAX_INPUT_DOWN, PAX_INPUT_LEFT,  PAX_INPUT_RIGHT,
                           PAX_INPUT_A,  PAX_INPUT_B,    PAX_INPUT_X,     PAX_INPUT_Y,
                           PAX_INPUT_L,  PAX_INPUT_R,    PAX_INPUT_START, PAX_INPUT_SELECT};
    const char *names[] = {"dpadUp", "dpadDown", "dpadLeft",  "dpadRight", "A",     "B",
                           "X",      "Y",        "shoulderL", "shoulderR", "start", "back"};
    const char *defaults[] = {"up arrow", "down arrow", "left arrow", "right arrow", "space", "b",
                              "x",        "y",          "l",          "r",           "enter", "0"};
    for (int i = 0; i < 12; i++)
        if (pad.values[buttons[i]]) {
            inputButtons.push_back(names[i]);
            auto c = inputControls.find(names[i]);
            inputKeys.push_back(c == inputControls.end() ? defaults[i] : c->second);
        }
    if (pax_svc->input_keyboard_read) {
        pax_keyboard_event_t e;
        while (pax_svc->input_keyboard_read(&e)) {
            auto k = keyName(e.key);
            if (k.empty())
                continue;
            if (e.down) {
                if ((k == "f5" || k == "f6" || k == "f7") && keys.count(k))
                    continue;
                if (keys.insert(k).second)
                    freshKeys.insert(k);
                pressedKeys.insert(k);
                inputBuffer.push_back(e.key >= 33 && e.key <= 126 ? std::string(1, (char)e.key)
                                                                  : k);
            } else
                keys.erase(k);
        }
    }
    inputKeys.insert(inputKeys.end(), keys.begin(), keys.end());
    // Keep a complete tap visible for one poll even when both edges are queued.
    for (const auto &k : pressedKeys)
        if (std::find(inputKeys.begin(), inputKeys.end(), k) == inputKeys.end())
            inputKeys.push_back(k);
    static int mouseWidth = 480, mouseHeight = 360;
    if (mouseWidth != PAX_W || mouseHeight != PAX_H) {
        mouseX *= (float)PAX_W / mouseWidth;
        mouseY *= (float)PAX_H / mouseHeight;
        tx = (int)mouseX;
        ty = (int)mouseY;
        mouseWidth = PAX_W;
        mouseHeight = PAX_H;
    }
    bool down = false;
    int x = INT_MIN, y = INT_MIN, cw = 800, ch = 480;
    if (pax_svc->display_get_size)
        pax_svc->display_get_size(&cw, &ch);
    bool contact = pax_svc->touch_read && pax_svc->touch_read(&x, &y);
    // The desktop host supplies absolute coordinates even when not pressed.
    // Hardware touch_read leaves these outputs untouched without a contact.
    bool absolute = x != INT_MIN && y != INT_MIN;
    if (!contact)
        pax_native_cursor = absolute;
    bool inside = false;
    if (absolute) {
        float fit = std::min((float)cw / PAX_W, (float)ch / PAX_H);
        int ow = std::max(1, (int)(PAX_W * fit + .5f)), oh = std::max(1, (int)(PAX_H * fit + .5f));
        int ox = (cw - ow) / 2, oy = (ch - oh) / 2;
        inside = x >= ox && y >= oy && x < ox + ow && y < oy + oh;
        tx = std::clamp((x - ox) * PAX_W / ow, 0, PAX_W - 1);
        ty = std::clamp((y - oy) * PAX_H / oh, 0, PAX_H - 1);
        mouseX = tx;
        mouseY = ty;
        down = contact && inside;
    }
    if (pax_svc->input_mouse_read) {
        int dx, dy, b;
        if (pax_svc->input_mouse_read(&dx, &dy, &b)) {
            pax_mouse_present = true;
            // A desktop hover/contact already supplied this sample's position.
            if (!absolute) {
                float fit = std::max(.01f, std::min((float)cw / PAX_W, (float)ch / PAX_H));
                mouseX = std::clamp(mouseX + dx / fit, 0.f, (float)PAX_W - 1);
                mouseY = std::clamp(mouseY + dy / fit, 0.f, (float)PAX_H - 1);
                tx = (int)mouseX;
                ty = (int)mouseY;
            }
            mouseButtons = b;
        }
        down = down || ((mouseButtons & 1) && (!absolute || inside));
    }
    const bool playback = pax_project_playing || pax_editor_playing;
    const float stageScale = std::max(.01f, Render::renderScale);
    const int pointerX = playback ? (int)std::lround((tx - PAX_W / 2.f) / stageScale) : tx;
    const int pointerY = playback ? (int)std::lround((PAX_H / 2.f - ty) / stageScale) : ty;
    mousePointer.isMoving = pointerX != mousePointer.x || pointerY != mousePointer.y;
    mousePointer.x = pointerX;
    mousePointer.y = pointerY;
    mousePointer.isPressed = down;
    mousePointer.mouseButton = Mouse::LEFT;
    const bool newPress = down && mousePointer.heldFrames == 0;
    // The runtime click dispatcher owns the playback hold counter. The editor
    // and picker still need their own first-press detection without dispatch.
    if (!playback)
        mousePointer.heldFrames = down ? mousePointer.heldFrames + 1 : 0;
    mouseScrollDelta = 0;
    if (pax_svc->input_mouse_scroll_read)
        pax_svc->input_mouse_scroll_read(&mouseScrollDelta);
    if ((pax_project_playing || pax_editor_playing) &&
        ((newPress && tx >= PAX_W - 164 && tx < PAX_W - 82 && ty < 34) ||
         std::find(inputBuffer.begin(), inputBuffer.end(), "f7") != inputBuffer.end())) {
        Scratch::turbo = !Scratch::turbo;
        Scratch::forceRedraw = true;
        pax_svc->log_printf("Scratch: Turbo %s\n", Scratch::turbo ? "ON" : "OFF");
    }
    if ((pax_project_playing || pax_editor_playing) && ((down && tx >= PAX_W - 78 && ty < 34) || keys.count("escape") || keys.count("f6") ||
        std::find(inputBuffer.begin(), inputBuffer.end(), "f6") != inputBuffer.end()))
        Scratch::shouldStop = true;
    if (playback && !Scratch::sprites.empty()) {
        // Match upstream windowing backends: input must dispatch Scratch hats.
        // Keep typed text intact; executeKeyHats also appends to inputBuffer.
        for (const auto &k : freshKeys)
            keyHeldDuration[k] = 0;
        auto typed = inputBuffer;
        BlockExecutor::executeKeyHats();
        inputBuffer = std::move(typed);
        const bool pointerDown = mousePointer.isPressed;
        const int held = mousePointer.heldFrames;
        const bool onStage = std::abs(pointerX) <= Scratch::projectWidth / 2.f &&
                             std::abs(pointerY) <= Scratch::projectHeight / 2.f;
        const bool playerControl = tx >= PAX_W - 164 && ty < 34;
        const bool blocked = !onStage || playerControl ||
                             Render::isPointOverMonitor(tx, ty);
        if (blocked)
            mousePointer.isPressed = false;
        BlockExecutor::doSpriteClicking();
        mousePointer.isPressed = pointerDown;
        if (blocked)
            mousePointer.heldFrames = down ? held + 1 : 0;
    }
    if (!Scratch::sprites.empty() && pad.values[PAX_INPUT_SELECT] && pad.values[PAX_INPUT_B])
        Scratch::shouldStop = true;
    static long long quitSince = 0;
    if (pad.values[PAX_INPUT_MENU] ||
        (pad.values[PAX_INPUT_START] && pad.values[PAX_INPUT_SELECT])) {
        if (!quitSince)
            quitSince = pax_time_us();
        if (pax_time_us() - quitSince > 2000000)
            OS::toExit = true;
    } else
        quitSince = 0;
}
std::string Input::openSoftwareKeyboard(const char *hint) {
    std::string answer;
    bool enterWasDown = true;
    while (!OS::toExit) {
        getInput();
        bool enter = std::find(inputKeys.begin(), inputKeys.end(), "enter") != inputKeys.end();
        if (enter && !enterWasDown)
            break;
        enterWasDown = enter;
        for (const auto &key : inputBuffer) {
            if (key == "backspace") {
                if (!answer.empty())
                    answer.pop_back();
            } else if (key == "space")
                answer += ' ';
            else if (key.size() == 1)
                answer += key;
        }

        Render::beginFrame(0, 20, 25, 40);
        pax_draw_text(hint, PAX_W / 2, 100, 0xffffffff, 1, true);
        pax_draw_text(answer, PAX_W / 2, 150, 0xffffffff, 1, true);
        Render::endFrame();
        pax_svc->delay_ms(10);
    }
    return answer;
}
void Input::setClipboardText(const std::string &) {}
