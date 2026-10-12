/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sqlite_esp.h"

#include <stdint.h>
#include <stddef.h>

#include "esp_heap_caps.h"
#include "sqlite3.h"

// Each allocation stores its requested size in front of the payload for xSize.
#define SQLITE_ESP_HEADER_SIZE 8
#define SQLITE_ESP_CAPS_PREFERRED (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SQLITE_ESP_CAPS_FALLBACK (MALLOC_CAP_8BIT)

static void *mem_malloc(int size)
{
    if (size <= 0) {
        return NULL;
    }
    uint8_t *block = heap_caps_malloc_prefer((size_t)size + SQLITE_ESP_HEADER_SIZE, 2,
                                             SQLITE_ESP_CAPS_PREFERRED,
                                             SQLITE_ESP_CAPS_FALLBACK);
    if (block == NULL) {
        return NULL;
    }
    *(size_t *)block = (size_t)size;
    return block + SQLITE_ESP_HEADER_SIZE;
}

static void mem_free(void *payload)
{
    if (payload != NULL) {
        heap_caps_free((uint8_t *)payload - SQLITE_ESP_HEADER_SIZE);
    }
}

static void *mem_realloc(void *payload, int size)
{
    if (payload == NULL) {
        return mem_malloc(size);
    }
    if (size <= 0) {
        mem_free(payload);
        return NULL;
    }
    uint8_t *block = heap_caps_realloc_prefer((uint8_t *)payload - SQLITE_ESP_HEADER_SIZE,
                                              (size_t)size + SQLITE_ESP_HEADER_SIZE, 2,
                                              SQLITE_ESP_CAPS_PREFERRED,
                                              SQLITE_ESP_CAPS_FALLBACK);
    if (block == NULL) {
        return NULL;
    }
    *(size_t *)block = (size_t)size;
    return block + SQLITE_ESP_HEADER_SIZE;
}

static int mem_size(void *payload)
{
    return payload != NULL
               ? (int)*(size_t *)((uint8_t *)payload - SQLITE_ESP_HEADER_SIZE)
               : 0;
}

static int mem_roundup(int size)
{
    return (size + 7) & ~7;
}

static int mem_init(void *app_data)
{
    (void)app_data;
    return SQLITE_OK;
}

static void mem_shutdown(void *app_data)
{
    (void)app_data;
}

static const sqlite3_mem_methods s_mem_methods = {
    .xMalloc = mem_malloc,
    .xFree = mem_free,
    .xRealloc = mem_realloc,
    .xSize = mem_size,
    .xRoundup = mem_roundup,
    .xInit = mem_init,
    .xShutdown = mem_shutdown,
    .pAppData = NULL,
};

int sqlite_esp_configure_memory(void)
{
    return sqlite3_config(SQLITE_CONFIG_MALLOC, &s_mem_methods);
}
