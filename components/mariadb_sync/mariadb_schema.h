/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

class MariaDbClient;

namespace mariadb_sync {

enum class SchemaResultState { Ready, NeedsMigration, Incompatible, Error };

struct SchemaCheck {
    SchemaResultState state = SchemaResultState::Error;
    std::string message;
};

SchemaCheck inspect_schema(MariaDbClient &client);
bool migrate_schema(MariaDbClient &client, std::string &error);

} // namespace mariadb_sync
