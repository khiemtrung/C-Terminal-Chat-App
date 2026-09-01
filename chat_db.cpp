#include "chat_db.hpp"

#include "sha256.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <utility>

namespace {

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 128 bits of random hex entropy, unique per account.
std::string generate_salt() {
    std::random_device rd;
    std::mt19937_64 generator(
        rd() ^ static_cast<std::uint64_t>(now_seconds()) ^
        reinterpret_cast<std::uintptr_t>(&generator));
    std::uniform_int_distribution<unsigned int> dist(0, 255);

    static const char* kHex = "0123456789abcdef";
    std::string salt;
    salt.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const unsigned int byte = dist(generator);
        salt.push_back(kHex[(byte >> 4) & 0x0F]);
        salt.push_back(kHex[byte & 0x0F]);
    }
    return salt;
}

// Stored form: "<salt>$<sha256(salt + password)>".
std::string hash_password(const std::string& salt, const std::string& password) {
    return salt + "$" + sha256_hex(salt + password);
}

} // namespace

ChatDatabase::ChatDatabase(std::string path) : path_(std::move(path)), db_(nullptr) {}

ChatDatabase::~ChatDatabase() {
    if (db_ != nullptr) {
        sqlite3_close(static_cast<sqlite3*>(db_));
    }
}

bool ChatDatabase::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ != nullptr) {
        return true;
    }

    sqlite3* raw = nullptr;
    if (sqlite3_open(path_.c_str(), &raw) != SQLITE_OK) {
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        return false;
    }
    db_ = raw;

    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            room TEXT NOT NULL,
            username TEXT NOT NULL,
            content TEXT NOT NULL,
            sent_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_messages_room_sent ON messages(room, sent_at);
    )";

    char* error = nullptr;
    if (sqlite3_exec(raw, schema, nullptr, nullptr, &error) != SQLITE_OK) {
        if (error != nullptr) {
            sqlite3_free(error);
        }
        return false;
    }
    return true;
}


bool ChatDatabase::register_user(const std::string& username, const std::string& password, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = "failed to prepare statement";
        return false;
    }

    const std::string salt = generate_salt();
    const std::string password_hash = hash_password(salt, password);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, now_seconds());

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT) {
        error = "username is already taken";
        return false;
    }
    if (rc != SQLITE_DONE) {
        error = "failed to store the account";
        return false;
    }
    return true;
}

bool ChatDatabase::authenticate_user(const std::string& username, const std::string& password, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password_hash FROM users WHERE username = ?;";
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = "failed to prepare statement";
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* stored_bytes = sqlite3_column_text(stmt, 0);
        const std::string stored_hash = stored_bytes != nullptr
                                            ? reinterpret_cast<const char*>(stored_bytes)
                                            : "";
        sqlite3_finalize(stmt);

        const std::size_t separator = stored_hash.find('$');
        if (separator == std::string::npos) {
            error = "corrupt account record";
            return false;
        }
        const std::string salt = stored_hash.substr(0, separator);
        const std::string digest = stored_hash.substr(separator + 1);
        if (digest == sha256_hex(salt + password)) {
            return true;
        }
    } else {
        sqlite3_finalize(stmt);
    }

    error = "invalid username or password";
    return false;
}

bool ChatDatabase::add_message(const ChatMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO messages (room, username, content, sent_at) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, message.room.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, message.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, message.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, message.timestamp);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<ChatMessage> ChatDatabase::get_recent_messages(const std::string& room, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChatMessage> messages;
    if (db_ == nullptr) {
        return messages;
    }

    // Select the newest `limit` rows in the room, then return them oldest-first.
    const char* sql = R"(
        SELECT room, username, content, sent_at FROM (
            SELECT id, room, username, content, sent_at
            FROM messages
            WHERE room = ?
            ORDER BY sent_at DESC, id DESC
            LIMIT ?
        ) ORDER BY sent_at ASC, id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return messages;
    }
    sqlite3_bind_text(stmt, 1, room.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChatMessage message;
        const unsigned char* room_bytes = sqlite3_column_text(stmt, 0);
        const unsigned char* user_bytes = sqlite3_column_text(stmt, 1);
        const unsigned char* content_bytes = sqlite3_column_text(stmt, 2);
        message.room = room_bytes != nullptr ? reinterpret_cast<const char*>(room_bytes) : "";
        message.username = user_bytes != nullptr ? reinterpret_cast<const char*>(user_bytes) : "";
        message.content = content_bytes != nullptr ? reinterpret_cast<const char*>(content_bytes) : "";
        message.timestamp = sqlite3_column_int64(stmt, 3);
        messages.push_back(std::move(message));
    }
    sqlite3_finalize(stmt);
    return messages;
}

std::vector<std::string> ChatDatabase::list_rooms(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> rooms;
    if (db_ == nullptr) {
        return rooms;
    }

    const char* sql = "SELECT DISTINCT room FROM messages ORDER BY room LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rooms;
    }
    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* room_bytes = sqlite3_column_text(stmt, 0);
        if (room_bytes != nullptr) {
            rooms.emplace_back(reinterpret_cast<const char*>(room_bytes));
        }
    }
    sqlite3_finalize(stmt);
    return rooms;
}

