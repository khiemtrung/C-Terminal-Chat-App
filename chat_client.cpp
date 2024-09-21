#include <iostream>
#include <string>
#include <thread>
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

void receive_messages(SOCKET sock) {
    char buffer[1024] = {0};
    while (true) {
        int bytes_received = recv(sock, buffer, 1024, 0);
        if (bytes_received <= 0) {
            std::cout << "Disconnected from server." << std::endl;
            break;
        }
        std::cout << buffer << std::endl;
        memset(buffer, 0, sizeof(buffer));
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

    std::thread receive_thread(receive_messages, sock);

    std::string message;
    while (true) {
        std::getline(std::cin, message);
        if (message == "exit") {
            break;
        }
        send(sock, message.c_str(), message.length(), 0);
    }

    closesocket(sock);
    receive_thread.join();

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}