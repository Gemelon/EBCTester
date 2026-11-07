/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include "led_settings.h"
#include "led_policy.h"
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include "cJSON.h"
#include "ebc_config.h"
#include "ebc_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "status_led.h"
#include "test_control.h"
#include "web_interface.h"

extern const unsigned char led_html_start[] asm("_binary_led_settings_html_start");
extern const unsigned char led_html_end[] asm("_binary_led_settings_html_end");
extern const unsigned char led_js_start[] asm("_binary_led_settings_js_start");
extern const unsigned char led_js_end[] asm("_binary_led_settings_js_end");

namespace {
constexpr char TAG[] = "led_settings";
constexpr size_t MAX_BODY = 2048;
struct Group { const char *id; const char *label; led_color_t (*color)(const led_input_t *); };
led_color_t off_color(const led_input_t *) { return {}; }
const Group groups[] = {
    {"off", "Off", off_color},
    {"tester", "Tester mode", led_tester_color},
    {"frames", "Message frame status", nullptr},
};
static_assert(sizeof(groups) / sizeof(groups[0]) == LED_GROUP_COUNT);
std::mutex config_mutex;
led_config_t config;
led_frame_state_t frame_state{};

esp_err_t text(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

esp_err_t load_config()
{
    led_config_defaults(&config);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("leds", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    led_config_t stored{};
    size_t length = sizeof(stored);
    err = nvs_get_blob(handle, "config", &stored, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (length != sizeof(stored) || !led_config_valid(&stored)) {
        ESP_LOGE(TAG, "Invalid saved LED configuration; refusing to start LED services");
        return ESP_ERR_INVALID_ARG;
    }
    config = stored;
    return ESP_OK;
}

esp_err_t render_pixels(status_led_pixel_t *pixels, size_t count)
{
    led_config_t snapshot;
    {
        std::lock_guard<std::mutex> guard(config_mutex);
        snapshot = config;
    }
    ebc_link_status_t link{};
    ebc_link_get_status(&link);
    const led_input_t input = {
        .connected = link.connected,
        .automatic = test_control_automatic_operation_running(),
        .manual = link.has_frame && ebc_link_mode_is_active(link.frame.mode),
        .communication_error = link.last_message_invalid || link.model_mismatch ||
                               (link.has_model && !link.model_enabled && !link.model_version_pending),
        .valid_age_ms = link.last_frame_age_ms,
        .invalid_age_ms = link.last_invalid_frame_age_ms,
        .valid_count = link.frames_ok,
    };
    const led_color_t frames = led_frame_color(&frame_state, &input, esp_timer_get_time() / 1000);
    for (size_t i = 0; i < count; ++i) {
        pixels[i] = {};
        if (i >= snapshot.count) continue;
        const led_assignment_t assignment = snapshot.leds[i];
        const led_color_t color = assignment.group == LED_GROUP_FRAMES ? frames :
                                  groups[assignment.group].color(&input);
        pixels[i] = {color.red, color.green, color.blue, assignment.ota != 0};
    }
    return ESP_OK;
}

esp_err_t get_settings(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) return ESP_OK;
    led_config_t snapshot;
    {
        std::lock_guard<std::mutex> guard(config_mutex);
        snapshot = config;
    }
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return text(req, "500 Internal Server Error", "Could not allocate LED settings.");
    cJSON_AddNumberToObject(root.get(), "count", snapshot.count);
    cJSON_AddNumberToObject(root.get(), "maxCount", EBC_LED_MAX);
    cJSON_AddBoolToObject(root.get(), "hardwareEnabled",
                         ebc_config_is_active() && ebc_config_get()->ota.status_led_enabled);
    cJSON *catalog = cJSON_AddArrayToObject(root.get(), "groups");
    cJSON *leds = cJSON_AddArrayToObject(root.get(), "leds");
    if (!catalog || !leds) return text(req, "500 Internal Server Error", "Could not allocate LED settings.");
    for (const Group &group : groups) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) return text(req, "500 Internal Server Error", "Could not allocate LED group.");
        cJSON_AddStringToObject(entry, "id", group.id);
        cJSON_AddStringToObject(entry, "label", group.label);
        cJSON_AddItemToArray(catalog, entry);
    }
    for (unsigned i = 0; i < EBC_LED_MAX; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) return text(req, "500 Internal Server Error", "Could not allocate LED assignment.");
        cJSON_AddStringToObject(entry, "group", groups[snapshot.leds[i].group].id);
        cJSON_AddBoolToObject(entry, "ota", snapshot.leds[i].ota);
        cJSON_AddItemToArray(leds, entry);
    }
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!body) return text(req, "500 Internal Server Error", "Could not encode LED settings.");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.get());
}

esp_err_t save_settings(httpd_req_t *req)
{
    if (!web_interface_authorize_settings_request(req)) return ESP_OK;
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return text(req, "400 Bad Request", "Invalid LED settings body.");
    }
    std::unique_ptr<char[]> body(new (std::nothrow) char[req->content_len + 1]);
    if (!body) return text(req, "500 Internal Server Error", "Could not allocate LED settings.");
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body.get() + received, req->content_len - received);
        if (count <= 0) return text(req, "400 Bad Request", "Incomplete LED settings request.");
        received += static_cast<size_t>(count);
    }
    body[received] = '\0';
    if (std::strlen(body.get()) != received || std::strstr(body.get(), "\\u0000")) {
        return text(req, "400 Bad Request", "Invalid LED settings JSON.");
    }
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_ParseWithLengthOpts(body.get(), received + 1, nullptr, true), cJSON_Delete);
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(root.get(), "count");
    const cJSON *leds = cJSON_GetObjectItemCaseSensitive(root.get(), "leds");
    if (!cJSON_IsObject(root.get()) || !cJSON_IsNumber(count) || !std::isfinite(count->valuedouble) ||
        std::floor(count->valuedouble) != count->valuedouble ||
        count->valuedouble < 1 || count->valuedouble > EBC_LED_MAX || !cJSON_IsArray(leds) ||
        cJSON_GetArraySize(leds) != EBC_LED_MAX) {
        return text(req, "400 Bad Request", "Invalid LED count or assignments.");
    }
    led_config_t next{};
    next.version = EBC_LED_CONFIG_VERSION;
    next.count = static_cast<uint8_t>(count->valuedouble);
    for (unsigned i = 0; i < EBC_LED_MAX; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(leds, i);
        const cJSON *group = cJSON_GetObjectItemCaseSensitive(entry, "group");
        const cJSON *ota = cJSON_GetObjectItemCaseSensitive(entry, "ota");
        if (!cJSON_IsObject(entry) || !cJSON_IsString(group) || !cJSON_IsBool(ota)) {
            return text(req, "400 Bad Request", "Invalid LED function group.");
        }
        unsigned selected = LED_GROUP_COUNT;
        for (unsigned g = 0; g < LED_GROUP_COUNT; ++g) {
            if (std::strcmp(group->valuestring, groups[g].id) == 0) selected = g;
        }
        if (selected == LED_GROUP_COUNT) return text(req, "400 Bad Request", "Unknown LED function group.");
        next.leds[i] = {static_cast<uint8_t>(selected), static_cast<uint8_t>(cJSON_IsTrue(ota))};
    }
    std::lock_guard<std::mutex> guard(config_mutex);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("leds", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "config", &next, sizeof(next));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save LED assignments: %s", esp_err_to_name(err));
        return text(req, "500 Internal Server Error", "Could not save LED settings.");
    }
    config = next;
    return text(req, "200 OK", "LED settings saved and applied.");
}

const httpd_uri_t routes[] = {
    {.uri = "/api/leds/settings", .method = HTTP_GET, .handler = get_settings, .user_ctx = nullptr},
    {.uri = "/api/leds/settings", .method = HTTP_POST, .handler = save_settings, .user_ctx = nullptr},
};
}

extern "C" esp_err_t led_settings_init(void)
{
    esp_err_t err = load_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load LED settings: %s", esp_err_to_name(err));
        return err;
    }
    err = status_led_set_renderer(EBC_LED_MAX, render_pixels);
    if (err != ESP_OK) return err;
    static const web_interface_extension_t extension = {
        .id = "led-settings", .label = "LEDs",
        .content = led_html_start, .content_size = static_cast<size_t>(led_html_end - led_html_start),
        .script = led_js_start, .script_size = static_cast<size_t>(led_js_end - led_js_start),
        .routes = routes, .route_count = sizeof(routes) / sizeof(routes[0]), .placement = "settings",
        .visible = nullptr,
    };
    return web_interface_register_extension(&extension);
}
