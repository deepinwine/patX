cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg dependencies
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)

# Source files
set(CPP_SOURCES
    src/cpp/main.cpp
)

# Create executable
add_executable(patx ${CPP_SOURCES})
target_include_directories(patx PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Link dependencies
target_link_libraries(patx PRIVATE 
    unofficial::sqlite3::sqlite3
    absl::strings
)

message(STATUS "patX build configured with vcpkg")