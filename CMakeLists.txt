cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 简单的源文件列表
set(CPP_SOURCES
    src/cpp/main.cpp
)

# 创建可执行文件
add_executable(patx ${CPP_SOURCES})
target_include_directories(patx PUBLIC include)

message(STATUS "patX build configured (minimal version)")