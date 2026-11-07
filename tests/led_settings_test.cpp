/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "ebc_config.h"
#include "ebc_link.h"
#include "status_led.h"
#include "led_policy.h"

#define asm(symbol)
#define ESP_LOGE(...) ((void)0)
#define HTTP_GET 0
#define HTTP_POST 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 4
using nvs_handle_t = int;
struct httpd_req_t {
    size_t content_len = 0;
    std::string body, response, status = "200 OK";
    size_t received = 0;
};
struct httpd_uri_t {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
};
#include "../external/ota-suite/components/web_interface/include/web_interface.h"

static const web_interface_extension_t *registered_extension;
static bool authorized = true, fail_commit;
static unsigned saves;
static std::vector<unsigned char> stored, staged;
static ebc_config_t mock_config{};
static ebc_link_status_t mock_link{};
static bool automatic;
static uint64_t now_ms = 1000;
extern "C" const ebc_config_t *ebc_config_get(void) { return &mock_config; }
extern "C" bool ebc_config_is_active(void) { return true; }
extern "C" bool test_control_automatic_operation_running(void) { return automatic; }
extern "C" void ebc_link_get_status(ebc_link_status_t *out) { *out = mock_link; }
extern "C" bool ebc_link_mode_is_active(uint8_t mode) { return mode >= 0x0A && mode <= 0x0C; }
static int64_t esp_timer_get_time() { return now_ms * 1000; }
static const char *esp_err_to_name(esp_err_t) { return "mock"; }
static esp_err_t nvs_open(const char *ns, int, nvs_handle_t *out)
{ assert(std::strcmp(ns, "leds") == 0); *out = 1; return ESP_OK; }
static void nvs_close(nvs_handle_t) {}
static esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *out, size_t *size)
{
    if (stored.empty()) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= stored.size());
    std::memcpy(out, stored.data(), stored.size());
    *size = stored.size();
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *value, size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(value);
    staged.assign(bytes, bytes + size);
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t)
{
    if (fail_commit) return ESP_FAIL;
    stored = staged;
    saves++;
    return ESP_OK;
}
bool web_interface_authorize_request(httpd_req_t *, bool) { return authorized; }
bool web_interface_authorize_settings_request(httpd_req_t *) { return authorized; }
static esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return ESP_OK; }
static esp_err_t httpd_resp_set_type(httpd_req_t *, const char *) { return ESP_OK; }
static esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status) { req->status = status; return ESP_OK; }
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *body) { req->response = body; return ESP_OK; }
static int httpd_req_recv(httpd_req_t *req, char *out, size_t size)
{
    if (size > 11) size = 11;
    std::memcpy(out, req->body.data() + req->received, size);
    req->received += size;
    return static_cast<int>(size);
}
esp_err_t web_interface_register_extension(const web_interface_extension_t *extension)
{
    if (!extension || !extension->id || !extension->id[0] || std::strlen(extension->id) > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(extension->id); *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    registered_extension = extension;
    assert(std::strcmp(extension->placement, "settings") == 0);
    assert(extension->visible == nullptr && extension->route_count == 2);
    return ESP_OK;
}
extern "C" esp_err_t status_led_set_renderer(size_t count, status_led_renderer_t renderer)
{ assert(count == 16 && renderer); return ESP_OK; }

#include "../main/led_settings.cpp"
const unsigned char led_html_start[] = "", led_html_end[] = "";
const unsigned char led_js_start[] = "", led_js_end[] = "";

static httpd_req_t request(std::string body)
{
    httpd_req_t req;
    req.body = std::move(body);
    req.content_len = req.body.size();
    return req;
}
static std::string body(unsigned count = 4)
{
    std::string out = "{\"count\":" + std::to_string(count) + ",\"leds\":[";
    for (unsigned i = 0; i < 16; ++i) {
        if (i) out += ",";
        out += i % 2 ? "{\"group\":\"frames\",\"ota\":false}" : "{\"group\":\"tester\",\"ota\":true}";
    }
    return out + "]}";
}
int main()
{
    assert(led_settings_init() == ESP_OK);
    assert(config.count == 2);
    assert(config.leds[0].group == LED_GROUP_TESTER && config.leds[0].ota);
    httpd_req_t get;
    assert(registered_extension != nullptr);
    assert(std::strcmp(registered_extension->id, "led-settings") == 0);
    assert(std::strcmp(registered_extension->placement, "settings") == 0);
    assert(registered_extension->route_count == 2);
    assert(std::strcmp(registered_extension->routes[0].uri, "/api/leds/settings") == 0);
    assert(registered_extension->routes[0].handler(&get) == ESP_OK);
    assert(get.response.find("\"maxCount\":16") != std::string::npos);
    assert(get.response.find("Message frame status") != std::string::npos);
    for (const std::string &invalid : {body(0), body(17), std::string("{}"),
         std::string("{\"count\":2,\"leds\":[]}"), std::string("null")}) {
        auto req = request(invalid);
        save_settings(&req);
        assert(req.status == "400 Bad Request" && saves == 0);
    }
    for (const std::string &bad_group : {"unknown", "<script>"}) {
        auto invalid = body();
        invalid.replace(invalid.find("tester"), 6, bad_group);
        auto req = request(invalid);
        save_settings(&req);
        assert(req.status == "400 Bad Request" && saves == 0);
    }
    auto fractional = body();
    fractional.replace(fractional.find(":4"), 2, ":4.5");
    auto req = request(fractional);
    save_settings(&req);
    assert(req.status == "400 Bad Request" && saves == 0);
    req = request(body());
    authorized = false;
    save_settings(&req);
    assert(saves == 0 && config.count == 2);
    authorized = true;
    fail_commit = true;
    req = request(body());
    save_settings(&req);
    assert(req.status == "500 Internal Server Error" && config.count == 2);
    fail_commit = false;
    req = request(body(16));
    save_settings(&req);
    assert(req.status == "200 OK" && config.count == 16 && saves == 1);
    assert(load_config() == ESP_OK && config.count == 16);
    status_led_pixel_t pixels[16]{};
    mock_link.connected = mock_link.has_frame = true;
    mock_link.last_frame_age_ms = 0;
    mock_link.last_invalid_frame_age_ms = UINT32_MAX;
    mock_link.frame.mode = 0x0A;
    assert(render_pixels(pixels, 16) == ESP_OK);
    assert(pixels[0].blue == 24 && pixels[0].ota_override);
    assert(pixels[1].green == 3 && !pixels[1].ota_override);
    mock_link.last_message_invalid = true;
    mock_link.last_invalid_frame_age_ms = 0;
    render_pixels(pixels, 16);
    assert(pixels[1].red == 24 && pixels[1].green == 8);
    mock_link.last_message_invalid = false;
    mock_link.last_invalid_frame_age_ms = 100;
    automatic = true;
    render_pixels(pixels, 16);
    assert(pixels[0].red == 24 && pixels[0].blue == 0);
    req = request(body(1));
    save_settings(&req);
    render_pixels(pixels, 16);
    for (unsigned i = 1; i < 16; ++i) {
        assert(pixels[i].red == 0 && pixels[i].green == 0 && pixels[i].blue == 0 && !pixels[i].ota_override);
    }
    assert(config.leds[14].ota == 1);
    stored[0] = 99;
    assert(load_config() == ESP_ERR_INVALID_ARG);
    puts("LED settings tests passed: settings registration, defaults, count/groups, auth, persistence, failures and live rendering.");
}
