/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RTC_VALID_TIME_MIN INT64_C(1704067200)
#define RTC_SYNC_INTERVAL_MS UINT32_C(21600000)

bool ds3231_decode_time(const uint8_t registers[7], int64_t *utc);
bool ds3231_encode_time(int64_t utc, uint8_t registers[7]);
