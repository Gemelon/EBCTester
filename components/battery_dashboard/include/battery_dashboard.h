#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the measurement dashboard tab and its JSON API; call before the web server starts. */
esp_err_t battery_dashboard_init(void);

#ifdef __cplusplus
}
#endif
