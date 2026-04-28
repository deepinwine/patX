cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 极致性能编译优化
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto -march=native -DNDEBUG")
endif()

# 查找依赖
find_package(SQLite3 REQUIRED)
find_package(absl CONFIG REQUIRED)

# C++ 源文件
set(CPP_SOURCES
    src/cpp/core/database.cpp
    src/cpp/index/index.cpp
)

# 头文件
set(CPP_HEADERS
    include/patent.hpp
    include/database.hpp
    include/index.hpp
)

# 创建静态库
add_library(patx_cpp STATIC ${CPP_SOURCES} ${CPP_HEADERS})
target_include_directories(patx_cpp PUBLIC include)
target_link_libraries(patx_cpp 
    SQLite::SQLite3
    absl::container
)

# Rust 库 (通过 Cargo 构建)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/libpatx_core.a
    COMMAND cargo build --release
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building Rust library"
)

# 主可执行文件
add_executable(patx src/cpp/main.cpp)
target_link_libraries(patx 
    patx_cpp
    ${CMAKE_BINARY_DIR}/libpatx_core.a
)

# 测试
enable_testing()
add_executable(patx_test tests/test_main.cpp)
target_link_libraries(patx_test patx_cpp)