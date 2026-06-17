#include "common.hpp"
#include <csignal>

std::mutex clients_mtx;
std::mutex banned_mtx;
std::mutex users_mtx;
std::mutex log_mtx;
std::vector<ClientInfo> clients;
std::map<std::string, std::chrono::steady_clock::time_point> banned_ips;
int max_clients_limit = 5;
std::atomic<bool> server_running{false};
socket_t listen_sock_global = INVALID_SOCK;
std::thread accept_thread;
std::ofstream log_file;
std::unordered_map<std::string, size_t> users;
std::string users_filename = "users.txt";

bool init_sockets() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void cleanup_sockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool send_all(socket_t sock, const std::string& data) {
    int total = data.size();
    int sent = 0;
    while (sent < total) {
        int res = send(sock, data.c_str() + sent, total - sent, 0);
        if (res <= 0) return false;
        sent += res;
    }
    return true;
}

bool send_line(socket_t sock, const std::string& line) {
    return send_all(sock, line + "\n");
}

std::string get_ip(const sockaddr_in& addr) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    return std::string(ip_str);
}

bool is_banned(const std::string& ip) {
    std::lock_guard<std::mutex> lock(banned_mtx);
    auto it = banned_ips.find(ip);
    if (it != banned_ips.end()) {
        if (std::chrono::steady_clock::now() < it->second)
            return true;
        else
            banned_ips.erase(it);
    }
    return false;
}

void ban_ip(const std::string& ip, int duration_seconds) {
    std::lock_guard<std::mutex> lock(banned_mtx);
    banned_ips[ip] = std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds);
}

void log_message(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mtx);
    if (!log_file.is_open()) return;
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char time_buf[26];
    ctime_r(&t, time_buf);
    time_buf[24] = '\0';
    log_file << time_buf << " " << msg << std::endl;
}

void broadcast_all(const std::string& message, socket_t exclude,
                   const std::string& prefix, const std::string& color) {
    std::lock_guard<std::mutex> lock(clients_mtx);
    for (auto& c : clients) {
        if (c.socket != exclude) {
            send_line(c.socket, color + prefix + message + RESET);
        }
    }
    log_message("ALL: " + prefix + message);
}

void broadcast_room(const std::string& room, const std::string& message, socket_t exclude,
                    const std::string& prefix, const std::string& color) {
    std::lock_guard<std::mutex> lock(clients_mtx);
    for (auto& c : clients) {
        if (c.room == room && c.socket != exclude) {
            send_line(c.socket, color + prefix + message + RESET);
        }
    }
    log_message("ROOM " + room + ": " + prefix + message);
}

void remove_client(socket_t sock, const std::string& nickname) {
    bool existed = false;
    std::string old_room;
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        auto it = std::remove_if(clients.begin(), clients.end(),
                                 [sock](const ClientInfo& c) { return c.socket == sock; });
        if (it != clients.end()) {
            old_room = it->room;
            clients.erase(it, clients.end());
            existed = true;
        }
    }
    CLOSE_SOCKET(sock);
    if (existed && !nickname.empty()) {
        broadcast_room(old_room, nickname + " покинул чат.", INVALID_SOCK, "Система: ", SYS_COL);
    }
}

void load_users() {
    std::lock_guard<std::mutex> lock(users_mtx);
    std::ifstream f(users_filename);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string nick = line.substr(0, pos);
            size_t hash = std::stoull(line.substr(pos+1));
            users[nick] = hash;
        }
    }
}

void save_users() {
    std::lock_guard<std::mutex> lock(users_mtx);
    std::ofstream f(users_filename);
    if (!f) return;
    for (auto& p : users) {
        f << p.first << ":" << p.second << "\n";
    }
}