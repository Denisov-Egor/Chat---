#ifndef COMMON_HPP
#define COMMON_HPP

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <deque>
#include <chrono>
#include <sstream>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include <functional>
#include <ctime>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socklen_t = int;
    #define CLOSE_SOCKET(s) closesocket(s)
    #define SOCKET_ERRNO WSAGetLastError()
    using socket_t = SOCKET;
    const socket_t INVALID_SOCK = INVALID_SOCKET;
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netinet/in.h>
    #include <cerrno>
    #define CLOSE_SOCKET(s) close(s)
    #define SOCKET_ERRNO errno
    using socket_t = int;
    const socket_t INVALID_SOCK = -1;
#endif

inline std::string nick_color(const std::string& nick) {
    if (nick.empty()) return "\033[0m";
    const char* colors[] = {
        "\033[91m", "\033[92m", "\033[93m", "\033[94m",
        "\033[95m", "\033[96m", "\033[31m", "\033[32m",
        "\033[33m", "\033[34m", "\033[35m", "\033[36m"
    };
    return colors[std::tolower(nick[0]) % 12];
}

const std::string RESET   = "\033[0m";
const std::string SYS_COL = "\033[90m";
const std::string ERR_COL = "\033[91m";
const std::string BELL    = "\a";

struct ClientInfo {
    socket_t socket;
    std::string nickname;
    std::string ip;
    std::string room = "lobby";
    bool muted = false;
    std::deque<std::chrono::steady_clock::time_point> msg_times;

    ClientInfo(socket_t s, const std::string& n, const std::string& i)
        : socket(s), nickname(n), ip(i) {}
};

extern std::mutex clients_mtx;
extern std::mutex banned_mtx;
extern std::mutex users_mtx;
extern std::mutex log_mtx;
extern std::vector<ClientInfo> clients;
extern std::map<std::string, std::chrono::steady_clock::time_point> banned_ips;
extern int max_clients_limit;
extern std::atomic<bool> server_running;
extern socket_t listen_sock_global;
extern std::thread accept_thread;
extern std::ofstream log_file;
extern std::unordered_map<std::string, size_t> users;
extern std::string users_filename;

bool init_sockets();
void cleanup_sockets();
bool send_all(socket_t sock, const std::string& data);
bool send_line(socket_t sock, const std::string& line);
std::string get_ip(const sockaddr_in& addr);
bool is_banned(const std::string& ip);
void ban_ip(const std::string& ip, int duration_seconds = 60);
void broadcast_all(const std::string& message, socket_t exclude = INVALID_SOCK,
                   const std::string& prefix = "", const std::string& color = "");
void broadcast_room(const std::string& room, const std::string& message, socket_t exclude = INVALID_SOCK,
                    const std::string& prefix = "", const std::string& color = "");
void log_message(const std::string& msg);
void remove_client(socket_t sock, const std::string& nickname);
void load_users();
void save_users();

#endif