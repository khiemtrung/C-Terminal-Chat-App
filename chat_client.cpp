#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"

#ifndef _WIN32
    #include <netinet/in.h>
#endif

void receive_messages(SOCKET sock) {
    char buffer[4096];
    std::vector<std::uint8_t> pending;
    while (true) {
        const int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            std::cout << "Disconnected from server." << std::endl;
            break;
        }

        pending.insert(pending.end(), buffer, buffer + bytes_received);
        std::vector<std::string> frames;
        if (!extract_frames(pending, frames)) {
            std::cerr << "Server sent an invalid frame." << std::endl;
            break;
        }

        for (const std::string& message : frames) {
            std::cout << message << std::endl;
        }
    }
}

int main() {
    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "Failed to initialize Winsock" << std::endl;
            return 1;
        }
    #endif

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect to server" << std::endl;
        closesocket(sock);
        return 1;
    }

    std::cout << "Connected to server. Start chatting!" << std::endl;

    std::string username;
    std::cout << "Enter your username: ";
    std::getline(std::cin, username);
    if (!send_frame(sock, username)) {
        std::cerr << "Failed to send username." << std::endl;
        closesocket(sock);
        return 1;
    }
    std::cout << "Use /join <room> to switch rooms. Type 'exit' to quit." << std::endl;

    std::thread receive_thread(receive_messages, sock);

    std::string message;
    while (true) {
        std::getline(std::cin, message);
        if (message == "exit") {
            break;
        }
        if (!send_frame(sock, message)) {
            std::cerr << "Failed to send message." << std::endl;
            break;
        }
    }

    shutdown(sock, SHUT_RDWR);
    closesocket(sock);
    receive_thread.join();

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}