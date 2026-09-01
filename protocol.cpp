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