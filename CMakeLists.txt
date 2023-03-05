cmake_minimum_required(VERSION 2.6)

project(gayo CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE debug)
set(CMAKE_C_COMPILER /usr/bin/gcc-11)
set(CMAKE_CXX_COMPILER /usr/bin/g++-11)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_compile_options(-fsanitize=address)
add_link_options(-fsanitize=address)

find_package(fmt)

include_directories(gayo)

add_executable(gayo_demo examples/gayo.cc)
add_executable(echo_server examples/echo_server.cc)


target_link_libraries(gayo_demo PRIVATE fmt::fmt)
target_link_libraries(echo_server PRIVATE Threads::Threads fmt::fmt)