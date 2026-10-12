#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EBC command codes (10-byte frames: FA cmd p1 p2 p3 xor F8, values base 240). */
#define EBC_CMD_DISCHARGE_CC 0x01
#define EBC_CMD_STOP 0x02
#define EBC_CMD_CONNECT 0x05
#define EBC_CMD_DISCONNECT 0x06
#define EBC_CMD_DISCHARGE_CP 0x11
#define EBC_CMD_CHARGE_CV 0x21

/* Largest parameter value representable with two base-240 digits. */
#define EBC_PARAM_MAX (240 * 240 - 1)

/* One decoded 19-byte status frame. */
typedef struct {
    uint8_t mode;
    uint8_t model;
    bool auxiliary; /* auxiliary frames carry no measurement values */
    float current_a;
    float voltage_v;
    float capacity_ah;
    float setpoint_1;
    float setpoint_2;
    float setpoint_3;
} ebc_frame_t;

typedef struct {
    bool connected;
    bool has_frame;
    bool auto_connect;
    bool command_pending;
    ebc_frame_t frame; /* last measurement frame */
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t last_frame_age_ms;
} ebc_link_status_t;

/* Callbacks run in the link task; they must not block for long. */
typedef struct {
    void (*on_frame)(const ebc_frame_t *frame, void *context);
    void (*on_connection_lost)(void *context);
    void (*on_tick)(void *context); /* about every 500 ms */
    void *context;
} ebc_link_listener_t;

/* Configures UART1 (tester) and UART2 (dongle) and starts the receive task. */
esp_err_t ebc_link_init(const ebc_link_listener_t *listener);
void ebc_link_get_status(ebc_link_status_t *status);
/* Sends the command right after the next valid status frame (half-duplex link).
 * Returns ESP_ERR_INVALID_STATE if not connected or another command is pending. */
esp_err_t ebc_link_queue_command(uint8_t command, uint16_t p1, uint16_t p2, uint16_t p3);
/* Sends connect immediately when not connected. */
esp_err_t ebc_link_connect(void);
/* Releases the EBC keypad lock and turns auto-connect off. */
esp_err_t ebc_link_disconnect(void);
void ebc_link_set_auto_connect(bool enabled);
const char *ebc_link_mode_name(uint8_t mode);
bool ebc_link_mode_is_active(uint8_t mode);
bool ebc_link_mode_is_idle(uint8_t mode);

#ifdef __cplusplus
}
#endif
