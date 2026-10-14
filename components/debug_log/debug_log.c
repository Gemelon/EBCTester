/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debug_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    uint32_t seq;
    uint16_t length;
    char text[DEBUG_LOG_LINE_MAX + 1];
} debug_log_entry_t;

/* ANSI escape sequence states used while filtering colour codes. */
enum {
    ESCAPE_NONE = 0,
    ESCAPE_SEEN_ESC,
    ESCAPE_IN_SEQUENCE,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static debug_log_entry_t *s_entries;
static size_t s_head;
static size_t s_count;
static uint32_t s_next_seq = 1;
static char s_pending[DEBUG_LOG_LINE_MAX + 1];
static size_t s_pending_length;
static uint8_t s_escape;
static volatile bool s_capture;
static vprintf_like_t s_original_vprintf;
static atomic_uint_fast32_t s_dropped;
static atomic_bool s_resync;

/* Appends the assembled line to the ring buffer; caller holds the spinlock. */
static void commit_line_locked(void)
{
    if (s_pending_length == 0) {
        return;
    }
    debug_log_entry_t *entry = &s_entries[(s_head + s_count) % DEBUG_LOG_LINE_COUNT];
    entry->seq = s_next_seq++;
    entry->length = (uint16_t)s_pending_length;
    memcpy(entry->text, s_pending, s_pending_length);
    entry->text[s_pending_length] = '\0';
    if (s_count < DEBUG_LOG_LINE_COUNT) {
        ++s_count;
    } else {
        s_head = (s_head + 1) % DEBUG_LOG_LINE_COUNT;
    }
    s_pending_length = 0;
}

/* The log framework emits one message as several chunks, so lines are
 * assembled here and committed on each newline.
 *
 * Capturing has the lowest priority of all users of the ring buffer: the
 * spinlock is only ever tried once. When a reader or a clear request holds it,
 * the chunk is discarded and counted instead of waiting, so logging can never
 * delay the caller. */
static void append_chunk(const char *data, size_t length)
{
    if (portTRY_ENTER_CRITICAL_SAFE(&s_lock, portMUX_TRY_LOCK) != pdPASS) {
        atomic_fetch_add_explicit(&s_dropped, 1, memory_order_relaxed);
        atomic_store_explicit(&s_resync, true, memory_order_relaxed);
        return;
    }
    if (atomic_exchange_explicit(&s_resync, false, memory_order_relaxed)) {
        /* A previous chunk was lost, so the partially assembled line is
         * incomplete. Drop it instead of merging unrelated text. */
        s_pending_length = 0;
        s_escape = ESCAPE_NONE;
    }
    if (s_entries != NULL) {
        for (size_t i = 0; i < length; ++i) {
            const unsigned char ch = (unsigned char)data[i];
            if (s_escape == ESCAPE_SEEN_ESC) {
                s_escape = (ch == '[') ? ESCAPE_IN_SEQUENCE : ESCAPE_NONE;
                continue;
            }
            if (s_escape == ESCAPE_IN_SEQUENCE) {
                if (ch >= 0x40 && ch <= 0x7e) {
                    s_escape = ESCAPE_NONE;
                }
                continue;
            }
            if (ch == 0x1b) {
                s_escape = ESCAPE_SEEN_ESC;
            } else if (ch == '\n') {
                commit_line_locked();
            } else if (ch == '\r' || ch == '\0') {
                continue;
            } else if (s_pending_length < DEBUG_LOG_LINE_MAX) {
                s_pending[s_pending_length++] = (char)ch;
            }
        }
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

static int debug_log_vprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int written = s_original_vprintf != NULL ? s_original_vprintf(format, args)
                                                   : vprintf(format, args);
    if (s_capture) {
        char buffer[DEBUG_LOG_LINE_MAX + 65];
        const int length = vsnprintf(buffer, sizeof(buffer), format, copy);
        if (length > 0) {
            const size_t usable = (size_t)length < sizeof(buffer) ? (size_t)length
                                                                  : sizeof(buffer) - 1;
            append_chunk(buffer, usable);
        }
    }
    va_end(copy);
    return written;
}

esp_err_t debug_log_init(void)
{
    if (s_original_vprintf != NULL) {
        return ESP_OK;
    }
    const esp_err_t err = debug_log_set_enabled(true);
    if (err != ESP_OK) {
        return err;
    }
    s_original_vprintf = esp_log_set_vprintf(debug_log_vprintf);
    return ESP_OK;
}

esp_err_t debug_log_set_enabled(bool enabled)
{
    if (!enabled) {
        s_capture = false;
        portENTER_CRITICAL(&s_lock);
        debug_log_entry_t *released = s_entries;
        s_entries = NULL;
        s_head = 0;
        s_count = 0;
        s_pending_length = 0;
        s_escape = ESCAPE_NONE;
        portEXIT_CRITICAL(&s_lock);
        free(released);
        return ESP_OK;
    }
    if (s_entries == NULL) {
        debug_log_entry_t *buffer = heap_caps_calloc(DEBUG_LOG_LINE_COUNT, sizeof(*buffer),
                                                     MALLOC_CAP_SPIRAM);
        if (buffer == NULL) {
            buffer = calloc(DEBUG_LOG_LINE_COUNT, sizeof(*buffer));
        }
        if (buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_lock);
        if (s_entries == NULL) {
            s_entries = buffer;
            buffer = NULL;
            s_head = 0;
            s_count = 0;
            s_pending_length = 0;
            s_escape = ESCAPE_NONE;
        }
        portEXIT_CRITICAL(&s_lock);
        free(buffer);
    }
    atomic_store_explicit(&s_dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&s_resync, false, memory_order_relaxed);
    s_capture = true;
    return ESP_OK;
}

bool debug_log_is_enabled(void)
{
    return s_capture;
}

void debug_log_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_head = 0;
    s_count = 0;
    s_pending_length = 0;
    s_escape = ESCAPE_NONE;
    portEXIT_CRITICAL(&s_lock);
    atomic_store_explicit(&s_dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&s_resync, false, memory_order_relaxed);
}

uint32_t debug_log_get_dropped(void)
{
    return (uint32_t)atomic_load_explicit(&s_dropped, memory_order_relaxed);
}

void debug_log_get_cursor(uint32_t *first_seq, uint32_t *next_seq, size_t *count)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t next = s_next_seq;
    const size_t buffered = s_entries != NULL ? s_count : 0;
    const uint32_t first = buffered > 0 ? s_entries[s_head].seq : next;
    portEXIT_CRITICAL(&s_lock);
    if (first_seq != NULL) *first_seq = first;
    if (next_seq != NULL) *next_seq = next;
    if (count != NULL) *count = buffered;
}

bool debug_log_read_line(uint32_t wanted_seq, uint32_t *seq, char *text, size_t capacity)
{
    if (text == NULL || capacity == 0) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    if (s_entries != NULL && s_count > 0) {
        const uint32_t first = s_entries[s_head].seq;
        const uint32_t target = wanted_seq > first ? wanted_seq : first;
        if (target < s_next_seq) {
            const debug_log_entry_t *entry =
                &s_entries[(s_head + (size_t)(target - first)) % DEBUG_LOG_LINE_COUNT];
            size_t length = entry->length;
            if (length >= capacity) {
                length = capacity - 1;
            }
            memcpy(text, entry->text, length);
            text[length] = '\0';
            if (seq != NULL) {
                *seq = entry->seq;
            }
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}
