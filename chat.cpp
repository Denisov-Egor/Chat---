#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <set>
#include <map>
#include <deque>
#include <chrono>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ctime>

// Кроссплатформенные сокеты
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
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

// Поддержка тестов: подменяем сетевые вызовы заглушками
#ifdef TESTING
    #undef CLOSE_SOCKET
    #define CLOSE_SOCKET(s) ((void)(s))
    inline bool send_all(socket_t, const std::string&) { return true; }
    inline bool send_line(socket_t sock, const std::string& line) { return true; }
    inline std::string get_ip(const sockaddr_in&) { return "127.0.0.1"; }
#endif

// Цвета
std::string nick_color(const std::string& nick) {
    if (nick.empty()) return "\033[0m";
    const char* colors[] = {
        "\033[91m", "\033[92m", "\033[93m", "\033[94m",
        "\033[95m", "\033[96m", "\033[31m", "\033[32m",
        "\033[33m", "\033[34m", "\033[35m", "\033[36m"
    };
    int idx = std::tolower(static_cast<unsigned char>(nick[0])) % 12;
    return colors[idx];
}

const std::string RESET   = "\033[0m";
const std::string SYS_COL = "\033[90m";
const std::string ERR_COL = "\033[91m";

// Глобальные данные
std::mutex clients_mtx;
std::mutex banned_mtx;
std::mutex rooms_mtx;

const size_t MAX_NICK_LENGTH = 32;

struct ClientInfo {
    socket_t socket;
    std::string nickname;
    std::string ip;
    bool muted = false;
    std::deque<std::chrono::steady_clock::time_point> msg_times;
    std::string current_room;
    std::set<std::string> ignored_nicks;

    ClientInfo(socket_t s, const std::string& n, const std::string& i)
        : socket(s), nickname(n), ip(i) {}
};

std::vector<ClientInfo> clients;
std::set<std::string> banned_ips;
std::map<std::string, std::set<socket_t>> rooms;
int max_clients_limit = 5;

// Управление сервером и логированием
std::atomic<bool> server_running{false};
socket_t listen_sock_global = INVALID_SOCK;
std::thread accept_thread;

std::mutex admin_cout_mtx;
std::ofstream log_file;
bool logging_to_file = false;

// Потокобезопасное логирование
void log_event(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);
    std::string full_msg = std::string(time_buf) + " " + msg;

    {
        std::lock_guard<std::mutex> lock(admin_cout_mtx);
        std::cout << full_msg << std::endl;
        if (logging_to_file && log_file.is_open()) {
            log_file << full_msg << std::endl;
            log_file.flush();
        }
    }
}

int safe_stoi(const std::string& str, bool& ok) {
    ok = false;
    if (str.empty()) return 0;
    char* end = nullptr;
    long val = std::strtol(str.c_str(), &end, 10);
    if (end != str.c_str() && *end == '\0' && val >= 0) {
        ok = true;
        return static_cast<int>(val);
    }
    return 0;
}

bool init_sockets() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void cleanup_sockets() {
#ifdef _WIN32
    WSACleanup();
#endif
    if (logging_to_file && log_file.is_open()) {
        log_file.close();
    }
}

#ifndef TESTING
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
#endif

void broadcast_global(const std::string& message, socket_t exclude = INVALID_SOCK) {
    std::lock_guard<std::mutex> lock(clients_mtx);
    for (auto& c : clients) {
        if (c.socket != exclude) {
            send_line(c.socket, message);
        }
    }
}

void broadcast_to_room(const std::string& room, const std::string& message, socket_t exclude = INVALID_SOCK) {
    std::lock_guard<std::mutex> lock(rooms_mtx);
    auto it = rooms.find(room);
    if (it == rooms.end()) return;
    for (socket_t s : it->second) {
        if (s != exclude) {
            send_line(s, message);
        }
    }
}

void remove_client(socket_t sock, const std::string& nickname) {
    std::string room;
    bool existed = false;
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        auto it = std::remove_if(clients.begin(), clients.end(),
                                 [sock](const ClientInfo& c) { return c.socket == sock; });
        if (it != clients.end()) {
            room = it->current_room;
            clients.erase(it, clients.end());
            existed = true;
        }
    }

    if (!room.empty()) {
        {
            std::lock_guard<std::mutex> lock(rooms_mtx);
            auto rit = rooms.find(room);
            if (rit != rooms.end()) {
                rit->second.erase(sock);
                if (rit->second.empty()) {
                    rooms.erase(rit);
                }
            }
        }
        if (existed) {
            broadcast_to_room(room, SYS_COL + "Система: " + nickname + " покинул комнату " + room + RESET);
            log_event(nickname + " вышел из комнаты " + room);
        }
    }

    CLOSE_SOCKET(sock);
    if (existed && !nickname.empty()) {
        broadcast_global(SYS_COL + "Система: " + nickname + " покинул чат." + RESET);
        log_event(nickname + " покинул чат");
    }
}

void leave_current_room(socket_t sock, const std::string& nick, const std::string& room) {
    if (room.empty()) return;
    {
        std::lock_guard<std::mutex> lock(rooms_mtx);
        auto it = rooms.find(room);
        if (it != rooms.end()) {
            it->second.erase(sock);
            if (it->second.empty()) {
                rooms.erase(it);
            }
        }
    }
    broadcast_to_room(room, SYS_COL + "Система: " + nick + " покинул комнату " + room + RESET);
    log_event(nick + " покинул комнату " + room);
}

void process_command(const std::string& line, socket_t sock) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    std::string my_nick, my_room;
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        for (auto& c : clients) {
            if (c.socket == sock) {
                my_nick = c.nickname;
                my_room = c.current_room;
                break;
            }
        }
    }
    if (my_nick.empty()) return;

    if (cmd == "/help") {
        std::string help =
            "Доступные команды:\n"
            "  /help          - эта справка\n"
            "  /list          - список участников\n"
            "  /rooms         - список комнат\n"
            "  /join #комната - войти в комнату\n"
            "  /create #комната - создать комнату и войти\n"
            "  /leave         - выйти из комнаты (вернуться в общий чат)\n"
            "  /msg <ник> <текст> - личное сообщение\n"
            "  /nick <новый>  - сменить ник\n"
            "  /ignore <ник>  - игнорировать сообщения от пользователя\n"
            "  /unignore <ник> - перестать игнорировать\n"
            "  /quit          - выйти из чата\n";
        send_line(sock, SYS_COL + help + RESET);
    }
    else if (cmd == "/list") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        std::string list = "Участники (" + std::to_string(clients.size()) + "):\n";
        for (auto& c : clients) {
            list += "  " + nick_color(c.nickname) + c.nickname + RESET;
            if (c.muted) list += " [muted]";
            if (!c.current_room.empty()) list += " (комната " + c.current_room + ")";
            list += "\n";
        }
        send_line(sock, list);
    }
    else if (cmd == "/rooms") {
        std::lock_guard<std::mutex> lock(rooms_mtx);
        std::string list = "Комнаты (" + std::to_string(rooms.size()) + "):\n";
        for (auto& pair : rooms) {
            list += "  " + pair.first + " (" + std::to_string(pair.second.size()) + " участников)\n";
        }
        send_line(sock, SYS_COL + list + RESET);
    }
    else if (cmd == "/join" || cmd == "/create") {
        std::string room;
        if (!(iss >> room)) {
            send_line(sock, ERR_COL + "Использование: " + cmd + " #комната" + RESET);
            return;
        }
        if (room.empty() || room[0] != '#') {
            send_line(sock, ERR_COL + "Имя комнаты должно начинаться с #" + RESET);
            return;
        }

        {
            std::unique_lock<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket == sock) {
                    if (!c.current_room.empty() && c.current_room != room) {
                        std::string old_room = c.current_room;
                        c.current_room.clear();
                        lock.unlock();
                        leave_current_room(sock, my_nick, old_room);
                        break;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket == sock) {
                    c.current_room = room;
                    break;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(rooms_mtx);
            rooms[room].insert(sock);
        }

        send_line(sock, SYS_COL + "Вы вошли в комнату " + room + RESET);
        broadcast_to_room(room, SYS_COL + "Система: " + my_nick + " присоединился к комнате " + room + RESET, sock);
        log_event(my_nick + " вошёл в комнату " + room);
    }
    else if (cmd == "/leave") {
        std::string old_room;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket == sock) {
                    if (c.current_room.empty()) {
                        send_line(sock, ERR_COL + "Вы не находитесь в комнате" + RESET);
                        return;
                    }
                    old_room = c.current_room;
                    c.current_room.clear();
                    break;
                }
            }
        }
        leave_current_room(sock, my_nick, old_room);
        send_line(sock, SYS_COL + "Вы вышли из комнаты " + old_room + RESET);
    }
    else if (cmd == "/msg") {
        std::string target, text;
        if (!(iss >> target)) {
            send_line(sock, ERR_COL + "Использование: /msg <ник> <текст>" + RESET);
            return;
        }
        std::getline(iss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        if (text.empty()) {
            send_line(sock, ERR_COL + "Сообщение не может быть пустым" + RESET);
            return;
        }
        std::lock_guard<std::mutex> lock(clients_mtx);
        bool found = false;
        for (auto& c : clients) {
            if (c.nickname == target) {
                send_line(c.socket, nick_color(my_nick) + "[ЛС от " + my_nick + "] " + RESET + text);
                found = true;
                break;
            }
        }
        if (!found) {
            send_line(sock, ERR_COL + "Пользователь '" + target + "' не найден" + RESET);
        }
    }
    else if (cmd == "/nick") {
        std::string new_nick;
        if (!(iss >> new_nick)) {
            send_line(sock, ERR_COL + "Использование: /nick <новый ник>" + RESET);
            return;
        }
        if (new_nick.empty() || new_nick.length() > MAX_NICK_LENGTH ||
            new_nick.find_first_of(" /\r\n") != std::string::npos) {
            send_line(sock, ERR_COL + "Ник должен быть от 1 до " + std::to_string(MAX_NICK_LENGTH) +
                      " символов и не содержать пробелов или запрещённых символов" + RESET);
            return;
        }
        if (new_nick == "Система") {
            send_line(sock, ERR_COL + "Запрещённый ник" + RESET);
            return;
        }

        std::string old_nick;
        bool nick_exists = false;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            ClientInfo* client = nullptr;
            for (auto& c : clients) {
                if (c.socket == sock) {
                    client = &c;
                    break;
                }
            }
            if (!client) return;

            for (auto& c : clients) {
                if (c.nickname == new_nick) {
                    nick_exists = true;
                    break;
                }
            }
            if (!nick_exists) {
                old_nick = client->nickname;
                client->nickname = new_nick;
            }
        }

        if (nick_exists) {
            send_line(sock, ERR_COL + "Ник '" + new_nick + "' уже занят" + RESET);
            return;
        }

        // Обновляем старый ник в игнор-листах других пользователей
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket != sock) {
                    auto it = c.ignored_nicks.find(old_nick);
                    if (it != c.ignored_nicks.end()) {
                        c.ignored_nicks.erase(it);
                        c.ignored_nicks.insert(new_nick);
                    }
                }
            }
        }

        broadcast_global(SYS_COL + "Система: " + old_nick + " теперь известен как " + new_nick + RESET);
        if (!my_room.empty()) {
            broadcast_to_room(my_room, SYS_COL + "Система: " + old_nick + " теперь известен как " + new_nick + RESET);
        }
        log_event(old_nick + " сменил ник на " + new_nick);
    }
    else if (cmd == "/ignore") {
        std::string target;
        if (!(iss >> target)) {
            send_line(sock, ERR_COL + "Использование: /ignore <ник>" + RESET);
            return;
        }
        if (target == my_nick) {
            send_line(sock, ERR_COL + "Нельзя игнорировать самого себя" + RESET);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            bool target_exists = false;
            for (auto& c : clients) {
                if (c.nickname == target) {
                    target_exists = true;
                    break;
                }
            }
            if (!target_exists) {
                send_line(sock, ERR_COL + "Пользователь '" + target + "' не найден" + RESET);
                return;
            }

            for (auto& c : clients) {
                if (c.socket == sock) {
                    if (c.ignored_nicks.insert(target).second) {
                        send_line(sock, SYS_COL + "Вы теперь игнорируете пользователя " + target + RESET);
                        log_event(my_nick + " начал игнорировать " + target);
                    } else {
                        send_line(sock, ERR_COL + "Пользователь " + target + " уже в вашем списке игнорируемых" + RESET);
                    }
                    break;
                }
            }
        }
    }
    else if (cmd == "/unignore") {
        std::string target;
        if (!(iss >> target)) {
            send_line(sock, ERR_COL + "Использование: /unignore <ник>" + RESET);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket == sock) {
                    if (c.ignored_nicks.erase(target) > 0) {
                        send_line(sock, SYS_COL + "Вы больше не игнорируете пользователя " + target + RESET);
                        log_event(my_nick + " перестал игнорировать " + target);
                    } else {
                        send_line(sock, ERR_COL + "Пользователь " + target + " не был в вашем списке игнорируемых" + RESET);
                    }
                    break;
                }
            }
        }
    }
    else {
        send_line(sock, ERR_COL + "Неизвестная команда. Введите /help для списка" + RESET);
    }
}

void handle_client(socket_t sock, sockaddr_in client_addr) {
    std::string ip = get_ip(client_addr);

    {
        std::lock_guard<std::mutex> lock(banned_mtx);
        if (banned_ips.find(ip) != banned_ips.end()) {
            send_line(sock, ERR_COL + "Вы забанены в этом чате." + RESET);
            CLOSE_SOCKET(sock);
            return;
        }
    }

    std::string buffer;
    char chunk[256];
    while (true) {
        int bytes = recv(sock, chunk, sizeof(chunk) - 1, 0);
        if (bytes <= 0) {
            CLOSE_SOCKET(sock);
            return;
        }
        chunk[bytes] = '\0';
        buffer += chunk;
        size_t pos = buffer.find('\n');
        if (pos != std::string::npos) {
            std::string nick = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            nick.erase(std::remove(nick.begin(), nick.end(), '\r'), nick.end());
            nick.erase(std::remove(nick.begin(), nick.end(), ' '), nick.end());
            if (nick.empty() || nick.length() > MAX_NICK_LENGTH ||
                nick == "Система" || nick.find_first_of("/\r\n") != std::string::npos) {
                send_line(sock, ERR_COL + "Недопустимый ник. Используйте от 1 до " +
                          std::to_string(MAX_NICK_LENGTH) + " символов без пробелов и /." + RESET);
                CLOSE_SOCKET(sock);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                bool exists = false;
                for (auto& c : clients) {
                    if (c.nickname == nick) {
                        exists = true;
                        break;
                    }
                }
                if (exists) {
                    send_line(sock, ERR_COL + "Ник '" + nick + "' уже занят." + RESET);
                    CLOSE_SOCKET(sock);
                    return;
                }
                if (max_clients_limit > 0 && clients.size() >= (size_t)max_clients_limit) {
                    send_line(sock, ERR_COL + "Чат заполнен (максимум " + std::to_string(max_clients_limit) + ")." + RESET);
                    CLOSE_SOCKET(sock);
                    return;
                }
                clients.push_back(ClientInfo(sock, nick, ip));
            }

            send_line(sock, SYS_COL + "Добро пожаловать, " + nick + "! Введите /help для справки." + RESET);
            broadcast_global(SYS_COL + "Система: " + nick + " присоединился к чату." + RESET, sock);
            log_event(nick + " (" + ip + ") присоединился к чату");
            break;
        }
    }

    std::string msg_buffer;
    char data_chunk[1024];
    while (true) {
        int bytes = recv(sock, data_chunk, sizeof(data_chunk) - 1, 0);
        if (bytes <= 0) {
            std::string my_nick;
            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) {
                    if (c.socket == sock) {
                        my_nick = c.nickname;
                        break;
                    }
                }
            }
            remove_client(sock, my_nick);
            return;
        }
        data_chunk[bytes] = '\0';
        msg_buffer += data_chunk;

        size_t pos;
        while ((pos = msg_buffer.find('\n')) != std::string::npos) {
            std::string line = msg_buffer.substr(0, pos);
            msg_buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.empty()) continue;

            std::string nick, room;
            bool muted = false;
            bool exists = false;
            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) {
                    if (c.socket == sock) {
                        nick = c.nickname;
                        room = c.current_room;
                        muted = c.muted;
                        exists = true;
                        break;
                    }
                }
            }

            if (!exists) return;

            if (line == "/quit") {
                remove_client(sock, nick);
                return;
            }

            if (line[0] == '/') {
                process_command(line, sock);
                continue;
            }

            if (muted) {
                send_line(sock, ERR_COL + "Вы заглушены и не можете отправлять сообщения." + RESET);
                continue;
            }

            bool drop = false;
            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) {
                    if (c.socket == sock) {
                        auto now = std::chrono::steady_clock::now();
                        c.msg_times.push_back(now);
                        while (!c.msg_times.empty() &&
                               std::chrono::duration_cast<std::chrono::seconds>(now - c.msg_times.front()).count() >= 2) {
                            c.msg_times.pop_front();
                        }
                        if (c.msg_times.size() >= 4) {
                            send_line(sock, ERR_COL + "Слишком часто! Подождите немного." + RESET);
                            drop = true;
                        }
                        break;
                    }
                }
            }
            if (drop) continue;

            std::string color = nick_color(nick);
            std::string full_msg = color + nick + ": " + line + RESET;
            std::string log_text = "[" + (room.empty() ? "общий" : room) + "] " + nick + ": " + line;

            if (room.empty()) {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) {
                    if (c.current_room.empty() && c.socket != sock) {
                        if (c.ignored_nicks.count(nick) == 0) {
                            send_line(c.socket, full_msg);
                        }
                    }
                }
            } else {
                std::set<socket_t> target_sockets;
                {
                    std::lock_guard<std::mutex> lock_rooms(rooms_mtx);
                    auto it = rooms.find(room);
                    if (it != rooms.end()) {
                        target_sockets = it->second;
                    }
                }
                if (!target_sockets.empty()) {
                    std::lock_guard<std::mutex> lock_clients(clients_mtx);
                    for (socket_t s : target_sockets) {
                        if (s != sock) {
                            for (auto& c : clients) {
                                if (c.socket == s) {
                                    if (c.ignored_nicks.count(nick) == 0) {
                                        send_line(s, full_msg);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            log_event(log_text);
        }
    }
}

void process_admin_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "/help") {
        std::cout << "Команды администратора:\n"
                  << "  /list              - список участников\n"
                  << "  /rooms             - список комнат\n"
                  << "  /room_members #room - участники комнаты\n"
                  << "  /delete_room #room - удалить комнату\n"
                  << "  /kick <ник>        - выгнать участника из чата\n"
                  << "  /ban <ник>         - забанить по IP\n"
                  << "  /mute <ник>        - запретить сообщения\n"
                  << "  /unmute <ник>      - разрешить сообщения\n"
                  << "  /quit              - остановить сервер\n";
    }
    else if (cmd == "/list") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        std::cout << "Участники (" << clients.size() << "):\n";
        for (auto& c : clients) {
            std::cout << "  " << c.nickname;
            if (c.muted) std::cout << " [muted]";
            if (!c.current_room.empty()) std::cout << " (комната " << c.current_room << ")";
            std::cout << " (" << c.ip << ")\n";
        }
    }
    else if (cmd == "/rooms") {
        std::lock_guard<std::mutex> lock(rooms_mtx);
        std::cout << "Комнаты (" << rooms.size() << "):\n";
        for (auto& pair : rooms) {
            std::cout << "  " << pair.first << " (" << pair.second.size() << " участников)\n";
        }
    }
    else if (cmd == "/room_members") {
        std::string room;
        if (!(iss >> room)) {
            std::cout << "Использование: /room_members #комната\n";
            return;
        }

        std::set<socket_t> member_sockets;
        {
            std::lock_guard<std::mutex> lock(rooms_mtx);
            auto it = rooms.find(room);
            if (it == rooms.end()) {
                std::cout << "Комната не найдена.\n";
                return;
            }
            member_sockets = it->second;
        }

        std::cout << "Участники комнаты " << room << ":\n";
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (socket_t s : member_sockets) {
                for (auto& c : clients) {
                    if (c.socket == s) {
                        std::cout << "  " << c.nickname << "\n";
                        break;
                    }
                }
            }
        }
    }
    else if (cmd == "/delete_room") {
        std::string room;
        if (!(iss >> room)) {
            std::cout << "Использование: /delete_room #комната\n";
            return;
        }

        std::set<socket_t> affected;
        {
            std::lock_guard<std::mutex> lock(rooms_mtx);
            auto it = rooms.find(room);
            if (it == rooms.end()) {
                std::cout << "Комната не найдена.\n";
                return;
            }
            affected = it->second;
            rooms.erase(it);
        }

        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (affected.count(c.socket)) {
                    c.current_room.clear();
                }
            }
        }

        for (socket_t s : affected) {
            send_line(s, SYS_COL + "Система: Комната " + room + " удалена администратором. Вы возвращены в общий чат." + RESET);
        }
        log_event("[АДМИН] Комната " + room + " удалена администратором");
        std::cout << "Комната " << room << " удалена.\n";
    }
    else if (cmd == "/kick") {
        std::string target;
        if (!(iss >> target)) {
            std::cout << "Использование: /kick <ник>\n";
            return;
        }

        socket_t target_sock = INVALID_SOCK;
        std::string target_nick, target_room;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (it->nickname == target) {
                    target_sock = it->socket;
                    target_nick = it->nickname;
                    target_room = it->current_room;
                    clients.erase(it);
                    break;
                }
            }
        }

        if (target_sock != INVALID_SOCK) {
            send_line(target_sock, SYS_COL + "Вас исключили из чата" + RESET);
            CLOSE_SOCKET(target_sock);
            broadcast_global(SYS_COL + "Система: " + target_nick + " исключён администратором." + RESET);
            if (!target_room.empty()) {
                leave_current_room(target_sock, target_nick, target_room);
            }
            log_event("[АДМИН] " + target_nick + " исключён из чата");
            std::cout << "Пользователь " << target_nick << " исключён.\n";
        } else {
            std::cout << "Пользователь не найден.\n";
        }
    }
    else if (cmd == "/ban") {
        std::string target;
        if (!(iss >> target)) {
            std::cout << "Использование: /ban <ник>\n";
            return;
        }

        std::string ip_to_ban;
        socket_t target_sock = INVALID_SOCK;
        std::string target_nick, target_room;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (it->nickname == target) {
                    ip_to_ban = it->ip;
                    target_sock = it->socket;
                    target_nick = it->nickname;
                    target_room = it->current_room;
                    clients.erase(it);
                    break;
                }
            }
        }

        if (!ip_to_ban.empty()) {
            {
                std::lock_guard<std::mutex> lock(banned_mtx);
                banned_ips.insert(ip_to_ban);
            }
            send_line(target_sock, SYS_COL + "Вы забанены" + RESET);
            CLOSE_SOCKET(target_sock);
            broadcast_global(SYS_COL + "Система: " + target_nick + " забанен администратором." + RESET);
            if (!target_room.empty()) {
                leave_current_room(target_sock, target_nick, target_room);
            }
            log_event("[АДМИН] " + target_nick + " забанен (IP: " + ip_to_ban + ")");
            std::cout << "Пользователь " << target_nick << " забанен (IP: " << ip_to_ban << ").\n";
        } else {
            std::cout << "Пользователь не найден.\n";
        }
    }
    else if (cmd == "/mute") {
        std::string target;
        if (!(iss >> target)) {
            std::cout << "Использование: /mute <ник>\n";
            return;
        }

        bool found = false;
        socket_t target_sock;
        std::string target_nick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.nickname == target) {
                    c.muted = true;
                    found = true;
                    target_sock = c.socket;
                    target_nick = c.nickname;
                    break;
                }
            }
        }

        if (found) {
            send_line(target_sock, SYS_COL + "Вам запретили отправлять сообщения" + RESET);
            broadcast_global(SYS_COL + "Система: " + target_nick + " заглушен администратором." + RESET);
            log_event("[АДМИН] " + target_nick + " заглушен");
            std::cout << "Пользователь " << target_nick << " заглушен.\n";
        } else {
            std::cout << "Пользователь не найден.\n";
        }
    }
    else if (cmd == "/unmute") {
        std::string target;
        if (!(iss >> target)) {
            std::cout << "Использование: /unmute <ник>\n";
            return;
        }

        bool found = false;
        socket_t target_sock;
        std::string target_nick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.nickname == target) {
                    c.muted = false;
                    found = true;
                    target_sock = c.socket;
                    target_nick = c.nickname;
                    break;
                }
            }
        }

        if (found) {
            send_line(target_sock, SYS_COL + "Вам снова разрешили отправлять сообщения" + RESET);
            broadcast_global(SYS_COL + "Система: " + target_nick + " может снова говорить." + RESET);
            log_event("[АДМИН] " + target_nick + " размучен");
            std::cout << "Пользователь " << target_nick << " размучен.\n";
        } else {
            std::cout << "Пользователь не найден.\n";
        }
    }
    else if (cmd == "/quit") {
        std::cout << "Остановка сервера...\n";
        log_event("[АДМИН] Остановка сервера");
        server_running = false;

        if (listen_sock_global != INVALID_SOCK) {
            CLOSE_SOCKET(listen_sock_global);
            listen_sock_global = INVALID_SOCK;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                send_line(c.socket, SYS_COL + "Сервер остановлен. До свидания!" + RESET);
                CLOSE_SOCKET(c.socket);
            }
            clients.clear();
        }
    }
    else {
        std::cout << "Неизвестная команда. Введите /help для списка.\n";
    }
}

void accept_loop(int port) {
    listen_sock_global = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_global == INVALID_SOCK) {
        std::cerr << "Не удалось создать сокет" << std::endl;
        server_running = false;
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock_global, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Ошибка bind" << std::endl;
        CLOSE_SOCKET(listen_sock_global);
        listen_sock_global = INVALID_SOCK;
        server_running = false;
        return;
    }

    if (listen(listen_sock_global, SOMAXCONN) < 0) {
        std::cerr << "Ошибка listen" << std::endl;
        CLOSE_SOCKET(listen_sock_global);
        listen_sock_global = INVALID_SOCK;
        server_running = false;
        return;
    }

    std::cout << "Сервер запущен на порту " << port;
    if (max_clients_limit > 0)
        std::cout << ", максимум клиентов: " << max_clients_limit;
    else
        std::cout << ", без ограничений";
    std::cout << std::endl;
    std::cout << "Введите /help для списка команд администратора.\n";
    log_event("[СЕРВЕР] Сервер запущен на порту " + std::to_string(port));

    while (server_running) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_sock = accept(listen_sock_global, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_sock == INVALID_SOCK) {
            if (server_running) std::cerr << "Ошибка accept" << std::endl;
            break;
        }
        std::thread(handle_client, client_sock, client_addr).detach();
    }

    if (listen_sock_global != INVALID_SOCK) {
        CLOSE_SOCKET(listen_sock_global);
        listen_sock_global = INVALID_SOCK;
    }
    log_event("[СЕРВЕР] Сервер остановлен");
}

void run_client(const std::string& ip, int port) {
    std::string nick;
    std::cout << "Введите ваш ник (1-" << MAX_NICK_LENGTH << " символов, без пробелов и /): ";
    std::getline(std::cin, nick);
    while (nick.empty() || nick.length() > MAX_NICK_LENGTH ||
           nick.find_first_of(" /\r\n") != std::string::npos || nick == "Система") {
        std::cout << "Недопустимый ник. Введите снова (1-" << MAX_NICK_LENGTH << " символов): ";
        std::getline(std::cin, nick);
    }

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK) {
        std::cerr << "Не удалось создать сокет" << std::endl;
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    std::cout << "Подключение к " << ip << ":" << port << "..." << std::endl;
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Не удалось подключиться к серверу" << std::endl;
        CLOSE_SOCKET(sock);
        return;
    }

    if (!send_all(sock, nick + "\n")) {
        std::cerr << "Ошибка отправки ника" << std::endl;
        CLOSE_SOCKET(sock);
        return;
    }

    std::cout << "Подключено как '" << nick << "'. Введите /quit для выхода, /help для справки." << std::endl;

    std::atomic<bool> connected{true};
    std::mutex cout_mtx;

    std::thread recv_thr([&]() {
        std::string buf;
        char tmp[1024];
        while (connected) {
            int bytes = recv(sock, tmp, sizeof(tmp) - 1, 0);
            if (bytes <= 0) {
                {
                    std::lock_guard<std::mutex> lock(cout_mtx);
                    if (bytes == 0)
                        std::cout << SYS_COL << "[Соединение закрыто сервером]" << RESET << std::endl;
                    else
                        std::cout << ERR_COL << "[Ошибка приёма]" << RESET << std::endl;
                }
                connected = false;
                break;
            }
            tmp[bytes] = '\0';
            buf += tmp;
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string msg = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!msg.empty() && msg.back() == '\r') msg.pop_back();
                {
                    std::lock_guard<std::mutex> lock(cout_mtx);
                    std::cout << msg << RESET << std::endl;
                }
            }
        }
    });

    std::thread send_thr([&]() {
        std::string input;
        while (connected) {
            if (!std::getline(std::cin, input)) break;
            if (input.empty()) continue;
            input += '\n';
            if (!send_all(sock, input)) {
                {
                    std::lock_guard<std::mutex> lock(cout_mtx);
                    std::cout << ERR_COL << "[Ошибка отправки]" << RESET << std::endl;
                }
                connected = false;
                break;
            }
            if (input == "/quit\n") break;
        }
        connected = false;
    });

    send_thr.join();
    CLOSE_SOCKET(sock);
    recv_thr.join();
    std::cout << "Чат завершён." << std::endl;
}

#ifndef TESTING   // <-- исключаем main при сборке тестов
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Использование:\n"
                  << "  Сервер: " << argv[0] << " server [порт] [макс_клиентов(0=безлимит)] [лог_файл]\n"
                  << "  Клиент: " << argv[0] << " client <IP-адрес> [порт]\n"
                  << "По умолчанию: порт 12345, максимум клиентов 5" << std::endl;
        return 1;
    }

    if (!init_sockets()) {
        std::cerr << "Не удалось инициализировать сокеты" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    int port = 12345;
    int max_clients = 5;

    if (mode == "server") {
        if (argc >= 3) {
            bool ok;
            port = safe_stoi(argv[2], ok);
            if (!ok) {
                std::cerr << "Некорректный номер порта" << std::endl;
                cleanup_sockets();
                return 1;
            }
        }
        if (argc >= 4) {
            bool ok;
            max_clients = safe_stoi(argv[3], ok);
            if (!ok) {
                std::cerr << "Некорректное значение лимита клиентов" << std::endl;
                cleanup_sockets();
                return 1;
            }
            if (max_clients < 0) max_clients = 0;
        }
        if (argc >= 5) {
            log_file.open(argv[4], std::ios::app);
            if (log_file.is_open()) {
                logging_to_file = true;
            } else {
                std::cerr << "Не удалось открыть файл для логирования: " << argv[4] << std::endl;
            }
        }

        max_clients_limit = max_clients;
        server_running = true;
        accept_thread = std::thread(accept_loop, port);

        std::string admin_input;
        while (server_running) {
            if (!std::getline(std::cin, admin_input)) {
                break;
            }
            if (!admin_input.empty()) {
                process_admin_command(admin_input);
            }
        }

        if (accept_thread.joinable()) {
            accept_thread.join();
        }
        std::cout << "Сервер остановлен.\n";
    } else if (mode == "client") {
        if (argc < 3) {
            std::cerr << "Укажите IP-адрес сервера" << std::endl;
            cleanup_sockets();
            return 1;
        }
        std::string ip = argv[2];
        if (argc >= 4) {
            bool ok;
            port = safe_stoi(argv[3], ok);
            if (!ok) {
                std::cerr << "Некорректный номер порта" << std::endl;
                cleanup_sockets();
                return 1;
            }
        }
        run_client(ip, port);
    } else {
        std::cerr << "Неизвестный режим. Используйте 'server' или 'client'." << std::endl;
    }

    cleanup_sockets();
    return 0;
}
#endif