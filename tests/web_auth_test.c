/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Only ESP transport, clock, randomness and NVS are mocked; auth and JSON parsing are real. */
#define asm(symbol)
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define HTTP_GET 0
#define HTTP_POST 1
#define NVS_READONLY 0
#define NVS_READWRITE 1

typedef int esp_err_t;
typedef int nvs_handle_t;
typedef void *httpd_handle_t;
typedef struct {
    int method;
    const char *uri;
    size_t content_len;
    const char *body;
    size_t received;
    const char *cookies;
    const char *csrf;
    const char *password;
    const char *marker;
    char status[64];
    char response[256];
    char cookie[160];
    char location[32];
} httpd_req_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;

static int64_t clock_us = 5000000;
static unsigned random_sequence;
static uint8_t stored_days;
static uint8_t pending_days;
static bool storage_failure;
static httpd_uri_t registered[5];
static size_t registered_count;

static int64_t esp_timer_get_time(void) { return clock_us; }
static const char *network_manager_admin_password(void) { return "test-admin-password"; }
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock storage error"; }
static void esp_fill_random(void *data, size_t size)
{
    unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) bytes[i] = (unsigned char)++random_sequence;
}
static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "web_auth") == 0);
    *handle = 1;
    return mode == NVS_READONLY && stored_days == 0 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
static esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    assert(strcmp(key, "session_days") == 0);
    *value = stored_days;
    return ESP_OK;
}
static esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    assert(strcmp(key, "session_days") == 0);
    pending_days = value;
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (storage_failure) return ESP_FAIL;
    stored_days = pending_days;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { (void)handle; }
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *value, size_t size)
{
    const char *source = NULL;
    if (strcmp(name, "Cookie") == 0) source = req->cookies;
    if (strcmp(name, "X-CSRF-Token") == 0) source = req->csrf;
    if (strcmp(name, "X-Admin-Password") == 0) source = req->password;
    if (strcmp(name, "X-Web-Login") == 0) source = req->marker;
    if (source == NULL || strlen(source) >= size) return ESP_FAIL;
    strcpy(value, source);
    return ESP_OK;
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value)
{
    if (strcmp(name, "Set-Cookie") == 0) snprintf(req->cookie, sizeof(req->cookie), "%s", value);
    if (strcmp(name, "Location") == 0) snprintf(req->location, sizeof(req->location), "%s", value);
    assert(strcmp(name, "WWW-Authenticate") != 0);
    return ESP_OK;
}
static esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    snprintf(req->status, sizeof(req->status), "%s", status);
    return ESP_OK;
}
static esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req; (void)type;
    return ESP_OK;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *message)
{
    snprintf(req->response, sizeof(req->response), "%s", message);
    return ESP_OK;
}
static esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, size_t length)
{
    (void)req; (void)data; (void)length;
    return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *req, char *body, size_t size)
{
    if (size > 7) size = 7;
    memcpy(body, req->body + req->received, size);
    req->received += size;
    return (int)size;
}
static esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    (void)server;
    assert(registered_count < 5);
    registered[registered_count++] = *route;
    return ESP_OK;
}

#include "../external/ota-suite/components/web_interface/web_auth.c"

const unsigned char login_start[] = "";
const unsigned char login_end[] = "";
const unsigned char login_script_start[] = "";
const unsigned char login_script_end[] = "";

static httpd_req_t request(const char *uri, const char *body, const char *cookie)
{
    httpd_req_t req = {0};
    req.method = body == NULL ? HTTP_GET : HTTP_POST;
    req.uri = uri;
    req.body = body;
    req.content_len = body == NULL ? 0 : strlen(body);
    req.cookies = cookie;
    req.csrf = web_auth_csrf_token();
    req.marker = "1";
    strcpy(req.status, "200 OK");
    return req;
}

static httpd_req_t login(const char *body)
{
    httpd_req_t req = request("/api/auth/login", body, NULL);
    assert(login_handler(&req) == ESP_OK);
    return req;
}

int main(void)
{
    assert(web_auth_init() == ESP_OK);
    assert(web_auth_session_days() == 1);
    assert(web_auth_register(NULL) == ESP_OK);
    assert(registered_count == 5);
    assert(strlen(web_auth_csrf_token()) == 32);
#ifdef CONFIG_OTA_SUITE_WEB_SETTINGS_REQUIRE_PASSWORD
    assert(web_auth_settings_password_required());
#else
    assert(!web_auth_settings_password_required());
#endif
    httpd_req_t req = request("/", NULL, NULL);
    assert(!web_interface_authorize_request(&req, false));
    assert(strcmp(req.status, "303 See Other") == 0);
    assert(strcmp(req.location, "/login") == 0);
    req = request("/api/status", NULL, NULL);
    assert(!web_interface_authorize_request(&req, false));
    assert(strcmp(req.status, "401 Unauthorized") == 0);

    req = login("{\"username\":\"admin\",\"password\":\"wrong\"}");
    assert(strcmp(req.status, "403 Forbidden") == 0);
    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\"}");
    assert(strcmp(req.status, "403 Forbidden") == 0);
    clock_us += 1000000;
    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\"}");
    assert(strcmp(req.status, "200 OK") == 0);
    assert(strstr(req.cookie, "Max-Age=86400; HttpOnly; SameSite=Lax") != NULL);
    char cookie[160];
    strcpy(cookie, req.cookie);

    req = request("/api/status", NULL, cookie);
    assert(web_interface_authorize_request(&req, false));
    req = request("/api/tester/stop", "", cookie);
    req.csrf = "wrong";
    assert(!web_interface_authorize_request(&req, true));
    assert(strcmp(req.status, "403 Forbidden") == 0);
    req.csrf = web_auth_csrf_token();
    assert(web_interface_authorize_request(&req, true));

    req = request("/api/auth/settings", "{\"sessionDays\":7}", cookie);
#ifdef CONFIG_OTA_SUITE_WEB_SETTINGS_REQUIRE_PASSWORD
    assert(!web_interface_authorize_settings_request(&req));
    req.password = "incorrect";
    assert(!web_interface_authorize_settings_request(&req));
    clock_us += 1000000;
#else
    assert(web_interface_authorize_settings_request(&req));
#endif
    req.password = "test-admin-password";
    strcpy(req.status, "200 OK");
    assert(settings_handler(&req) == ESP_OK);
    assert(strcmp(req.status, "200 OK") == 0);
    assert(web_auth_session_days() == 7 && stored_days == 7);
    req = request("/api/auth/settings", "{\"sessionDays\":1}", cookie);
    req.csrf = "invalid";
    req.password = "test-admin-password";
    settings_handler(&req);
    assert(strcmp(req.status, "403 Forbidden") == 0);
    assert(web_auth_session_days() == 7);

    const char *invalid[] = {"{}", "{\"sessionDays\":0}", "{\"sessionDays\":2}",
        "{\"sessionDays\":7.5}", "{\"sessionDays\":\"7\"}", "{\"sessionDays\":null}",
        "{\"sessionDays\":7}garbage", "{\"sessionDays\":7,\"x\":\"\\u0000\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        req = request("/api/auth/settings", invalid[i], cookie);
        req.password = "test-admin-password";
        settings_handler(&req);
        assert(strcmp(req.status, "400 Bad Request") == 0);
        assert(web_auth_session_days() == 7);
    }
    storage_failure = true;
    req = request("/api/auth/settings", "{\"sessionDays\":1}", cookie);
    req.password = "test-admin-password";
    settings_handler(&req);
    assert(strcmp(req.status, "500 Internal Server Error") == 0);
    assert(web_auth_session_days() == 7 && stored_days == 7);
    storage_failure = false;

    const int64_t weekly_login_at = clock_us;
    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\"}");
    assert(strstr(req.cookie, "Max-Age=604800; HttpOnly; SameSite=Lax") != NULL);
    char weekly_cookie[160];
    strcpy(weekly_cookie, req.cookie);
    clock_us = 6000000 + INT64_C(86400) * 1000000 - 1;
    req = request("/api/status", NULL, cookie);
    assert(web_interface_authorize_request(&req, false));
    clock_us++;
    assert(!web_interface_authorize_request(&req, false));
    req = request("/api/status", NULL, weekly_cookie);
    assert(web_interface_authorize_request(&req, false));
    clock_us = weekly_login_at + INT64_C(604800) * 1000000 - 1;
    assert(web_interface_authorize_request(&req, false));
    clock_us++;
    assert(!web_interface_authorize_request(&req, false));

    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\"}");
    strcpy(cookie, req.cookie);
    req = request("/api/auth/logout", "", cookie);
    logout_handler(&req);
    assert(strstr(req.cookie, "Max-Age=0") != NULL);
    req = request("/api/status", NULL, cookie);
    assert(!web_interface_authorize_request(&req, false));

    req = request("/api/auth/login", "{\"username\":\"admin\",\"password\":\"test-admin-password\"}", NULL);
    req.marker = NULL;
    login_handler(&req);
    assert(strcmp(req.status, "403 Forbidden") == 0);
    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\\u0000other\"}");
    assert(strcmp(req.status, "403 Forbidden") == 0);
    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\"}junk");
    assert(strcmp(req.status, "403 Forbidden") == 0);

    req = login("{\"username\":\"admin\",\"password\":\"test-admin-password\"}");
    strcpy(cookie, req.cookie);
    assert(web_auth_init() == ESP_OK);
    assert(web_auth_session_days() == 7);
    req = request("/api/status", NULL, cookie);
    assert(!web_interface_authorize_request(&req, false));

    web_session_t table[WEB_SESSION_COUNT] = {0};
    const char *token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(web_session_create(table, token, 100, 2) == NULL);
    assert(web_session_create(table, "short", 100, 1) == NULL);
    for (size_t i = 0; i < WEB_SESSION_COUNT; ++i) assert(web_session_create(table, token, 100, 1));
    assert(web_session_create(table, token, 100, 1) == NULL);
    assert(web_session_find(table, "other=x; ota_session=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 101));
    assert(!web_session_find(table, "ota_session=short", 101));
    assert(!web_session_find(table, "other_ota_session=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 101));
    assert(!web_session_find(table, "ota_session=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef; ota_session=short", 101));
    assert(!web_session_find(table, "ota_session=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 86500));
    assert(web_session_create(table, token, 86500, 7));
    stored_days = 3;
    assert(web_auth_init() == ESP_ERR_INVALID_STATE);
    puts("Authentication tests passed: daily/weekly expiry, cookies, CSRF, password guards, NVS, logout, restart.");
    return 0;
}
