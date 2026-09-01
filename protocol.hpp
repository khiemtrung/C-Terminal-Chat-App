#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define SHUT_RDWR SD_BOTH
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

constexpr std::size_t kFrameHeaderSize = sizeof(std::uint32_t);
constexpr std::size_t kMaxFramePayloadSize = 64 * 1024;

bool send_all(SOCKET socket, const char* data, std::size_t length);
bool send_frame(SOCKET socket, const std::string& payload);

// Returns false when the buffer contains an invalid frame length.
bool extract_frames(std::vector<std::uint8_t>& pending, std::vector<std::string>& frames);