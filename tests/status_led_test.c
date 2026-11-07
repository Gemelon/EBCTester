/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "status_led.h"
#include "ota_manager.h"

#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
typedef unsigned TickType_t;
typedef void *led_strip_handle_t;
typedef struct {
    int strip_gpio_num;
    unsigned max_leds;
    int led_model;
    int color_component_format;
} led_strip_config_t;
typedef struct { unsigned resolution_hz; struct { bool with_dma; } flags; } led_strip_rmt_config_t;
#define LED_MODEL_WS2812 1
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB 1
static status_led_pixel_t output[16];
static unsigned refreshes;
static esp_err_t output_error;
static int64_t esp_timer_get_time(void) { return 1000000; }
static TickType_t xTaskGetTickCount(void) { return 0; }
static void vTaskDelayUntil(TickType_t *wake, TickType_t delay) { (void)wake; (void)delay; }
static void vTaskDelete(void *task) { (void)task; }
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
void ota_manager_get_status(ota_status_t *status) { status->state = OTA_IDLE; }
static esp_err_t led_strip_set_pixel(led_strip_handle_t strip, unsigned i, unsigned r, unsigned g, unsigned b)
{ (void)strip; output[i] = (status_led_pixel_t){r, g, b, false}; return output_error; }
static esp_err_t led_strip_set_pixel_hsv(led_strip_handle_t strip, unsigned i, unsigned h, unsigned s, unsigned v)
{ (void)h; (void)s; return led_strip_set_pixel(strip, i, v, v, v); }
static esp_err_t led_strip_refresh(led_strip_handle_t strip) { (void)strip; refreshes++; return output_error; }
static esp_err_t led_strip_del(led_strip_handle_t strip) { (void)strip; return ESP_OK; }
static esp_err_t led_strip_new_rmt_device(const led_strip_config_t *config,
                                         const led_strip_rmt_config_t *rmt, led_strip_handle_t *out)
{ (void)rmt; assert(config->max_leds == 4); *out = (void *)1; return ESP_OK; }

#include "../external/ota-suite/components/status_led/status_led.c"

static esp_err_t normal(status_led_pixel_t *out, size_t count)
{
    assert(count == 4);
    out[0] = (status_led_pixel_t){0, 24, 0, false};
    out[1] = (status_led_pixel_t){0, 3, 0, true};
    out[2] = (status_led_pixel_t){0, 24, 0, true};
    out[3] = (status_led_pixel_t){0, 3, 0, false};
    return ESP_OK;
}
int main(void)
{
    assert(status_led_set_renderer(0, normal) == ESP_ERR_INVALID_ARG);
    assert(status_led_set_renderer(17, normal) == ESP_ERR_INVALID_ARG);
    assert(status_led_set_renderer(4, NULL) == ESP_ERR_INVALID_ARG);
    assert(status_led_set_renderer(4, normal) == ESP_OK);
    assert(status_led_set_gpio(48) == ESP_OK);
    assert(status_led_start() == ESP_OK);
    assert(status_led_start() == ESP_ERR_INVALID_STATE);
    assert(output[0].green == 24 && output[1].green == 3);
    assert(render(OTA_DOWNLOADING, 0) == ESP_OK);
    assert(output[1].blue == 2 && output[2].blue == 2 && output[0].green == 24 && output[3].green == 3);
    assert(render(OTA_DOWNLOADING, 1000) == ESP_OK && output[1].blue == 24);
    assert(render(OTA_VERIFYING, 1000) == ESP_OK && output[1].red == 24 && output[1].green == 24);
    assert(render(OTA_REBOOTING, 0) == ESP_OK && output[1].green == 24 && output[1].red == 0);
    assert(render(OTA_FAILED, 0) == ESP_OK && output[1].red == 24);
    assert(render(OTA_FAILED, 500) == ESP_OK && output[1].red == 0);
    assert(render(OTA_IDLE, 0) == ESP_OK && output[1].green == 3);
    assert(refreshes == 8);
    output_error = ESP_FAIL;
    assert(render(OTA_IDLE, 0) == ESP_FAIL);
    free(pixels);
    puts("LED driver tests passed: independent assignments, selective OTA override, pulse/blink phases and error propagation.");
    return 0;
}
