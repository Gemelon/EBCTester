/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "network_manager.h"

#define ESP_LOGI(...) ((void)0)

typedef struct { const char *key; const char *value; } mdns_txt_item_t;
static const char *ap_name;
static char advertised_hostname[33], advertised_instance[33];
static esp_err_t init_error, hostname_error, service_error;
static unsigned hostname_calls, service_calls, cleanup_calls;

void network_manager_get_status(network_status_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->ap_ssid, sizeof(out->ap_ssid), "%s", ap_name);
}

static esp_err_t mdns_init(void) { return init_error; }
static esp_err_t mdns_hostname_set(const char *hostname)
{
    ++hostname_calls;
    snprintf(advertised_hostname, sizeof(advertised_hostname), "%s", hostname);
    return hostname_error;
}
static esp_err_t mdns_service_add(const char *instance, const char *service, const char *protocol,
                                  uint16_t port, mdns_txt_item_t *txt, size_t count)
{
    ++service_calls;
    assert(strcmp(service, "_http") == 0 && strcmp(protocol, "_tcp") == 0 && port == 80);
    assert(txt == NULL && count == 0);
    snprintf(advertised_instance, sizeof(advertised_instance), "%s", instance);
    return service_error;
}
static void mdns_free(void) { ++cleanup_calls; }

#include "../main/local_discovery.c"

static void reset(void)
{
    init_error = hostname_error = service_error = ESP_OK;
    hostname_calls = service_calls = cleanup_calls = 0;
}

int main(void)
{
    const char *names[] = {"ebct-ABC123", "ebct-000001"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        reset();
        ap_name = names[i];
        assert(local_discovery_start() == ESP_OK);
        assert(strcmp(advertised_hostname, ap_name) == 0);
        assert(strcmp(advertised_instance, ap_name) == 0);
        assert(hostname_calls == 1 && service_calls == 1 && cleanup_calls == 0);
    }
    reset();
    init_error = ESP_ERR_NO_MEM;
    assert(local_discovery_start() == ESP_ERR_NO_MEM);
    assert(hostname_calls == 0 && service_calls == 0 && cleanup_calls == 0);
    reset();
    hostname_error = ESP_ERR_INVALID_ARG;
    assert(local_discovery_start() == ESP_ERR_INVALID_ARG);
    assert(service_calls == 0 && cleanup_calls == 1);
    reset();
    service_error = ESP_FAIL;
    assert(local_discovery_start() == ESP_FAIL);
    assert(hostname_calls == 1 && service_calls == 1 && cleanup_calls == 1);
    puts("Local discovery reuses each AP name and cleans up failed mDNS startup.");
    return 0;
}
