cmake_minimum_required(VERSION 3.10)
project(ChatProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXE_LINKER_FLAGS "-static")

add_executable(ChatServer chat_server.cpp)
add_executable(ChatClient chat_client.cpp)

if(WIN32)
    target_link_libraries(ChatServer wsock32 ws2_32)
    target_link_libraries(ChatClient wsock32 ws2_32)
endif()