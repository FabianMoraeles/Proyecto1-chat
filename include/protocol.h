#pragma once
#include <cstdint>
#include <string>
#include <arpa/inet.h>
#include <unistd.h>

// Message type IDs per protocol standard
enum MsgType : uint8_t {
    MSG_REGISTER        = 1,
    MSG_GENERAL         = 2,
    MSG_DM              = 3,
    MSG_CHANGE_STATUS   = 4,
    MSG_LIST_USERS      = 5,
    MSG_GET_USER_INFO   = 6,
    MSG_QUIT            = 7,
    // Server → Client
    MSG_SERVER_RESPONSE = 10,
    MSG_ALL_USERS       = 11,
    MSG_FOR_DM          = 12,
    MSG_BROADCAST       = 13,
    MSG_USER_INFO_RESP  = 14,
};

// Send a framed message: [type(1B)][len(4B BE)][payload]
inline bool send_msg(int fd, uint8_t type, const std::string& payload) {
    uint8_t  header[5];
    uint32_t net_len = htonl((uint32_t)payload.size());
    header[0] = type;
    memcpy(header + 1, &net_len, 4);

    if (write(fd, header, 5) != 5) return false;
    if (!payload.empty()) {
        ssize_t sent = 0, total = (ssize_t)payload.size();
        const char* buf = payload.data();
        while (sent < total) {
            ssize_t n = write(fd, buf + sent, total - sent);
            if (n <= 0) return false;
            sent += n;
        }
    }
    return true;
}

// Receive a framed message. Returns false on connection closed/error.
inline bool recv_msg(int fd, uint8_t& type, std::string& payload) {
    uint8_t header[5];
    ssize_t n = 0;
    while (n < 5) {
        ssize_t r = read(fd, header + n, 5 - n);
        if (r <= 0) return false;
        n += r;
    }
    type = header[0];
    uint32_t net_len;
    memcpy(&net_len, header + 1, 4);
    uint32_t len = ntohl(net_len);

    payload.resize(len);
    ssize_t received = 0;
    while (received < (ssize_t)len) {
        ssize_t r = read(fd, &payload[received], len - received);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}
