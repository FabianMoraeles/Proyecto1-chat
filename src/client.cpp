/*
 * client.cpp  –  Chat client  (CC3064 Proyecto #1)
 *
 * Usage:  ./client <username> <server_ip> <server_port>
 *
 * UI layout (ncurses):
 *   ┌─────────────────────────────────────────────────┐
 *   │  CHAT APP  │ user: alice │ status: ACTIVE        │  ← header
 *   ├─────────────────────────────────────────────────┤
 *   │                                                 │
 *   │   (message history)                             │  ← chat window
 *   │                                                 │
 *   ├─────────────────────────────────────────────────┤
 *   │  > _                                            │  ← input bar
 *   └─────────────────────────────────────────────────┘
 *
 * Commands (typed in input bar):
 *   /users             – list connected users
 *   /info <user>       – get user info
 *   /status <s>        – change status: active | busy | inactive
 *   /dm <user> <msg>   – send direct message
 *   /help              – show help
 *   /quit              – disconnect and exit
 *   <anything else>    – broadcast to all
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <sstream>
#include <cstring>
#include <csignal>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#include <ncurses.h>

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

// ── UI globals ────────────────────────────────────────────────────────────────
static WINDOW* g_header  = nullptr;
static WINDOW* g_chat    = nullptr;
static WINDOW* g_input   = nullptr;

static std::mutex      g_chat_mutex;
static std::vector<std::pair<int,std::string>> g_messages; // (color_pair, text)

static std::atomic<bool>       g_running{true};
static std::atomic<int>        g_sock{-1};
static std::string             g_username;
static std::string             g_local_ip;
static chat::StatusEnum        g_status = chat::ACTIVE;

// ── Color pairs ───────────────────────────────────────────────────────────────
#define COL_HEADER   1
#define COL_BORDER   2
#define COL_MSG_SELF 3
#define COL_MSG_DM   4
#define COL_MSG_SRV  5
#define COL_MSG_DEF  6
#define COL_INPUT    7
#define COL_ACTIVE   8
#define COL_BUSY     9
#define COL_INACTIVE 10

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::string status_str(chat::StatusEnum s) {
    switch(s) {
        case chat::ACTIVE:         return "ACTIVE";
        case chat::DO_NOT_DISTURB: return "BUSY";
        case chat::INVISIBLE:      return "INACTIVE";
        default:                   return "UNKNOWN";
    }
}

static int status_color(chat::StatusEnum s) {
    switch(s) {
        case chat::ACTIVE:         return COL_ACTIVE;
        case chat::DO_NOT_DISTURB: return COL_BUSY;
        default:                   return COL_INACTIVE;
    }
}

// ── UI functions ──────────────────────────────────────────────────────────────
static void draw_header() {
    if (!g_header) return;
    int cols = getmaxx(g_header);

    wattron(g_header, COLOR_PAIR(COL_HEADER) | A_BOLD);
    werase(g_header);

    std::string title = " 💬 Simple Chat  │  user: " + g_username + "  │  status: " + status_str(g_status);
    mvwprintw(g_header, 0, 0, "%s", title.c_str());

    // Fill rest of header line
    int pad = cols - (int)title.size();
    for (int i = 0; i < pad && i < cols; i++) waddch(g_header, ' ');

    wattroff(g_header, COLOR_PAIR(COL_HEADER) | A_BOLD);
    wrefresh(g_header);
}

static void push_message(int color, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_chat_mutex);
    // Word-wrap at chat window width
    int W = g_chat ? getmaxx(g_chat) - 2 : 78;
    std::string line = text;
    while ((int)line.size() > W) {
        g_messages.push_back({color, line.substr(0, W)});
        line = "  " + line.substr(W);
    }
    g_messages.push_back({color, line});

    // Redraw chat window
    if (!g_chat) return;
    werase(g_chat);
    box(g_chat, 0, 0);
    int rows = getmaxy(g_chat) - 2;
    int start = (int)g_messages.size() > rows ? (int)g_messages.size() - rows : 0;
    for (int i = 0; i < rows && (start + i) < (int)g_messages.size(); i++) {
        auto& [col, msg] = g_messages[start + i];
        wattron(g_chat, COLOR_PAIR(col));
        mvwprintw(g_chat, i + 1, 1, "%s", msg.c_str());
        wattroff(g_chat, COLOR_PAIR(col));
    }
    wrefresh(g_chat);
}

static void redraw_all() {
    draw_header();
    {
        std::lock_guard<std::mutex> lk(g_chat_mutex);
        werase(g_chat);
        box(g_chat, 0, 0);
        int rows = getmaxy(g_chat) - 2;
        int start = (int)g_messages.size() > rows ? (int)g_messages.size() - rows : 0;
        for (int i = 0; i < rows && (start + i) < (int)g_messages.size(); i++) {
            auto& [col, msg] = g_messages[start + i];
            wattron(g_chat, COLOR_PAIR(col));
            mvwprintw(g_chat, i + 1, 1, "%s", msg.c_str());
            wattroff(g_chat, COLOR_PAIR(col));
        }
        wrefresh(g_chat);
    }
    werase(g_input);
    box(g_input, 0, 0);
    wattron(g_input, COLOR_PAIR(COL_INPUT));
    mvwprintw(g_input, 1, 1, "> ");
    wattroff(g_input, COLOR_PAIR(COL_INPUT));
    wrefresh(g_input);
}

static void init_ui() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    start_color();
    use_default_colors();
    init_pair(COL_HEADER,   COLOR_BLACK,  COLOR_CYAN);
    init_pair(COL_BORDER,   COLOR_CYAN,   -1);
    init_pair(COL_MSG_SELF, COLOR_CYAN,   -1);
    init_pair(COL_MSG_DM,   COLOR_MAGENTA,-1);
    init_pair(COL_MSG_SRV,  COLOR_YELLOW, -1);
    init_pair(COL_MSG_DEF,  COLOR_WHITE,  -1);
    init_pair(COL_INPUT,    COLOR_GREEN,  -1);
    init_pair(COL_ACTIVE,   COLOR_GREEN,  -1);
    init_pair(COL_BUSY,     COLOR_RED,    -1);
    init_pair(COL_INACTIVE, COLOR_YELLOW, -1);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    g_header = newwin(1,     cols, 0,        0);
    g_chat   = newwin(rows-4, cols, 1,       0);
    g_input  = newwin(3,     cols, rows - 3, 0);

    scrollok(g_chat, TRUE);
    redraw_all();
}

static void cleanup_ui() {
    if (g_header) { delwin(g_header); g_header = nullptr; }
    if (g_chat)   { delwin(g_chat);   g_chat   = nullptr; }
    if (g_input)  { delwin(g_input);  g_input  = nullptr; }
    endwin();
}

// ── Network receive thread ────────────────────────────────────────────────────
static void recv_thread() {
    int sock = g_sock.load();
    while (g_running) {
        uint8_t     type;
        std::string payload;
        if (!recv_msg(sock, type, payload)) {
            if (g_running) {
                push_message(COL_MSG_SRV, "[!] Disconnected from server.");
                g_running = false;
            }
            break;
        }

        switch (type) {
        case MSG_SERVER_RESPONSE: {
            chat::ServerResponse sr;
            if (!sr.ParseFromString(payload)) break;
            if (!sr.is_successful()) {
                push_message(COL_BUSY, "[ERROR] " + sr.message());
            } else {
                // Status change notifications
                if (sr.message() == "STATUS_ACTIVE") {
                    g_status = chat::ACTIVE;
                    draw_header();
                    push_message(COL_ACTIVE, "[*] Your status is now ACTIVE.");
                } else if (sr.message() == "STATUS_INACTIVE") {
                    g_status = chat::INVISIBLE;
                    draw_header();
                    push_message(COL_INACTIVE, "[*] You have been set INACTIVE due to inactivity.");
                } else {
                    push_message(COL_MSG_SRV, "[Server] " + sr.message());
                }
            }
            break;
        }
        case MSG_BROADCAST: {
            chat::BroadcastDelivery bd;
            if (!bd.ParseFromString(payload)) break;
            std::string line;
            if (bd.username_origin() == g_username)
                line = "[You] " + bd.message();
            else if (bd.username_origin() == "SERVER")
                line = "*** " + bd.message() + " ***";
            else
                line = "[" + bd.username_origin() + "] " + bd.message();

            int col = (bd.username_origin() == g_username) ? COL_MSG_SELF :
                      (bd.username_origin() == "SERVER")    ? COL_MSG_SRV  : COL_MSG_DEF;
            push_message(col, line);
            break;
        }
        case MSG_FOR_DM: {
            chat::ForDm fdm;
            if (!fdm.ParseFromString(payload)) break;
            push_message(COL_MSG_DM, "[DM from " + fdm.username_des() + "] " + fdm.message());
            break;
        }
        case MSG_ALL_USERS: {
            chat::AllUsers au;
            if (!au.ParseFromString(payload)) break;
            push_message(COL_MSG_SRV, "── Connected users ──────────────");
            for (int i = 0; i < au.usernames_size(); i++) {
                std::string s = "  • " + au.usernames(i);
                int col = COL_MSG_DEF;
                if (i < au.status_size()) {
                    s += "  [" + status_str(au.status(i)) + "]";
                    col = status_color(au.status(i));
                }
                push_message(col, s);
            }
            push_message(COL_MSG_SRV, "─────────────────────────────────");
            break;
        }
        case MSG_USER_INFO_RESP: {
            chat::GetUserInfoResponse gir;
            if (!gir.ParseFromString(payload)) break;
            push_message(COL_MSG_SRV, "── User info ────────────────────");
            push_message(COL_MSG_DEF, "  Username : " + gir.username());
            push_message(COL_MSG_DEF, "  IP       : " + gir.ip_address());
            push_message(COL_MSG_DEF, "  Status   : " + status_str(gir.status()));
            push_message(COL_MSG_SRV, "─────────────────────────────────");
            break;
        }
        default: break;
        }
    }
}

// ── Command processing ────────────────────────────────────────────────────────
static void print_help() {
    push_message(COL_MSG_SRV, "== Help ================================");
    push_message(COL_MSG_DEF, "  <message>           broadcast to all");
    push_message(COL_MSG_DEF, "  /dm <user> <msg>    direct message");
    push_message(COL_MSG_DEF, "  /users              list users");
    push_message(COL_MSG_DEF, "  /info <user>        user info");
    push_message(COL_MSG_DEF, "  /status <s>         active|busy|inactive");
    push_message(COL_MSG_DEF, "  /help               show this help");
    push_message(COL_MSG_DEF, "  /quit               disconnect");
    push_message(COL_MSG_SRV, "========================================");
}

static void process_command(const std::string& line) {
    if (line.empty()) return;
    int sock = g_sock.load();

    if (line == "/quit") {
        chat::Quit q;
        q.set_quit(true);
        q.set_ip(g_local_ip);
        std::string p; q.SerializeToString(&p);
        send_msg(sock, MSG_QUIT, p);
        g_running = false;
        return;
    }

    if (line == "/help") { print_help(); return; }

    if (line == "/users") {
        chat::ListUsers lu;
        lu.set_username(g_username);
        lu.set_ip(g_local_ip);
        std::string p; lu.SerializeToString(&p);
        send_msg(sock, MSG_LIST_USERS, p);
        return;
    }

    if (line.substr(0, 6) == "/info ") {
        std::string target = line.substr(6);
        if (target.empty()) { push_message(COL_BUSY, "Usage: /info <username>"); return; }
        chat::GetUserInfo gui;
        gui.set_username_des(target);
        gui.set_username(g_username);
        gui.set_ip(g_local_ip);
        std::string p; gui.SerializeToString(&p);
        send_msg(sock, MSG_GET_USER_INFO, p);
        return;
    }

    if (line.substr(0, 8) == "/status ") {
        std::string s = line.substr(8);
        chat::StatusEnum newstatus;
        if      (s == "active")   newstatus = chat::ACTIVE;
        else if (s == "busy")     newstatus = chat::DO_NOT_DISTURB;
        else if (s == "inactive") newstatus = chat::INVISIBLE;
        else { push_message(COL_BUSY, "Valid statuses: active | busy | inactive"); return; }

        chat::ChangeStatus cs;
        cs.set_status(newstatus);
        cs.set_username(g_username);
        cs.set_ip(g_local_ip);
        std::string p; cs.SerializeToString(&p);
        send_msg(sock, MSG_CHANGE_STATUS, p);
        g_status = newstatus;
        draw_header();
        push_message(COL_MSG_SRV, "[*] Status changed to " + status_str(newstatus));
        return;
    }

    if (line.substr(0, 4) == "/dm ") {
        std::istringstream iss(line.substr(4));
        std::string target, msg;
        iss >> target;
        std::getline(iss, msg);
        if (msg.size() > 0 && msg[0] == ' ') msg = msg.substr(1);
        if (target.empty() || msg.empty()) { push_message(COL_BUSY, "Usage: /dm <user> <message>"); return; }

        chat::MessageDM dm;
        dm.set_message(msg);
        dm.set_status(g_status);
        dm.set_username_des(target);
        dm.set_ip(g_local_ip);
        std::string p; dm.SerializeToString(&p);
        send_msg(sock, MSG_DM, p);
        push_message(COL_MSG_DM, "[DM to " + target + "] " + msg);
        return;
    }

    if (!line.empty() && line[0] == '/') {
        push_message(COL_BUSY, "Unknown command. Type /help for help.");
        return;
    }

    // Broadcast
    chat::MessageGeneral mg;
    mg.set_message(line);
    mg.set_status(g_status);
    mg.set_username_origin(g_username);
    mg.set_ip(g_local_ip);
    std::string p; mg.SerializeToString(&p);
    send_msg(sock, MSG_GENERAL, p);
}

// ── Input loop ────────────────────────────────────────────────────────────────
static void input_loop() {
    std::string buf;
    int input_x = 3; // after "> "

    while (g_running) {
        // Redraw input box
        werase(g_input);
        box(g_input, 0, 0);
        wattron(g_input, COLOR_PAIR(COL_INPUT) | A_BOLD);
        mvwprintw(g_input, 1, 1, "> ");
        wattroff(g_input, COLOR_PAIR(COL_INPUT) | A_BOLD);
        mvwprintw(g_input, 1, input_x, "%s", buf.c_str());
        wmove(g_input, 1, input_x + (int)buf.size());
        wrefresh(g_input);

        int ch = wgetch(g_input);
        if (!g_running) break;

        if (ch == '\n' || ch == KEY_ENTER) {
            std::string cmd = buf;
            buf.clear();
            if (!cmd.empty()) process_command(cmd);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!buf.empty()) buf.pop_back();
        } else if (ch >= 32 && ch < 256) {
            int max_w = getmaxx(g_input) - input_x - 2;
            if ((int)buf.size() < max_w) buf += (char)ch;
        } else if (ch == KEY_RESIZE) {
            // terminal resize
            endwin();
            refresh();
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            wresize(g_header, 1, cols);
            wresize(g_chat,   rows - 4, cols);
            mvwin(g_input,    rows - 3, 0);
            wresize(g_input,  3, cols);
            redraw_all();
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <server_ip> <server_port>\n";
        return 1;
    }
    g_username        = argv[1];
    std::string ip    = argv[2];
    int         port  = std::stoi(argv[3]);

    // Connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        // Try hostname resolution
        struct hostent* he = gethostbyname(ip.c_str());
        if (!he) { std::cerr << "Cannot resolve: " << ip << "\n"; return 1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }
    g_sock.store(sock);

    // Get our local IP
    sockaddr_in local{};
    socklen_t   llen = sizeof(local);
    getsockname(sock, (sockaddr*)&local, &llen);
    g_local_ip = inet_ntoa(local.sin_addr);

    // Send REGISTER
    {
        chat::Register reg;
        reg.set_username(g_username);
        reg.set_ip(g_local_ip);
        std::string p; reg.SerializeToString(&p);
        send_msg(sock, MSG_REGISTER, p);

        // Wait for server response
        uint8_t type; std::string payload;
        if (!recv_msg(sock, type, payload) || type != MSG_SERVER_RESPONSE) {
            std::cerr << "No response from server\n"; return 1;
        }
        chat::ServerResponse sr;
        sr.ParseFromString(payload);
        if (!sr.is_successful()) {
            std::cerr << "Registration failed: " << sr.message() << "\n"; return 1;
        }
    }

    // Init UI
    signal(SIGPIPE, SIG_IGN);
    init_ui();

    push_message(COL_MSG_SRV, "Connected as " + g_username + "  –  type /help for commands");

    // Start receive thread
    std::thread rt(recv_thread);
    rt.detach();

    // Input loop (blocks until /quit or disconnect)
    input_loop();

    g_running = false;
    cleanup_ui();
    close(sock);
    std::cout << "Bye!\n";
    return 0;
}
