#include "protocol.hpp"

#include <cstring>
#include <limits>

bool send_all(SOCKET socket, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const std::size_t remaining = length - sent;
        const int bytes_sent = send(
            socket,
            data + sent,
            static_cast<int>(remaining),
            0);
        if (bytes_sent <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(bytes_sent);
    }
    return true;
}

bool send_frame(SOCKET socket, const std::string& payload) {
    if (payload.size() > kMaxFramePayloadSize) {
        return false;
    }

    const std::uint32_t payload_length = htonl(static_cast<std::uint32_t>(payload.size()));
    return send_all(socket, reinterpret_cast<const char*>(&payload_length), sizeof(payload_length)) &&
           send_all(socket, payload.data(), payload.size());
}

bool extract_frames(std::vector<std::uint8_t>& pending, std::vector<std::string>& frames) {
    std::size_t offset = 0;

    while (pending.size() - offset >= kFrameHeaderSize) {
        std::uint32_t network_length = 0;
        std::memcpy(&network_length, pending.data() + offset, kFrameHeaderSize);
        const std::size_t payload_length = ntohl(network_length);

        if (payload_length > kMaxFramePayloadSize) {
            return false;
        }

        const std::size_t frame_size = kFrameHeaderSize + payload_length;
        if (pending.size() - offset < frame_size) {
            break;
        }

        frames.emplace_back(
            reinterpret_cast<const char*>(pending.data() + offset + kFrameHeaderSize),
            payload_length);
        offset += frame_size;
    }

    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

namespace {

inline void append_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline void append_u32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline void append_u64(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

bool take_bytes(const std::string& data, std::size_t& offset, std::size_t length, std::string& out) {
    if (offset > data.size() || data.size() - offset < length) {
        return false;
    }
    out.assign(data, offset, length);
    offset += length;
    return true;
}

bool take_u16(const std::string& data, std::size_t& offset, std::uint16_t& out) {
    if (offset > data.size() || data.size() - offset < 2) {
        return false;
    }
    out = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset])) << 8) |
        static_cast<std::uint8_t>(data[offset + 1]));
    offset += 2;
    return true;
}

bool take_u32(const std::string& data, std::size_t& offset, std::uint32_t& out) {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    out = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset])) << 24) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 1])) << 16) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 2])) << 8) |
          static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + 3]));
    offset += 4;
    return true;
}

bool take_u64(const std::string& data, std::size_t& offset, std::uint64_t& out) {
    if (offset > data.size() || data.size() - offset < 8) {
        return false;
    }
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | static_cast<std::uint8_t>(data[offset + i]);
    }
    offset += 8;
    return true;
}
} // namespace

std::string encode_message(MsgType type, const std::string& payload) {
    std::string frame;
    frame.reserve(payload.size() + 1);
    frame.push_back(static_cast<char>(static_cast<std::uint8_t>(type)));
    frame += payload;
    return frame;
}

bool decode_message(const std::string& frame, MsgType& type, std::string& payload) {
    if (frame.empty()) {
        return false;
    }
    type = static_cast<MsgType>(static_cast<std::uint8_t>(frame[0]));
    payload.assign(frame.begin() + 1, frame.end());
    return true;
}

bool send_message(SOCKET socket, MsgType type, const std::string& payload) {
    return send_frame(socket, encode_message(type, payload));
}

std::string encode_chat_message(const ChatMessage& message) {
    std::string payload;
    append_u16(payload, static_cast<std::uint16_t>(message.room.size()));
    payload += message.room;
    append_u16(payload, static_cast<std::uint16_t>(message.username.size()));
    payload += message.username;
    append_u64(payload, static_cast<std::uint64_t>(message.timestamp));
    append_u32(payload, static_cast<std::uint32_t>(message.content.size()));
    payload += message.content;
    return payload;
}

bool decode_chat_message(const std::string& payload, ChatMessage& message) {
    std::size_t offset = 0;
    std::uint16_t room_len = 0;
    std::uint16_t user_len = 0;
    std::uint32_t content_len = 0;
    std::uint64_t timestamp = 0;

    if (!take_u16(payload, offset, room_len) ||
        !take_bytes(payload, offset, room_len, message.room) ||
        !take_u16(payload, offset, user_len) ||
        !take_bytes(payload, offset, user_len, message.username) ||
        !take_u64(payload, offset, timestamp) ||
        !take_u32(payload, offset, content_len) ||
        !take_bytes(payload, offset, content_len, message.content)) {
        return false;
    }
    message.timestamp = static_cast<std::int64_t>(timestamp);
    return true;
}

std::string encode_history(const std::vector<ChatMessage>& messages) {
    std::string payload;
    append_u16(payload, static_cast<std::uint16_t>(messages.size()));
    for (const ChatMessage& message : messages) {
        payload += encode_chat_message(message);
    }
    return payload;
}

bool decode_history(const std::string& payload, std::vector<ChatMessage>& messages) {
    std::size_t offset = 0;
    std::uint16_t count = 0;
    if (!take_u16(payload, offset, count)) {
        return false;
    }
    messages.clear();
    messages.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        ChatMessage message;
        std::uint16_t room_len = 0;
        std::uint16_t user_len = 0;
        std::uint32_t content_len = 0;
        std::uint64_t timestamp = 0;

        if (!take_u16(payload, offset, room_len) ||
            !take_bytes(payload, offset, room_len, message.room) ||
            !take_u16(payload, offset, user_len) ||
            !take_bytes(payload, offset, user_len, message.username) ||
            !take_u64(payload, offset, timestamp) ||
            !take_u32(payload, offset, content_len) ||
            !take_bytes(payload, offset, content_len, message.content)) {
            return false;
        }
        message.timestamp = static_cast<std::int64_t>(timestamp);
        messages.push_back(std::move(message));
    }
    return true;
}

std::string encode_direct_message(const DirectMessage& message) {
    std::string payload;
    append_u16(payload, static_cast<std::uint16_t>(message.from.size()));
    payload += message.from;
    append_u16(payload, static_cast<std::uint16_t>(message.to.size()));
    payload += message.to;
    append_u64(payload, static_cast<std::uint64_t>(message.timestamp));
    append_u32(payload, static_cast<std::uint32_t>(message.content.size()));
    payload += message.content;
    return payload;
}

bool decode_direct_message(const std::string& payload, DirectMessage& message) {
    std::size_t offset = 0;
    std::uint16_t from_len = 0;
    std::uint16_t to_len = 0;
    std::uint32_t content_len = 0;
    std::uint64_t timestamp = 0;

    if (!take_u16(payload, offset, from_len) ||
        !take_bytes(payload, offset, from_len, message.from) ||
        !take_u16(payload, offset, to_len) ||
        !take_bytes(payload, offset, to_len, message.to) ||
        !take_u64(payload, offset, timestamp) ||
        !take_u32(payload, offset, content_len) ||
        !take_bytes(payload, offset, content_len, message.content)) {
        return false;
    }
    message.timestamp = static_cast<std::int64_t>(timestamp);
    return true;
}

std::string encode_user_list(const std::vector<std::string>& usernames) {
    std::string payload;
    append_u16(payload, static_cast<std::uint16_t>(usernames.size()));
    for (const std::string& name : usernames) {
        append_u16(payload, static_cast<std::uint16_t>(name.size()));
        payload += name;
    }
    return payload;
}

bool decode_user_list(const std::string& payload, std::vector<std::string>& usernames) {
    std::size_t offset = 0;
    std::uint16_t count = 0;
    if (!take_u16(payload, offset, count)) {
        return false;
    }
    usernames.clear();
    usernames.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint16_t name_len = 0;
        std::string name;
        if (!take_u16(payload, offset, name_len) || !take_bytes(payload, offset, name_len, name)) {
            return false;
        }
        usernames.push_back(std::move(name));
    }
    return true;
}
