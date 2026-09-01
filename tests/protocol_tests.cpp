#include "protocol.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

std::vector<std::uint8_t> make_frame(const std::string& payload) {
    const std::uint32_t network_length = htonl(static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> frame(kFrameHeaderSize + payload.size());
    std::memcpy(frame.data(), &network_length, kFrameHeaderSize);
    std::memcpy(frame.data() + kFrameHeaderSize, payload.data(), payload.size());
    return frame;
}

int main() {
    const std::vector<std::uint8_t> first_frame = make_frame("first message");
    std::vector<std::uint8_t> pending(first_frame.begin(), first_frame.begin() + 3);
    std::vector<std::string> frames;

    assert(extract_frames(pending, frames));
    assert(frames.empty());

    pending.insert(pending.end(), first_frame.begin() + 3, first_frame.end());
    assert(extract_frames(pending, frames));
    assert(frames.size() == 1);
    assert(frames[0] == "first message");
    assert(pending.empty());

    const std::vector<std::uint8_t> second_frame = make_frame("second message");
    pending.insert(pending.end(), first_frame.begin(), first_frame.end());
    pending.insert(pending.end(), second_frame.begin(), second_frame.end());
    frames.clear();
    assert(extract_frames(pending, frames));
    assert(frames.size() == 2);
    assert(frames[0] == "first message");
    assert(frames[1] == "second message");
    assert(pending.empty());

    const std::uint32_t oversized_length = htonl(static_cast<std::uint32_t>(kMaxFramePayloadSize + 1));
    pending.resize(kFrameHeaderSize);
    std::memcpy(pending.data(), &oversized_length, kFrameHeaderSize);
    frames.clear();
    assert(!extract_frames(pending, frames));

    std::cout << "Protocol tests passed." << std::endl;
    return 0;
}