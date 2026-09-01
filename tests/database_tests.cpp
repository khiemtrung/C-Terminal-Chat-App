#include "chat_db.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

std::string temp_db_path() {
    const std::string path = "database_test_" + std::to_string(
        static_cast<long long>(reinterpret_cast<std::uintptr_t>(&path))) + ".db";
    return path;
}

void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-journal").c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

} // namespace

int main() {
    const std::string db_path = temp_db_path();
    cleanup(db_path);

    {
        ChatDatabase db(db_path);
        assert(db.open());

        // --- Registration ----------------------------------------------------
        std::string error;
        assert(db.register_user("alice", "password123", error));
        assert(db.register_user("bob", "secret1", error));

        // Duplicate usernames are rejected.
        assert(!db.register_user("alice", "other123", error));
        assert(error.find("taken") != std::string::npos);

        // Invalid credentials are rejected; valid ones succeed.
        assert(!db.authenticate_user("alice", "wrongpass", error));
        assert(db.authenticate_user("alice", "password123", error));
        assert(db.authenticate_user("bob", "secret1", error));
        assert(!db.authenticate_user("nobody", "password123", error));

        // --- Message history -------------------------------------------------
        ChatMessage message;
        message.room = "general";
        message.username = "alice";
        message.content = "hello room";
        message.timestamp = 1000;
        assert(db.add_message(message));

        message.room = "general";
        message.username = "bob";
        message.content = "hi alice";
        message.timestamp = 1001;
        assert(db.add_message(message));

        message.room = "dev";
        message.username = "bob";
        message.content = "let's build";
        message.timestamp = 1002;
        assert(db.add_message(message));

        // Recent history is returned oldest-first.
        std::vector<ChatMessage> history = db.get_recent_messages("general", 50);
        assert(history.size() == 2);
        assert(history[0].content == "hello room");
        assert(history[1].content == "hi alice");
        assert(history[0].room == "general");
        assert(history[0].username == "alice");

        // Limit is honored.
        history = db.get_recent_messages("general", 1);
        assert(history.size() == 1);
        assert(history[0].content == "hi alice");

        // Rooms are listed per distinct room.
        const std::vector<std::string> rooms = db.list_rooms(100);
        assert(rooms.size() == 2);
        assert(rooms[0] == "dev");
        assert(rooms[1] == "general");

        // --- Direct messages (persisted via a synthetic "dm:a:b" room key) ---
        ChatMessage dm;
        dm.room = "dm:alice:bob";
        dm.username = "alice";
        dm.content = "hey bob, private note";
        dm.timestamp = 2000;
        assert(db.add_message(dm));

        dm.username = "bob";
        dm.content = "got it, thanks";
        dm.timestamp = 2001;
        assert(db.add_message(dm));

        const std::vector<ChatMessage> dm_history = db.get_recent_messages("dm:alice:bob", 50);
        assert(dm_history.size() == 2);
        assert(dm_history[0].username == "alice");
        assert(dm_history[0].content == "hey bob, private note");
        assert(dm_history[1].username == "bob");
        assert(dm_history[1].content == "got it, thanks");

        // The DM thread does not leak into the regular room's history.
        const std::vector<ChatMessage> general_history = db.get_recent_messages("general", 50);
        assert(general_history.size() == 2);
    }

    cleanup(db_path);

    // Re-opening the same file persists the account (re-register must fail).
    {
        ChatDatabase db(db_path);
        assert(db.open());
        std::string error;
        assert(db.register_user("carol", "persist1", error));

        ChatDatabase reopened(db_path);
        assert(reopened.open());
        assert(!reopened.register_user("carol", "persist1", error));
        assert(reopened.authenticate_user("carol", "persist1", error));
        assert(!reopened.authenticate_user("carol", "wrong", error));
    }

    cleanup(db_path);
    std::cout << "All database tests passed." << std::endl;
    return 0;
}
