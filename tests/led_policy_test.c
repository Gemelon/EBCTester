/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdio.h>
#include "led_policy.h"

int main(void)
{
    led_config_t config;
    led_config_defaults(&config);
    assert(led_config_valid(&config) && config.count == 2);
    assert(config.leds[0].group == LED_GROUP_TESTER && config.leds[0].ota);
    assert(config.leds[1].group == LED_GROUP_FRAMES && !config.leds[1].ota);
    config.count = 16;
    assert(led_config_valid(&config));
    config.count = 17;
    assert(!led_config_valid(&config));
    config.count = 0;
    assert(!led_config_valid(&config));
    config.count = 1;
    config.leds[15].group = LED_GROUP_COUNT;
    assert(!led_config_valid(&config));
    led_config_defaults(&config);
    config.leds[0].ota = 2;
    assert(!led_config_valid(&config));
    led_config_defaults(&config);
    config.version++;
    assert(!led_config_valid(&config));

    led_input_t input = {0};
    assert(led_tester_color(&input).green == 0);
    input.connected = true;
    assert(led_tester_color(&input).green == 24);
    input.manual = true;
    assert(led_tester_color(&input).blue == 24);
    input.automatic = true;
    led_color_t automatic = led_tester_color(&input);
    assert(automatic.red == 24 && automatic.green == 8 && automatic.blue == 0);
    input.connected = false;
    assert(led_tester_color(&input).red == 0);

    led_frame_state_t state = {0};
    input.valid_age_ms = input.invalid_age_ms = UINT32_MAX;
    assert(led_frame_color(&state, &input, 1000).green == 0);
    input.valid_age_ms = 0;
    input.valid_count = 1;
    assert(led_frame_color(&state, &input, 1100).green == 48);
    input.valid_age_ms = 100;
    assert(led_frame_color(&state, &input, 1200).green == 3);
    input.valid_count = 4;
    assert(led_frame_color(&state, &input, 1300).green == 48);
    assert(led_frame_color(&state, &input, 1400).green == 3);
    assert(led_frame_color(&state, &input, 1500).green == 48);
    assert(led_frame_color(&state, &input, 1600).green == 3);
    assert(led_frame_color(&state, &input, 1700).green == 48);
    assert(led_frame_color(&state, &input, 1800).green == 3);
    assert(state.pending_flashes == 0);
    input.valid_age_ms = 5000;
    assert(led_frame_color(&state, &input, 6000).green == 3);
    input.valid_age_ms = 5001;
    assert(led_frame_color(&state, &input, 6001).green == 0);
    input.invalid_age_ms = 0;
    led_color_t error = led_frame_color(&state, &input, 6100);
    assert(error.red == 24 && error.green == 8 && error.blue == 0);
    input.valid_age_ms = 0;
    input.invalid_age_ms = 100;
    assert(led_frame_color(&state, &input, 6200).red == 0);
    input.communication_error = true;
    assert(led_frame_color(&state, &input, 6300).red == 24);
    input.valid_age_ms = input.invalid_age_ms = 5001;
    assert(led_frame_color(&state, &input, 6400).red == 0);
    puts("LED policy tests passed: defaults, limits, all modes, per-message flashes, 5 s timeout, errors and recovery.");
    return 0;
}
