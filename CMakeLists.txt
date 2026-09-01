cmake_minimum_required(VERSION 3.10)
project(ChatProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

add_executable(ChatServer chat_server.cpp protocol.cpp)
add_executable(ChatClient chat_client.cpp protocol.cpp)

if(BUILD_TESTING)
    add_executable(ProtocolTests tests/protocol_tests.cpp protocol.cpp)
    target_include_directories(ProtocolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ProtocolTests COMMAND ProtocolTests)
endif()

if(WIN32)
    target_link_libraries(ChatServer wsock32 ws2_32)
    target_link_libraries(ChatClient wsock32 ws2_32)
    if(BUILD_TESTING)
        target_link_libraries(ProtocolTests wsock32 ws2_32)
    endif()
endif()