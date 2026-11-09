/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ebc_config_web.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include "cJSON.h"
#include "ebc_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ota_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "test_control.h"
#include "web_interface.h"

extern const unsigned char maker_html_start[] asm("_binary_maker_settings_html_start");
extern const unsigned char maker_html_end[] asm("_binary_maker_settings_html_end");
extern const unsigned char maker_js_start[] asm("_binary_maker_settings_js_start");
extern const unsigned char maker_js_end[] asm("_binary_maker_settings_js_end");
extern const unsigned char reset_html_start[] asm("_binary_config_reset_html_start");
extern const unsigned char reset_html_end[] asm("_binary_config_reset_html_end");
extern const unsigned char reset_js_start[] asm("_binary_config_reset_js_start");
extern const unsigned char reset_js_end[] asm("_binary_config_reset_js_end");

namespace {
constexpr char TAG[] = "maker_settings";
constexpr size_t MAX_BODY = 8192;
enum class Type { Gpio, U16, U32, Bool, Bus, String, Password };
struct Field {
    const char *section;
    const char *name;
    const char *label;
    size_t offset;
    size_t size;
    Type type;
    int minimum;
    int maximum;
};
#define FIELD(section, member, name, label, type, min, max) \
    {#section, name, label, offsetof(ebc_config_t, section.member), \
     sizeof(((ebc_config_t *)nullptr)->section.member), Type::type, min, max}
const Field fields[] = {
    FIELD(rtc, enabled, "enabled", "Enable DS3231 RTC", Bool, 0, 1),
    FIELD(rtc, int_gpio, "int_gpio", "RTC INT GPIO (-1: not connected; reserved input)", Gpio, -1, 48),
    FIELD(rtc, sda_gpio, "sda_gpio", "I2C SDA GPIO", Gpio, -1, 48),
    FIELD(rtc, scl_gpio, "scl_gpio", "I2C SCL GPIO", Gpio, -1, 48),
    FIELD(tester, tester_uart.tx_gpio, "tester_tx_gpio", "Tester UART TX GPIO", Gpio, -1, 48),
    FIELD(tester, tester_uart.rx_gpio, "tester_rx_gpio", "Tester UART RX GPIO", Gpio, -1, 48),
    FIELD(tester, tester_uart.baud, "tester_baud", "Tester UART baud rate", U32, 1200, 115200),
    FIELD(tester, dongle_uart.tx_gpio, "dongle_tx_gpio", "Dongle UART TX GPIO", Gpio, -1, 48),
    FIELD(tester, dongle_uart.rx_gpio, "dongle_rx_gpio", "Dongle UART RX GPIO", Gpio, -1, 48),
    FIELD(tester, dongle_uart.baud, "dongle_baud", "Dongle UART baud rate", U32, 1200, 115200),
    FIELD(tester, averaging_length, "averaging_length", "Frames per stored average", U16, 1, 120),
    FIELD(tester, device_id, "device_id", "Device ID", String, 0, 0),
    FIELD(tester, mqtt_enabled, "mqtt_enabled", "Enable Home Assistant MQTT", Bool, 0, 1),
    FIELD(ota, ap_password, "ap_password", "Access point password", Password, 0, 0),
    FIELD(ota, sntp_server, "sntp_server", "SNTP server", String, 0, 0),
    FIELD(ota, manifest_url, "manifest_url", "Firmware manifest URL", String, 0, 0),
    FIELD(ota, allow_http, "allow_http", "Allow unencrypted HTTP firmware updates", Bool, 0, 1),
    FIELD(ota, status_led_enabled, "status_led_enabled", "Enable status LED", Bool, 0, 1),
    FIELD(ota, led_gpio, "led_gpio", "Status LED GPIO", Gpio, -1, 48),
    FIELD(ota, web_password, "web_password", "Web password", Password, 0, 0),
    FIELD(ota, settings_require_password, "settings_require_password", "Confirm settings changes with password", Bool, 0, 1),
    FIELD(ota, sd_languages, "sd_languages", "Load additional languages from SD", Bool, 0, 1),
    FIELD(ota, primary_tabs, "primary_tabs", "Primary tabs (comma-separated)", String, 0, 0),
    FIELD(ota, tab_order, "tab_order", "Tab order (comma-separated)", String, 0, 0),
    FIELD(sd, bus, "bus", "SD bus", Bus, 0, 1),
    FIELD(sd, sdmmc_freq_khz, "sdmmc_freq_khz", "SDMMC frequency (kHz)", U32, 400, 20000),
    FIELD(sd, spi_freq_khz, "spi_freq_khz", "SPI frequency (kHz)", U32, 400, 20000),
    FIELD(sd, card_detect_gpio, "card_detect_gpio", "Card-detect GPIO (-1: not connected)", Gpio, -1, 48),
    FIELD(sd, mount_point, "mount_point", "SD mount point", String, 0, 0),
    FIELD(sd, clk_gpio, "clk_gpio", "SDMMC CLK GPIO", Gpio, -1, 48),
    FIELD(sd, cmd_gpio, "cmd_gpio", "SDMMC CMD GPIO", Gpio, -1, 48),
    FIELD(sd, d0_gpio, "d0_gpio", "SDMMC D0 GPIO", Gpio, -1, 48),
    FIELD(sd, d1_gpio, "d1_gpio", "SDMMC D1 GPIO", Gpio, -1, 48),
    FIELD(sd, d2_gpio, "d2_gpio", "SDMMC D2 GPIO", Gpio, -1, 48),
    FIELD(sd, d3_gpio, "d3_gpio", "SDMMC D3 GPIO", Gpio, -1, 48),
    FIELD(sd, spi_miso_gpio, "spi_miso_gpio", "SPI MISO GPIO", Gpio, -1, 48),
    FIELD(sd, spi_mosi_gpio, "spi_mosi_gpio", "SPI MOSI GPIO", Gpio, -1, 48),
    FIELD(sd, spi_cs_gpio, "spi_cs_gpio", "SPI CS GPIO", Gpio, -1, 48),
    FIELD(sd, spi_clk_gpio, "spi_clk_gpio", "SPI CLK GPIO", Gpio, -1, 48),
};
#undef FIELD
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
enum class RestartAction { Restart, ResetConfig, ResetAll };
TaskHandle_t restart_task;
RestartAction restart_action;

esp_err_t text(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

bool authorize_maker(httpd_req_t *req, bool mutation)
{
    if (mutation ? !web_interface_authorize_settings_request(req) :
                   !web_interface_authorize_request(req, false)) return false;
    if (ebc_config_get_mode() != EBC_FW_MAKER) {
        text(req, "403 Forbidden", "Maker settings are unavailable in FactoryMode.");
        return false;
    }
    return true;
}

Json read_json(httpd_req_t *req)
{
    Json json(nullptr, cJSON_Delete);
    if (req->content_len == 0 || req->content_len > MAX_BODY) return json;
    std::unique_ptr<char[]> body(new (std::nothrow) char[req->content_len + 1]);
    if (!body) return json;
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body.get() + received, req->content_len - received);
        if (count <= 0) return json;
        received += static_cast<size_t>(count);
    }
    body[received] = '\0';
    if (std::strlen(body.get()) == received && std::strstr(body.get(), "\\u0000") == nullptr) {
        json.reset(cJSON_ParseWithLengthOpts(body.get(), received + 1, nullptr, true));
    }
    std::memset(body.get(), 0, received);
    return json;
}

double number(const ebc_config_t &cfg, const Field &field)
{
    const auto *value = reinterpret_cast<const unsigned char *>(&cfg) + field.offset;
    switch (field.type) {
    case Type::Gpio: { int8_t n; std::memcpy(&n, value, sizeof(n)); return n; }
    case Type::U16: { uint16_t n; std::memcpy(&n, value, sizeof(n)); return n; }
    case Type::U32: { uint32_t n; std::memcpy(&n, value, sizeof(n)); return n; }
    case Type::Bus: { ebc_sd_bus_t n; std::memcpy(&n, value, sizeof(n)); return n; }
    default: return *value;
    }
}

const char *type_name(Type type)
{
    switch (type) {
    case Type::String: return "text";
    case Type::Password: return "password";
    case Type::Bool: return "checkbox";
    case Type::Bus: return "bus";
    default: return "number";
    }
}

esp_err_t get_handler(httpd_req_t *req)
{
    if (!authorize_maker(req, false)) return ESP_OK;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON *array = root ? cJSON_AddArrayToObject(root.get(), "fields") : nullptr;
    bool ok = array && cJSON_AddBoolToObject(root.get(), "active", ebc_config_is_active()) &&
              cJSON_AddStringToObject(root.get(), "mode", "maker");
    const ebc_config_t *cfg = ebc_config_get();
    for (const Field &field : fields) {
        if (!ok) break;
        cJSON *item = cJSON_CreateObject();
        if (item == nullptr) { ok = false; break; }
        ok = cJSON_AddItemToArray(array, item) &&
             cJSON_AddStringToObject(item, "section", field.section) &&
             cJSON_AddStringToObject(item, "name", field.name) &&
             cJSON_AddStringToObject(item, "label", field.label) &&
             cJSON_AddStringToObject(item, "type", type_name(field.type)) &&
             cJSON_AddNumberToObject(item, "min", field.minimum) &&
             cJSON_AddNumberToObject(item, "max", field.maximum) &&
             cJSON_AddNumberToObject(item, "maxLength", field.size - 1);
        const char *value = reinterpret_cast<const char *>(cfg) + field.offset;
        if (ok && field.type == Type::Password) {
            ok = cJSON_AddBoolToObject(item, "configured", value[0] != '\0') != nullptr;
        } else if (ok && field.type == Type::String) {
            ok = cJSON_AddStringToObject(item, "value", value) != nullptr;
        } else if (ok && field.type == Type::Bool) {
            ok = cJSON_AddBoolToObject(item, "value", number(*cfg, field) != 0) != nullptr;
        } else if (ok) {
            ok = cJSON_AddNumberToObject(item, "value", number(*cfg, field)) != nullptr;
        }
    }
    std::unique_ptr<char, decltype(&cJSON_free)> body(ok ? cJSON_PrintUnformatted(root.get()) : nullptr, cJSON_free);
    if (!body) return text(req, "500 Internal Server Error", "Cannot create configuration response.");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.get());
}

bool parse_field(ebc_config_t &cfg, const Field &field, const cJSON *item)
{
    auto *destination = reinterpret_cast<unsigned char *>(&cfg) + field.offset;
    if (field.type == Type::Password && item == nullptr) return true;
    if (field.type == Type::String || field.type == Type::Password) {
        if (!cJSON_IsString(item) || std::strlen(item->valuestring) >= field.size) return false;
        if (field.type == Type::Password && item->valuestring[0] == '\0') return true;
        std::memset(destination, 0, field.size);
        std::memcpy(destination, item->valuestring, std::strlen(item->valuestring));
        return true;
    }
    if (field.type == Type::Bool) {
        if (!cJSON_IsBool(item)) return false;
        *destination = cJSON_IsTrue(item) ? 1 : 0;
        return true;
    }
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < field.minimum || item->valuedouble > field.maximum) return false;
    switch (field.type) {
    case Type::Gpio: { int8_t n = static_cast<int8_t>(item->valuedouble); std::memcpy(destination, &n, sizeof(n)); break; }
    case Type::U16: { uint16_t n = static_cast<uint16_t>(item->valuedouble); std::memcpy(destination, &n, sizeof(n)); break; }
    case Type::U32: { uint32_t n = static_cast<uint32_t>(item->valuedouble); std::memcpy(destination, &n, sizeof(n)); break; }
    case Type::Bus: { auto n = static_cast<ebc_sd_bus_t>(item->valueint); std::memcpy(destination, &n, sizeof(n)); break; }
    default: return false;
    }
    return true;
}

bool known_fields(const cJSON *root)
{
    for (const cJSON *section = root->child; section; section = section->next) {
        if (std::strcmp(section->string, "active") == 0) continue;
        if (!cJSON_IsObject(section) ||
            (std::strcmp(section->string, "tester") != 0 &&
             std::strcmp(section->string, "ota") != 0 &&
             std::strcmp(section->string, "sd") != 0 &&
             std::strcmp(section->string, "rtc") != 0)) return false;
        for (const cJSON *item = section->child; item; item = item->next) {
            bool known = false;
            for (const Field &field : fields) {
                if (std::strcmp(section->string, field.section) == 0 &&
                    std::strcmp(item->string, field.name) == 0) known = true;
            }
            if (!known) return false;
        }
    }
    return true;
}

void wipe_passwords(cJSON *root)
{
    cJSON *ota = cJSON_GetObjectItemCaseSensitive(root, "ota");
    for (const char *key : {"ap_password", "web_password"}) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(ota, key);
        if (cJSON_IsString(value)) std::memset(value->valuestring, 0, std::strlen(value->valuestring));
    }
}

void restart_worker(void *)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = ESP_OK;
    if (restart_action == RestartAction::ResetConfig) err = ebc_config_factory_reset();
    else if (restart_action == RestartAction::ResetAll) err = ebc_config_full_reset();
    else esp_restart();
    ESP_LOGE(TAG, "Requested restart/reset did not complete: %s", esp_err_to_name(err));
    restart_task = nullptr;
    vTaskDelete(nullptr);
}

bool prepare_restart(httpd_req_t *req, RestartAction action)
{
    if (restart_task != nullptr || test_control_configuration_busy() || ota_manager_is_busy()) {
        text(req, "409 Conflict", "Stop the test and wait for recording to finish before changing device configuration.");
        return false;
    }
    restart_action = action;
    if (xTaskCreate(restart_worker, "config_restart", 6144, nullptr, 3, &restart_task) != pdPASS) {
        restart_task = nullptr;
        ESP_LOGE(TAG, "Cannot create configuration restart task");
        text(req, "500 Internal Server Error", "Cannot prepare restart.");
        return false;
    }
    return true;
}

esp_err_t save_handler(httpd_req_t *req)
{
    if (!authorize_maker(req, true)) return ESP_OK;
    Json json = read_json(req);
    const cJSON *active = cJSON_GetObjectItemCaseSensitive(json.get(), "active");
    if (!cJSON_IsObject(json.get()) || !cJSON_IsBool(active) || !known_fields(json.get())) {
        wipe_passwords(json.get());
        return text(req, "400 Bad Request", "Invalid configuration object.");
    }
    ebc_config_t next = *ebc_config_get();
    for (const Field &field : fields) {
        const cJSON *section = cJSON_GetObjectItemCaseSensitive(json.get(), field.section);
        if (!parse_field(next, field, cJSON_GetObjectItemCaseSensitive(section, field.name))) {
            wipe_passwords(json.get());
            std::memset(&next, 0, sizeof(next));
            char message[128];
            std::snprintf(message, sizeof(message), "Invalid configuration field: %s.", field.name);
            return text(req, "400 Bad Request", message);
        }
    }
    wipe_passwords(json.get());
    bool enabled = cJSON_IsTrue(active);
    if (ebc_config_validate(&next, enabled) != ESP_OK) {
        std::memset(&next, 0, sizeof(next));
        return text(req, "400 Bad Request", "Invalid configuration. Check passwords, GPIO assignments, reserved pins and selected SD bus.");
    }
    if (!prepare_restart(req, RestartAction::Restart)) {
        std::memset(&next, 0, sizeof(next));
        return ESP_OK;
    }
    esp_err_t err = ebc_config_save_with_active(&next, enabled);
    std::memset(&next, 0, sizeof(next));
    if (err != ESP_OK) {
        vTaskDelete(restart_task);
        restart_task = nullptr;
        ESP_LOGE(TAG, "Cannot save Maker configuration: %s", esp_err_to_name(err));
        return text(req, "500 Internal Server Error", "Configuration could not be saved.");
    }
    esp_err_t response = text(req, "200 OK", "Configuration saved. Restarting; reconnect to the access point if necessary.");
    xTaskNotifyGive(restart_task);
    return response;
}

esp_err_t reset_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_settings_request(req)) return ESP_OK;
    Json json = read_json(req);
    const cJSON *scope = cJSON_GetObjectItemCaseSensitive(json.get(), "scope");
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(json.get(), "confirm");
    if (!cJSON_IsObject(json.get()) || !cJSON_IsString(scope) || !cJSON_IsTrue(confirm) ||
        (std::strcmp(scope->valuestring, "config") != 0 && std::strcmp(scope->valuestring, "all") != 0)) {
        return text(req, "400 Bad Request", "Confirm reset and select config or all.");
    }
    RestartAction action = std::strcmp(scope->valuestring, "all") == 0 ?
                           RestartAction::ResetAll : RestartAction::ResetConfig;
    if (!prepare_restart(req, action)) return ESP_OK;
    esp_err_t response = text(req, "200 OK", "Reset requested. Restarting; reconnect to the access point.");
    xTaskNotifyGive(restart_task);
    return response;
}

const httpd_uri_t maker_routes[] = {
    {.uri = "/api/maker/config", .method = HTTP_GET, .handler = get_handler, .user_ctx = nullptr},
    {.uri = "/api/maker/config", .method = HTTP_POST, .handler = save_handler, .user_ctx = nullptr},
};
const httpd_uri_t reset_routes[] = {
    {.uri = "/api/config/reset", .method = HTTP_POST, .handler = reset_handler, .user_ctx = nullptr},
};
web_interface_extension_t maker_extension{};
web_interface_extension_t reset_extension{};
}

extern "C" esp_err_t ebc_config_web_init(void)
{
    if (ebc_config_get_mode() != EBC_FW_MAKER) return ESP_OK;
    maker_extension = {"maker-settings", "Maker Settings", maker_html_start,
        static_cast<size_t>(maker_html_end - maker_html_start), maker_js_start,
        static_cast<size_t>(maker_js_end - maker_js_start), maker_routes,
        sizeof(maker_routes) / sizeof(maker_routes[0]), "tab", nullptr};
    return web_interface_register_extension(&maker_extension);
}

extern "C" esp_err_t ebc_config_web_register_reset(void)
{
    reset_extension = {"config-reset", "Device reset", reset_html_start,
        static_cast<size_t>(reset_html_end - reset_html_start), reset_js_start,
        static_cast<size_t>(reset_js_end - reset_js_start), reset_routes,
        sizeof(reset_routes) / sizeof(reset_routes[0]), "settings", nullptr};
    return web_interface_register_extension(&reset_extension);
}
