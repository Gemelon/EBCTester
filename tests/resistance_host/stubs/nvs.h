#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
inline esp_err_t nvs_open(const char *, int, nvs_handle_t *h) { *h = 1; return ESP_OK; }
inline esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t) { return ESP_OK; }
inline esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t) { return ESP_OK; }
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
inline void nvs_close(nvs_handle_t) {}
