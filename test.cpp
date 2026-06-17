#include <gtest/gtest.h>
#include "common.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <fstream>

class ChatTest : public ::testing::Test {
protected:
    void SetUp() override {
        {
            std::lock_guard<std::mutex> lock(clients_mtx);
            clients.clear();
        }
        {
            std::lock_guard<std::mutex> lock(banned_mtx);
            banned_ips.clear();
        }
        {
            std::lock_guard<std::mutex> lock(users_mtx);
            users.clear();
        }
        extern std::string users_filename;
        users_filename = "test_users.txt";
        std::remove("test_users.txt");
        std::remove("test_log.txt");

        extern std::ofstream log_file;
        log_file.close();
        log_file.open("test_log.txt", std::ios::app);
    }

    void TearDown() override {
        extern std::ofstream log_file;
        log_file.close();
        std::remove("test_users.txt");
        std::remove("test_log.txt");
    }
};

TEST_F(ChatTest, BanIp_AddsToBanlist) {
    std::string ip = "192.168.1.1";
    EXPECT_FALSE(is_banned(ip));
    ban_ip(ip, 60);
    EXPECT_TRUE(is_banned(ip));
}

TEST_F(ChatTest, BanIp_ZeroSecondsExpiresImmediately) {
    std::string ip = "10.0.0.1";
    ban_ip(ip, 0);
    EXPECT_FALSE(is_banned(ip));
}

TEST_F(ChatTest, BanIp_ExpiredAfterTime) {
    std::string ip = "172.16.0.1";
    ban_ip(ip, 1);
    EXPECT_TRUE(is_banned(ip));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(is_banned(ip));
}

TEST_F(ChatTest, BanIp_OverrideExtends) {
    std::string ip = "10.10.10.10";
    ban_ip(ip, 60);
    ban_ip(ip, 120);
    EXPECT_TRUE(is_banned(ip));
}

TEST_F(ChatTest, LoadUsers_EmptyFile) {
    load_users();
    std::lock_guard<std::mutex> lock(users_mtx);
    EXPECT_TRUE(users.empty());
}

TEST_F(ChatTest, SaveAndLoadUsers_PreservesData) {
    {
        std::lock_guard<std::mutex> lock(users_mtx);
        users["alice"] = 12345;
        users["bob"]   = 67890;
    }
    save_users();
    {
        std::lock_guard<std::mutex> lock(users_mtx);
        users.clear();
    }
    load_users();
    std::lock_guard<std::mutex> lock(users_mtx);
    ASSERT_EQ(users.size(), 2u);
    EXPECT_EQ(users["alice"], 12345u);
    EXPECT_EQ(users["bob"], 67890u);
}

TEST_F(ChatTest, LogMessage_WritesToFile) {
    log_message("Тестовое сообщение");
    extern std::ofstream log_file;
    log_file.close();

    std::ifstream infile("test_log.txt");
    std::string content((std::istreambuf_iterator<char>(infile)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("Тестовое сообщение"), std::string::npos);
}

std::pair<socket_t, socket_t> create_socket_pair() {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    return {sv[0], sv[1]};
}

TEST_F(ChatTest, SendAll_SendsDataCorrectly) {
    auto p = create_socket_pair();
    socket_t reader = p.first;
    socket_t writer = p.second;
    std::string msg = "Hello, World!";
    EXPECT_TRUE(send_all(writer, msg));
    char buf[128] = {0};
    int bytes = recv(reader, buf, sizeof(buf) - 1, 0);
    EXPECT_GT(bytes, 0);
    EXPECT_EQ(std::string(buf, bytes), msg);
    CLOSE_SOCKET(reader);
    CLOSE_SOCKET(writer);
}

TEST_F(ChatTest, SendLine_AppendsNewline) {
    auto p = create_socket_pair();
    socket_t reader = p.first;
    socket_t writer = p.second;
    EXPECT_TRUE(send_line(writer, "Test"));
    char buf[128] = {0};
    int bytes = recv(reader, buf, sizeof(buf) - 1, 0);
    EXPECT_GT(bytes, 0);
    std::string received(buf, bytes);
    EXPECT_EQ(received, "Test\n");
    CLOSE_SOCKET(reader);
    CLOSE_SOCKET(writer);
}

TEST_F(ChatTest, BroadcastRoom_SendsOnlyToRoomMembers) {
    auto p1 = create_socket_pair();
    socket_t sock1 = p1.first, reader1 = p1.second;
    auto p2 = create_socket_pair();
    socket_t sock2 = p2.first, reader2 = p2.second;

    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        clients.push_back(ClientInfo(sock1, "Alice", "127.0.0.1"));
        clients.push_back(ClientInfo(sock2, "Bob", "127.0.0.1"));
        clients.back().room = "games";
    }

    broadcast_room("lobby", "Hello lobby", INVALID_SOCK, "", "");

    char buf[256] = {0};
    int bytes1 = recv(reader1, buf, sizeof(buf)-1, MSG_DONTWAIT);
    EXPECT_GT(bytes1, 0);
    EXPECT_NE(std::string(buf, bytes1).find("Hello lobby"), std::string::npos);

    int bytes2 = recv(reader2, buf, sizeof(buf)-1, MSG_DONTWAIT);
    EXPECT_LE(bytes2, 0);

    CLOSE_SOCKET(reader1); CLOSE_SOCKET(sock1);
    CLOSE_SOCKET(reader2); CLOSE_SOCKET(sock2);
}

TEST_F(ChatTest, RemoveClient_BroadcastsDeparture) {
    auto p = create_socket_pair();
    socket_t sock = p.first, reader = p.second;
    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        clients.push_back(ClientInfo(sock, "Charlie", "10.0.0.1"));
    }

    remove_client(sock, "Charlie");

    extern std::ofstream log_file;
    log_file.close();

    std::ifstream infile("test_log.txt");
    std::string content((std::istreambuf_iterator<char>(infile)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("Charlie покинул чат"), std::string::npos);

    CLOSE_SOCKET(reader);
}

TEST_F(ChatTest, NickValidation_RejectsEmptyOrSpaces) {
    auto is_valid = [](const std::string& nick) -> bool {
        return !nick.empty() && nick != "Система" && nick.find('/') == std::string::npos
               && nick.find_first_of(" \r\n") == std::string::npos;
    };
    EXPECT_FALSE(is_valid(""));
    EXPECT_FALSE(is_valid(" "));
    EXPECT_FALSE(is_valid("Система"));
    EXPECT_FALSE(is_valid("bad/nick"));
    EXPECT_TRUE(is_valid("ValidNick"));
    EXPECT_TRUE(is_valid("User_123"));
}

TEST_F(ChatTest, UniqueNickInClients_PreventsDuplicate) {
    auto add_client = [](socket_t sock, const std::string& nick, const std::string& ip) -> bool {
        std::lock_guard<std::mutex> lock(clients_mtx);
        for (auto& c : clients) {
            if (c.nickname == nick) return false;
        }
        if (max_clients_limit > 0 && clients.size() >= (size_t)max_clients_limit) return false;
        clients.push_back(ClientInfo(sock, nick, ip));
        return true;
    };
    EXPECT_TRUE(add_client(1, "Alice", "127.0.0.1"));
    EXPECT_FALSE(add_client(2, "Alice", "127.0.0.2"));
    EXPECT_EQ(clients.size(), 1u);
}

TEST_F(ChatTest, MaxClientsLimit_Enforced) {
    max_clients_limit = 2;
    auto add_client = [](socket_t sock, const std::string& nick, const std::string& ip) -> bool {
        std::lock_guard<std::mutex> lock(clients_mtx);
        if (max_clients_limit > 0 && clients.size() >= (size_t)max_clients_limit) return false;
        clients.push_back(ClientInfo(sock, nick, ip));
        return true;
    };
    EXPECT_TRUE(add_client(1, "One", "1.1.1.1"));
    EXPECT_TRUE(add_client(2, "Two", "2.2.2.2"));
    EXPECT_FALSE(add_client(3, "Three", "3.3.3.3"));
    EXPECT_EQ(clients.size(), 2u);
}

int main(int argc, char **argv) {
    init_sockets();
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    cleanup_sockets();
    return result;
}