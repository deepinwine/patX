cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
set(BUILD_SHARED_LIBS OFF)

# vcpkg - static
set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(wxWidgets CONFIG REQUIRED)

# OpenXLSX for Excel parsing
find_package(OpenXLSX CONFIG REQUIRED)

# All source files
set(CPP_SOURCES
    src/cpp/main_gui.cpp
    src/cpp/database.cpp
    src/cpp/undo_manager.cpp
    src/cpp/core/migration.cpp
    src/cpp/io/excel_io.cpp
)

# GUI executable
add_executable(patx WIN32 ${CPP_SOURCES})

target_include_directories(patx PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cpp
)

# Static link
target_link_libraries(patx PRIVATE
    unofficial::sqlite3::sqlite3
    wx::core
    wx::base
    wx::adv
    wx::aui
    OpenXLSX::OpenXLSX
)

set_target_properties(patx PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# MSVC UTF-8 encoding for Chinese characters
if(MSVC)
    target_compile_options(patx PRIVATE "/utf-8")
endif()

message(STATUS "patX v${PROJECT_VERSION} - Full Patent Manager")
message(STATUS "Dependencies: SQLite3, wxWidgets, OpenXLSX")