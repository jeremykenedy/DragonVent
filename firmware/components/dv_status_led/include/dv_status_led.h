#pragma once

// User-button LED (GPIO 27, active-high). Matches stock firmware's mode
// indicator: off in auto mode, blinking in manual mode. The SOLID mode is
// exposed for future callers (e.g. a "device is on / captive portal up"
// hint) but the vent policy itself only uses OFF and BLINK.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DV_STATUS_LED_OFF,
    DV_STATUS_LED_SOLID,
    DV_STATUS_LED_BLINK,
} dv_status_led_mode_t;

esp_err_t dv_status_led_start(void);
void      dv_status_led_set(dv_status_led_mode_t mode);

// Configurable override of the stock ring behavior. NVS (namespace "app_nvs")
// keys "led_auto" / "led_manual" hold a dv_status_led_mode_t (0 off, 1 solid,
// 2 blink); defaults match stock (auto=off, manual=blink). apply reads the
// stored choice for the given policy mode and drives the ring accordingly.
void dv_status_led_apply_policy(bool auto_mode);
// Current stored choices, for the settings API.
void dv_status_led_get_policy(uint8_t *auto_mode_cfg, uint8_t *manual_mode_cfg);
// Persist a choice (values clamped to 0..2). Does not re-drive the ring.
esp_err_t dv_status_led_set_policy(uint8_t auto_mode_cfg, uint8_t manual_mode_cfg);
