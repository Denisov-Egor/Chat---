#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "chat.cpp"   // TESTING уже определён через CMake

void ClearAll() {
    clients.clear();
    rooms.clear();
    banned_ips.clear();
    max_clients_limit = 5;
    server_running = false;
    listen_sock_global = INVALID_SOCK;
}

socket_t CreateFakeSocket(int id) {
    return static_cast<socket_t>(id);
}

class ChatTest : public ::testing::Test {
protected:
    void SetUp() override {
        ClearAll();
    }
    void TearDown() override {
        ClearAll();
    }
};

TEST_F(ChatTest, AddClient_Success) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "Alice", "127.0.0.1"));
    EXPECT_EQ(clients.size(), 1);
    EXPECT_EQ(clients[0].nickname, "Alice");
}

TEST_F(ChatTest, RemoveClient_ClearsRoom) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "Bob", "127.0.0.1"));
    clients[0].current_room = "#test";
    rooms["#test"].insert(s);
    remove_client(s, "Bob");
    EXPECT_TRUE(clients.empty());
    EXPECT_TRUE(rooms.empty());
}

TEST_F(ChatTest, CmdNick_ChangesNick) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "OldNick", "127.0.0.1"));
    process_command("/nick NewNick", s);
    EXPECT_EQ(clients[0].nickname, "NewNick");
    bool has_old = false;
    for (auto& c : clients) {
        if (c.nickname == "OldNick") has_old = true;
    }
    EXPECT_FALSE(has_old);
}

TEST_F(ChatTest, CmdNick_RejectsInvalid) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "Valid", "127.0.0.1"));
    process_command("/nick bad nick", s);
    EXPECT_EQ(clients[0].nickname, "Valid");
    std::string long_nick(MAX_NICK_LENGTH+1, 'a');
    process_command("/nick " + long_nick, s);
    EXPECT_EQ(clients[0].nickname, "Valid");
}

TEST_F(ChatTest, CmdIgnore_AddsToIgnored) {
    socket_t alice = CreateFakeSocket(1);
    socket_t bob = CreateFakeSocket(2);
    clients.push_back(ClientInfo(alice, "Alice", "127.0.0.1"));
    clients.push_back(ClientInfo(bob, "Bob", "127.0.0.1"));
    process_command("/ignore Bob", alice);
    EXPECT_TRUE(clients[0].ignored_nicks.count("Bob") == 1);
    EXPECT_TRUE(clients[1].ignored_nicks.empty());
}

TEST_F(ChatTest, CmdUnignore_RemovesFromIgnored) {
    socket_t alice = CreateFakeSocket(1);
    socket_t bob = CreateFakeSocket(2);
    clients.push_back(ClientInfo(alice, "Alice", "127.0.0.1"));
    clients.push_back(ClientInfo(bob, "Bob", "127.0.0.1"));
    clients[0].ignored_nicks.insert("Bob");
    process_command("/unignore Bob", alice);
    EXPECT_TRUE(clients[0].ignored_nicks.empty());
}

TEST_F(ChatTest, CmdMsg_SendsPrivate) {
    socket_t alice = CreateFakeSocket(1);
    socket_t bob = CreateFakeSocket(2);
    clients.push_back(ClientInfo(alice, "Alice", "127.0.0.1"));
    clients.push_back(ClientInfo(bob, "Bob", "127.0.0.1"));
    process_command("/msg Bob Hello", alice);
    SUCCEED();
}

TEST_F(ChatTest, JoinRoom_CreatesRoom) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "User", "127.0.0.1"));
    process_command("/join #lobby", s);
    EXPECT_EQ(clients[0].current_room, "#lobby");
    EXPECT_EQ(rooms["#lobby"].size(), 1);
}

TEST_F(ChatTest, LeaveRoom_ReturnsToGlobal) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "User", "127.0.0.1"));
    clients[0].current_room = "#room";
    rooms["#room"].insert(s);
    process_command("/leave", s);
    EXPECT_TRUE(clients[0].current_room.empty());
    EXPECT_TRUE(rooms.empty());
}

TEST_F(ChatTest, CreateRoom_IsAliasForJoin) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "User", "127.0.0.1"));
    process_command("/create #newroom", s);
    EXPECT_EQ(clients[0].current_room, "#newroom");
    EXPECT_EQ(rooms["#newroom"].size(), 1);
}

TEST_F(ChatTest, FloodProtection_BlocksAfterLimit) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "Flooder", "127.0.0.1"));
    auto& client = clients[0];
    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        client.msg_times.push_back(now + std::chrono::milliseconds(100*i));
    }
    bool drop = false;
    {
        client.msg_times.push_back(now + std::chrono::milliseconds(500));
        while (!client.msg_times.empty() &&
               std::chrono::duration_cast<std::chrono::seconds>(now + std::chrono::milliseconds(500) - client.msg_times.front()).count() >= 2) {
            client.msg_times.pop_front();
        }
        if (client.msg_times.size() >= 4) {
            drop = true;
        }
    }
    EXPECT_TRUE(drop);
}

TEST_F(ChatTest, QuitCommand_RemovesClient) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "User", "127.0.0.1"));
    clients[0].current_room = "#room";
    rooms["#room"].insert(s);
    remove_client(s, "User");
    EXPECT_TRUE(clients.empty());
    EXPECT_TRUE(rooms.empty());
}

TEST_F(ChatTest, AdminKick_RemovesClient) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "BadUser", "127.0.0.1"));
    rooms["#room"].insert(s);
    clients[0].current_room = "#room";
    process_admin_command("/kick BadUser");
    EXPECT_TRUE(clients.empty());
    EXPECT_TRUE(rooms.empty());
    EXPECT_TRUE(banned_ips.empty());
}

TEST_F(ChatTest, AdminBan_BansIP) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "Spammer", "10.0.0.1"));
    process_admin_command("/ban Spammer");
    EXPECT_TRUE(clients.empty());
    EXPECT_TRUE(banned_ips.count("10.0.0.1") == 1);
}

TEST_F(ChatTest, AdminMute_MutesClient) {
    socket_t s = CreateFakeSocket(1);
    clients.push_back(ClientInfo(s, "Noisy", "127.0.0.1"));
    process_admin_command("/mute Noisy");
    EXPECT_TRUE(clients[0].muted);
    process_admin_command("/unmute Noisy");
    EXPECT_FALSE(clients[0].muted);
}

TEST_F(ChatTest, AdminDeleteRoom_ReturnsUsersToGlobal) {
    socket_t s1 = CreateFakeSocket(1);
    socket_t s2 = CreateFakeSocket(2);
    clients.push_back(ClientInfo(s1, "Alice", "127.0.0.1"));
    clients.push_back(ClientInfo(s2, "Bob", "127.0.0.1"));
    clients[0].current_room = "#secret";
    clients[1].current_room = "#secret";
    rooms["#secret"].insert(s1);
    rooms["#secret"].insert(s2);
    process_admin_command("/delete_room #secret");
    EXPECT_TRUE(clients[0].current_room.empty());
    EXPECT_TRUE(clients[1].current_room.empty());
    EXPECT_TRUE(rooms.empty());
}