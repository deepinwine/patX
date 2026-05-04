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

# poppler-cpp for PDF parsing (disabled due to wxWidgets link conflicts)
# The fallback pdftotext approach will be used instead
set(POPPLER_CPP_FOUND FALSE)
# if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/x64-windows-static/include/poppler/cpp/poppler-document.h")
#     set(POPPLER_CPP_FOUND TRUE)
#     set(POPPLER_CPP_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/x64-windows-static/include")
#     set(POPPLER_CPP_LIBRARIES "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/installed/x64-windows-static/lib/poppler-cpp.lib")
#     message(STATUS "PDF import enabled (poppler-cpp found manually)")
# else()
#     # Try vcpkg root
#     if(EXISTS "C:/vcpkg/installed/x64-windows-static/include/poppler/cpp/poppler-document.h")
#         set(POPPLER_CPP_FOUND TRUE)
#         set(POPPLER_CPP_INCLUDE_DIRS "C:/vcpkg/installed/x64-windows-static/include")
#         set(POPPLER_CPP_LIBRARIES "C:/vcpkg/installed/x64-windows-static/lib/poppler-cpp.lib")
#         message(STATUS "PDF import enabled (poppler-cpp found)")
#     else()
#         message(STATUS "PDF import disabled (poppler-cpp not found)")
#     endif()
# endif()
message(STATUS "PDF import using pdftotext fallback (poppler-cpp disabled due to link conflicts)")

# All source files
set(CPP_SOURCES
    src/cpp/main_gui.cpp
    src/cpp/database.cpp
    src/cpp/undo_manager.cpp
    src/cpp/core/migration.cpp
    src/cpp/io/excel_io.cpp
)

# PDF parser (only if poppler is available)
# TODO: poppler-cpp has linking conflicts with wxWidgets on Windows
# Temporarily disabled - using fallback approach
# if(POPPLER_CPP_FOUND)
#     list(APPEND CPP_SOURCES src/cpp/io/pdf_parser.cpp)
# endif()

# GUI executable
add_executable(patx WIN32 ${CPP_SOURCES})

target_include_directories(patx PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cpp
)

# Add poppler include dirs if found
if(POPPLER_CPP_FOUND)
    target_include_directories(patx PRIVATE ${POPPLER_CPP_INCLUDE_DIRS})
endif()

# Static link
target_link_libraries(patx PRIVATE
    unofficial::sqlite3::sqlite3
    wx::core
    wx::base
    wx::adv
    wx::aui
    OpenXLSX::OpenXLSX
)

# Link poppler if found
if(POPPLER_CPP_FOUND)
    target_link_libraries(patx PRIVATE ${POPPLER_CPP_LIBRARIES})
    target_compile_definitions(patx PRIVATE POPPLER_CPP_AVAILABLE)
endif()

set_target_properties(patx PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# MSVC UTF-8 encoding for Chinese characters
if(MSVC)
    target_compile_options(patx PRIVATE "/utf-8")
endif()

message(STATUS "patX v${PROJECT_VERSION} - Full Patent Manager")
message(STATUS "Dependencies: SQLite3, wxWidgets, OpenXLSX")