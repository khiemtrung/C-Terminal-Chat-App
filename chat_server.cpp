#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

std::vector<SOCKET> clients;
std::mutex clients_mutex;

void broadcast(const std::string& message, SOCKET sender) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (SOCKET client : clients) {
        if (client != sender) {
            send(client, message.c_str(), message.length(), 0);
        }
    }
}

void handle_client(SOCKET client_socket) {
    char buffer[1024] = {0};
    while (true) {
        int bytes_received = recv(client_socket, buffer, 1024, 0);
        if (bytes_received <= 0) {
            break;
        }
        std::string message(buffer, bytes_received);
        broadcast(message, client_socket);
        memset(buffer, 0, sizeof(buffer));
    }
    
    std::lock_guard<std::mutex> lock(clients_mutex);
    clients.erase(std::remove(clients.begin(), clients.end(), client_socket), clients.end());
    closesocket(client_socket);
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

        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.push_back(client_socket);

        std::thread(handle_client, client_socket).detach();
    }

    closesocket(server_socket);

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}