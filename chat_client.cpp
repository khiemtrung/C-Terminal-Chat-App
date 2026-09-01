// chat_client.cpp
// v2.0: login/register flow, colored terminal UI, timestamps and history
// rendering.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <netinet/in.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace ansi {
constexpr const char* kReset = "\033[0m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kGray = "\033[90m";
constexpr const char* kBoldCyan = "\033[1;36m";
constexpr const char* kBoldGreen = "\033[1;32m";
constexpr const char* kBoldRed = "\033[1;31m";
constexpr const char* kBoldYellow = "\033[1;33m";
constexpr const char* kMagenta = "\033[35m";
constexpr const char* kBoldMagenta = "\033[1;35m";
} // namespace ansi

namespace {

constexpr int kDefaultPort = 8080;

std::atomic<bool> g_connected{false};
std::atomic<bool> g_authenticated{false};
std::atomic<bool> g_auth_failed{false};
std::mutex g_io_mutex;
std::string g_username;
std::string g_room;
bool g_color = false;

// ---------------------------------------------------------------------------
// Terminal helpers
// ---------------------------------------------------------------------------

void init_console() {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode) != 0) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        g_color = true;
    }
#else
    g_color = ::isatty(STDOUT_FILENO) != 0;
#endif
}

std::string colored(const char* code, const std::string& text) {
    if (!g_color) {
        return text;
    }
    return std::string(code) + text + ansi::kReset;
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool valid_username(const std::string& username) {
    if (username.size() < 3 || username.size() > 20) {
        return false;
    }
    for (const char c : username) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

std::string format_time(std::int64_t epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[16] = {0};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

// Stable per-username color chosen from a readable palette.
std::string username_color(const std::string& username) {
    static const std::array<const char*, 8> kPalette = {
        "\033[1;36m", "\033[1;33m", "\033[1;35m", "\033[1;32m",
        "\033[1;34m", "\033[1;91m", "\033[93m",   "\033[96m"};
    if (!g_color) {
        return "";
    }
    unsigned int hash = 0;
    for (const char c : username) {
        hash = hash * 31 + static_cast<unsigned char>(c);
    }
    return kPalette[hash % kPalette.size()];
}

std::string build_prompt() {
    const std::string room = g_room.empty() ? "connecting" : g_room;
    std::string prompt = colored(ansi::kBoldCyan, "[" + room + "]");
    if (!g_username.empty()) {
        prompt += " " + username_color(g_username) + g_username + colored(ansi::kReset, "");
    }
    prompt += colored(ansi::kBoldGreen, "> ");
    return prompt;
}

// Prints a message, clearing whatever prompt is currently on screen.
void print_message(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_io_mutex);
    if (g_color) {
        std::cout << "\r\033[K" << line << std::endl;
    } else {
        std::cout << line << std::endl;
    }
}
// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void print_chat_message(const ChatMessage& message, bool from_history) {
    const std::string prefix = from_history ? colored(ansi::kGray, "› ") : "";
    const std::string time = colored(ansi::kGray, "[" + format_time(message.timestamp) + "]");
    const std::string body = from_history ? colored(ansi::kDim, message.content) : message.content;

    // DM history is stored under a synthetic "dm:a:b" room key; render it like
    // a direct message instead of exposing that internal key to the user.
    if (message.room.rfind("dm:", 0) == 0) {
        const std::size_t split = message.room.find(':', 3);
        const std::string user_a = message.room.substr(3, split - 3);
        const std::string user_b = split == std::string::npos ? "" : message.room.substr(split + 1);
        const std::string other = user_a == g_username ? user_b : user_a;
        const std::string tag = colored(ansi::kBoldMagenta, "\u2709 ");
        const std::string label = colored(ansi::kMagenta, message.username == g_username ? "you \u2192 " + other
                                                                                          : message.username + " \u2192 you");
        print_message(prefix + time + " " + tag + label + ": " + body);
        return;
    }

    const std::string room = colored(ansi::kCyan, "[" + message.room + "]");
    const std::string name = username_color(message.username) + message.username + colored(ansi::kReset, "");
    print_message(prefix + time + " " + room + " " + name + ": " + body);
}

void print_history(const std::vector<ChatMessage>& messages) {
    for (const ChatMessage& message : messages) {
        print_chat_message(message, true);
    }
    print_message(colored(ansi::kGray, "— loaded " + std::to_string(messages.size()) + " message(s) —"));
}

void print_help() {
    print_message(colored(ansi::kBoldCyan, "Commands:"));
    print_message("  " + colored(ansi::kBoldYellow, "/join <room>") + "      switch to a room (history is replayed)");
    print_message("  " + colored(ansi::kBoldYellow, "/history [N]") + "     show the last N messages (default 50)");
    print_message("  " + colored(ansi::kBoldYellow, "/rooms") + "           list rooms with online user counts");
    print_message("  " + colored(ansi::kBoldYellow, "/msg <user> <text>") + "  send a direct message (alias /w)");
    print_message("  " + colored(ansi::kBoldYellow, "/who [room]") + "      list online users in a room (default current)");
    print_message("  " + colored(ansi::kBoldYellow, "/dmhistory <user> [N]") + " show the last N direct messages with a user");
    print_message("  " + colored(ansi::kBoldYellow, "/help") + "            show this help");
    print_message("  " + colored(ansi::kBoldYellow, "/quit") + ", /q or exit  leave the chat");
    print_message("  anything else     is sent as a chat message");
}

std::string make_banner() {
    const std::string line = colored(ansi::kBoldCyan,
                                     "┌─────────────────────────────────────────────┐");
    const std::string bottom = colored(ansi::kBoldCyan,
                                       "└─────────────────────────────────────────────┘");
    const std::string title = colored(ansi::kBoldCyan, "│") + "        C++ Terminal Chat " +
                              colored(ansi::kBoldYellow, "v2.0") +
                              "               " + colored(ansi::kBoldCyan, "│");
    const std::string subtitle = colored(ansi::kBoldCyan, "│") + "   accounts · rooms · persisted history      " +
                                 colored(ansi::kBoldCyan, "│");
    return line + "\n" + title + "\n" + subtitle + "\n" + bottom;
}

std::string read_password(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string password;
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool ok = input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode) != 0;
    if (ok) {
        SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
    }
    std::getline(std::cin, password);
    if (ok) {
        SetConsoleMode(input, mode);
    }
#else
    termios old_settings{};
    const bool changed = tcgetattr(STDIN_FILENO, &old_settings) == 0;
    if (changed) {
        termios new_settings = old_settings;
        new_settings.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_settings);
    }
    std::getline(std::cin, password);
    if (changed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_settings);
    }
#endif
    std::cout << std::endl;
    return password;
}

// ---------------------------------------------------------------------------
// Receive thread
// ---------------------------------------------------------------------------

void receive_messages(SOCKET sock) {
    char buffer[4096];
    std::vector<std::uint8_t> pending;

    while (g_connected) {
        const int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            if (g_connected.exchange(false)) {
                print_message(colored(ansi::kBoldRed, "Disconnected from server."));
            }
            break;
        }

        pending.insert(pending.end(), buffer, buffer + bytes_received);
        std::vector<std::string> frames;
        if (!extract_frames(pending, frames)) {
            print_message(colored(ansi::kBoldRed, "Server sent an invalid frame."));
            g_connected = false;
            break;
        }

        for (const std::string& frame : frames) {
            if (!g_connected) {
                break;
            }
            MsgType type;
            std::string payload;
            if (!decode_message(frame, type, payload)) {
                continue;
            }

            switch (type) {
                case MsgType::AuthOk: {
                    const std::size_t separator = payload.find('\n');
                    g_username = payload.substr(0, separator);
                    g_room = payload.substr(separator + 1);
                    g_authenticated = true;
                    break;
                }
                case MsgType::AuthFail: {
                    print_message(colored(ansi::kBoldRed, "✗ ") + colored(ansi::kRed, payload));
                    g_auth_failed = true;
                    break;
                }
                case MsgType::System:
                    print_message(colored(ansi::kCyan, "* ") + payload);
                    break;
                case MsgType::ChatMsg: {
                    ChatMessage message;
                    if (decode_chat_message(payload, message)) {
                        print_chat_message(message, false);
                    }
                    break;
                }
                case MsgType::History: {
                    std::vector<ChatMessage> messages;
                    if (decode_history(payload, messages)) {
                        print_history(messages);
                    }
                    break;
                }
                case MsgType::Joined: {
                    g_room = payload;
                    print_message(colored(ansi::kBoldYellow, "* ") +
                                  colored(ansi::kYellow, "You are now in room '" + payload + "'."));
                    break;
                }
                case MsgType::DirectMsg: {
                    DirectMessage message;
                    if (decode_direct_message(payload, message)) {
                        const std::string time = colored(ansi::kGray, "[" + format_time(message.timestamp) + "]");
                        const std::string arrow = message.from == g_username
                                                       ? "you \xE2\x86\x92 " + message.to
                                                       : message.from + " \xE2\x86\x92 you";
                        print_message(time + " " + colored(ansi::kBoldMagenta, "\u2709 ") +
                                     colored(ansi::kMagenta, arrow) + ": " + message.content);
                    }
                    break;
                }
                case MsgType::UsersList: {
                    std::vector<std::string> names;
                    if (decode_user_list(payload, names)) {
                        std::string line = "Online";
                        if (names.empty()) {
                            line += ": (none)";
                        } else {
                            line += ": ";
                            for (std::size_t i = 0; i < names.size(); ++i) {
                                if (i != 0) {
                                    line += ", ";
                                }
                                line += names[i];
                            }
                        }
                        print_message(colored(ansi::kCyan, "* ") + line);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Authentication + main loop
// ---------------------------------------------------------------------------

bool authenticate(SOCKET sock) {
    while (true) {
        std::cout << "  " << colored(ansi::kBoldCyan, "[1]") << " Log in" << std::endl;
        std::cout << "  " << colored(ansi::kBoldCyan, "[2]") << " Create a new account" << std::endl;
        std::cout << "  " << colored(ansi::kBoldCyan, "[q]") << " Quit" << std::endl;
        std::cout << "Choose: " << std::flush;

        std::string choice;
        if (!std::getline(std::cin, choice)) {
            return false; // EOF (piped input exhausted)
        }
        choice = trim(choice);

        MsgType request;
        if (choice == "1" || choice == "login") {
            request = MsgType::Login;
        } else if (choice == "2" || choice == "register") {
            request = MsgType::Register;
        } else if (choice == "q" || choice == "quit" || choice == "exit") {
            std::cout << "Goodbye." << std::endl;
            return false;
        } else {
            std::cout << colored(ansi::kRed, "Invalid choice. Try again.") << std::endl;
            continue;
        }

        std::string username;
        std::cout << "Username: " << std::flush;
        if (!std::getline(std::cin, username)) {
            return false; // EOF
        }
        username = trim(username);
        const std::string password = read_password("Password: ");
        if (std::cin.eof()) {
            return false;
        }

        if (request == MsgType::Register) {
            if (!valid_username(username)) {
                print_message(colored(ansi::kRed, "Username must be 3-20 characters (letters, digits, '_' or '-')."));
                continue;
            }
            if (password.size() < 6) {
                print_message(colored(ansi::kRed, "Password must be at least 6 characters."));
                continue;
            }
        }
        if (username.empty()) {
            print_message(colored(ansi::kRed, "Username cannot be empty."));
            continue;
        }

        std::cout << colored(ansi::kGray, request == MsgType::Register ? "Creating account..." : "Logging in...") << std::endl;
        if (!send_message(sock, request, username + "\n" + password)) {
            print_message(colored(ansi::kRed, "Failed to send credentials."));
            return false;
        }

        // Wait for the server's AuthOk / AuthFail reply.
        while (g_connected && !g_authenticated && !g_auth_failed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_auth_failed) {
            g_auth_failed = false;
            continue; // back to menu instead of quitting
        }
        return g_authenticated;
    }
}

int main(int argc, char* argv[]) {
    init_console();

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? std::atoi(argv[2]) : kDefaultPort;

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return 1;
    }
#endif

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server address: " << host << std::endl;
        return 1;
    }

    std::cout << make_banner() << std::endl;
    std::cout << "Connecting to " << host << ":" << port << " ..." << std::endl;
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }
    std::cout << colored(ansi::kBoldGreen, "✔ Connected!") << std::endl;

    g_connected = true;
    std::thread receive_thread(receive_messages, sock);

    // --- Authentication -----------------------------------------------------
    if (!authenticate(sock)) {
        g_connected = false;
        shutdown(sock, SHUT_RDWR);
        closesocket(sock);
        receive_thread.join();
        return 1;
    }

    // --- Chat loop ----------------------------------------------------------
    print_message(colored(ansi::kCyan, "* ") + "Type /help for a list of commands.");
    std::string line;
    while (g_connected) {
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << build_prompt() << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            break; // EOF
        }
        const std::string input = trim(line);
        if (input.empty()) {
            continue;
        }

        if (input == "exit" || input == "/quit" || input == "/q") {
            send_message(sock, MsgType::Quit);
            break;
        }
        if (input == "/help") {
            print_help();
            continue;
        }
        if (input.rfind("/join ", 0) == 0) {
            send_message(sock, MsgType::Join, trim(input.substr(6)));
            continue;
        }
        if (input.rfind("/history", 0) == 0) {
            send_message(sock, MsgType::HistoryReq, trim(input.substr(8)));
            continue;
        }
        if (input == "/rooms") {
            send_message(sock, MsgType::RoomsReq);
            continue;
        }
        if (input.rfind("/msg ", 0) == 0 || input.rfind("/w ", 0) == 0) {
            const std::string rest = trim(input.substr(input[1] == 'w' ? 3 : 5));
            const std::size_t space = rest.find(' ');
            if (space == std::string::npos || rest.substr(0, space).empty() || trim(rest.substr(space + 1)).empty()) {
                print_message(colored(ansi::kRed, "Usage: /msg <user> <message>"));
                continue;
            }
            const std::string target = rest.substr(0, space);
            const std::string content = trim(rest.substr(space + 1));
            send_message(sock, MsgType::Direct, target + "\n" + content);
            continue;
        }
        if (input == "/who" || input.rfind("/who ", 0) == 0) {
            const std::string room_arg = input.size() > 4 ? trim(input.substr(4)) : "";
            send_message(sock, MsgType::UsersReq, room_arg);
            continue;
        }
        if (input.rfind("/dmhistory ", 0) == 0) {
            const std::string rest = trim(input.substr(11));
            if (rest.empty()) {
                print_message(colored(ansi::kRed, "Usage: /dmhistory <user> [N]"));
                continue;
            }
            send_message(sock, MsgType::HistoryReq, "@" + rest);
            continue;
        }
        if (input.size() > 512) {
            print_message(colored(ansi::kRed, "Message is too long (max 512 characters)."));
            continue;
        }
        send_message(sock, MsgType::Chat, input);
    }

    g_connected = false;
    shutdown(sock, SHUT_RDWR);
    closesocket(sock);
    receive_thread.join();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

