/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include "led_policy.h"
#include <string.h>
#include <limits.h>

void led_config_defaults(led_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->version = EBC_LED_CONFIG_VERSION;
    config->count = 2;
    config->leds[0] = (led_assignment_t){LED_GROUP_TESTER, 1};
    config->leds[1] = (led_assignment_t){LED_GROUP_FRAMES, 0};
}

bool led_config_valid(const led_config_t *config)
{
    if (config->version != EBC_LED_CONFIG_VERSION || config->count < 1 || config->count > EBC_LED_MAX) {
        return false;
    }
    for (unsigned i = 0; i < EBC_LED_MAX; ++i) {
        if (config->leds[i].group >= LED_GROUP_COUNT || config->leds[i].ota > 1) return false;
    }
    return true;
}

led_color_t led_tester_color(const led_input_t *input)
{
    if (!input->connected) return (led_color_t){0, 0, 0};
    if (input->automatic) return (led_color_t){24, 8, 0};
    if (input->manual) return (led_color_t){0, 0, 24};
    return (led_color_t){0, 24, 0};
}

led_color_t led_frame_color(led_frame_state_t *state, const led_input_t *input, uint64_t now_ms)
{
    uint32_t delta = state->initialized ? input->valid_count - state->previous_count : 0;
    state->initialized = true;
    state->previous_count = input->valid_count;
    if (input->valid_age_ms > 5000 && input->invalid_age_ms > 5000) {
        state->pending_flashes = 0;
        state->flashing = false;
        return (led_color_t){0, 0, 0};
    }
    if (input->communication_error || input->invalid_age_ms < input->valid_age_ms) {
        state->pending_flashes = 0;
        state->flashing = false;
        return (led_color_t){24, 8, 0};
    }
    if (delta > UINT32_MAX - state->pending_flashes) delta = UINT32_MAX - state->pending_flashes;
    state->pending_flashes += delta;
    if (state->flashing && now_ms - state->phase_started_ms >= 100) {
        state->flashing = false;
        state->phase_started_ms = now_ms;
    }
    if (!state->flashing && state->pending_flashes &&
        now_ms - state->phase_started_ms >= 100) {
        --state->pending_flashes;
        state->flashing = true;
        state->phase_started_ms = now_ms;
    }
    return state->flashing ? (led_color_t){0, 48, 0} : (led_color_t){0, 3, 0};
}
