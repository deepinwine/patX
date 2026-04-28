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

# 查找 poppler (可选)
find_package(poppler-cpp QUIET)

# C++ 源文件
set(CPP_SOURCES
    src/cpp/core/database.cpp
    src/cpp/core/migration.cpp
    src/cpp/index/index.cpp
    src/cpp/io/excel_io.cpp
)

# 添加 PDF 解析源文件 (如果 poppler 可用)
if (poppler-cpp_FOUND)
    list(APPEND CPP_SOURCES src/cpp/io/pdf_parser.cpp)
    message(STATUS "Poppler found - PDF parsing enabled")
else()
    message(STATUS "Poppler not found - PDF parsing disabled")
endif()

# 头文件
set(CPP_HEADERS
    include/patent.hpp
    include/database.hpp
    include/index.hpp
    include/excel_io.hpp
    include/migration.hpp
    include/pdf_parser.hpp
)

# 创建静态库
add_library(patx_cpp STATIC ${CPP_SOURCES})
target_include_directories(patx_cpp PUBLIC include)
target_link_libraries(patx_cpp 
    SQLite::SQLite3
    absl::container
)

# 链接 poppler (如果可用)
if (poppler-cpp_FOUND)
    target_link_libraries(patx_cpp poppler-cpp)
    target_compile_definitions(patx_cpp PRIVATE POPPLER_AVAILABLE=1)
endif()

# Rust 库 (通过 Cargo 构建)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/target/release/libpatx_core.a
    COMMAND cargo build --release
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building Rust library"
)

# 主可执行文件
add_executable(patx src/cpp/main.cpp)
target_link_libraries(patx 
    patx_cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/target/release/libpatx_core.a
    pthread
    dl
)

# 测试
enable_testing()

# 单元测试
add_executable(patx_test tests/test_main.cpp)
target_link_libraries(patx_test patx_cpp)

# 性能基准测试
add_executable(patx_benchmark tests/benchmark.cpp)
target_link_libraries(patx_benchmark patx_cpp)

# 安装
install(TARGETS patx patx_cpp DESTINATION bin)
install(FILES ${CPP_HEADERS} DESTINATION include)
