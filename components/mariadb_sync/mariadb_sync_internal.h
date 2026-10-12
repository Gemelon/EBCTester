/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "esp_err.h"

namespace mariadb_sync {

struct Settings {
    bool enabled = false;
    std::string host;
    uint16_t port = 3306;
    std::string user = "ebc_a20_user";
    std::string password;
    std::string database = "ebc_a20";
    uint16_t interval_minutes = 15;
};

enum class State { Idle, Testing, Syncing, Ok, Error };

struct Status {
    State state = State::Idle;
    bool busy = false;
    std::string message;
    time_t last_success = 0;
    time_t last_attempt = 0;
    uint32_t last_runs = 0;
    uint32_t last_measurements = 0;
    uint32_t pending_runs = 0;
    std::string server_version;
};

enum Command : uint32_t {
    COMMAND_TEST = 1u << 0,
    COMMAND_SYNC = 1u << 1,
    COMMAND_FULL_SYNC = 1u << 2,
};

Settings get_settings();
/* An empty password keeps the stored one. */
esp_err_t save_settings(const Settings &settings, bool keep_password);
Status get_status();
/* Returns ESP_ERR_INVALID_STATE while a job is already running. */
esp_err_t request(uint32_t command);
esp_err_t start_worker();
esp_err_t register_web_extension();

} // namespace mariadb_sync
