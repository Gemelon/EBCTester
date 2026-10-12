#include "mariadb_client.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/sockets.h"
#include "psa/crypto.h"

namespace {

constexpr uint32_t CLIENT_LONG_PASSWORD = 0x00000001;
constexpr uint32_t CLIENT_LONG_FLAG = 0x00000004;
constexpr uint32_t CLIENT_CONNECT_WITH_DB = 0x00000008;
constexpr uint32_t CLIENT_PROTOCOL_41 = 0x00000200;
constexpr uint32_t CLIENT_TRANSACTIONS = 0x00002000;
constexpr uint32_t CLIENT_SECURE_CONNECTION = 0x00008000;
constexpr uint32_t CLIENT_PLUGIN_AUTH = 0x00080000;
constexpr size_t MAX_SEND_PACKET = 256 * 1024;
constexpr size_t MAX_RECEIVE_PACKET = 64 * 1024;
constexpr int CONNECT_TIMEOUT_S = 5;
constexpr int IO_TIMEOUT_S = 15;
constexpr char NATIVE_PASSWORD[] = "mysql_native_password";

void append_u32_le(std::vector<uint8_t> &data, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        data.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

void append_c_string(std::vector<uint8_t> &data, const std::string &value)
{
    data.insert(data.end(), value.begin(), value.end());
    data.push_back(0);
}

bool read_lenenc(const std::vector<uint8_t> &packet, size_t &pos, uint64_t &value)
{
    if (pos >= packet.size()) {
        return false;
    }
    const uint8_t first = packet[pos++];
    size_t bytes = 0;
    if (first < 0xFB) {
        value = first;
        return true;
    }
    if (first == 0xFC) {
        bytes = 2;
    } else if (first == 0xFD) {
        bytes = 3;
    } else if (first == 0xFE) {
        bytes = 8;
    } else {
        return false;
    }
    if (pos + bytes > packet.size()) {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < bytes; i++) {
        value |= static_cast<uint64_t>(packet[pos + i]) << (8 * i);
    }
    pos += bytes;
    return true;
}

bool is_eof_packet(const std::vector<uint8_t> &packet)
{
    return !packet.empty() && packet[0] == 0xFE && packet.size() < 9;
}

std::string error_from_packet(const std::vector<uint8_t> &packet)
{
    if (packet.size() < 3) {
        return "MariaDB error without description";
    }
    const unsigned code = packet[1] | (packet[2] << 8);
    size_t text_start = 3;
    if (packet.size() >= 9 && packet[3] == '#') {
        text_start = 9;
    }
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "MariaDB error %u: ", code);
    return std::string(prefix) + std::string(packet.begin() + text_start, packet.end());
}

bool send_all(int fd, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(fd, data + sent, length - sent, 0);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool receive_all(int fd, uint8_t *data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        const ssize_t result = recv(fd, data + received, length - received, 0);
        if (result <= 0) {
            return false;
        }
        received += static_cast<size_t>(result);
    }
    return true;
}

bool sha1(const uint8_t *data, size_t length, uint8_t out[20])
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    size_t hash_length = 0;
    return psa_hash_compute(PSA_ALG_SHA_1, data, length, out, 20, &hash_length) == PSA_SUCCESS && hash_length == 20;
}

/* SHA1(password) XOR SHA1(seed + SHA1(SHA1(password))) */
bool native_password_response(const std::string &password, const std::vector<uint8_t> &seed,
                              std::vector<uint8_t> &response)
{
    uint8_t hash1[20];
    uint8_t hash2[20];
    uint8_t hash3[20];
    if (!sha1(reinterpret_cast<const uint8_t *>(password.data()), password.size(), hash1) ||
        !sha1(hash1, sizeof(hash1), hash2)) {
        return false;
    }
    std::vector<uint8_t> input(seed);
    input.insert(input.end(), hash2, hash2 + sizeof(hash2));
    if (!sha1(input.data(), input.size(), hash3)) {
        return false;
    }
    response.resize(sizeof(hash1));
    for (size_t i = 0; i < response.size(); ++i) {
        response[i] = hash1[i] ^ hash3[i];
    }
    return true;
}

} // namespace

std::string mariadb_text_literal(const char *text, size_t length)
{
    static const char HEX_DIGITS[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2 + 32);
    out += "CONVERT(X'";
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        out.push_back(HEX_DIGITS[ch >> 4]);
        out.push_back(HEX_DIGITS[ch & 0x0F]);
    }
    out += "' USING utf8mb4)";
    return out;
}

MariaDbClient::~MariaDbClient()
{
    close();
}

void MariaDbClient::close()
{
    if (fd_ >= 0) {
        const uint8_t quit = 0x01;
        send_packet(0, &quit, 1);
        ::close(fd_);
        fd_ = -1;
    }
}

bool MariaDbClient::open_socket(const Config &config, std::string &error)
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    char port[8];
    snprintf(port, sizeof(port), "%u", static_cast<unsigned>(config.port));
    if (getaddrinfo(config.host.c_str(), port, &hints, &result) != 0 || result == nullptr) {
        error = "Server " + config.host + " not found";
        return false;
    }
    fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd_ < 0) {
        freeaddrinfo(result);
        error = std::string("Socket error: ") + strerror(errno);
        return false;
    }
    const int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd_, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno != EINPROGRESS) {
        error = std::string("Connection failed: ") + strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (rc != 0) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd_, &writable);
        timeval timeout = {};
        timeout.tv_sec = CONNECT_TIMEOUT_S;
        rc = select(fd_ + 1, nullptr, &writable, nullptr, &timeout);
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (rc <= 0) {
            error = "Connection timed out: " + config.host;
        } else if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
                   socket_error != 0) {
            error = std::string("Connection failed: ") + strerror(socket_error);
            rc = -1;
        }
        if (rc <= 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    timeval io_timeout = {};
    io_timeout.tv_sec = IO_TIMEOUT_S;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
    const int no_delay = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return true;
}

bool MariaDbClient::connect(const Config &config, std::string &error)
{
    close();
    if (config.host.empty() || config.user.empty() || config.database.empty()) {
        error = "Server, user and database must be configured";
        return false;
    }
    if (!open_socket(config, error)) {
        return false;
    }
    if (!authenticate(config, error)) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!execute("SET time_zone='+00:00'", error)) {
        close();
        return false;
    }
    return true;
}

bool MariaDbClient::send_packet(uint8_t sequence, const uint8_t *data, size_t length)
{
    if (fd_ < 0 || length > MAX_SEND_PACKET) {
        return false;
    }
    const uint8_t header[4] = {
        static_cast<uint8_t>(length & 0xFF), static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF), sequence};
    return send_all(fd_, header, sizeof(header)) && send_all(fd_, data, length);
}

bool MariaDbClient::receive_packet(uint8_t &sequence, std::vector<uint8_t> &data)
{
    uint8_t header[4];
    if (fd_ < 0 || !receive_all(fd_, header, sizeof(header))) {
        return false;
    }
    const size_t length = static_cast<size_t>(header[0]) | (static_cast<size_t>(header[1]) << 8) |
                          (static_cast<size_t>(header[2]) << 16);
    if (length > MAX_RECEIVE_PACKET) {
        return false;
    }
    sequence = header[3];
    data.resize(length);
    return length == 0 || receive_all(fd_, data.data(), length);
}

bool MariaDbClient::authenticate(const Config &config, std::string &error)
{
    uint8_t sequence = 0;
    std::vector<uint8_t> packet;
    if (!receive_packet(sequence, packet) || packet.empty()) {
        error = "No response from MariaDB (check the port)";
        return false;
    }
    if (packet[0] == 0xFF) {
        error = error_from_packet(packet);
        return false;
    }
    if (packet.size() < 34) {
        error = "Invalid MariaDB handshake";
        return false;
    }

    size_t offset = 1;
    while (offset < packet.size() && packet[offset] != 0) {
        offset++;
    }
    server_version_.assign(packet.begin() + 1, packet.begin() + offset);
    if (offset + 1 + 4 + 8 + 1 + 2 > packet.size()) {
        error = "Incomplete MariaDB handshake";
        return false;
    }
    offset += 1 + 4;
    std::vector<uint8_t> seed(packet.begin() + offset, packet.begin() + offset + 8);
    offset += 9;
    uint32_t server_capabilities = packet[offset] | (packet[offset + 1] << 8);
    offset += 2;
    offset += 1 + 2; /* character set, status flags */
    if (offset + 2 <= packet.size()) {
        server_capabilities |= static_cast<uint32_t>(packet[offset] | (packet[offset + 1] << 8)) << 16;
        offset += 2;
    }
    if ((server_capabilities & CLIENT_PROTOCOL_41) == 0) {
        error = "MariaDB server does not support protocol 4.1";
        return false;
    }
    uint8_t auth_length = 0;
    if (offset < packet.size()) {
        auth_length = packet[offset++];
    }
    if (offset + 10 > packet.size()) {
        error = "Incomplete MariaDB handshake (salt)";
        return false;
    }
    offset += 10;
    const size_t second_seed_length = std::min<size_t>(
        packet.size() - offset, std::max<size_t>(13, auth_length > 8 ? auth_length - 8 : 13));
    for (size_t i = 0; i < second_seed_length && packet[offset + i] != 0; i++) {
        seed.push_back(packet[offset + i]);
    }
    offset += second_seed_length;

    std::string plugin = NATIVE_PASSWORD;
    if ((server_capabilities & CLIENT_PLUGIN_AUTH) != 0 && offset < packet.size()) {
        size_t plugin_end = offset;
        while (plugin_end < packet.size() && packet[plugin_end] != 0) {
            plugin_end++;
        }
        plugin.assign(packet.begin() + offset, packet.begin() + plugin_end);
    }

    std::vector<uint8_t> auth_response;
    if (plugin == NATIVE_PASSWORD && !native_password_response(config.password, seed, auth_response)) {
        error = "SHA-1 calculation failed";
        return false;
    }

    uint32_t client_capabilities = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
                                   CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION;
    if ((server_capabilities & CLIENT_PLUGIN_AUTH) != 0) {
        client_capabilities |= CLIENT_PLUGIN_AUTH;
    }
    if ((server_capabilities & CLIENT_CONNECT_WITH_DB) != 0) {
        client_capabilities |= CLIENT_CONNECT_WITH_DB;
    }

    std::vector<uint8_t> response;
    append_u32_le(response, client_capabilities);
    append_u32_le(response, 16 * 1024 * 1024);
    response.push_back(45); /* utf8mb4_general_ci */
    response.insert(response.end(), 23, 0);
    append_c_string(response, config.user);
    response.push_back(static_cast<uint8_t>(auth_response.size()));
    response.insert(response.end(), auth_response.begin(), auth_response.end());
    if ((client_capabilities & CLIENT_CONNECT_WITH_DB) != 0) {
        append_c_string(response, config.database);
    }
    if ((client_capabilities & CLIENT_PLUGIN_AUTH) != 0) {
        append_c_string(response, NATIVE_PASSWORD);
    }
    if (!send_packet(sequence + 1, response.data(), response.size()) ||
        !receive_packet(sequence, packet) || packet.empty()) {
        error = "No response to login";
        return false;
    }

    if (packet[0] == 0xFE) {
        size_t plugin_end = 1;
        while (plugin_end < packet.size() && packet[plugin_end] != 0) {
            plugin_end++;
        }
        const std::string switch_plugin(packet.begin() + 1, packet.begin() + plugin_end);
        if (switch_plugin != NATIVE_PASSWORD) {
            error = "Authentication '" + switch_plugin +
                    "' is not supported; create a user with mysql_native_password";
            return false;
        }
        size_t seed_end = std::min(plugin_end + 1, packet.size());
        while (seed_end < packet.size() && packet[seed_end] != 0) {
            seed_end++;
        }
        std::vector<uint8_t> switch_seed(packet.begin() + std::min(plugin_end + 1, packet.size()),
                                         packet.begin() + seed_end);
        if (switch_seed.empty() ||
            !native_password_response(config.password, switch_seed, auth_response) ||
            !send_packet(sequence + 1, auth_response.data(), auth_response.size()) ||
            !receive_packet(sequence, packet) || packet.empty()) {
            error = "MariaDB authentication switch failed";
            return false;
        }
    }
    if (packet[0] == 0xFF) {
        error = error_from_packet(packet);
        return false;
    }
    if (packet[0] != 0x00) {
        error = "Unexpected login response (plugin " + plugin + ")";
        return false;
    }
    if ((client_capabilities & CLIENT_CONNECT_WITH_DB) == 0) {
        std::vector<uint8_t> init_db{0x02};
        init_db.insert(init_db.end(), config.database.begin(), config.database.end());
        if (!send_packet(0, init_db.data(), init_db.size()) || !receive_packet(sequence, packet) ||
            packet.empty() || packet[0] != 0x00) {
            error = packet.empty() || packet[0] != 0xFF ? "Could not select database"
                                                        : error_from_packet(packet);
            return false;
        }
    }
    return true;
}

bool MariaDbClient::send_query(const std::string &sql, std::string &error)
{
    if (fd_ < 0) {
        error = "No MariaDB connection";
        return false;
    }
    if (sql.size() + 1 > MAX_SEND_PACKET) {
        error = "SQL statement too large";
        return false;
    }
    std::vector<uint8_t> command;
    command.reserve(sql.size() + 1);
    command.push_back(0x03);
    command.insert(command.end(), sql.begin(), sql.end());
    if (!send_packet(0, command.data(), command.size())) {
        error = "Sending to MariaDB failed";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool MariaDbClient::execute(const std::string &sql, std::string &error, uint64_t *affected_rows,
                            uint64_t *insert_id)
{
    if (!send_query(sql, error)) {
        return false;
    }
    uint8_t sequence = 0;
    std::vector<uint8_t> packet;
    if (!receive_packet(sequence, packet) || packet.empty()) {
        error = "No response from MariaDB";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (packet[0] == 0xFF) {
        error = error_from_packet(packet);
        return false;
    }
    if (packet[0] != 0x00) {
        error = "Unexpected MariaDB response (result set instead of OK)";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    size_t pos = 1;
    uint64_t affected = 0;
    uint64_t last_id = 0;
    if (!read_lenenc(packet, pos, affected) || !read_lenenc(packet, pos, last_id)) {
        error = "Invalid OK packet";
        return false;
    }
    if (affected_rows != nullptr) {
        *affected_rows = affected;
    }
    if (insert_id != nullptr) {
        *insert_id = last_id;
    }
    return true;
}

bool MariaDbClient::query(const std::string &sql, std::vector<std::vector<std::string>> &rows,
                          std::string &error, size_t max_rows)
{
    rows.clear();
    if (!send_query(sql, error)) {
        return false;
    }
    uint8_t sequence = 0;
    std::vector<uint8_t> packet;
    auto protocol_error = [&](const char *message) {
        error = message;
        ::close(fd_);
        fd_ = -1;
        return false;
    };
    if (!receive_packet(sequence, packet) || packet.empty()) {
        return protocol_error("No response from MariaDB");
    }
    if (packet[0] == 0xFF) {
        error = error_from_packet(packet);
        return false;
    }
    if (packet[0] == 0x00) {
        return true;
    }
    size_t pos = 0;
    uint64_t columns = 0;
    if (!read_lenenc(packet, pos, columns) || columns == 0 || columns > 32) {
        return protocol_error("Unexpected column count");
    }
    for (uint64_t i = 0; i < columns; i++) {
        if (!receive_packet(sequence, packet) || packet.empty()) {
            return protocol_error("Missing column definition");
        }
    }
    if (!receive_packet(sequence, packet) || !is_eof_packet(packet)) {
        return protocol_error("Missing end of column definitions");
    }
    for (;;) {
        if (!receive_packet(sequence, packet) || packet.empty()) {
            return protocol_error("Incomplete result set");
        }
        if (is_eof_packet(packet)) {
            return true;
        }
        if (packet[0] == 0xFF) {
            error = error_from_packet(packet);
            return false;
        }
        std::vector<std::string> row;
        pos = 0;
        for (uint64_t i = 0; i < columns; i++) {
            if (pos < packet.size() && packet[pos] == 0xFB) {
                row.emplace_back();
                pos++;
                continue;
            }
            uint64_t length = 0;
            if (!read_lenenc(packet, pos, length) || pos + length > packet.size()) {
                return protocol_error("Invalid result row");
            }
            row.emplace_back(reinterpret_cast<const char *>(packet.data() + pos),
                             static_cast<size_t>(length));
            pos += static_cast<size_t>(length);
        }
        if (rows.size() < max_rows) {
            rows.push_back(std::move(row));
        }
    }
}
