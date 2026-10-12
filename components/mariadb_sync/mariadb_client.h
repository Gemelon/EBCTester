#pragma once

#include <cstdint>
#include <string>
#include <vector>

/* Minimal MariaDB/MySQL text-protocol client (mysql_native_password, no TLS). */
class MariaDbClient {
public:
    struct Config {
        std::string host;
        uint16_t port = 3306;
        std::string user;
        std::string password;
        std::string database;
    };

    MariaDbClient() = default;
    MariaDbClient(const MariaDbClient &) = delete;
    MariaDbClient &operator=(const MariaDbClient &) = delete;
    ~MariaDbClient();

    bool connect(const Config &config, std::string &error);
    void close();
    bool connected() const { return fd_ >= 0; }
    const std::string &server_version() const { return server_version_; }

    /* Executes a statement that returns an OK packet. */
    bool execute(const std::string &sql, std::string &error,
                 uint64_t *affected_rows = nullptr, uint64_t *insert_id = nullptr);
    /* Executes a statement and collects at most max_rows text rows (NULL becomes ""). */
    bool query(const std::string &sql, std::vector<std::vector<std::string>> &rows,
               std::string &error, size_t max_rows = 64);

private:
    bool open_socket(const Config &config, std::string &error);
    bool authenticate(const Config &config, std::string &error);
    bool send_packet(uint8_t sequence, const uint8_t *data, size_t length);
    bool receive_packet(uint8_t &sequence, std::vector<uint8_t> &data);
    bool send_query(const std::string &sql, std::string &error);

    int fd_ = -1;
    std::string server_version_;
};

/* Builds a utf8mb4 SQL string literal from arbitrary bytes without quoting issues. */
std::string mariadb_text_literal(const char *text, size_t length);
