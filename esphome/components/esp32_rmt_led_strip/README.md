# Elecrow RGBW compatibility

Based on this workspace's ESPHome esp32_rmt_led_strip driver, retaining its current channel_colors mapping and RMT transmission implementation. Ported the mode-aware update_state, RGB/WHITE traits and color_interlock option from the user's working esphome006/config/components/esp32_rmt_led_strip component. With interlock enabled, nonzero white takes priority over RGB. Elecrow uses WRGB on GPIO8 with the WS2814 custom timings in scratch-io.yaml; its white emitter appears yellow. Effects retain the normal addressable-light behavior.
