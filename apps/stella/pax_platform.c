/* Native Odroid/ESP timer surface used by the Stella PAX wrapper. */
#include "psram_app.h"
#include "odroid_audio.h"
#include "odroid_display.h"
#include "odroid_input.h"
#include "odroid_settings.h"

#include <stdint.h>
#include <sys/stat.h>

extern const app_services_t *_pax_svc;
extern void app_return_to_launcher(void);
static void stella_check_close(void) {
    /* A loader close first taps the native Menu control. Check in menu
     * delays/audio as well as the frame loop so that menu cannot trap quit. */
    if (_pax_svc && _pax_svc->input_l3_read && _pax_svc->input_l3_read())
        app_return_to_launcher();
}

volatile int odroid_paddle_adc_raw = -1;
bool odroid_input_xy_menu_disable = false;
bool odroid_input_touch_buttons_disable = false;

void pax_delay_ms(int ms)
{
    if (ms > 0 && _pax_svc && _pax_svc->delay_ms) _pax_svc->delay_ms(ms);
    stella_check_close();
}

int64_t pax_esp_timer_get_time(void)
{
    return _pax_svc && _pax_svc->get_time_us ? _pax_svc->get_time_us() : 0;
}

void odroid_paddle_adc_init(void) {}

void odroid_audio_init(int sample_rate)
{
    if (_pax_svc && _pax_svc->audio_init) _pax_svc->audio_init(sample_rate);
}
void odroid_audio_terminate(void) {}
void odroid_audio_volume_set(int volume)
{
    if (_pax_svc && _pax_svc->settings_volume_set)
        _pax_svc->settings_volume_set(volume);
}
odroid_volume_level odroid_audio_volume_get(void)
{
    return _pax_svc && _pax_svc->settings_volume_get
        ? (int)_pax_svc->settings_volume_get() : 4;
}
void odroid_audio_volume_change(void)
{
    odroid_audio_volume_set((odroid_audio_volume_get() + 1) % 5);
}
void odroid_audio_submit(short *buffer, int frames)
{
    stella_check_close();
    if (_pax_svc && _pax_svc->audio_submit)
        _pax_svc->audio_submit(buffer, frames);
}
void odroid_audio_submit_zero(void) {}
int odroid_audio_sample_rate_get(void) { return 31400; }

void ili9341_init(void) {}
void ili9341_write_frame_rgb565(const uint16_t *buffer)
{
    /* The legacy buffer is 320x240, not the current full canvas.
     * The custom writer scales it and presents the completed frame. */
    if (_pax_svc && _pax_svc->display_write_frame_custom)
        _pax_svc->display_write_frame_custom(buffer, 320, 240, 2.0f, false);
}
void ili9341_write_frame_rgb565_ex(const uint16_t *buffer, bool byte_swap_input)
{
    if (_pax_svc && _pax_svc->display_write_frame_custom)
        _pax_svc->display_write_frame_custom(buffer, 320, 240, 2.0f,
                                               byte_swap_input);
}
void ili9341_write_frame_rgb565_custom(const uint16_t *buffer, uint16_t w,
                                       uint16_t h, float scale,
                                       bool byte_swap_input)
{
    if (_pax_svc && _pax_svc->display_write_frame_custom)
        _pax_svc->display_write_frame_custom(buffer, w, h, scale,
                                               byte_swap_input);
}

char *odroid_util_GetFileName(const char *path) { return (char *)path; }
char *odroid_util_GetFileExtenstion(const char *path)
{
    const char *dot = path;
    if (path) while (*path) { if (*path++ == '.') dot = path; }
    return (char *)dot;
}
char *odroid_util_GetFileNameWithoutExtension(const char *path) { return (char *)path; }

int32_t odroid_settings_Volume_get(void)
{
    return _pax_svc && _pax_svc->settings_volume_get
        ? _pax_svc->settings_volume_get() : 4;
}
void odroid_settings_Volume_set(int32_t value)
{
    if (_pax_svc && _pax_svc->settings_volume_set)
        _pax_svc->settings_volume_set(value);
}
ODROID_START_ACTION odroid_settings_StartAction_get(void)
{
    return ODROID_START_ACTION_NORMAL;
}
void odroid_settings_StartAction_set(ODROID_START_ACTION value) { (void)value; }

/* newlib's stat wrapper calls this target hook. Stella only needs existence
 * checks for optional save files; the launcher VFS is the source of truth. */
int _stat(const char *path, struct stat *st)
{
    if (!_pax_svc || !_pax_svc->file_open) return -1;
    void *f = _pax_svc->file_open(path, "rb");
    if (!f) return -1;
    if (st) {
        st->st_mode = S_IFREG;
        st->st_size = 0;
    }
    _pax_svc->file_close(f);
    return 0;
}
