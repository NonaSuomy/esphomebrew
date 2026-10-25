#define PAX_APP_SIDE 1
#include "pax_platform.h"

#include <stddef.h>
#include <string.h>
#include "pax_render_profile.h"
#include "pax_pixels.h"

static uint16_t *s_frame565;
static int s_frame_capacity;
static uint16_t *s_canvas565;
static bool s_debug_muted;
static unsigned s_debug_count;
static uint64_t s_phase_us[PAX_PHASES], s_profile_start;
static unsigned s_profile_frames;
static unsigned long s_profile_cycle;

void pax_stratagus_profile_add(unsigned phase, unsigned microseconds)
{
	if (phase<PAX_PHASES) s_phase_us[phase]+=microseconds;
}
void pax_stratagus_profile_frame(unsigned long cycle)
{
	const uint64_t now=_pax_svc->get_time_us();
	if (!s_profile_start) { s_profile_start=now; s_profile_cycle=cycle; }
	++s_profile_frames;
	if (now-s_profile_start<5000000) return;
	const unsigned fps10=s_profile_frames*10000000ULL/(now-s_profile_start);
	const unsigned cps10=(cycle-s_profile_cycle)*10000000ULL/(now-s_profile_start);
	const uint64_t parts=s_phase_us[PAX_CLEAR]+s_phase_us[PAX_TERRAIN]+s_phase_us[PAX_SPRITES]+s_phase_us[PAX_FOG];
	const unsigned hud=(s_phase_us[PAX_DRAW]>parts ? s_phase_us[PAX_DRAW]-parts : 0)/s_profile_frames/1000;
	const unsigned logic=(s_phase_us[PAX_LOGIC]>s_phase_us[PAX_INPUT_WAIT] ?
		s_phase_us[PAX_LOGIC]-s_phase_us[PAX_INPUT_WAIT] : 0)/s_profile_frames/1000;
	if (_pax_svc->log_printf) _pax_svc->log_printf(
		"Stargus: timing fps=%u.%u cps=%u.%u clear=%u terrain=%u sprites=%u fog=%u hud=%u present=%u logic=%u units=%u triggers=%u input_wait=%u ms\n",
		fps10/10,fps10%10,cps10/10,cps10%10,(unsigned)(s_phase_us[PAX_CLEAR]/s_profile_frames/1000),
		(unsigned)(s_phase_us[PAX_TERRAIN]/s_profile_frames/1000),
		(unsigned)(s_phase_us[PAX_SPRITES]/s_profile_frames/1000),
		(unsigned)(s_phase_us[PAX_FOG]/s_profile_frames/1000),hud,
		(unsigned)(s_phase_us[PAX_PRESENT]/s_profile_frames/1000),logic,
		(unsigned)(s_phase_us[PAX_UNIT_LOGIC]/s_profile_frames/1000),
		(unsigned)(s_phase_us[PAX_TRIGGER_LOGIC]/s_profile_frames/1000),
		(unsigned)(s_phase_us[PAX_INPUT_WAIT]/s_profile_frames/1000));
	memset(s_phase_us,0,sizeof(s_phase_us)); s_profile_frames=0; s_profile_start=now; s_profile_cycle=cycle;
}

void pax_stratagus_debug(const char *message)
{
	// Quiet builds must still expose actionable Lua failures, otherwise a
	// broken HUD callback looks like an ignored touch/menu button.
	if (message && strstr(message,"Lua error") && _pax_svc && _pax_svc->log_printf) {
		_pax_svc->log_printf("Stargus: %s\n",message); return;
	}
#ifdef PAX_STRATAGUS_WARCRAFT
	// These lifecycle/error markers must survive all logging profiles.
	if (message && _pax_svc && _pax_svc->log_printf &&
		(!strncmp(message,"Warcraft:",9) || !strncmp(message,"native exception:",17) ||
		 !strncmp(message,"native Guichan exception:",25))) {
		_pax_svc->log_printf("Stargus: %s\n",message); return;
	}
#endif
#if defined(STRATAGUS_PAX_QUIET)
	(void)message;
	return;
#elif defined(STRATAGUS_PAX_PHASE_DEBUG)
	if (_pax_svc && _pax_svc->log_printf && message &&
		(strstr(message, "mission") || strstr(message, "LoadCcl") ||
				 strstr(message, "MapLoaded") || strstr(message, "LoadUI") ||
				 strstr(message, "pcall") || strstr(message, "Lua error") ||
				 strstr(message, "Tileset") || strstr(message, "PreMenu") ||
			 strstr(message, "LoadFonts") || strstr(message, "GraphicLoad") ||
				 strstr(message, "Guichan") || strstr(message, "StartMap") ||
				 strstr(message, "CreateGame") || strstr(message, "GameMainLoop") ||
				 strstr(message, "LoadUnitTypes") ||
			 strstr(message, "SetTile") || strstr(message, "CreateUnit") ||
			 strstr(message, "Map"))) {
		_pax_svc->log_printf("Stargus: %s\n", message);
	}
	return;
#elif defined(STRATAGUS_PAX_DIRECT_MISSION)
	(void)message;
	return;
#else
	/* The ESPHome logger currently has a bounded formatting path for PAX
	 * messages.  Keep diagnostics useful without allowing engine startup
	 * chatter to corrupt the next string operation. */
	if (!s_debug_muted && s_debug_count < 24 && _pax_svc &&
		_pax_svc->log_printf && message) {
		++s_debug_count;
		_pax_svc->log_printf("Stargus: %s\n", message);
	}
#endif
}

void pax_stratagus_debug_mute(void)
{
	s_debug_muted = true;
}

/* These narrow hooks are intentionally independent from SDL.  The Stratagus
 * SDL replacement will call them after the engine's software renderer has
 * produced a frame. */
void pax_stratagus_video_init(int width, int height)
{
	if (_pax_svc && _pax_svc->log_printf) {
		_pax_svc->log_printf("Stargus: video init %dx%d\n", width, height);
	}
	if (_pax_svc && _pax_svc->display_set_canvas) {
		/* Menus and gameplay share the native 800x480 RGB565 canvas. */
		_pax_svc->display_set_canvas(800, 480);
	}
}

#ifdef PAX_STRATAGUS_WARCRAFT
#include "pax_video_host_abi.h"
#endif
void pax_stratagus_video_present565(const void *rgb565, int width, int height)
{
#if defined(PAX_STRATAGUS_WARCRAFT) && defined(__riscv)
	if (_pax_svc && rgb565 && height==480 && width==800 &&
		_pax_svc->display_write_frame_custom) {
		// The host scaler is uniform: 640x480 at 1.25 becomes 800x600,
		// clipping the bottom of menus on the 800x480 canvas. Use the full
		// canvas stretch below for 640x480; only native 800x480 is direct.
		pax_video_host_ilp32f((const uint16_t *)rgb565,width,height,
			width==640 ? 0x3fa00000u : 0x3f800000u,_pax_svc->display_write_frame_custom);
		return;
	}
#endif
	if (!_pax_svc || !rgb565 || !_pax_svc->display_get_framebuffer || !_pax_svc->display_flush) return;
	uint16_t *canvas=_pax_svc->display_get_framebuffer();
	if (!canvas) return;
	if (width==800 && height==480) {
		if (canvas!=rgb565) memcpy(canvas,rgb565,800*480*2);
	} else {
		const uint16_t *pixels=(const uint16_t *)rgb565;
		for (int y=0;y<480;++y) for (int x=0;x<800;++x)
			canvas[y*800+x]=pixels[(y*height/480)*width+x*width/800];
	}
	_pax_svc->display_flush();
}

void pax_stratagus_video_present(const void *argb8888, int width, int height)
{
	static bool reported;
	static bool submit_started;
	if (!reported && _pax_svc && _pax_svc->log_printf) {
		_pax_svc->log_printf("Stargus: first video present %dx%d\n", width, height);
		reported = true;
	}
	if (!_pax_svc || !argb8888 ||
		width <= 0 || height <= 0) {
		if (_pax_svc && _pax_svc->log_printf) {
			_pax_svc->log_printf("Stargus: video submit unavailable svc=%p cb=%p pixels=%p size=%dx%d\n",
				(void *)_pax_svc,
				_pax_svc->display_write_frame_custom ? (void *)_pax_svc->display_write_frame_custom : nullptr,
				argb8888, width, height);
		}
		return;
	}
#if defined(STRATAGUS_PAX) && defined(STRATAGUS_PAX_DIRECT_MISSION)
	/* Isolate engine startup from the loader's frame callback while diagnosing
	 * the ESP32 mission path.  The conversion path is restored after the Lua
	 * and map bootstrap are running reliably. */
	return;
#endif
	/* Convert directly into the loader-owned canvas. This needs no extra
	 * PSRAM buffer and preserves every pixel of the native 800x480 game HUD.
	 * The compact menu is scaled here only while its 320x200 surface is used. */
	if (_pax_svc->display_get_framebuffer && _pax_svc->display_flush) {
		uint16_t *canvas = _pax_svc->display_get_framebuffer();
		if (canvas) {
			const uint32_t *pixels = (const uint32_t *)argb8888;
			if (width == 800 && height == 480) {
				pax_rgb565_row(canvas,pixels,800*480);
				_pax_svc->display_flush();
				return;
			}
			for (int y = 0; y < 480; ++y) {
				const uint32_t *src = pixels + ((y * height) / 480) * width;
				uint16_t *dst = canvas + y * 800;
				for (int x = 0; x < 800; ++x) {
					const uint32_t p = src[(x * width) / 800];
					dst[x] = (uint16_t)(((p >> 8) & 0xf800u) |
						((p >> 5) & 0x07e0u) | ((p >> 3) & 0x001fu));
				}
			}
			_pax_svc->display_flush();
			return;
		}
	}
	/* A full 800x480 conversion allocates 768 KiB and can fail after the
	 * engine bootstrap allocations.  Use the compact 320x200 output geometry
	 * used by the other software-rendered PAXs. */
	const int out_width = 320;
	const int out_height = 200;
	const int pixels = out_width * out_height;
	if (pixels > s_frame_capacity) {
		if (s_frame565 && _pax_svc->mem_free) _pax_svc->mem_free(s_frame565);
		s_frame565 = (uint16_t *)_pax_svc->mem_caps_alloc(
			(size_t)pixels * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM);
		if (!s_frame565) s_frame565 = (uint16_t *)_pax_svc->mem_alloc(
			(size_t)pixels * sizeof(uint16_t));
		s_frame_capacity = s_frame565 ? pixels : 0;
	}
	if (!s_frame565) return;
	const uint8_t *src_bytes = (const uint8_t *)argb8888;
	for (int y = 0; y < out_height; ++y) {
		const int sy = (y * height) / out_height;
		const uint32_t *src = (const uint32_t *)(src_bytes + sy * width * 4);
		for (int x = 0; x < out_width; ++x) {
			const uint32_t p = src[(x * width) / out_width];
			s_frame565[y * out_width + x] = (uint16_t)(((p >> 8) & 0xf800u) |
				((p >> 5) & 0x07e0u) | ((p >> 3) & 0x001fu));
		}
	}
	if (!submit_started && _pax_svc->log_printf) {
		_pax_svc->log_printf("Stargus: display submit begin buffer=%p size=%dx%d native=800x480\n",
			(void *)s_frame565, out_width, out_height);
		submit_started = true;
	}
	/* The custom callback is the stable path for the PAX loader.  The direct
	 * DSI back-buffer path is useful for the normal loader, but on this
	 * diagnostic build it can fault before the mission hook is reached. */
	if (false && _pax_svc->display_write_frame_rgb565 && _pax_svc->display_flush) {
		if (!s_canvas565) {
			s_canvas565 = (uint16_t *)_pax_svc->mem_caps_alloc(
				800u * 480u * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM);
			if (!s_canvas565 && _pax_svc->mem_alloc)
				s_canvas565 = (uint16_t *)_pax_svc->mem_alloc(
					800u * 480u * sizeof(uint16_t));
		}
		if (s_canvas565) {
			memset(s_canvas565, 0, 800u * 480u * sizeof(uint16_t));
			for (int y = 0; y < 480; ++y) {
				const int sy = (y * out_height) / 480;
				for (int x = 0; x < 768; ++x) {
					const int sx = (x * out_width) / 768;
					s_canvas565[y * 800 + 16 + x] =
						s_frame565[sy * out_width + sx];
				}
			}
			_pax_svc->display_write_frame_rgb565(s_canvas565);
			_pax_svc->display_flush();
		} else if (_pax_svc->display_write_frame_custom) {
			_pax_svc->display_write_frame_custom(s_frame565, out_width, out_height,
				2.4f, false);
		}
	} else if (_pax_svc->display_write_frame_custom) {
		_pax_svc->display_write_frame_custom(s_frame565, out_width, out_height,
			2.4f, false);
	}
	if (submit_started && _pax_svc->log_printf) {
		static bool submit_returned;
		if (!submit_returned) {
			_pax_svc->log_printf("Stargus: display submit returned\n");
			submit_returned = true;
		}
	}
}

int pax_stratagus_menu_poll(int page, int selected)
{
	static int previous[PAX_INPUT_MAX];
	static bool previous_touch;
	int action = 0;
	pax_gamepad_state_t state{};
	if (_pax_svc && _pax_svc->input_gamepad_read) {
		_pax_svc->input_gamepad_read(&state);
		// Closing the shared menu can unblock this read with the launcher's
		// close flag already set. Observe it before forcing a smoke-test mission.
		pax_stratagus_input_poll();
		if (pax_exit_requested) return 0;
		const auto pressed = [&](int input) {
			const int now = state.values[input] != 0;
			const int edge = now && !previous[input];
			previous[input] = now;
			return edge;
		};
		if (pressed(PAX_INPUT_UP)) action = -1;
		if (pressed(PAX_INPUT_DOWN)) action = 1;
		if (pressed(PAX_INPUT_A) || pressed(PAX_INPUT_SELECT)) action = 2;
		if (pressed(PAX_INPUT_B)) action = 3;
	}
	if (_pax_svc && _pax_svc->input_keyboard_read) {
		pax_keyboard_event_t event{};
		while (_pax_svc->input_keyboard_read(&event)) {
			if (!event.down) continue;
			switch (event.key) {
				case PAX_KEY_UP: case 273: action = -1; break;
				case PAX_KEY_DOWN: case 274: action = 1; break;
				case PAX_KEY_ENTER: case ' ': action = 2; break;
				case 27: case 'x': case 'X': action = 3; break;
				default: break;
			}
		}
	}
	if (_pax_svc && _pax_svc->touch_read) {
		int x = 0, y = 0;
		const bool touching = _pax_svc->touch_read(&x, &y) != 0;
		if (!touching) {
			previous_touch = false;
		} else if (!previous_touch) {
			previous_touch = true;
			/* touch_read reports the active 800x480 canvas.  The software menu
			 * is 320x200 and is presented across that canvas. */
			const int menu_x = (x * 320) / 800;
			const int menu_y = (y * 200) / 480;
			const int count = page == 0 ? 4 : (page == 3 ? 2 : 3);
			for (int i = 0; i < count; ++i) {
				const int row_y = 56 + i * 26;
				if (menu_x >= 84 && menu_x < 236 && menu_y >= row_y && menu_y < row_y + 19) {
					if (_pax_svc->log_printf)
						_pax_svc->log_printf("Stargus: touch menu row=%d canvas=%d,%d\n", i, x, y);
					action = 100 + i;
					break;
				}
			}
		}
	}
	return action;
}

void pax_stratagus_input_poll(void)
{
	if ((_pax_svc && _pax_svc->input_l3_read && _pax_svc->input_l3_read()))
		pax_exit_requested = 1;
}

void pax_stratagus_audio_submit(const int16_t *stereo, int frames)
{
	if (_pax_svc && _pax_svc->audio_submit && stereo && frames > 0) {
		_pax_svc->audio_submit((short *)stereo, frames);
	}
}

void pax_stratagus_platform_shutdown(void)
{
	pax_stratagus_audio_stop();
	pax_stratagus_video_shutdown();
	if (s_frame565 && _pax_svc && _pax_svc->mem_free) {
		_pax_svc->mem_free(s_frame565);
	}
	if (s_canvas565 && _pax_svc && _pax_svc->mem_free) {
		_pax_svc->mem_free(s_canvas565);
	}
	s_frame565 = nullptr;
	s_canvas565 = nullptr;
	s_frame_capacity = 0;
}
