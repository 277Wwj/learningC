cmake_minimum_required(VERSION 3.16)
project(learning CXX)

set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)

add_library(reactor_core INTERFACE)
target_include_directories(reactor_core INTERFACE include)

add_executable(webserver src/webserver.cpp)
target_link_libraries(webserver PRIVATE reactor_core)

add_executable (webserver_thread src/webserver_thread.cpp)
target_link_libraries(webserver_thread PRIVATE reactor_core Threads::Threads)

add_executable(webserver_multithread src/webserver_multithread.cpp)
target_link_libraries(webserver_multithread PRIVATE reactor_core Threads::Threads)

add_executable (chat_server src/chat_server.cpp)
target_link_libraries(chat_server PRIVATE reactor_core)

enable_testing()
find_package(GTest REQUIRED)
add_executable (buffer_test tests/buffer_test.cpp tests/eof.cpp)
target_link_libraries(buffer_test PRIVATE reactor_core GTest::gtest GTest::gtest_main)
add_test(NAME buffer_test COMMAND buffer_test)


