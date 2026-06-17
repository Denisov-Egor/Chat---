#include "common.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "client <IP> [порт]\n";
        return 1;
    }
    if (!init_sockets()) return 1;
    std::string ip = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 12345;

    std::string nick;
    std::cout << "Ваш ник: ";
    std::getline(std::cin, nick);
    while (nick.empty() || nick.find_first_of(" /\r\n") != std::string::npos || nick == "Система") {
        std::cout << "Некорректный ник. Введите ещё раз: ";
        std::getline(std::cin, nick);
    }

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK) { std::cerr << "Ошибка сокета\n"; return 1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Не удалось подключиться\n";
        CLOSE_SOCKET(sock); cleanup_sockets(); return 1;
    }

    if (!send_all(sock, nick + "\n")) { std::cerr << "Ошибка отправки\n"; CLOSE_SOCKET(sock); cleanup_sockets(); return 1; }

    char buf[1024];
    int r = recv(sock, buf, sizeof(buf)-1, 0);
    if (r <= 0) { std::cerr << "Соединение закрыто\n"; CLOSE_SOCKET(sock); cleanup_sockets(); return 1; }
    buf[r] = '\0';
    std::string prompt(buf);
    if (!prompt.empty() && prompt.back() == '\n') prompt.pop_back();
    std::cout << prompt << " ";
    std::string password;
    std::getline(std::cin, password);
    if (!send_all(sock, password + "\n")) { std::cerr << "Ошибка отправки\n"; CLOSE_SOCKET(sock); cleanup_sockets(); return 1; }

    std::string response;
    char tmp[1024];
    int bytes = recv(sock, tmp, sizeof(tmp)-1, 0);
    if (bytes <= 0) { std::cerr << "Соединение закрыто\n"; CLOSE_SOCKET(sock); cleanup_sockets(); return 1; }
    tmp[bytes] = '\0';
    response = tmp;
    size_t nl = response.find('\n');
    if (nl != std::string::npos) response = response.substr(0, nl);

    if (response.find("OK:") != 0) {
        std::cout << response << std::endl;
        CLOSE_SOCKET(sock); cleanup_sockets(); return 1;
    }

    std::cout << response.substr(4) << std::endl;

    std::atomic<bool> connected{true};
    std::mutex cout_mtx;

    std::thread recv_thr([&]() {
        std::string data;
        char tmp[1024];
        while (connected) {
            int bytes = recv(sock, tmp, sizeof(tmp)-1, 0);
            if (bytes <= 0) {
                std::lock_guard<std::mutex> lock(cout_mtx);
                std::cout << SYS_COL << "[Соединение закрыто]" << RESET << std::endl;
                connected = false;
                break;
            }
            tmp[bytes] = '\0';
            data += tmp;
            size_t pos;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string msg = data.substr(0, pos);
                data.erase(0, pos+1);
                if (!msg.empty() && msg.back() == '\r') msg.pop_back();

                std::string highlighted;
                size_t nick_pos = 0;
                while (nick_pos < msg.size()) {
                    size_t found = msg.find(nick, nick_pos);
                    if (found != std::string::npos) {
                        highlighted += msg.substr(nick_pos, found - nick_pos);
                        highlighted += "\033[1;33m" + nick + RESET + BELL;
                        nick_pos = found + nick.size();
                    } else {
                        highlighted += msg.substr(nick_pos);
                        break;
                    }
                }
                std::lock_guard<std::mutex> lock(cout_mtx);
                std::cout << highlighted << std::endl;
            }
        }
    });

    std::thread send_thr([&]() {
        std::string input;
        while (connected) {
            if (!std::getline(std::cin, input)) break;
            if (input.empty()) continue;
            if (!send_all(sock, input + "\n")) {
                std::lock_guard<std::mutex> lock(cout_mtx);
                std::cout << ERR_COL << "[Ошибка отправки]" << RESET << std::endl;
                connected = false;
                break;
            }
            if (input == "/quit") break;
        }
        connected = false;
    });

    send_thr.join();
    CLOSE_SOCKET(sock);
    recv_thr.join();
    std::cout << "Чат завершён." << std::endl;
    cleanup_sockets();
    return 0;
}