#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>

#include "protocol.hpp"

#ifndef _WIN32
    #include <netinet/in.h>
#endif

namespace {

constexpr const char* kDefaultRoom = "general";

// Basic account identity: a client-chosen username plus the room it currently occupies.
struct ClientInfo {
    SOCKET socket;
    std::string username;
    std::string room;
};

std::vector<ClientInfo> clients;
std::mutex clients_mutex;

void send_system_message(SOCKET socket, const std::string& message) {
    send_frame(socket, "* " + message);
}

// Sends `message` to every member of `room` except `exclude` (pass INVALID_SOCKET to exclude none).
void broadcast_to_room(const std::string& room, const std::string& message, SOCKET exclude) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const ClientInfo& client : clients) {
        if (client.room == room && client.socket != exclude) {
            send_frame(client.socket, message);
        }
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

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Falls back to a generated guest name when the client sends an empty or oversized username.
std::string sanitize_username(const std::string& raw_username, SOCKET socket) {
    std::string username = trim(raw_username);
    if (username.empty() || username.size() > 32) {
        username = "guest" + std::to_string(static_cast<int>(socket));
    }
    return username;
}

// Pops the next complete frame from `queue`, refilling it from the socket as needed.
bool read_next_frame(SOCKET socket, std::vector<std::uint8_t>& pending, std::vector<std::string>& queue, std::string& out_frame) {
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

} // namespace

void handle_client(SOCKET client_socket) {
    std::vector<std::uint8_t> pending;
    std::vector<std::string> queue;

    // The first frame a client sends is its requested username (basic account handshake).
    std::string raw_username;
    if (!read_next_frame(client_socket, pending, queue, raw_username)) {
        closesocket(client_socket);
        return;
    }
    const std::string username = sanitize_username(raw_username, client_socket);

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.push_back({client_socket, username, kDefaultRoom});
    }

    std::cout << username << " joined room '" << kDefaultRoom << "'" << std::endl;
    broadcast_to_room(kDefaultRoom, "* " + username + " joined " + kDefaultRoom, client_socket);
    send_system_message(client_socket, "Welcome " + username + "! Room: " + kDefaultRoom + ". Use /join <room> to switch rooms.");

    std::string current_room = kDefaultRoom;
    std::string frame;
    while (read_next_frame(client_socket, pending, queue, frame)) {
        if (frame.rfind("/join ", 0) == 0) {
            const std::string new_room = trim(frame.substr(6));
            if (new_room.empty()) {
                send_system_message(client_socket, "Room name cannot be empty.");
                continue;
            }
            if (new_room == current_room) {
                send_system_message(client_socket, "You are already in room '" + current_room + "'.");
                continue;
            }
            broadcast_to_room(current_room, "* " + username + " left " + current_room, client_socket);
            current_room = new_room;
            set_room(client_socket, current_room);
            broadcast_to_room(current_room, "* " + username + " joined " + current_room, client_socket);
            send_system_message(client_socket, "Switched to room '" + current_room + "'.");
        } else {
            broadcast_to_room(current_room, "[" + current_room + "] " + username + ": " + frame, client_socket);
        }
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(
            std::remove_if(clients.begin(), clients.end(),
                            [client_socket](const ClientInfo& client) { return client.socket == client_socket; }),
            clients.end());
    }
    broadcast_to_room(current_room, "* " + username + " left " + current_room, INVALID_SOCKET);
    closesocket(client_socket);
    std::cout << username << " disconnected." << std::endl;
}

int main() {
    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "Failed to initialize Winsock" << std::endl;
            return 1;
        }
    #endif

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed" << std::endl;
        closesocket(server_socket);
        return 1;
    }

    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed" << std::endl;
        closesocket(server_socket);
        return 1;
    }

    std::cout << "Server is listening on port 8080..." << std::endl;

    while (true) {
        SOCKET client_socket = accept(server_socket, nullptr, nullptr);
        if (client_socket == INVALID_SOCKET) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        std::thread(handle_client, client_socket).detach();
    }

    closesocket(server_socket);

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}