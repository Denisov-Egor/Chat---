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

// Кроссплатформенные сокеты
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

// Цвета для вывода
std::string nick_color(const std::string& nick) {
    if (nick.empty()) return "\033[0m";
    const char* colors[] = {
        "\033[91m", "\033[92m", "\033[93m", "\033[94m",
        "\033[95m", "\033[96m", "\033[31m", "\033[32m",
        "\033[33m", "\033[34m", "\033[35m", "\033[36m"
    };
    int idx = std::tolower(nick[0]) % 12;
    return colors[idx];
}

const std::string RESET   = "\033[0m";
const std::string SYS_COL = "\033[90m";
const std::string ERR_COL = "\033[91m";

// Глобальные данные сервера
std::mutex clients_mtx;
std::mutex banned_mtx;

struct ClientInfo {
    socket_t socket;
    std::string nickname;
    std::string ip;
    bool muted = false;
    std::deque<std::chrono::steady_clock::time_point> msg_times;

    ClientInfo(socket_t s, const std::string& n, const std::string& i)
        : socket(s), nickname(n), ip(i) {}
};

std::vector<ClientInfo> clients;
std::set<std::string> banned_ips;
int max_clients_limit = 5;

// Управление сервером
std::atomic<bool> server_running{false};
socket_t listen_sock_global = INVALID_SOCK;
std::thread accept_thread;

// Вспомогательные функции
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

void broadcast(const std::string& message, socket_t exclude = INVALID_SOCK,
               const std::string& prefix = "", const std::string& color = "") {
    std::lock_guard<std::mutex> lock(clients_mtx);
    for (auto& c : clients) {
        if (c.socket != exclude) {
            std::string full = color + prefix + message + RESET;
            send_line(c.socket, full);
        }
    }
}

// Удаление клиента из списка (вызывается только при реальном выходе)
void remove_client(socket_t sock, const std::string& nickname) {
    bool existed = false;
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        auto it = std::remove_if(clients.begin(), clients.end(),
                                 [sock](const ClientInfo& c) { return c.socket == sock; });
        if (it != clients.end()) {
            clients.erase(it, clients.end());
            existed = true;
        }
    }
    CLOSE_SOCKET(sock);
    if (existed && !nickname.empty()) {
        broadcast(nickname + " покинул чат.", INVALID_SOCK, "Система: ", SYS_COL);
    }
}

// Обработка команд от клиентов (использует сокет, не ссылку на ClientInfo)
void process_command(const std::string& line, socket_t sock) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    // Поиск клиента под мьютексом
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
    if (my_nick.empty()) return; // клиент уже удалён

    if (cmd == "/help") {
        std::string help =
            "Доступные команды:\n"
            "  /help          - эта справка\n"
            "  /list          - список участников\n"
            "  /whisper <ник> <текст> - личное сообщение\n"
            "  /nick <новый>  - сменить ник\n"
            "  /quit          - выйти из чата\n";
        send_line(sock, SYS_COL + help + RESET);
    }
    else if (cmd == "/list") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        std::string list = "Участники (" + std::to_string(clients.size()) + "):\n";
        for (auto& c : clients) {
            list += "  " + nick_color(c.nickname) + c.nickname + RESET;
            if (c.muted) list += " [muted]";
            list += "\n";
        }
        send_line(sock, list);
    }
    else if (cmd == "/whisper") {
        std::string target, text;
        if (!(iss >> target)) {
            send_line(sock, ERR_COL + "Использование: /whisper <ник> <текст>" + RESET);
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
        if (new_nick.empty() || new_nick.find_first_of(" /\r\n") != std::string::npos) {
            send_line(sock, ERR_COL + "Ник не должен содержать пробелы или запрещённые символы" + RESET);
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
            // Проверяем, существует ли клиент
            ClientInfo* client = nullptr;
            for (auto& c : clients) {
                if (c.socket == sock) {
                    client = &c;
                    break;
                }
            }
            if (!client) return; // клиент удалён

            // Проверяем занятость нового ника
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

        broadcast(old_nick + " теперь известен как " + new_nick, INVALID_SOCK, "Система: ", SYS_COL);
    }
    else {
        send_line(sock, ERR_COL + "Неизвестная команда. Введите /help для списка" + RESET);
    }
}

// Поток для одного клиента
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

    // Получение ника
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
            if (nick.empty() || nick == "Система" || nick.find_first_of("/\r\n") != std::string::npos) {
                send_line(sock, ERR_COL + "Недопустимый ник." + RESET);
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
            broadcast(nick + " присоединился к чату.", sock, "Система: ", SYS_COL);
            break;
        }
    }

    // Приём сообщений
    std::string msg_buffer;
    char data_chunk[1024];
    while (true) {
        int bytes = recv(sock, data_chunk, sizeof(data_chunk) - 1, 0);
        if (bytes <= 0) {
            // Соединение разорвано – пытаемся удалить клиента, если он ещё в списке
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
            remove_client(sock, my_nick); // если ник пуст – не будет сообщения
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

            // Получаем актуальные данные клиента под блокировкой
            std::string nick;
            bool muted = false;
            bool exists = false;
            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) {
                    if (c.socket == sock) {
                        nick = c.nickname;
                        muted = c.muted;
                        exists = true;
                        break;
                    }
                }
            }

            if (!exists) {
                // Клиент был удалён администратором – просто выходим
                return;
            }

            if (line == "/quit") {
                broadcast(nick + " покинул чат.", INVALID_SOCK, "Система: ", SYS_COL);
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

            // Защита от флуда
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
            broadcast(nick + ": " + line, INVALID_SOCK, "", color);
        }
    }
}

// Обработка команд администратора сервера (без изменений, уже безопасна)
void process_admin_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "/help") {
        std::cout << "Команды администратора:\n"
                  << "  /list          - список участников\n"
                  << "  /kick <ник>    - выгнать участника\n"
                  << "  /ban <ник>     - забанить по IP\n"
                  << "  /mute <ник>    - запретить сообщения\n"
                  << "  /unmute <ник>  - разрешить сообщения\n"
                  << "  /quit          - остановить сервер\n";
    }
    else if (cmd == "/list") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        std::cout << "Участники (" << clients.size() << "):\n";
        for (auto& c : clients) {
            std::cout << "  " << c.nickname;
            if (c.muted) std::cout << " [muted]";
            std::cout << " (" << c.ip << ")\n";
        }
    }
    else if (cmd == "/kick") {
        std::string target;
        if (!(iss >> target)) {
            std::cout << "Использование: /kick <ник>\n";
            return;
        }

        socket_t target_sock = INVALID_SOCK;
        std::string target_nick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (it->nickname == target) {
                    target_sock = it->socket;
                    target_nick = it->nickname;
                    clients.erase(it);
                    break;
                }
            }
        }

        if (target_sock != INVALID_SOCK) {
            send_line(target_sock, SYS_COL + "Вас исключили из чата" + RESET);
            CLOSE_SOCKET(target_sock);
            broadcast(target_nick + " исключён администратором.", INVALID_SOCK, "Система: ", SYS_COL);
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
        std::string target_nick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (it->nickname == target) {
                    ip_to_ban = it->ip;
                    target_sock = it->socket;
                    target_nick = it->nickname;
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
            broadcast(target_nick + " забанен администратором.", INVALID_SOCK, "Система: ", SYS_COL);
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
            broadcast(target_nick + " заглушен администратором.", INVALID_SOCK, "Система: ", SYS_COL);
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
            broadcast(target_nick + " может снова говорить.", INVALID_SOCK, "Система: ", SYS_COL);
            std::cout << "Пользователь " << target_nick << " размучен.\n";
        } else {
            std::cout << "Пользователь не найден.\n";
        }
    }
    else if (cmd == "/quit") {
        std::cout << "Остановка сервера...\n";
        server_running = false;
        if (listen_sock_global != INVALID_SOCK) {
            CLOSE_SOCKET(listen_sock_global);
            listen_sock_global = INVALID_SOCK;
        }
    }
    else {
        std::cout << "Неизвестная команда. Введите /help для списка.\n";
    }
}

// Поток приёма подключений
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
}

// Клиент (без изменений)
void run_client(const std::string& ip, int port) {
    std::string nick;
    std::cout << "Введите ваш ник: ";
    std::getline(std::cin, nick);
    while (nick.empty() || nick.find_first_of(" /\r\n") != std::string::npos || nick == "Система") {
        std::cout << "Ник не может быть пустым, содержать пробелы или '/'. Введите ещё раз: ";
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

// main
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Использование:\n"
                  << "  Сервер: " << argv[0] << " server [порт] [макс_клиентов(0=безлимит)]\n"
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
        if (argc >= 3) port = std::stoi(argv[2]);
        if (argc >= 4) max_clients = std::stoi(argv[3]);
        if (max_clients < 0) max_clients = 0;
        max_clients_limit = max_clients;

        server_running = true;
        accept_thread = std::thread(accept_loop, port);

        // Главный поток — консоль администратора
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
        if (argc >= 4) port = std::stoi(argv[3]);
        run_client(ip, port);
    } else {
        std::cerr << "Неизвестный режим. Используйте 'server' или 'client'." << std::endl;
    }

    cleanup_sockets();
    return 0;
}