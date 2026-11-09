/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t rtc_clock_start(void);
void rtc_clock_ntp_synced(int64_t utc);
bool rtc_clock_get_utc(int64_t *utc);
