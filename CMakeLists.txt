cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg dependencies
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(wxWidgets CONFIG REQUIRED)

# GUI version - WIN32 means GUI subsystem
add_executable(patx WIN32
    src/cpp/main_gui.cpp
)

target_include_directories(patx PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(patx PRIVATE 
    unofficial::sqlite3::sqlite3
    wx::core
    wx::base
    wx::adv
)

message(STATUS "patX GUI build configured")