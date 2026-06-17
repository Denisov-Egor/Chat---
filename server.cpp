#include "common.hpp"

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
                my_room = c.room;
                break;
            }
        }
    }
    if (my_nick.empty()) return;

    if (cmd == "/help") {
        std::string help =
            "Команды:\n"
            "  /help, /list, /whisper <ник> <текст>, /nick <новый>,\n"
            "  /join <комната>, /leave, /rooms, /quit";
        send_line(sock, SYS_COL + help + RESET);
    }
    else if (cmd == "/list") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        int count = 0;
        for (auto& c : clients) if (c.room == my_room) ++count;
        std::string list = "Участники " + my_room + " (" + std::to_string(count) + "):\n";
        for (auto& c : clients) {
            if (c.room == my_room) {
                list += "  " + nick_color(c.nickname) + c.nickname + RESET;
                if (c.muted) list += " [muted]";
                list += "\n";
            }
        }
        send_line(sock, list);
    }
    else if (cmd == "/whisper") {
        std::string target, text;
        if (!(iss >> target)) {
            send_line(sock, ERR_COL + "/whisper <ник> <текст>" + RESET);
            return;
        }
        std::getline(iss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0,1);
        if (text.empty()) {
            send_line(sock, ERR_COL + "Сообщение не может быть пустым" + RESET);
            return;
        }
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.nickname == target) {
                    send_line(c.socket, nick_color(my_nick) + "[ЛС от " + my_nick + "] " + RESET + text);
                    found = true;
                    break;
                }
            }
        }
        if (!found) send_line(sock, ERR_COL + "Пользователь '" + target + "' не найден" + RESET);
    }
    else if (cmd == "/nick") {
        std::string new_nick;
        if (!(iss >> new_nick)) {
            send_line(sock, ERR_COL + "/nick <новый ник>" + RESET);
            return;
        }
        if (new_nick.empty() || new_nick.find_first_of(" /\r\n") != std::string::npos) {
            send_line(sock, ERR_COL + "Недопустимый ник" + RESET);
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
            for (auto& c : clients) if (c.socket == sock) { client = &c; break; }
            if (!client) return;
            for (auto& c : clients) if (c.nickname == new_nick) { nick_exists = true; break; }
            if (!nick_exists) {
                old_nick = client->nickname;
                client->nickname = new_nick;
            }
        }
        if (nick_exists) {
            send_line(sock, ERR_COL + "Ник '" + new_nick + "' уже занят" + RESET);
            return;
        }
        broadcast_room(my_room, old_nick + " теперь " + new_nick, INVALID_SOCK, "Система: ", SYS_COL);
    }
    else if (cmd == "/join") {
        std::string new_room;
        if (!(iss >> new_room)) {
            send_line(sock, ERR_COL + "/join <комната>" + RESET);
            return;
        }
        if (new_room[0] != '#') new_room = "#" + new_room;
        std::string old_room;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket == sock) {
                    old_room = c.room;
                    c.room = new_room;
                    break;
                }
            }
        }
        broadcast_room(old_room, my_nick + " покинул комнату.", INVALID_SOCK, "Система: ", SYS_COL);
        broadcast_room(new_room, my_nick + " присоединился к комнате.", INVALID_SOCK, "Система: ", SYS_COL);
        send_line(sock, SYS_COL + "Вы в комнате " + new_room + RESET);
    }
    else if (cmd == "/leave") {
        std::string old_room;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) {
                if (c.socket == sock) {
                    old_room = c.room;
                    c.room = "lobby";
                    break;
                }
            }
        }
        broadcast_room(old_room, my_nick + " покинул комнату.", INVALID_SOCK, "Система: ", SYS_COL);
        broadcast_room("lobby", my_nick + " вернулся в лобби.", INVALID_SOCK, "Система: ", SYS_COL);
        send_line(sock, SYS_COL + "Вы в лобби." + RESET);
    }
    else if (cmd == "/rooms") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        std::map<std::string, int> room_counts;
        for (auto& c : clients) room_counts[c.room]++;
        std::string list = "Комнаты:\n";
        for (auto& p : room_counts) {
            list += "  " + p.first + " (" + std::to_string(p.second) + ")\n";
        }
        send_line(sock, SYS_COL + list + RESET);
    }
    else {
        send_line(sock, ERR_COL + "Неизвестная команда. /help" + RESET);
    }
}

// ==================== ИСПРАВЛЕННАЯ handle_client ====================
void handle_client(socket_t sock, sockaddr_in client_addr) {
    static std::map<std::string, int> login_attempts;
    static std::mutex attempt_mtx;

    std::string ip = get_ip(client_addr);
    if (is_banned(ip)) {
        send_line(sock, ERR_COL + "Ваш IP временно заблокирован." + RESET);
        CLOSE_SOCKET(sock);
        return;
    }

    // Вспомогательная лямбда для чтения строки до разделителя \r или \n
    auto read_line = [&](std::string& line) -> bool {
        char ch;
        line.clear();
        while (true) {
            int n = recv(sock, &ch, 1, 0);
            if (n <= 0) return false;       // соединение закрыто или ошибка
            if (ch == '\n') break;          // конец строки (LF)
            if (ch == '\r') {
                // возможно, дальше идёт \n – пытаемся прочитать
                // ставим сокет в неблокирующий режим? Нет, оставим блокирующим с MSG_PEEK
                // проще: просто допускаем \r как конец строки, но если следующий символ \n, то его пропускаем
                // используем recv с MSG_DONTWAIT для проверки
                int next = recv(sock, &ch, 1, MSG_DONTWAIT);
                if (next == 1 && ch != '\n') {
                    // если следующий символ не \n, вернём его обратно? Не можем.
                    // Поэтому упростим: \r и \r\n оба считаются концом строки, а одиночный \r завершает, даже если за ним не \n
                    break;
                } else if (next == 1 && ch == '\n') {
                    // поглотили \n после \r, отлично
                    break;
                } else if (next == 0) {
                    return false; // соединение закрыто
                } else {
                    // ошибка или данных нет – просто завершаем по \r
                    break;
                }
            }
            line += ch;
        }
        // удаляем возможные пробельные символы в конце (на всякий случай)
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        return true;
    };

    // ---------- Чтение ника ----------
    std::string nick;
    if (!read_line(nick)) {
        CLOSE_SOCKET(sock);
        return;
    }

    // Проверка ника
    if (nick.empty() || nick == "Система" || nick.find('/') != std::string::npos) {
        send_line(sock, ERR_COL + "Недопустимый ник." + RESET);
        CLOSE_SOCKET(sock);
        return;
    }

    // ---------- Чтение пароля ----------
    send_line(sock, "Пароль (или новый для регистрации): ");
    std::string password;
    if (!read_line(password)) {
        CLOSE_SOCKET(sock);
        return;
    }

    // Хеширование и проверка
    size_t pass_hash = std::hash<std::string>{}(password);
    bool auth_ok = false;
    {
        std::lock_guard<std::mutex> lock(users_mtx);
        auto it = users.find(nick);
        if (it != users.end()) {
            if (it->second == pass_hash) auth_ok = true;
        } else {
            users[nick] = pass_hash;
            save_users();
            auth_ok = true;
        }
    }

    if (!auth_ok) {
        send_line(sock, ERR_COL + "Неверный пароль." + RESET);
        {
            std::lock_guard<std::mutex> lock(attempt_mtx);
            if (++login_attempts[ip] >= 3) {
                ban_ip(ip);
                login_attempts[ip] = 0;
            }
        }
        CLOSE_SOCKET(sock);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(attempt_mtx);
        login_attempts[ip] = 0;
    }

    // Добавление клиента
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        for (auto& c : clients) {
            if (c.nickname == nick) {
                send_line(sock, ERR_COL + "Этот ник уже используется." + RESET);
                CLOSE_SOCKET(sock);
                return;
            }
        }
        if (max_clients_limit > 0 && clients.size() >= (size_t)max_clients_limit) {
            send_line(sock, ERR_COL + "Чат заполнен." + RESET);
            CLOSE_SOCKET(sock);
            return;
        }
        clients.push_back(ClientInfo(sock, nick, ip));
    }

    send_line(sock, SYS_COL + "OK: Добро пожаловать, " + nick + "! Вы в #lobby." + RESET);
    broadcast_room("lobby", nick + " присоединился.", sock, "Система: ", SYS_COL);

    // ---------- Основной цикл приёма сообщений ----------
    std::string msg_buf;
    char data_chunk[1024];
    while (true) {
        int bytes = recv(sock, data_chunk, sizeof(data_chunk)-1, 0);
        if (bytes <= 0) {
            std::string my_nick;
            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) if (c.socket == sock) { my_nick = c.nickname; break; }
            }
            remove_client(sock, my_nick);
            return;
        }
        data_chunk[bytes] = '\0';
        msg_buf += data_chunk;

        size_t pos;
        while ((pos = msg_buf.find('\n')) != std::string::npos) {
            std::string line = msg_buf.substr(0, pos);
            msg_buf.erase(0, pos+1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::string cur_nick, cur_room;
            bool muted = false;
            {
                std::lock_guard<std::mutex> lock(clients_mtx);
                for (auto& c : clients) {
                    if (c.socket == sock) {
                        cur_nick = c.nickname;
                        cur_room = c.room;
                        muted = c.muted;
                        break;
                    }
                }
            }
            if (cur_nick.empty()) return;

            if (line == "/quit") {
                remove_client(sock, cur_nick);
                return;
            }

            if (line[0] == '/') {
                process_command(line, sock);
                continue;
            }

            if (muted) {
                send_line(sock, ERR_COL + "Вы заглушены." + RESET);
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
                               std::chrono::duration_cast<std::chrono::seconds>(now - c.msg_times.front()).count() >= 2)
                            c.msg_times.pop_front();
                        if (c.msg_times.size() >= 4) {
                            send_line(sock, ERR_COL + "Флуд-контроль. Подождите." + RESET);
                            drop = true;
                        }
                        break;
                    }
                }
            }
            if (drop) continue;

            broadcast_room(cur_room, cur_nick + ": " + line, INVALID_SOCK, "", nick_color(cur_nick));
        }
    }
}

void process_admin_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "/help") {
        std::cout << "Админ: /list /kick <ник> /ban <ник> /mute <ник> /unmute <ник> /deluser <ник> /quit\n";
    }
    else if (cmd == "/list") {
        std::lock_guard<std::mutex> lock(clients_mtx);
        std::cout << "Участники (" << clients.size() << "):\n";
        for (auto& c : clients) {
            std::cout << "  " << c.nickname << " [" << c.room << "]";
            if (c.muted) std::cout << " [muted]";
            std::cout << " (" << c.ip << ")\n";
        }
    }
    else if (cmd == "/kick") {
        std::string target;
        if (!(iss >> target)) { std::cout << "/kick <ник>\n"; return; }
        socket_t tsock = INVALID_SOCK; std::string tnick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto it = clients.begin(); it != clients.end(); ++it)
                if (it->nickname == target) { tsock = it->socket; tnick = it->nickname; clients.erase(it); break; }
        }
        if (tsock != INVALID_SOCK) {
            send_line(tsock, SYS_COL + "Вас исключили." + RESET);
            CLOSE_SOCKET(tsock);
            broadcast_all(tnick + " исключён админом.", INVALID_SOCK, "Система: ", SYS_COL);
            std::cout << tnick << " исключён.\n";
        } else std::cout << "Не найден.\n";
    }
    else if (cmd == "/ban") {
        std::string target;
        if (!(iss >> target)) { std::cout << "/ban <ник>\n"; return; }
        std::string ip_to_ban; socket_t tsock = INVALID_SOCK; std::string tnick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto it = clients.begin(); it != clients.end(); ++it)
                if (it->nickname == target) {
                    ip_to_ban = it->ip; tsock = it->socket; tnick = it->nickname;
                    clients.erase(it); break;
                }
        }
        if (!ip_to_ban.empty()) {
            ban_ip(ip_to_ban, 3600);
            send_line(tsock, SYS_COL + "Вы забанены." + RESET);
            CLOSE_SOCKET(tsock);
            broadcast_all(tnick + " забанен.", INVALID_SOCK, "Система: ", SYS_COL);
            std::cout << tnick << " забанен.\n";
        } else std::cout << "Не найден.\n";
    }
    else if (cmd == "/mute") {
        std::string target;
        if (!(iss >> target)) { std::cout << "/mute <ник>\n"; return; }
        bool found = false; socket_t tsock; std::string tnick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) if (c.nickname == target) { c.muted = true; found = true; tsock = c.socket; tnick = c.nickname; break; }
        }
        if (found) {
            send_line(tsock, SYS_COL + "Вы заглушены." + RESET);
            broadcast_all(tnick + " заглушен.", INVALID_SOCK, "Система: ", SYS_COL);
            std::cout << tnick << " заглушен.\n";
        } else std::cout << "Не найден.\n";
    }
    else if (cmd == "/unmute") {
        std::string target;
        if (!(iss >> target)) { std::cout << "/unmute <ник>\n"; return; }
        bool found = false; socket_t tsock; std::string tnick;
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            for (auto& c : clients) if (c.nickname == target) { c.muted = false; found = true; tsock = c.socket; tnick = c.nickname; break; }
        }
        if (found) {
            send_line(tsock, SYS_COL + "Вы снова можете говорить." + RESET);
            broadcast_all(tnick + " размучен.", INVALID_SOCK, "Система: ", SYS_COL);
            std::cout << tnick << " размучен.\n";
        } else std::cout << "Не найден.\n";
    }
    else if (cmd == "/deluser") {
        std::string target;
        if (!(iss >> target)) { std::cout << "/deluser <ник>\n"; return; }
        {
            std::lock_guard<std::mutex> lock(users_mtx);
            if (users.erase(target)) {
                save_users();
                std::cout << "Пользователь " << target << " удалён.\n";
            } else std::cout << "Не найден.\n";
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
    else std::cout << "Неизвестная команда.\n";
}

void accept_loop(int port) {
    listen_sock_global = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_global == INVALID_SOCK) { std::cerr << "Ошибка сокета\n"; server_running = false; return; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_sock_global, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Ошибка bind\n"; CLOSE_SOCKET(listen_sock_global); server_running = false; return;
    }
    if (listen(listen_sock_global, SOMAXCONN) < 0) {
        std::cerr << "Ошибка listen\n"; CLOSE_SOCKET(listen_sock_global); server_running = false; return;
    }
    std::cout << "Сервер на порту " << port << " (макс " << max_clients_limit << ")\n";
    std::cout << "Админ-команды: /help\n";

    while (server_running) {
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        socket_t client_sock = accept(listen_sock_global, (sockaddr*)&client_addr, &len);
        if (client_sock == INVALID_SOCK) {
            if (server_running) std::cerr << "Ошибка accept\n";
            break;
        }
        std::thread(handle_client, client_sock, client_addr).detach();
    }
    if (listen_sock_global != INVALID_SOCK) CLOSE_SOCKET(listen_sock_global);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "server <порт> [макс]\n";
        return 1;
    }
    if (!init_sockets()) return 1;
    int port = std::stoi(argv[1]);
    if (argc >= 3) max_clients_limit = std::stoi(argv[2]);
    load_users();
    log_file.open("chat.log", std::ios::app);
    server_running = true;
    accept_thread = std::thread(accept_loop, port);
    std::string admin_input;
    while (server_running) {
        if (!std::getline(std::cin, admin_input)) break;
        if (!admin_input.empty()) process_admin_command(admin_input);
    }
    if (accept_thread.joinable()) accept_thread.join();
    log_file.close();
    cleanup_sockets();
    std::cout << "Сервер остановлен.\n";
    return 0;
}