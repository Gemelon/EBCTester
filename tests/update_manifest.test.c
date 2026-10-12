/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "update_manifest.h"
#include "update_policy.h"

static const char *origin = "http://updates.example.org:8094/firmware.json";
static const char *fixture =
    "{\"project\":\"EBCTester\",\"target\":\"esp32s3\",\"version\":\"1.2.0\","
    "\"url\":\"http://updates.example.org:8094/EBCTester-1.2.0.bin\","
    "\"size\":100,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"notes\":\"Release notes\"}";

static bool parse(const char *text)
{
    update_candidate_t candidate = {0};
    return update_manifest_parse(text, strlen(text), origin, "EBCTester", "esp32s3", 1966080, &candidate);
}

static void reject_string(const char *key, const char *value)
{
    cJSON *json = cJSON_Parse(fixture);
    assert(json);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(json, key, cJSON_CreateString(value)));
    char *text = cJSON_PrintUnformatted(json);
    assert(!parse(text));
    cJSON_free(text);
    cJSON_Delete(json);
}

int main(void)
{
    uint32_t parts[3];
    assert(update_version_parse("1.2.0", parts));
    assert(update_version_newer("1.10.0", "1.9.99"));
    assert(update_version_newer("2.0.0", "1.99.99"));
    assert(!update_version_newer("1.2.0", "1.2.0"));
    assert(!update_version_newer("1.1.99", "1.2.0"));
    const char *invalid[] = {"1.2", "1.2.0-beta", "01.2.0", "-1.2.0", "1.2.0 ",
                             "4294967296.0.0", "1..0", ""};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert(!update_version_parse(invalid[i], parts));
    assert(update_manifest_url_valid(origin, true));
    assert(!update_manifest_url_valid(origin, false));
    assert(update_manifest_url_valid("https://updates.example.org/firmware.json", false));
    assert(!update_manifest_url_valid("http://user@host/firmware.json", true));
    assert(!update_manifest_url_valid("http://host/firmware.json?x=1", true));
    assert(!update_manifest_url_valid("http://host/../firmware.json", true));
    assert(!update_manifest_url_valid("http://host/firmware.json#x", true));
    assert(parse(fixture));
    reject_string("project", "Other");
    reject_string("target", "esp32");
    reject_string("version", "01.2.0");
    reject_string("url", "http://updates.example.org:8095/EBCTester-1.2.0.bin");
    reject_string("url", "https://updates.example.org:8094/EBCTester-1.2.0.bin");
    reject_string("url", "http://updates.example.org:8094/EBCTester.bin");
    reject_string("url", "http://updates.example.org:8094/EBCTester-1.2.1.bin");
    reject_string("url", "http://evil/EBCTester-1.2.0.bin");
    reject_string("url", "http://updates.example.org:8094/EBCTester-1.2.0.bin?x");
    reject_string("sha256", "bad");
    reject_string("sha256", "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg");
    reject_string("size", "100");
    char notes[514];
    memset(notes, 'n', sizeof(notes) - 1);
    notes[sizeof(notes) - 1] = 0;
    reject_string("notes", notes);
    double invalid_sizes[] = {0, -1, 0.5, 1966081};
    for (unsigned i = 0; i < sizeof(invalid_sizes) / sizeof(*invalid_sizes); ++i) {
        cJSON *json = cJSON_Parse(fixture);
        cJSON_SetNumberValue(cJSON_GetObjectItem(json, "size"), invalid_sizes[i]);
        char *text = cJSON_PrintUnformatted(json);
        assert(!parse(text));
        cJSON_free(text);
        cJSON_Delete(json);
    }
    cJSON *json = cJSON_Parse(fixture);
    cJSON_AddStringToObject(json, "version", "1.3.0");
    char *duplicate = cJSON_PrintUnformatted(json);
    assert(!parse(duplicate));
    cJSON_free(duplicate);
    cJSON_Delete(json);
    json = cJSON_Parse(fixture);
    cJSON_DeleteItemFromObjectCaseSensitive(json, "notes");
    char *missing = cJSON_PrintUnformatted(json);
    assert(!parse(missing));
    cJSON_free(missing);
    cJSON_Delete(json);
    assert(!parse("{\"notes\":\"\\u0000\"}"));
    assert(!parse("{\"x\":[[[1]]]}"));
    assert(!parse("[]"));
    assert(!parse("{"));
    assert(!parse(""));
    char trailing[1024];
    snprintf(trailing, sizeof(trailing), "%s trailing", fixture);
    assert(!parse(trailing));
    char oversized[2049];
    memset(oversized, ' ', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    assert(!parse(oversized));
    puts("PASS: manifest, URL, bounded parsing, numeric versions and types");
    return 0;
}
