/* PAX display/input adapter for Frodo's 8-bit C64 framebuffer. */

#include "C64.h"
#include "Display.h"
#include "Prefs.h"
#include "psram_app.h"
#include "pax_c64_compat.h"

#include <stdint.h>
#include <string.h>

typedef uint8_t pixel;

static pixel *pax_frame;
static uint16_t *pax_rgb565;
static uint16_t pax_palette[16];
static uint16_t pax_key_mapping[PAX_INPUT_MAX];
static int pax_close_frames;
/* USB Enter is also exposed by the shared loader as gamepad A/Start for
 * ordinary apps.  C64 BASIC needs it as the physical Return key instead. */
static bool pax_enter_held;
/* USB text events are delivered as taps.  Keep them visible to the C64
 * keyboard scanner for a few video frames so BASIC's debounce/scan cadence
 * cannot miss a character. */
static uint8_t pax_tap_frames[256];
/* Most C64 joystick games use Space as the keyboard fire substitute. */
static uint8_t pax_keyboard_fire_frames;
/* Diagnostic edge state for the shared gamepad ABI.  This is intentionally
 * only logged when it changes, so it does not add per-frame serial traffic. */
static uint32_t pax_last_pad_bits;
/* D64 images normally boot to BASIC READY.  The ROM picker enables this
 * small BASIC script so a selected disk starts without a physical keyboard. */
static bool pax_auto_run_enabled;
static uint8_t pax_auto_run_stage;
static uint16_t pax_auto_run_delay;
static uint16_t pax_auto_run_wait;
static uint16_t pax_auto_run_gap;
static uint8_t pax_auto_run_index;
static const char pax_auto_load_command[] = "LOAD\"*\",8,1\r";
static const char pax_auto_run_command[] = "RUN\r";

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) |
                      ((g & 0xfc) << 3) |
                      (b >> 3));
}

C64Display::C64Display(C64 *the_c64) : TheC64(the_c64)
{
    /* Use the loader's plain app heap here.  It is PSRAM-first on ESPHome
     * and is the same allocator used by Frodo's C++ objects; this avoids
     * requiring a named capability for the display-only scratch buffers. */
    pax_frame = (pixel *)_pax_svc->mem_alloc(DISPLAY_X * DISPLAY_Y);
    pax_rgb565 = (uint16_t *)_pax_svc->mem_alloc(
        DISPLAY_X * DISPLAY_Y * sizeof(uint16_t));
    if (pax_frame) memset(pax_frame, 0, DISPLAY_X * DISPLAY_Y);
    if (pax_rgb565)
        memset(pax_rgb565, 0, DISPLAY_X * DISPLAY_Y * sizeof(uint16_t));
    memset(pax_key_mapping, 0, sizeof(pax_key_mapping));
    pax_close_frames = 0;
    pax_enter_held = false;
    pax_keyboard_fire_frames = 0;
    pax_last_pad_bits = 0;
    pax_auto_run_enabled = false;
    pax_auto_run_stage = 4;
    pax_auto_run_delay = 0;
    pax_auto_run_wait = 0;
    pax_auto_run_gap = 0;
    pax_auto_run_index = 0;
    setStdKeymapping();
}

C64Display::~C64Display(void)
{
    if (pax_frame) _pax_svc->mem_free(pax_frame);
    if (pax_rgb565) _pax_svc->mem_free(pax_rgb565);
    pax_frame = NULL;
    pax_rgb565 = NULL;
}

void C64Display::Speedometer(int speed) { (void)speed; }

uint8 *C64Display::BitmapBase(void)
{
    return pax_frame;
}

int C64Display::BitmapXMod(void)
{
    return DISPLAY_X;
}

void C64Display::InitColors(uint8 *colors)
{
    for (int i = 0; i < 16; ++i) {
        pax_palette[i] = rgb565(palette_red[i], palette_green[i],
                                 palette_blue[i]);
    }
    for (int i = 0; i < 256; ++i) colors[i] = (uint8)(i & 0x0f);
}

void C64Display::resetUpdate(void)
{
    if (pax_frame) memset(pax_frame, 0, DISPLAY_X * DISPLAY_Y);
    if (pax_rgb565)
        memset(pax_rgb565, 0, DISPLAY_X * DISPLAY_Y * sizeof(uint16_t));
}

void C64Display::Update(void)
{
    if (!pax_frame || !pax_rgb565 || !_pax_svc ||
        !_pax_svc->display_write_frame_custom) return;

    for (int i = 0; i < DISPLAY_X * DISPLAY_Y; ++i)
        pax_rgb565[i] = pax_palette[pax_frame[i] & 0x0f];

    /* 384x272 at 1.70 fits inside the 800x480 landscape canvas while
     * preserving the C64's full border and pixel aspect reasonably well. */
    _pax_svc->display_write_frame_custom(
        pax_rgb565, DISPLAY_X, DISPLAY_Y, 1.70f, false);
}

void C64Display::NewPrefs(Prefs *prefs) { (void)prefs; }
void C64Display::setAutoRun(bool enabled)
{
    pax_auto_run_enabled = enabled;
    pax_auto_run_stage = enabled ? 0 : 4;
    pax_auto_run_delay = 240;  // allow the C64/1541 to reach BASIC READY
    pax_auto_run_wait = 0;
    pax_auto_run_gap = 0;
    pax_auto_run_index = 0;
}
bool C64Display::NumLock(void) { return false; }
void C64Display::fastMode(bool on) { ThePrefs.LimitSpeed = !on; }

void C64Display::getBitByteKey(char *bit, char *byte, char key)
{
    *bit = 99;
    *byte = 99;
    switch ((unsigned char)key) {
        case KEY_CUD:   *byte=0; *bit=7; break;
        case KEY_F5:    *byte=0; *bit=6; break;
        case KEY_F3:    *byte=0; *bit=5; break;
        case KEY_F1:    *byte=0; *bit=4; break;
        case KEY_F7:    *byte=0; *bit=3; break;
        case KEY_CLR:   *byte=0; *bit=2; break;
        case '\n':      *byte=0; *bit=1; break;
        case KEY_DEL:   *byte=0; *bit=0; break;
        case KEY_SHL:   *byte=1; *bit=7; break;
        case 'E':       *byte=1; *bit=6; break;
        case 'S':       *byte=1; *bit=5; break;
        case 'Z':       *byte=1; *bit=4; break;
        case '4':       *byte=1; *bit=3; break;
        case 'A':       *byte=1; *bit=2; break;
        case 'W':       *byte=1; *bit=1; break;
        case '3':       *byte=1; *bit=0; break;
        case 'X':       *byte=2; *bit=7; break;
        case 'T':       *byte=2; *bit=6; break;
        case 'F':       *byte=2; *bit=5; break;
        case 'C':       *byte=2; *bit=4; break;
        case '6':       *byte=2; *bit=3; break;
        case 'D':       *byte=2; *bit=2; break;
        case 'R':       *byte=2; *bit=1; break;
        case '5':       *byte=2; *bit=0; break;
        case 'V':       *byte=3; *bit=7; break;
        case 'U':       *byte=3; *bit=6; break;
        case 'H':       *byte=3; *bit=5; break;
        case 'B':       *byte=3; *bit=4; break;
        case '8':       *byte=3; *bit=3; break;
        case 'G':       *byte=3; *bit=2; break;
        case 'Y':       *byte=3; *bit=1; break;
        case '7':       *byte=3; *bit=0; break;
        case 'N':       *byte=4; *bit=7; break;
        case 'O':       *byte=4; *bit=6; break;
        case 'K':       *byte=4; *bit=5; break;
        case 'M':       *byte=4; *bit=4; break;
        case '0':       *byte=4; *bit=3; break;
        case 'J':       *byte=4; *bit=2; break;
        case 'I':       *byte=4; *bit=1; break;
        case '9':       *byte=4; *bit=0; break;
        case ',':       *byte=5; *bit=7; break;
        case '@':       *byte=5; *bit=6; break;
        case ':':       *byte=5; *bit=5; break;
        case '.':       *byte=5; *bit=4; break;
        case '-':       *byte=5; *bit=3; break;
        case 'L':       *byte=5; *bit=2; break;
        case 'P':       *byte=5; *bit=1; break;
        case '+':       *byte=5; *bit=0; break;
        case '/':       *byte=6; *bit=7; break;
        case '^':       *byte=6; *bit=6; break;
        case '=':       *byte=6; *bit=5; break;
        case KEY_SHR:   *byte=6; *bit=4; break;
        case KEY_HOM:   *byte=6; *bit=3; break;
        case ';':       *byte=6; *bit=2; break;
        case '*':       *byte=6; *bit=1; break;
        case KEY_POUND: *byte=6; *bit=0; break;
        case KEY_R_S:   *byte=7; *bit=7; break;
        case 'Q':       *byte=7; *bit=6; break;
        case KEY_COMM:  *byte=7; *bit=5; break;
        case ' ':       *byte=7; *bit=4; break;
        case '2':       *byte=7; *bit=3; break;
        case KEY_CTL:   *byte=7; *bit=2; break;
        case KEY_BAK:   *byte=7; *bit=1; break;
        case '1':       *byte=7; *bit=0; break;
    }
}

void C64Display::pressKey(uint8 *key_matrix, uint8 *rev_matrix, char key)
{
    if ((unsigned char)key == KEY_RESTORE) {
        TheC64->NMI();
        return;
    }
    char bit, byte;
    getBitByteKey(&bit, &byte, key);
    if (bit < 8 && byte < 8) {
        key_matrix[(unsigned char)byte] &= (uint8)~(1u << bit);
        rev_matrix[(unsigned char)bit] &= (uint8)~(1u << byte);
    }
}

void C64Display::unpressKey(uint8 *key_matrix, uint8 *rev_matrix, char key)
{
    char bit, byte;
    getBitByteKey(&bit, &byte, key);
    if (bit < 8 && byte < 8) {
        key_matrix[(unsigned char)byte] |= (uint8)(1u << bit);
        rev_matrix[(unsigned char)bit] |= (uint8)(1u << byte);
    }
}

void C64Display::pressMappedKey(uint8 *key_matrix, uint8 *rev_matrix,
                                uint8 *joystick1, uint8 *joystick2, int key)
{
    uint16_t mapped = pax_key_mapping[key];
    if (mapped < 0x100) {
        pressKey(key_matrix, rev_matrix, (char)mapped);
    } else if (mapped == JOY1_UP) *joystick1 &= (uint8)~0x01;
    else if (mapped == JOY1_DOWN) *joystick1 &= (uint8)~0x02;
    else if (mapped == JOY1_LEFT) *joystick1 &= (uint8)~0x04;
    else if (mapped == JOY1_RIGHT) *joystick1 &= (uint8)~0x08;
    else if (mapped == JOY1_BTN) *joystick1 &= (uint8)~0x10;
    else if (mapped == JOY2_UP) *joystick2 &= (uint8)~0x01;
    else if (mapped == JOY2_DOWN) *joystick2 &= (uint8)~0x02;
    else if (mapped == JOY2_LEFT) *joystick2 &= (uint8)~0x04;
    else if (mapped == JOY2_RIGHT) *joystick2 &= (uint8)~0x08;
    else if (mapped == JOY2_BTN) *joystick2 &= (uint8)~0x10;
}

void C64Display::unpressMappedKey(uint8 *key_matrix, uint8 *rev_matrix,
                                  uint8 *joystick1, uint8 *joystick2, int key)
{
    uint16_t mapped = pax_key_mapping[key];
    if (mapped < 0x100) {
        unpressKey(key_matrix, rev_matrix, (char)mapped);
    } else if (mapped == JOY1_UP) *joystick1 |= 0x01;
    else if (mapped == JOY1_DOWN) *joystick1 |= 0x02;
    else if (mapped == JOY1_LEFT) *joystick1 |= 0x04;
    else if (mapped == JOY1_RIGHT) *joystick1 |= 0x08;
    else if (mapped == JOY1_BTN) *joystick1 |= 0x10;
    else if (mapped == JOY2_UP) *joystick2 |= 0x01;
    else if (mapped == JOY2_DOWN) *joystick2 |= 0x02;
    else if (mapped == JOY2_LEFT) *joystick2 |= 0x04;
    else if (mapped == JOY2_RIGHT) *joystick2 |= 0x08;
    else if (mapped == JOY2_BTN) *joystick2 |= 0x10;
}

void C64Display::PollKeyboard(uint8 *key_matrix, uint8 *rev_matrix,
                              uint8 *joystick1, uint8 *joystick2)
{
    for (int i = 0; i < 8; ++i) key_matrix[i] = rev_matrix[i] = 0xff;
    *joystick1 = *joystick2 = 0xff;

    pax_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (_pax_svc && _pax_svc->input_gamepad_read)
        _pax_svc->input_gamepad_read(&pad);

    uint32_t pad_bits = 0;
    for (int i = 0; i < PAX_INPUT_MAX && i < 32; ++i) {
        if (pad.values[i]) pad_bits |= (uint32_t)1U << i;
    }
    if (pad_bits != pax_last_pad_bits && _pax_svc && _pax_svc->log_printf) {
        _pax_svc->log_printf("C64 gamepad bits=0x%08lx A=%d B=%d U=%d R=%d D=%d L=%d\n",
                              (unsigned long)pad_bits,
                              pad.values[PAX_INPUT_A], pad.values[PAX_INPUT_B],
                              pad.values[PAX_INPUT_UP], pad.values[PAX_INPUT_RIGHT],
                              pad.values[PAX_INPUT_DOWN], pad.values[PAX_INPUT_LEFT]);
        pax_last_pad_bits = pad_bits;
    }

    if (pad.values[PAX_INPUT_MENU] ||
        (_pax_svc->input_l3_read && _pax_svc->input_l3_read())) {
        if (++pax_close_frames >= 2) TheC64->Quit();
    } else {
        pax_close_frames = 0;
    }

    /* Consume the text queue before applying the generic gamepad state so a
     * keyboard Enter seen in this frame cannot also become C64 Run/Stop or
     * joystick fire.  The direct text event below still produces C64 Return. */
    bool pax_enter_activity = false;
    /* Automatic D64 boot commands must not depend on a physical USB keyboard
     * being present.  The gamepad-only path still needs the C64 key matrix
     * tap code below. */
    if (_pax_svc) {
        pax_keyboard_event_t event;
        while (_pax_svc->input_keyboard_read(&event)) {
            if (event.key == 13) {
                pax_enter_held = event.down;
                pax_enter_activity = true;
            }
            if (event.down && event.key == ' ')
                pax_keyboard_fire_frames = 4;
            if (event.down && event.key >= 0 && event.key < 256) {
                pax_tap_frames[event.key] = 4;
                if (_pax_svc->log_printf)
                    _pax_svc->log_printf("C64 keyboard event key=%d\n", event.key);
            }
        }
    }

    for (int i = 0; i < PAX_INPUT_MAX; ++i) {
        if (i == PAX_INPUT_MENU || i == PAX_INPUT_VOLUME) continue;
        if ((i == PAX_INPUT_A || i == PAX_INPUT_START) &&
            (pax_enter_held || pax_enter_activity)) continue;
        if (pad.values[i]) pressMappedKey(key_matrix, rev_matrix,
                                          joystick1, joystick2, i);
    }

    if (_pax_svc && _pax_svc->input_keyboard_read) {
        auto apply_basic = [&](int key, bool down) {
            if (down)
                pressKey(key_matrix, rev_matrix, (char)key);
            else
                unpressKey(key_matrix, rev_matrix, (char)key);
        };

        auto apply_host_key = [&](int host_key, bool down) {
            /* These are the Quake-style values used by the shared USB
             * keyboard queue for named keys.  Translate them to Frodo's
             * physical C64 keys before touching the matrix. */
            switch (host_key) {
                case 8:                 // generic backspace
                case 127:               // loader Backspace
                case 148:               // loader Delete
                    apply_basic(KEY_DEL, down);
                    return;
                case 13:                // loader Enter
                    apply_basic('\n', down);
                    return;
                case 132:               // loader Alt -> Commodore key
                    apply_basic(KEY_COMM, down);
                    return;
                case 133:               // loader Ctrl
                    apply_basic(KEY_CTL, down);
                    return;
                case 134:               // loader Shift
                    apply_basic(KEY_SHL, down);
                    return;
                case 135:               // F1
                    apply_basic(KEY_F1, down);
                    return;
                case 136:               // F2 = Shift+F1
                    apply_basic(KEY_SHL, down); apply_basic(KEY_F1, down);
                    return;
                case 137:               // F3
                    apply_basic(KEY_F3, down);
                    return;
                case 138:               // F4 = Shift+F3
                    apply_basic(KEY_SHL, down); apply_basic(KEY_F3, down);
                    return;
                case 139:               // F5
                    apply_basic(KEY_F5, down);
                    return;
                case 140:               // F6 = Shift+F5
                    apply_basic(KEY_SHL, down); apply_basic(KEY_F5, down);
                    return;
                case 141:               // F7
                    apply_basic(KEY_F7, down);
                    return;
                case 142:               // F8 = Shift+F7
                    apply_basic(KEY_SHL, down); apply_basic(KEY_F7, down);
                    return;
                case 147:               // Insert -> C64 CLR/HOME
                    apply_basic(KEY_CLR, down);
                    return;
                case 151:               // Home
                    apply_basic(KEY_HOM, down);
                    return;
                case PAX_KEY_UP:
                case PAX_KEY_DOWN:
                    apply_basic(KEY_CUD, down);
                    return;
                case PAX_KEY_RIGHT:
                case PAX_KEY_LEFT:
                    apply_basic(KEY_SHL, down);
                    apply_basic(KEY_CUD, down);
                    return;
                default:
                    break;
            }

            int key = host_key;
            if (key >= 'a' && key <= 'z')
                key -= ('a' - 'A');

            /* Common US-layout shifted symbols.  The HID decoder has already
             * converted these to ASCII, so synthesize the C64 Shift key with
             * the corresponding physical key. */
            int base = 0;
            switch (key) {
                case '!': base = '1'; break;
                case '"': base = '2'; break;
                case '#': base = '3'; break;
                case '$': base = '4'; break;
                case '%': base = '5'; break;
                case '&': base = '6'; break;
                case '\'': base = '7'; break;
                case '(': base = '8'; break;
                case ')': base = '9'; break;
                case '?': base = '/'; break;
                case '<': base = ','; break;
                case '>': base = '.'; break;
                case '_': base = '-'; break;
                default: break;
            }
            if (base != 0) {
                apply_basic(KEY_SHL, down);
                apply_basic(base, down);
            } else if (key > 0 && key < 256) {
                apply_basic(key, down);
            }
        };

        /* Type the disk boot commands as individual C64 key taps.  The gap
         * keeps BASIC's keyboard scanner from dropping adjacent characters;
         * the longer stage-2 wait covers the 1541 LOAD before RUN is sent. */
        if (pax_auto_run_enabled) {
            if (pax_auto_run_gap != 0) {
                --pax_auto_run_gap;
            } else if (pax_auto_run_stage == 0) {
                if (pax_auto_run_delay != 0)
                    --pax_auto_run_delay;
                else
                    pax_auto_run_stage = 1;
            } else if (pax_auto_run_stage == 1) {
                const char key = pax_auto_load_command[pax_auto_run_index];
                if (key != '\0') {
                    pax_tap_frames[(unsigned char)key] = 4;
                    ++pax_auto_run_index;
                    pax_auto_run_gap = 8;
                } else {
                    pax_auto_run_stage = 2;
                    pax_auto_run_wait = 400;  // about eight seconds at 50 Hz
                    pax_auto_run_index = 0;
                }
            } else if (pax_auto_run_stage == 2) {
                if (pax_auto_run_wait != 0)
                    --pax_auto_run_wait;
                else
                    pax_auto_run_stage = 3;
            } else if (pax_auto_run_stage == 3) {
                const char key = pax_auto_run_command[pax_auto_run_index];
                if (key != '\0') {
                    pax_tap_frames[(unsigned char)key] = 4;
                    ++pax_auto_run_index;
                    pax_auto_run_gap = 8;
                } else {
                    pax_auto_run_stage = 4;
                    pax_auto_run_enabled = false;
                    if (_pax_svc->log_printf)
                        _pax_svc->log_printf("C64 PAX: auto LOAD/RUN submitted\n");
                }
            }
        }

        for (int key = 0; key < 256; ++key) {
            if (pax_tap_frames[key] != 0) {
                apply_host_key(key, true);
                --pax_tap_frames[key];
            }
        }
        if (pax_keyboard_fire_frames != 0) {
            *joystick2 &= (uint8)~0x10;
            --pax_keyboard_fire_frames;
        }
    }
}

void C64Display::setStdKeymapping(void)
{
    /* Most C64 games, including Bomb Jack, read the second control port. */
    setKeymapping(PAX_INPUT_UP, JOY2_UP);
    setKeymapping(PAX_INPUT_DOWN, JOY2_DOWN);
    setKeymapping(PAX_INPUT_LEFT, JOY2_LEFT);
    setKeymapping(PAX_INPUT_RIGHT, JOY2_RIGHT);
    setKeymapping(PAX_INPUT_A, JOY2_BTN);
    /* The USB Switch driver presents Nintendo A/B as the generic PAX B/A
     * inputs respectively.  Treat both face buttons as C64 joystick fire so
     * either Nintendo face button works in joystick games. */
    setKeymapping(PAX_INPUT_B, JOY2_BTN);
    setKeymapping(PAX_INPUT_START, KEY_R_S);
    setKeymapping(PAX_INPUT_SELECT, KEY_F7);
    setKeymapping(PAX_INPUT_X, ' ');
    setKeymapping(PAX_INPUT_Y, ' ');
}

void C64Display::setKeymapping(char key, int c64Key)
{
    if ((unsigned char)key < PAX_INPUT_MAX)
        pax_key_mapping[(unsigned char)key] = (uint16_t)c64Key;
}

void C64Display::sendKeys(const char *keys) { (void)keys; }
void C64Display::doFlipScreen(void) {}
int C64Display::mousePress(void) { return -1; }
void C64Display::moveCursor(int x, int y) { (void)x; (void)y; }
void C64Display::showVirtualKeyboard(void) {}
void C64Display::hideVirtualKeyboard(void) {}
bool C64Display::isNAVrunning(void) { return false; }
void C64Display::reloadDiskNAV(int dsk) { (void)dsk; }

long ShowRequester(char *str, char *button1, char *button2)
{
    (void)button1;
    (void)button2;
    if (_pax_svc && _pax_svc->log_printf)
        _pax_svc->log_printf("C64: %s\n", str ? str : "request");
    return 1;
}
