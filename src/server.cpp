/*
 * server.cpp  –  Chat server  (CC3064 Proyecto #1)
 *
 * Usage:  ./server <port>
 *
 * Features:
 *   - Multithreaded: one thread per connected client
 *   - User registration / deregistration
 *   - Broadcast and direct messages
 *   - Status management (ACTIVE / DO_NOT_DISTURB / INVISIBLE)
 *   - Auto-sets INACTIVE (INVISIBLE) after INACTIVITY_SECS seconds
 *   - Lists all connected users + info queries
 */

#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <csignal>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// Proto includes
#include "../protos/register.pb.h"
#include "../protos/message_general.pb.h"
#include "../protos/message_dm.pb.h"
#include "../protos/change_status.pb.h"
#include "../protos/list_users.pb.h"
#include "../protos/get_user_info.pb.h"
#include "../protos/quit.pb.h"
#include "../protos/all_users.pb.h"
#include "../protos/broadcast_messages.pb.h"
#include "../protos/for_dm.pb.h"
#include "../protos/get_user_info_response.pb.h"
#include "../protos/server_response.pb.h"
#include "../protos/common.pb.h"

#include "../include/protocol.h"

// ── Inactivity timeout (seconds) ────────────────────────────────────────────
static constexpr int INACTIVITY_SECS = 30;

// ── Client record ────────────────────────────────────────────────────────────
struct ClientInfo {
    int         fd;
    std::string username;
    std::string ip;
    chat::StatusEnum status;
    std::chrono::steady_clock::time_point last_active;
};

// ── Global state ─────────────────────────────────────────────────────────────
static std::unordered_map<std::string, ClientInfo> g_clients; // keyed by username
static std::mutex g_mutex;
static std::atomic<bool> g_running{true};

// ── Helpers ───────────────────────────────────────────────────────────────────
static void send_server_response(int fd, int code, const std::string& msg, bool ok) {
    chat::ServerResponse resp;
    resp.set_status_code(code);
    resp.set_message(msg);
    resp.set_is_successful(ok);
    std::string payload;
    resp.SerializeToString(&payload);
    send_msg(fd, MSG_SERVER_RESPONSE, payload);
}

static void broadcast_message(const std::string& sender, const std::string& text) {
    chat::BroadcastDelivery bd;
    bd.set_message(text);
    bd.set_username_origin(sender);
    std::string payload;
    bd.SerializeToString(&payload);

    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& [name, ci] : g_clients) {
        send_msg(ci.fd, MSG_BROADCAST, payload);
    }
}

static void touch_activity(const std::string& username) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_clients.find(username);
    if (it != g_clients.end()) {
        it->second.last_active = std::chrono::steady_clock::now();
        if (it->second.status == chat::INVISIBLE) {
            it->second.status = chat::ACTIVE;
            // Notify client of restored status
            send_server_response(it->second.fd, 200, "STATUS_ACTIVE", true);
        }
    }
}

// ── Inactivity watcher thread ─────────────────────────────────────────────────
static void inactivity_watcher() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& [name, ci] : g_clients) {
            if (ci.status == chat::ACTIVE) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - ci.last_active).count();
                if (elapsed >= INACTIVITY_SECS) {
                    ci.status = chat::INVISIBLE; // INACTIVE
                    send_server_response(ci.fd, 200, "STATUS_INACTIVE", true);
                    std::cout << "[SERVER] " << name << " set to INACTIVE\n";
                }
            }
        }
    }
}

// ── Per-client thread ─────────────────────────────────────────────────────────
static void handle_client(int fd, std::string client_ip) {
    std::string username;

    // ── Step 1: expect REGISTER ──────────────────────────────────────────────
    {
        uint8_t     type;
        std::string payload;
        if (!recv_msg(fd, type, payload) || type != MSG_REGISTER) {
            close(fd);
            return;
        }
        chat::Register reg;
        if (!reg.ParseFromString(payload)) {
            close(fd);
            return;
        }
        username = reg.username();

        std::lock_guard<std::mutex> lk(g_mutex);
        // Check duplicate username or IP
        // IP check is skipped for localhost (127.0.0.1) to allow local testing
        bool is_local = (client_ip == "127.0.0.1");
        for (auto& [name, ci] : g_clients) {
            if (name == username) {
                send_server_response(fd, 400, "Username already taken", false);
                close(fd);
                return;
            }
            if (!is_local && ci.ip == client_ip) {
                send_server_response(fd, 400, "IP already connected", false);
                close(fd);
                return;
            }
        }
        // Register
        ClientInfo ci;
        ci.fd          = fd;
        ci.username    = username;
        ci.ip          = client_ip;
        ci.status      = chat::ACTIVE;
        ci.last_active = std::chrono::steady_clock::now();
        g_clients[username] = ci;

        send_server_response(fd, 200, "Welcome " + username, true);
        std::cout << "[+] " << username << " connected from " << client_ip << "\n";
    }

    // Announce join to everyone
    broadcast_message("SERVER", username + " has joined the chat!");

    // ── Step 2: message loop ─────────────────────────────────────────────────
    while (true) {
        uint8_t     type;
        std::string payload;
        if (!recv_msg(fd, type, payload)) break; // client disconnected

        switch (type) {

        case MSG_GENERAL: {
            chat::MessageGeneral mg;
            if (!mg.ParseFromString(payload)) break;
            touch_activity(username);
            broadcast_message(mg.username_origin(), mg.message());
            break;
        }

        case MSG_DM: {
            chat::MessageDM dm;
            if (!dm.ParseFromString(payload)) break;
            touch_activity(username);

            chat::ForDm fwd;
            fwd.set_username_des(username); // who sent it
            fwd.set_message(dm.message());
            std::string fwd_payload;
            fwd.SerializeToString(&fwd_payload);

            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_clients.find(dm.username_des());
            if (it != g_clients.end()) {
                send_msg(it->second.fd, MSG_FOR_DM, fwd_payload);
                send_server_response(fd, 200, "Message sent", true);
            } else {
                send_server_response(fd, 404, "User not found", false);
            }
            break;
        }

        case MSG_CHANGE_STATUS: {
            chat::ChangeStatus cs;
            if (!cs.ParseFromString(payload)) break;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_clients.find(username);
                if (it != g_clients.end()) {
                    it->second.status      = cs.status();
                    it->second.last_active = std::chrono::steady_clock::now();
                }
            }
            send_server_response(fd, 200, "Status updated", true);
            break;
        }

        case MSG_LIST_USERS: {
            chat::AllUsers au;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                for (auto& [name, ci] : g_clients) {
                    au.add_usernames(name);
                    au.add_status(ci.status);
                }
            }
            std::string p;
            au.SerializeToString(&p);
            send_msg(fd, MSG_ALL_USERS, p);
            break;
        }

        case MSG_GET_USER_INFO: {
            chat::GetUserInfo gui;
            if (!gui.ParseFromString(payload)) break;

            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_clients.find(gui.username_des());
            if (it != g_clients.end()) {
                chat::GetUserInfoResponse resp;
                resp.set_ip_address(it->second.ip);
                resp.set_username(it->second.username);
                resp.set_status(it->second.status);
                std::string p;
                resp.SerializeToString(&p);
                send_msg(fd, MSG_USER_INFO_RESP, p);
            } else {
                send_server_response(fd, 404, "User not found", false);
            }
            break;
        }

        case MSG_QUIT: {
            goto disconnect;
        }

        default:
            break;
        }
    }

disconnect:
    // Remove from client list
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_clients.erase(username);
    }
    close(fd);
    std::cout << "[-] " << username << " disconnected\n";
    broadcast_message("SERVER", username + " has left the chat.");
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }
    int port = std::stoi(argv[1]);

    // Ignore SIGPIPE so writes to closed sockets don't kill the server
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 64) < 0) { perror("listen"); return 1; }

    std::cout << "╔══════════════════════════════════╗\n";
    std::cout << "║   Chat Server  –  port " << port << "      ║\n";
    std::cout << "╚══════════════════════════════════╝\n";
    std::cout << "Waiting for connections...\n\n";

    // Start inactivity watcher
    std::thread(inactivity_watcher).detach();

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        std::thread(handle_client, client_fd, client_ip).detach();
    }

    close(server_fd);
    return 0;
}
