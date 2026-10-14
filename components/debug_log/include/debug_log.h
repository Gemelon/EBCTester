/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of log lines kept in the ring buffer. */
#define DEBUG_LOG_LINE_COUNT 500

/** Maximum number of characters stored per log line, excluding the terminator. */
#define DEBUG_LOG_LINE_MAX 159

/**
 * @brief Installs the log hook and allocates the ring buffer.
 *
 * Call this as early as possible in app_main so that start-up messages are
 * captured. Capture is active right away; apply the persisted setting later
 * with debug_log_set_enabled().
 *
 * Output written before the hook is installed, bootloader output and plain
 * printf() output are not captured.
 */
esp_err_t debug_log_init(void);

/**
 * @brief Enables or disables capturing.
 *
 * Disabling releases the ring buffer; enabling allocates it again. The hook
 * itself stays installed, so console output is never affected.
 */
esp_err_t debug_log_set_enabled(bool enabled);

/** @brief Returns true while log lines are captured. */
bool debug_log_is_enabled(void);

/** @brief Drops all buffered lines without changing the enabled state. */
void debug_log_clear(void);

/**
 * @brief Reports how many log chunks were discarded because the ring buffer
 *        was busy.
 *
 * Capturing never waits for the buffer lock, so a chunk is dropped whenever a
 * reader or a clear request holds it. The counter resets on debug_log_clear()
 * and when capturing is enabled.
 */
uint32_t debug_log_get_dropped(void);

/**
 * @brief Reports the buffer cursor.
 *
 * @param[out] first_seq Sequence number of the oldest buffered line, or the
 *                       next sequence number when the buffer is empty.
 * @param[out] next_seq  Sequence number the next captured line will use.
 * @param[out] count     Number of buffered lines.
 */
void debug_log_get_cursor(uint32_t *first_seq, uint32_t *next_seq, size_t *count);

/**
 * @brief Copies the oldest buffered line whose sequence number is >= wanted_seq.
 *
 * @param wanted_seq Lowest acceptable sequence number.
 * @param[out] seq   Sequence number of the returned line.
 * @param text       Destination buffer, always NUL terminated on success.
 * @param capacity   Size of @p text in bytes.
 * @return true when a line was copied, false when no such line exists.
 */
bool debug_log_read_line(uint32_t wanted_seq, uint32_t *seq, char *text, size_t capacity);

#ifdef __cplusplus
}
#endif
