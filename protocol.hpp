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

// ---------------------------------------------------------------------------
// Low-level framing: each TCP frame is [uint32 BE length][payload bytes].
// ---------------------------------------------------------------------------
bool send_all(SOCKET socket, const char* data, std::size_t length);
bool send_frame(SOCKET socket, const std::string& payload);

// Returns false when the buffer contains an invalid frame length.
bool extract_frames(std::vector<std::uint8_t>& pending, std::vector<std::string>& frames);

// ---------------------------------------------------------------------------
// Application message types. Every frame payload starts with a one-byte type
// followed by a type-specific payload.
// ---------------------------------------------------------------------------
enum class MsgType : std::uint8_t {
    Login    = 0x01, // client -> server : "username\npassword"
    Register = 0x02, // client -> server : "username\npassword"
    AuthOk   = 0x03, // server -> client : "username\nroom"
    AuthFail = 0x04, // server -> client : human-readable reason
    System   = 0x05, // server -> client : human-readable text
    Chat     = 0x06, // client -> server : message text
    ChatMsg  = 0x07, // server -> client : binary ChatMessage record
    Join     = 0x08, // client -> server : room name
    History  = 0x09, // server -> client : binary list of ChatMessage records
    Quit     = 0x0A, // client -> server : (no payload)
    HistoryReq = 0x0B, // client -> server : optional limit (decimal text), or "@user[ N]" for DM history
    RoomsReq  = 0x0C, // client -> server : (no payload)
    Joined   = 0x0D, // server -> client : room switch confirmation
    Direct   = 0x0E, // client -> server : "targetUser\ncontent"
    DirectMsg = 0x0F, // server -> client : binary DirectMessage record
    UsersReq = 0x10, // client -> server : room name (empty = caller's current room)
    UsersList = 0x11 // server -> client : binary list of usernames
};

struct ChatMessage {
    std::string room;
    std::string username;
    std::string content;
    std::int64_t timestamp; // unix epoch seconds
};

struct DirectMessage {
    std::string from;
    std::string to;
    std::string content;
    std::int64_t timestamp; // unix epoch seconds
};

// ---------------------------------------------------------------------------
// Typed message helpers. Binary fields are big-endian and length-prefixed so
// arbitrary content (including embedded newlines) round-trips losslessly.
// ---------------------------------------------------------------------------
std::string encode_message(MsgType type, const std::string& payload = "");
bool decode_message(const std::string& frame, MsgType& type, std::string& payload);
bool send_message(SOCKET socket, MsgType type, const std::string& payload = "");

// ChatMessage binary layout:
//   [u16 room_len][room][u16 user_len][user][i64 epoch][u32 content_len][content]
std::string encode_chat_message(const ChatMessage& message);
bool decode_chat_message(const std::string& payload, ChatMessage& message);

// History binary layout: [u16 count][ChatMessage * count]
std::string encode_history(const std::vector<ChatMessage>& messages);
bool decode_history(const std::string& payload, std::vector<ChatMessage>& messages);

// DirectMessage binary layout:
//   [u16 from_len][from][u16 to_len][to][i64 epoch][u32 content_len][content]
std::string encode_direct_message(const DirectMessage& message);
bool decode_direct_message(const std::string& payload, DirectMessage& message);

// User list binary layout: [u16 count][u16 name_len][name] * count
std::string encode_user_list(const std::vector<std::string>& usernames);
bool decode_user_list(const std::string& payload, std::vector<std::string>& usernames);
