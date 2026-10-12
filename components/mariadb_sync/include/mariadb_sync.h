#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the MariaDB backup card (settings tab) and starts the background sync worker.
 * Must be called before the web server starts. */
esp_err_t mariadb_sync_init(void);

#ifdef __cplusplus
}
#endif
