#include "dv_status_led.h"
#include "dv_board.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include "nvs.h"

#define BLINK_PERIOD_MS  200      // ~2.5 Hz — matches stock firmware feel

static _Atomic dv_status_led_mode_t s_mode = DV_STATUS_LED_OFF;
static TaskHandle_t                 s_task = NULL;

static void led_task(void *arg)
{
    (void)arg;
    bool blink_on = false;
    for (;;) {
        switch (atomic_load(&s_mode)) {
            case DV_STATUS_LED_OFF:
                gpio_set_level(DV_PIN_USER_BUTTON_LED, 0);
                blink_on = false;
                vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
                break;
            case DV_STATUS_LED_SOLID:
                gpio_set_level(DV_PIN_USER_BUTTON_LED, 1);
                blink_on = false;
                vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
                break;
            case DV_STATUS_LED_BLINK:
                blink_on = !blink_on;
                gpio_set_level(DV_PIN_USER_BUTTON_LED, blink_on);
                vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
                break;
        }
    }
}

esp_err_t dv_status_led_start(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DV_PIN_USER_BUTTON_LED,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    if (xTaskCreate(led_task, "dv_led", 2048, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dv_status_led_set(dv_status_led_mode_t mode)
{
    atomic_store(&s_mode, mode);
}

#define LED_NVS_NS "app_nvs"

static uint8_t clamp_mode(uint8_t v, uint8_t fallback)
{
    return v <= DV_STATUS_LED_BLINK ? v : fallback;
}

void dv_status_led_get_policy(uint8_t *auto_mode_cfg, uint8_t *manual_mode_cfg)
{
    uint8_t a = DV_STATUS_LED_OFF, m = DV_STATUS_LED_BLINK;   /* stock behavior */
    nvs_handle_t h;
    if (nvs_open(LED_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "led_auto", &v) == ESP_OK) a = clamp_mode(v, a);
        if (nvs_get_u8(h, "led_manual", &v) == ESP_OK) m = clamp_mode(v, m);
        nvs_close(h);
    }
    if (auto_mode_cfg) *auto_mode_cfg = a;
    if (manual_mode_cfg) *manual_mode_cfg = m;
}

esp_err_t dv_status_led_set_policy(uint8_t auto_mode_cfg, uint8_t manual_mode_cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LED_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "led_auto", clamp_mode(auto_mode_cfg, DV_STATUS_LED_OFF));
    if (err == ESP_OK) err = nvs_set_u8(h, "led_manual", clamp_mode(manual_mode_cfg, DV_STATUS_LED_BLINK));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void dv_status_led_apply_policy(bool auto_mode)
{
    uint8_t a, m;
    dv_status_led_get_policy(&a, &m);
    dv_status_led_set((dv_status_led_mode_t)(auto_mode ? a : m));
}
