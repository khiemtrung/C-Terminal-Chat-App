// chat_server.cpp
// v2.0: registered accounts, SQLite-backed message history and enhanced console
// output. Each connection authenticates before it can chat.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "chat_db.hpp"
#include "protocol.hpp"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <netinet/in.h>
    #include <unistd.h>
#endif

namespace {

constexpr const char* kDefaultRoom = "general";
constexpr const char* kDefaultDbPath = "chat.db";
constexpr int kDefaultPort = 8080;
constexpr int kHistoryLimit = 50;

struct ClientInfo {
    SOCKET socket;
    std::string username;
    std::string room;
};

std::vector<ClientInfo> clients;
std::mutex clients_mutex;

bool console_color = false;

void init_console_color() {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode) != 0) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        console_color = true;
    }
#else
    console_color = ::isatty(STDOUT_FILENO) != 0;
#endif
}

void server_log(const std::string& text) {
    if (console_color) {
        std::cout << "\033[1;32m[server]\033[0m " << text << std::endl;
    } else {
        std::cout << "[server] " << text << std::endl;
    }
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool valid_username(const std::string& username) {
    if (username.size() < 3 || username.size() > 20) {
        return false;
    }
    for (const char c : username) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

std::string sanitize_room(const std::string& raw_room) {
    std::string room = trim(raw_room);
    if (room.size() > 32) {
        room = room.substr(0, 32);
    }
    for (char& c : room) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != ' ') {
            c = '_';
        }
    }
    return room;
}

void send_system(SOCKET socket, const std::string& text) {
    send_message(socket, MsgType::System, text);
}

// Sends a pre-encoded frame (e.g. from encode_message) to every member of
// `room` except `exclude`. Recipients are snapshotted under the lock so a
// slow client cannot stall the rest of the server.
void broadcast_to_room(const std::string& room, const std::string& frame, SOCKET exclude) {
    std::vector<SOCKET> recipients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const ClientInfo& client : clients) {
            if (client.room == room && client.socket != exclude) {
                recipients.push_back(client.socket);
            }
        }
    }
    for (const SOCKET socket : recipients) {
        send_frame(socket, frame);
    }
}

void set_room(SOCKET socket, const std::string& room) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (ClientInfo& client : clients) {
        if (client.socket == socket) {
            client.room = room;
            return;
        }
    }
}

SOCKET find_client_by_username(const std::string& username) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const ClientInfo& client : clients) {
        if (client.username == username) {
            return client.socket;
        }
    }
    return INVALID_SOCKET;
}

// Synthetic room key so direct messages can reuse the messages table/history
// API without a schema change; order-independent so both sides see one thread.
std::string dm_room_key(const std::string& user_a, const std::string& user_b) {
    return user_a < user_b ? "dm:" + user_a + ":" + user_b : "dm:" + user_b + ":" + user_a;
}

std::vector<std::string> users_in_room(const std::string& room) {
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const ClientInfo& client : clients) {
        if (client.room == room) {
            names.push_back(client.username);
        }
    }
    return names;
}

// Pops the next complete frame from `queue`, refilling it from the socket as
// needed.
bool read_next_frame(SOCKET socket, std::vector<std::uint8_t>& pending,
                     std::vector<std::string>& queue, std::string& out_frame) {
    char buffer[4096];
    while (queue.empty()) {
        const int bytes_received = recv(socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            return false;
        }
        pending.insert(pending.end(), buffer, buffer + bytes_received);
        std::vector<std::string> frames;
        if (!extract_frames(pending, frames)) {
            return false;
        }
        queue.insert(queue.end(), frames.begin(), frames.end());
    }
    out_frame = queue.front();
    queue.erase(queue.begin());
    return true;
}

// Outcome of a single authentication attempt. `Retry` means an AuthFail was
// sent but the connection stays open so the client can try again.
enum class AuthResult { Success, Retry, Disconnected };

// Authenticates a freshly connected client. The first frame it sends must be a
// Login or Register request. On success `out_username` receives the account
// name; on failure an AuthFail frame is sent (Retry) unless the socket itself
// was closed (Disconnected).
AuthResult handle_auth(SOCKET socket, std::vector<std::uint8_t>& pending,
                       std::vector<std::string>& queue, ChatDatabase& db,
                       std::string& out_username) {
    std::string frame;
    if (!read_next_frame(socket, pending, queue, frame)) {
        return AuthResult::Disconnected;
    }

    MsgType type;
    std::string payload;
    if (!decode_message(frame, type, payload)) {
        send_message(socket, MsgType::AuthFail, "Malformed login request.");
        return AuthResult::Retry;
    }
    if (type != MsgType::Login && type != MsgType::Register) {
        send_message(socket, MsgType::AuthFail, "Please log in or register an account first.");
        return AuthResult::Retry;
    }

    const std::size_t separator = payload.find('\n');
    if (separator == std::string::npos) {
        send_message(socket, MsgType::AuthFail, "Invalid credentials format.");
        return AuthResult::Retry;
    }
    const std::string username = trim(payload.substr(0, separator));
    const std::string password = payload.substr(separator + 1);

    if (type == MsgType::Register) {
        std::string error;
        if (!valid_username(username)) {
            send_message(socket, MsgType::AuthFail,
                         "Username must be 3-20 characters (letters, digits, '_' or '-').");
            return AuthResult::Retry;
        }
        if (password.size() < 6) {
            send_message(socket, MsgType::AuthFail, "Password must be at least 6 characters.");
            return AuthResult::Retry;
        }
        if (!db.register_user(username, password, error)) {
            send_message(socket, MsgType::AuthFail, "Registration failed: " + error);
            return AuthResult::Retry;
        }
        send_system(socket, "Account '" + username + "' created. Welcome!");
        server_log("new account registered: " + username);
    } else {
        std::string error;
        if (!db.authenticate_user(username, password, error)) {
            send_message(socket, MsgType::AuthFail, "Login failed: " + error);
            return AuthResult::Retry;
        }
        server_log(username + " logged in.");
    }

    out_username = username;
    return AuthResult::Success;
}

void replay_history(SOCKET socket, const std::string& room, int limit, ChatDatabase& db) {
    const std::vector<ChatMessage> history = db.get_recent_messages(room, limit);
    if (history.empty()) {
        send_system(socket, "No message history in room '" + room + "' yet.");
        return;
    }
    send_message(socket, MsgType::History, encode_history(history));
}

void handle_history_request(SOCKET socket, const std::string& raw_limit,
                            const std::string& username, const std::string& room,
                            ChatDatabase& db) {
    std::string text = trim(raw_limit);
    std::string target_room = room;

    if (!text.empty() && text[0] == '@') {
        const std::size_t space = text.find(' ');
        const std::string target_user = space == std::string::npos ? text.substr(1) : text.substr(1, space - 1);
        if (target_user.empty()) {
            send_system(socket, "Usage: /dmhistory <user> [N]");
            return;
        }
        target_room = dm_room_key(username, target_user);
        text = space == std::string::npos ? "" : trim(text.substr(space + 1));
    }

    int limit = kHistoryLimit;
    if (!text.empty()) {
        try {
            limit = std::stoi(text);
        } catch (const std::exception&) {
            limit = kHistoryLimit;
        }
        if (limit <= 0) {
            limit = 1;
        }
        if (limit > 200) {
            limit = 200;
        }
    }
    replay_history(socket, target_room, limit, db);
}

void handle_join(SOCKET socket, const std::string& raw_room, const std::string& username,
                 std::string& current_room, ChatDatabase& db) {
    const std::string new_room = sanitize_room(raw_room);
    if (new_room.empty()) {
        send_system(socket, "Room name cannot be empty.");
        return;
    }
    if (new_room == current_room) {
        send_system(socket, "You are already in room '" + current_room + "'.");
        return;
    }

    broadcast_to_room(current_room, encode_message(MsgType::System, username + " left " + current_room), socket);
    current_room = new_room;
    set_room(socket, current_room);
    send_message(socket, MsgType::Joined, current_room);
    broadcast_to_room(current_room, encode_message(MsgType::System, username + " joined " + current_room), socket);
    replay_history(socket, current_room, kHistoryLimit, db);
}

std::string build_room_list(ChatDatabase& db) {
    std::map<std::string, int> online;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const ClientInfo& client : clients) {
            ++online[client.room];
        }
    }

    const std::vector<std::string> known_rooms = db.list_rooms(200);
    std::set<std::string> rooms(known_rooms.begin(), known_rooms.end());
    for (const auto& entry : online) {
        rooms.insert(entry.first);
    }

    std::string text = "Known rooms:";
    if (rooms.empty()) {
        text += "\n  (none yet)";
    }
    for (const std::string& room : rooms) {
        text += "\n  " + room + "  (online: " + std::to_string(online[room]) + ")";
    }
    return text;
}


} // namespace

void handle_client(SOCKET client_socket, ChatDatabase& db) {
    std::vector<std::uint8_t> pending;
    std::vector<std::string> queue;

    std::string username;
    while (true) {
        const AuthResult result = handle_auth(client_socket, pending, queue, db, username);
        if (result == AuthResult::Success) {
            break;
        }
        if (result == AuthResult::Disconnected) {
            closesocket(client_socket);
            return;
        }
        // Retry: give the client another chance to log in or register.
    }

    std::string room = kDefaultRoom;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.push_back({client_socket, username, room});
    }

    send_message(client_socket, MsgType::AuthOk, username + "\n" + room);
    send_system(client_socket, "Welcome, " + username + "! You are in room '" + room + "'.");
    send_system(client_socket, "Commands: /join <room> | /history [N] | /rooms | /msg <user> <text> | /who [room] | /dmhistory <user> [N] | /help | /quit");
    replay_history(client_socket, room, kHistoryLimit, db);

    server_log(username + " connected (room: " + room + ").");
    broadcast_to_room(room, encode_message(MsgType::System, username + " joined " + room), client_socket);

    bool running = true;
    std::string frame;
    while (running && read_next_frame(client_socket, pending, queue, frame)) {
        MsgType type;
        std::string payload;
        if (!decode_message(frame, type, payload)) {
            send_system(client_socket, "Received a malformed message.");
            continue;
        }

        switch (type) {
            case MsgType::Chat: {
                const std::string content = trim(payload);
                if (content.empty()) {
                    break;
                }
                const ChatMessage message{room, username, content, now_seconds()};
                if (!db.add_message(message)) {
                    send_system(client_socket, "Failed to store the message in history.");
                }
                broadcast_to_room(room,
                                  encode_message(MsgType::ChatMsg, encode_chat_message(message)),
                                  client_socket);
                break;
            }
            case MsgType::Join:
                handle_join(client_socket, payload, username, room, db);
                break;
            case MsgType::HistoryReq:
                handle_history_request(client_socket, payload, username, room, db);
                break;
            case MsgType::RoomsReq:
                send_system(client_socket, build_room_list(db));
                break;
            case MsgType::Direct: {
                const std::size_t separator = payload.find('\n');
                if (separator == std::string::npos) {
                    send_system(client_socket, "Usage: /msg <user> <message>");
                    break;
                }
                const std::string target = trim(payload.substr(0, separator));
                const std::string content = trim(payload.substr(separator + 1));
                if (target.empty() || content.empty()) {
                    send_system(client_socket, "Usage: /msg <user> <message>");
                    break;
                }
                const SOCKET target_socket = find_client_by_username(target);
                if (target_socket == INVALID_SOCKET) {
                    send_system(client_socket, "User '" + target + "' not found or offline.");
                    break;
                }
                const DirectMessage dm{username, target, content, now_seconds()};
                if (!db.add_message(ChatMessage{dm_room_key(username, target), username, content, dm.timestamp})) {
                    send_system(client_socket, "Failed to store the direct message in history.");
                }
                const std::string frame = encode_message(MsgType::DirectMsg, encode_direct_message(dm));
                send_frame(target_socket, frame);
                send_frame(client_socket, frame);
                break;
            }
            case MsgType::UsersReq: {
                const std::string target_room = trim(payload).empty() ? room : sanitize_room(payload);
                send_message(client_socket, MsgType::UsersList, encode_user_list(users_in_room(target_room)));
                break;
            }
            case MsgType::Quit:
                send_system(client_socket, "Goodbye, " + username + "!");
                running = false;
                break;
            default:
                send_system(client_socket, "Unsupported message type.");
                break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(
            std::remove_if(clients.begin(), clients.end(),
                           [client_socket](const ClientInfo& client) {
                               return client.socket == client_socket;
                           }),
            clients.end());
    }
    broadcast_to_room(room, encode_message(MsgType::System, username + " left " + room), INVALID_SOCKET);
    closesocket(client_socket);
    server_log(username + " disconnected.");
}

int main(int argc, char* argv[]) {
    init_console_color();

    // Writing to a socket that the peer has just closed raises SIGPIPE by
    // default and would kill the whole server. Ignore it: send() then simply
    // returns an error, which is handled per-socket.
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const int port = argc > 1 ? std::atoi(argv[1]) : kDefaultPort;
    const std::string db_path = argc > 2 ? argv[2] : kDefaultDbPath;
    if (port < 1 || port > 65535) {
        std::cerr << "Invalid port: " << port << std::endl;
        return 1;
    }

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return 1;
    }
#endif

    ChatDatabase db(db_path);
    if (!db.open()) {
        std::cerr << "Failed to open history database at '" << db_path << "'." << std::endl;
        return 1;
    }
    server_log("history database ready: " + db_path);

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (bind(server_socket, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed on port " << port << std::endl;
        closesocket(server_socket);
        return 1;
    }

    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed" << std::endl;
        closesocket(server_socket);
        return 1;
    }

    std::cout << "C++ Chat Server v2.0" << std::endl;
    std::cout << "Listening on 0.0.0.0:" << port << "  (history db: " << db_path << ")" << std::endl;

    while (true) {
        SOCKET client_socket = accept(server_socket, nullptr, nullptr);
        if (client_socket == INVALID_SOCKET) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }
        std::thread(handle_client, client_socket, std::ref(db)).detach();
    }

    closesocket(server_socket);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

