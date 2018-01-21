cmake_minimum_required(VERSION 2.8)
project(nobridge_daemon)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES
        nobridge-daemon.cpp)

add_executable(nobridge_daemon ${SOURCE_FILES})