/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "sd_card_manager.h"
#include "ui_localization.h"
#include "web_interface.h"

extern const unsigned char html_start[] asm("_binary_languages_html_start");
extern const unsigned char html_end[] asm("_binary_languages_html_end");
extern const unsigned char js_start[] asm("_binary_languages_js_start");
extern const unsigned char js_end[] asm("_binary_languages_js_end");

namespace {
constexpr size_t MAX_FILE = 128 * 1024;
constexpr size_t MAX_LANGUAGES = 16;
constexpr const char *DIRECTORY = CONFIG_SD_CARD_MANAGER_MOUNT_POINT "/languages";
constexpr const char *TAG = "ui_languages";
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool valid_code(const std::string &code)
{
    if (code.empty() || code.size() > 24 || code.front() < 'a' || code.front() > 'z') return false;
    for (char c : code) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return code.back() != '-';
}

esp_err_t text(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(req, message);
}

esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    if (!body) return text(req, "500 Internal Server Error", "Not enough memory for language response.");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t result = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return result;
}

bool read_language(const std::string &code, std::string &body, Json &json, std::string &error)
{
    const std::string file = std::string(DIRECTORY) + "/" + code + ".json";
    struct stat info = {};
    if (stat(file.c_str(), &info) != 0) { error = "Cannot inspect language file."; return false; }
    if (!S_ISREG(info.st_mode) || info.st_size < 2 || info.st_size > MAX_FILE) {
        error = "Language file must be a regular JSON file of at most 128 KiB."; return false;
    }
    FILE *stream = fopen(file.c_str(), "rb");
    if (!stream) { error = "Cannot open language file."; return false; }
    body.resize(static_cast<size_t>(info.st_size));
    const size_t count = fread(body.data(), 1, body.size(), stream);
    const bool failed = ferror(stream) != 0;
    const int closed = fclose(stream);
    if (failed || count != body.size() || closed != 0) { error = "Cannot read language file."; return false; }
    if (body.find('\0') != std::string::npos) { error = "Null bytes are not allowed in language files."; return false; }
    bool quoted = false, escaped = false;
    int depth = 0;
    for (char c : body) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 4) { error = "Language JSON nesting is too deep."; return false; }
        } else if (c == '}' || c == ']') --depth;
    }
    const char *end = nullptr;
    json.reset(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, &end, true));
    if (!json || !cJSON_IsObject(json.get())) { error = "Invalid language JSON."; return false; }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(json.get(), "version");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json.get(), "name");
    const cJSON *strings = cJSON_GetObjectItemCaseSensitive(json.get(), "strings");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !cJSON_IsString(id) || code != id->valuestring ||
        !cJSON_IsString(name) || strlen(name->valuestring) < 1 || strlen(name->valuestring) > 80 ||
        !cJSON_IsObject(strings)) {
        error = "Expected version 1, filename-matching code, name and strings object."; return false;
    }
    size_t entries = 0;
    for (const cJSON *item = strings->child; item; item = item->next) {
        if (++entries > 1024 || !item->string || strlen(item->string) > 80 ||
            !cJSON_IsString(item) || strlen(item->valuestring) > 4096) {
            error = "Language strings must be text (up to 1024 entries, 4096 bytes each)."; return false;
        }
    }
    return true;
}

struct StorageLock {
    bool held = false;
    void release() { if (held) { sd_card_manager_storage_release(); held = false; } }
    ~StorageLock() { release(); }
};

esp_err_t list(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) return ESP_OK;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return text(req, "500 Internal Server Error", "Not enough memory for languages.");
    cJSON *languages = cJSON_AddArrayToObject(root.get(), "languages");
    cJSON *errors = cJSON_AddArrayToObject(root.get(), "errors");
    if (!languages || !errors) return text(req, "500 Internal Server Error", "Not enough memory for languages.");
    sd_card_manager_status_t status = {};
    sd_card_manager_get_status(&status);
    if (!cJSON_AddBoolToObject(root.get(), "sdAvailable", status.mounted))
        return text(req, "500 Internal Server Error", "Not enough memory for languages.");
    if (!status.mounted) return send_json(req, root.get());
    StorageLock lock;
    lock.held = sd_card_manager_storage_acquire(pdMS_TO_TICKS(1000)) == ESP_OK;
    if (!lock.held) return text(req, "503 Service Unavailable", "SD card is busy. Cannot read languages.");
    DIR *dir = opendir(DIRECTORY);
    if (!dir) {
        if (errno == ENOENT) { lock.release(); return send_json(req, root.get()); }
        ESP_LOGE(TAG, "Cannot open language directory: errno=%d", errno);
        return text(req, "500 Internal Server Error", "Cannot open language directory.");
    }
    size_t count = 0;
    bool memory_ok = true;
    errno = 0;
    while (const dirent *entry = readdir(dir)) {
        const std::string filename = entry->d_name;
        if (filename.size() < 6 || filename.substr(filename.size() - 5) != ".json") continue;
        const std::string code = filename.substr(0, filename.size() - 5);
        std::string body, error;
        Json language(nullptr, cJSON_Delete);
        if (++count > MAX_LANGUAGES) {
            memory_ok = cJSON_AddItemToArray(errors, cJSON_CreateString("At most 16 SD language files are supported."));
            break;
        }
        if (!valid_code(code)) error = "Invalid language filename.";
        else if (!read_language(code, body, language, error)) ESP_LOGW(TAG, "%s: %s", filename.c_str(), error.c_str());
        if (!error.empty()) {
            const std::string message = filename + ": " + error;
            memory_ok = cJSON_AddItemToArray(errors, cJSON_CreateString(message.c_str()));
        } else {
            cJSON *item = cJSON_CreateObject();
            if (!item) { memory_ok = false; break; }
            memory_ok = cJSON_AddItemToArray(languages, item) &&
                cJSON_AddStringToObject(item, "code", code.c_str()) &&
                cJSON_AddStringToObject(item, "name", cJSON_GetObjectItemCaseSensitive(language.get(), "name")->valuestring);
        }
        if (!memory_ok) break;
        errno = 0;
    }
    const int directory_error = errno;
    const int close_result = closedir(dir);
    lock.release();
    if (!memory_ok) return text(req, "500 Internal Server Error", "Not enough memory for languages.");
    if (directory_error || close_result != 0)
        return text(req, "500 Internal Server Error", "Cannot finish reading language directory.");
    return send_json(req, root.get());
}

esp_err_t load(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) return ESP_OK;
    char query[64], code[25];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "code", code, sizeof(code)) != ESP_OK || !valid_code(code))
        return text(req, "400 Bad Request", "Invalid language code.");
    StorageLock lock;
    lock.held = sd_card_manager_storage_acquire(pdMS_TO_TICKS(1000)) == ESP_OK;
    if (!lock.held) return text(req, "503 Service Unavailable", "SD card unavailable or busy.");
    std::string body, error;
    Json json(nullptr, cJSON_Delete);
    if (!read_language(code, body, json, error)) {
        ESP_LOGW(TAG, "%s: %s", code, error.c_str());
        return text(req, "422 Unprocessable Entity", error.c_str());
    }
    lock.release();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, body.data(), body.size());
}
}

esp_err_t ui_localization_init(void)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/i18n/languages", .method = HTTP_GET, .handler = list, .user_ctx = nullptr},
        {.uri = "/api/i18n/language", .method = HTTP_GET, .handler = load, .user_ctx = nullptr},
    };
    static const web_interface_extension_t extension = {
        "languages", "Language", html_start, static_cast<size_t>(html_end-html_start),
        js_start, static_cast<size_t>(js_end-js_start), routes, 2, "settings", nullptr
    };
    return web_interface_register_extension(&extension);
}
