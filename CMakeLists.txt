cmake_minimum_required(VERSION 3.10)
project(ChatProject VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

# --- SQLite3 (required by the server for accounts and message history) -------
find_library(SQLITE3_LIBRARY NAMES sqlite3)
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h)

if(NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite3 library not found. Install libsqlite3-dev (Linux), "
                        "the sqlite3 package (macOS Homebrew) or a bundled amalgamation.")
endif()
if(NOT SQLITE3_INCLUDE_DIR)
    message(FATAL_ERROR "sqlite3.h not found. Make sure the SQLite development headers are installed.")
endif()

add_executable(ChatServer chat_server.cpp chat_db.cpp protocol.cpp sha256.cpp)
target_include_directories(ChatServer PRIVATE ${SQLITE3_INCLUDE_DIR})
target_link_libraries(ChatServer ${SQLITE3_LIBRARY})

add_executable(ChatClient chat_client.cpp protocol.cpp)

if(WIN32)
    target_link_libraries(ChatServer wsock32 ws2_32)
    target_link_libraries(ChatClient wsock32 ws2_32)
endif()

if(BUILD_TESTING)
    add_executable(ProtocolTests tests/protocol_tests.cpp protocol.cpp sha256.cpp)
    target_include_directories(ProtocolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ProtocolTests COMMAND ProtocolTests)

    add_executable(DatabaseTests tests/database_tests.cpp chat_db.cpp protocol.cpp sha256.cpp)
    target_include_directories(DatabaseTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLITE3_INCLUDE_DIR})
    target_link_libraries(DatabaseTests ${SQLITE3_LIBRARY})
    if(WIN32)
        target_link_libraries(DatabaseTests wsock32 ws2_32)
    endif()
    add_test(NAME DatabaseTests COMMAND DatabaseTests)
endif()
