# C++ Chat Application

## Overview
This project is a simple client-server chat application implemented in C++. It demonstrates basic networking concepts, multi-threading, and modern C++ features.

### Features
- Multi-client support
- Real-time messaging
- Cross-platform compatibility (Windows, macOS, Linux)
- Simple command-line interface
- Length-prefixed TCP frames, so fragmented or combined TCP reads preserve message boundaries
- Client-chosen usernames, shown on every message and join/leave notification
- Chat rooms: clients start in a default `general` room and can switch with `/join <room>`; messages only broadcast to clients in the same room

## Prerequisites
- C++14 compatible compiler (GCC 5+, Clang 3.4+, MSVC 2015+)
- CMake 3.10 or higher
- Git (for cloning the repository)

## Building the Project

### Clone the Repository
```bash
git clone https://github.com/yourusername/cpp-chat-app.git
cd cpp-chat-app
```

### Build Instructions
```bash
mkdir build
cd build
cmake ..
cmake --build .
ctest --test-dir . --output-on-failure
```

## Running the Application

### Starting the Server
```bash
./ChatServer
```
The server will start and listen on port 8080 by default.

### Connecting with a Client
```bash
./ChatClient
```
Open many Clients terminal to send message to it and see the result.

## Project Structure
- `src/` - Source files
  - `chat_server.cpp` - Server implementation
  - `chat_client.cpp` - Client implementation
- `include/` - Header files
- `CMakeLists.txt` - CMake build configuration
- `README.md` - This file

## Technical Details
- Uses TCP sockets for communication
- Implements a multi-threaded server to handle multiple clients
- Uses C++14 features for improved code quality and performance

## Configuration
- Default server port: 8080
- To change the port, modify the `PORT` constant in `chat_server.cpp` and `chat_client.cpp`

## Current Limitations
- Usernames are self-declared on connect (no passwords or persisted accounts) and rooms are created ad hoc by the first client to join them.
- There is no persistent message history, so joining a room does not replay earlier messages, and there is no transport encryption.
- It is intended for trusted local networks while those capabilities are implemented.

## Planned Enhancements
- Add registered user accounts with credentials (replacing the current self-declared usernames).
- Add SQLite-backed message history, replayed to clients when they join a room.

## Contributing
Contributions to this project are welcome! Please follow these steps:
1. Fork the repository
2. Create a new branch for your feature
3. Commit your changes
4. Push to the branch
5. Create a new Pull Request
