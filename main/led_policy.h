/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EBC_LED_MAX 16
#define EBC_LED_CONFIG_VERSION 1

typedef enum { LED_GROUP_OFF, LED_GROUP_TESTER, LED_GROUP_FRAMES, LED_GROUP_COUNT } led_group_t;
typedef struct { uint8_t group; uint8_t ota; } led_assignment_t;
typedef struct {
    uint8_t version;
    uint8_t count;
    led_assignment_t leds[EBC_LED_MAX];
} led_config_t;
typedef struct { uint8_t red, green, blue; } led_color_t;
typedef struct {
    bool connected;
    bool automatic;
    bool manual;
    bool communication_error;
    uint32_t valid_age_ms;
    uint32_t invalid_age_ms;
    uint32_t valid_count;
} led_input_t;
typedef struct {
    uint32_t previous_count;
    uint32_t pending_flashes;
    uint64_t phase_started_ms;
    bool initialized;
    bool flashing;
} led_frame_state_t;

#ifdef __cplusplus
extern "C" {
#endif
void led_config_defaults(led_config_t *config);
bool led_config_valid(const led_config_t *config);
led_color_t led_tester_color(const led_input_t *input);
led_color_t led_frame_color(led_frame_state_t *state, const led_input_t *input, uint64_t now_ms);
#ifdef __cplusplus
}
#endif
