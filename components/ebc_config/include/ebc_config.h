/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EBC_CONFIG_PASSWORD_MAX 64
#define EBC_CONFIG_SERVER_MAX 128
#define EBC_CONFIG_URL_MAX 1025
#define EBC_CONFIG_TABS_MAX 256
#define EBC_CONFIG_DEVICE_ID_MAX 65
#define EBC_CONFIG_MOUNT_POINT_MAX 64
#define EBC_CONFIG_MAGIC UINT32_C(0x45424332)
#define EBC_CONFIG_VERSION 2

typedef enum { EBC_FW_MAKER = 0, EBC_FW_FACTORY = 1 } ebc_fw_mode_t;
typedef enum { IO47_OPEN, IO47_LOW, IO47_HIGH, IO47_ERROR } ebc_io47_state_t;
typedef enum { SD_BUS_SDMMC_4BIT = 0, SD_BUS_SPI = 1 } ebc_sd_bus_t;

typedef struct {
    int8_t tx_gpio;
    int8_t rx_gpio;
    uint32_t baud;
} ebc_uart_config_t;

typedef struct {
    uint8_t enabled;
    int8_t int_gpio;
    int8_t sda_gpio;
    int8_t scl_gpio;
} ebc_rtc_config_t;

typedef struct {
    struct {
        ebc_uart_config_t tester_uart;
        ebc_uart_config_t dongle_uart;
        uint16_t averaging_length;
        uint8_t mqtt_enabled;
        char device_id[EBC_CONFIG_DEVICE_ID_MAX];
    } tester;
    struct {
        char ap_password[EBC_CONFIG_PASSWORD_MAX];
        char sntp_server[EBC_CONFIG_SERVER_MAX];
        char manifest_url[EBC_CONFIG_URL_MAX];
        char web_password[EBC_CONFIG_PASSWORD_MAX];
        char primary_tabs[EBC_CONFIG_TABS_MAX];
        char tab_order[EBC_CONFIG_TABS_MAX];
        uint8_t allow_http;
        uint8_t status_led_enabled;
        int8_t led_gpio;
        uint8_t settings_require_password;
        uint8_t sd_languages;
        uint8_t session_days;
        uint8_t login_enabled;
    } ota;
    struct {
        ebc_sd_bus_t bus;
        uint32_t sdmmc_freq_khz;
        uint32_t spi_freq_khz;
        int8_t card_detect_gpio;
        char mount_point[EBC_CONFIG_MOUNT_POINT_MAX];
        int8_t clk_gpio;
        int8_t cmd_gpio;
        int8_t d0_gpio;
        int8_t d1_gpio;
        int8_t d2_gpio;
        int8_t d3_gpio;
        int8_t spi_miso_gpio;
        int8_t spi_mosi_gpio;
        int8_t spi_cs_gpio;
        int8_t spi_clk_gpio;
    } sd;
#ifdef __cplusplus
    alignas(4)
#else
    _Alignas(4)
#endif
    ebc_rtc_config_t rtc;
} ebc_config_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t crc;
    ebc_config_t payload;
} ebc_config_blob_t;

esp_err_t ebc_config_init(void);
const ebc_config_t *ebc_config_get(void);
esp_err_t ebc_config_save(const ebc_config_t *cfg);
esp_err_t ebc_config_save_with_active(const ebc_config_t *cfg, bool active);
esp_err_t ebc_config_validate(const ebc_config_t *cfg, bool active);
bool ebc_config_is_active(void);
ebc_fw_mode_t ebc_config_get_mode(void);
esp_err_t ebc_config_set_active(bool active);
esp_err_t ebc_config_factory_reset(void);
esp_err_t ebc_config_full_reset(void);

#ifdef __cplusplus
}
#endif
