/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ebc_config.h"

#define asm(symbol)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define HTTP_GET 0
#define HTTP_POST 1
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
using TaskHandle_t = void *;
struct httpd_req_t {
    int method = HTTP_GET;
    const char *uri = "/api/maker/config";
    size_t content_len = 0;
    std::string body;
    size_t received = 0;
    std::string status = "200 OK";
    std::string response;
};
struct httpd_uri_t {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
};
using httpd_handle_t = void *;
#include "../external/ota-suite/components/web_interface/include/web_interface.h"

static ebc_config_t mock_config{};
static ebc_fw_mode_t mock_mode = EBC_FW_MAKER;
static bool active, authorized = true, busy, fail_save, fail_task;
static unsigned saves, notifications, resets, full_resets, restarts;
static std::vector<std::string> registered;
extern "C" const ebc_config_t *ebc_config_get(void) { return &mock_config; }
extern "C" ebc_fw_mode_t ebc_config_get_mode(void) { return mock_mode; }
extern "C" bool ebc_config_is_active(void) { return active; }
extern "C" esp_err_t ebc_config_validate(const ebc_config_t *cfg, bool)
{
    return cfg->tester.tester_uart.tx_gpio == 22 ? ESP_ERR_INVALID_ARG : ESP_OK;
}
extern "C" esp_err_t ebc_config_save_with_active(const ebc_config_t *cfg, bool enabled)
{
    saves++;
    if (fail_save) return ESP_FAIL;
    mock_config = *cfg;
    active = enabled;
    return ESP_OK;
}
extern "C" esp_err_t ebc_config_factory_reset(void) { resets++; return ESP_OK; }
extern "C" esp_err_t ebc_config_full_reset(void) { full_resets++; return ESP_OK; }
extern "C" bool test_control_configuration_busy(void) { return busy; }
static bool ota_manager_is_busy(void) { return false; }
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
static int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *out)
{
    if (fail_task) return 0;
    *out = reinterpret_cast<void *>(1);
    return pdPASS;
}
static void vTaskDelete(TaskHandle_t) {}
static unsigned ulTaskNotifyTake(int, int) { return 1; }
static void xTaskNotifyGive(TaskHandle_t) { notifications++; }
static void vTaskDelay(int) {}
static void esp_restart(void) { restarts++; }
static const char *esp_err_to_name(esp_err_t) { return "mock"; }
esp_err_t web_interface_register_extension(const web_interface_extension_t *extension)
{
    registered.emplace_back(extension->id);
    return ESP_OK;
}

#include "../main/ebc_config_web.cpp"

const unsigned char maker_html_start[] = "";
const unsigned char maker_html_end[] = "";
const unsigned char maker_js_start[] = "";
const unsigned char maker_js_end[] = "";
const unsigned char reset_html_start[] = "";
const unsigned char reset_html_end[] = "";
const unsigned char reset_js_start[] = "";
const unsigned char reset_js_end[] = "";

static std::string request_body()
{
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(root.get(), "active", true);
    for (const char *section : {"tester", "ota", "sd"}) cJSON_AddObjectToObject(root.get(), section);
    for (const Field &field : fields) {
        cJSON *section = cJSON_GetObjectItemCaseSensitive(root.get(), field.section);
        if (field.type == Type::String || field.type == Type::Password) {
            const char *value = field.type == Type::Password ? "" :
                                reinterpret_cast<const char *>(&mock_config) + field.offset;
            cJSON_AddStringToObject(section, field.name, value);
        } else if (field.type == Type::Bool) {
            cJSON_AddBoolToObject(section, field.name, number(mock_config, field) != 0);
        } else {
            cJSON_AddNumberToObject(section, field.name, number(mock_config, field));
        }
    }
    char *text = cJSON_PrintUnformatted(root.get());
    std::string result(text);
    cJSON_free(text);
    return result;
}
static httpd_req_t request(const std::string &body)
{
    httpd_req_t req;
    req.method = HTTP_POST;
    req.body = body;
    req.content_len = body.size();
    return req;
}

int main()
{
    std::strcpy(mock_config.ota.ap_password, "private-ap-password");
    std::strcpy(mock_config.ota.web_password, "private-web-password");
    std::strcpy(mock_config.ota.sntp_server, "pool.ntp.org");
    std::strcpy(mock_config.tester.device_id, "fixture");
    std::strcpy(mock_config.sd.mount_point, "/sdcard");
    mock_config.sd.sdmmc_freq_khz = mock_config.sd.spi_freq_khz = 10000;
    mock_config.tester.tester_uart.baud = mock_config.tester.dongle_uart.baud = 9600;
    mock_config.tester.averaging_length = 20;
    assert(ebc_config_web_init() == ESP_OK && registered.size() == 1 &&
           registered[0] == "maker-settings");
    assert(ebc_config_web_register_reset() == ESP_OK && registered.size() == 2 &&
           registered[1] == "config-reset");
    registered.clear();
    mock_mode = EBC_FW_FACTORY;
    assert(ebc_config_web_init() == ESP_OK && registered.empty());
    assert(ebc_config_web_register_reset() == ESP_OK && registered.size() == 1 &&
           registered[0] == "config-reset");
    httpd_req_t req;
    get_handler(&req);
    assert(req.status == "403 Forbidden");
    mock_mode = EBC_FW_MAKER;
    req = {};
    get_handler(&req);
    assert(req.status == "200 OK" && req.response.find("private-") == std::string::npos);
    assert(req.response.find("\"configured\":true") != std::string::npos);
    const std::string good = request_body();
    req = request(good);
    save_handler(&req);
    assert(req.status == "200 OK" && saves == 1 && notifications == 1 && active);
    assert(std::strcmp(mock_config.ota.web_password, "private-web-password") == 0);
    restart_task = nullptr;
    for (const std::string bad : {"{}", "{\"active\":true,\"unknown\":{}}",
                                 "{\"active\":true,\"tester\":{\"tester_tx_gpio\":1.5}}",
                                 "{\"active\":true,\"tester\":{\"tester_tx_gpio\":\"1\"}}"}) {
        req = request(bad);
        save_handler(&req);
        assert(req.status == "400 Bad Request" && saves == 1);
    }
    busy = true;
    req = request(good);
    save_handler(&req);
    assert(req.status == "409 Conflict" && saves == 1);
    busy = false;
    fail_save = true;
    req = request(good);
    save_handler(&req);
    assert(req.status == "500 Internal Server Error" && restart_task == nullptr && notifications == 1);
    fail_save = false;
    fail_task = true;
    req = request(good);
    save_handler(&req);
    assert(req.status == "500 Internal Server Error" && saves == 2);
    fail_task = false;
    authorized = false;
    req = request(good);
    save_handler(&req);
    assert(saves == 2);
    authorized = true;
    req = request("{\"scope\":\"all\",\"confirm\":false}");
    reset_handler(&req);
    assert(req.status == "400 Bad Request" && notifications == 1);
    req = request("{\"scope\":\"all\",\"confirm\":true}");
    reset_handler(&req);
    assert(req.status == "200 OK" && restart_action == RestartAction::ResetAll && notifications == 2);
    restart_worker(nullptr);
    assert(full_resets == 1 && resets == 0);
    req = request("{\"scope\":\"config\",\"confirm\":true}");
    reset_handler(&req);
    restart_worker(nullptr);
    assert(resets == 1);
    puts("Maker backend tests passed: mode gating, redaction, typed input, persistence failures, busy guard and resets.");
}
