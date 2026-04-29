cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Static linking
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
set(BUILD_SHARED_LIBS OFF)

# vcpkg dependencies - static
set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(wxWidgets CONFIG REQUIRED)

# GUI version - WIN32 means GUI subsystem
add_executable(patx WIN32
    src/cpp/main_gui.cpp
)

target_include_directories(patx PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Static link
target_link_libraries(patx PRIVATE 
    unofficial::sqlite3::sqlite3
    wx::core
    wx::base
    wx::adv
)

# No console window for GUI app
set_target_properties(patx PROPERTIES
    WIN32_EXECUTABLE TRUE
)

message(STATUS "patX Static GUI build configured")