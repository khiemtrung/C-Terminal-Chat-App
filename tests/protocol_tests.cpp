#include "protocol.hpp"
#include "sha256.hpp"

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

void test_framing() {
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
}

void test_sha256() {
    assert(sha256_hex("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(sha256_hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(sha256_hex("hello world") ==
           "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    std::cout << "SHA-256 vectors OK." << std::endl;
}

void test_typed_messages() {
    // encode/decode roundtrip with an empty payload.
    const std::string login_frame = encode_message(MsgType::Login, "");
    MsgType type;
    std::string payload;
    assert(decode_message(login_frame, type, payload));
    assert(type == MsgType::Login);
    assert(payload.empty());

    // Payload with embedded newlines.
    const std::string auth_payload = "alice\ns3cret";
    const std::string auth_frame = encode_message(MsgType::Register, auth_payload);
    assert(decode_message(auth_frame, type, payload));
    assert(type == MsgType::Register);
    assert(payload == auth_payload);

    // Empty frames are invalid.
    assert(!decode_message("", type, payload));
    std::cout << "Typed message encode/decode OK." << std::endl;
}

void test_chat_message_binary() {
    ChatMessage original;
    original.room = "general";
    original.username = "bob";
    original.timestamp = 1700000000;
    original.content = "hello\nworld — ünïcode ✓";

    ChatMessage decoded;
    assert(decode_chat_message(encode_chat_message(original), decoded));
    assert(decoded.room == original.room);
    assert(decoded.username == original.username);
    assert(decoded.timestamp == original.timestamp);
    assert(decoded.content == original.content);

    // Truncated payload must be rejected.
    const std::string encoded = encode_chat_message(original);
    assert(decode_chat_message(encoded.substr(0, encoded.size() / 2), decoded) == false);
    assert(!decode_chat_message("", decoded));
    std::cout << "ChatMessage binary roundtrip OK." << std::endl;
}

void test_history_binary() {
    std::vector<ChatMessage> originals;
    ChatMessage first;
    first.room = "general";
    first.username = "alice";
    first.timestamp = 1700000001;
    first.content = "one";
    originals.push_back(first);

    ChatMessage second;
    second.room = "dev";
    second.username = "carol";
    second.timestamp = 1700000002;
    second.content = "line1\nline2\nline3";
    originals.push_back(second);

    std::vector<ChatMessage> decoded;
    assert(decode_history(encode_history(originals), decoded));
    assert(decoded.size() == 2);
    assert(decoded[0].room == "general");
    assert(decoded[0].username == "alice");
    assert(decoded[0].content == "one");
    assert(decoded[1].room == "dev");
    assert(decoded[1].username == "carol");
    assert(decoded[1].content == "line1\nline2\nline3");
    assert(decoded[1].timestamp == 1700000002);

    // Empty history.
    std::vector<ChatMessage> empty;
    assert(decode_history(encode_history(empty), decoded));
    assert(decoded.empty());

    // Truncated payload must be rejected.
    const std::string encoded = encode_history(originals);
    assert(decode_history(encoded.substr(0, encoded.size() - 1), decoded) == false);
    assert(!decode_history("", decoded));
    std::cout << "History binary roundtrip OK." << std::endl;
}

void test_direct_message_binary() {
    DirectMessage original;
    original.from = "alice";
    original.to = "bob";
    original.timestamp = 1700000003;
    original.content = "hey — ünïcode ✓\nsecond line";

    DirectMessage decoded;
    assert(decode_direct_message(encode_direct_message(original), decoded));
    assert(decoded.from == original.from);
    assert(decoded.to == original.to);
    assert(decoded.timestamp == original.timestamp);
    assert(decoded.content == original.content);

    // Truncated payload must be rejected.
    const std::string encoded = encode_direct_message(original);
    assert(decode_direct_message(encoded.substr(0, encoded.size() / 2), decoded) == false);
    assert(!decode_direct_message("", decoded));
    std::cout << "DirectMessage binary roundtrip OK." << std::endl;
}

void test_user_list_binary() {
    const std::vector<std::string> names = {"alice", "bob", "carol"};
    std::vector<std::string> decoded;
    assert(decode_user_list(encode_user_list(names), decoded));
    assert(decoded == names);

    // Empty list.
    const std::vector<std::string> empty;
    assert(decode_user_list(encode_user_list(empty), decoded));
    assert(decoded.empty());

    // Truncated payload must be rejected.
    const std::string encoded = encode_user_list(names);
    assert(decode_user_list(encoded.substr(0, encoded.size() - 1), decoded) == false);
    std::cout << "User list binary roundtrip OK." << std::endl;
}

int main() {
    test_framing();
    test_sha256();
    test_typed_messages();
    test_chat_message_binary();
    test_history_binary();
    test_direct_message_binary();
    test_user_list_binary();

    std::cout << "All protocol tests passed." << std::endl;
    return 0;
}
