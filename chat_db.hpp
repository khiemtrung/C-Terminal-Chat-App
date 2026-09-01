#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "protocol.hpp"

// Owns the SQLite connection used for user accounts and room message history.
// All public methods are thread-safe (operations are serialized internally).
class ChatDatabase {
public:
    explicit ChatDatabase(std::string path);
    ~ChatDatabase();

    ChatDatabase(const ChatDatabase&) = delete;
    ChatDatabase& operator=(const ChatDatabase&) = delete;

    // Opens the database file and creates the schema if it does not exist.
    bool open();
    bool is_open() const { return db_ != nullptr; }

    // Account management. On failure `error` receives a human-readable reason.
    bool register_user(const std::string& username, const std::string& password, std::string& error);
    bool authenticate_user(const std::string& username, const std::string& password, std::string& error);

    // Message history.
    bool add_message(const ChatMessage& message);
    std::vector<ChatMessage> get_recent_messages(const std::string& room, int limit);
    std::vector<std::string> list_rooms(int limit);

private:
    std::string path_;
    void* db_; // sqlite3* (kept opaque so this header needs no sqlite3.h)
    mutable std::mutex mutex_;
};
