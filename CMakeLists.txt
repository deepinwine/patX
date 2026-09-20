cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # Static runtime + vcpkg static triplet (existing Windows setup, unchanged)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
    set(BUILD_SHARED_LIBS OFF)
    set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    find_package(wxWidgets CONFIG REQUIRED)
    find_package(OpenXLSX CONFIG REQUIRED)
    set(PATX_SQLITE_TARGET unofficial::sqlite3::sqlite3)
    set(PATX_WX_TARGETS wx::core wx::base wx::adv wx::aui)
    set(PATX_OPENXLSX_TARGET OpenXLSX::OpenXLSX)
    set(PATX_EXTRA_INCLUDE_DIRS "")
else()
    # macOS/Linux: homebrew wxWidgets ships dylibs + wx-config but no CMake
    # config files, so use CMake's own FindwxWidgets MODULE.
    find_package(wxWidgets REQUIRED COMPONENTS core base adv aui)
    find_package(SQLite3 REQUIRED)
    include(FetchContent)
    FetchContent_Declare(OpenXLSX
        GIT_REPOSITORY https://github.com/troldal/OpenXLSX.git
        GIT_TAG v0.5.1)
    set(OPENXLSX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(OPENXLSX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(OpenXLSX)
    set(PATX_SQLITE_TARGET SQLite::SQLite3)
    set(PATX_WX_TARGETS ${wxWidgets_LIBRARIES})
    set(PATX_OPENXLSX_TARGET OpenXLSX::OpenXLSX)
    set(PATX_EXTRA_INCLUDE_DIRS ${wxWidgets_INCLUDE_DIRS})
endif()

# poppler-cpp for PDF parsing (disabled due to wxWidgets link conflicts)
# The fallback pdftotext approach will be used instead
set(POPPLER_CPP_FOUND FALSE)
message(STATUS "PDF import using pdftotext fallback (poppler-cpp disabled due to link conflicts)")

# All source files
set(CPP_SOURCES
    src/cpp/main_gui.cpp
    src/cpp/database.cpp
    src/cpp/undo_manager.cpp
    src/cpp/core/migration.cpp
    src/cpp/io/excel_io.cpp
    src/cpp/web_dossier.cpp
    src/cpp/web_dossier_rules.cpp
    src/cpp/ui/web_dossier_dialogs.cpp
)

# GUI executable
add_executable(patx WIN32 ${CPP_SOURCES})

target_include_directories(patx PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cpp
    ${CMAKE_SOURCE_DIR}/third_party
    ${PATX_EXTRA_INCLUDE_DIRS}
)

# Add poppler include dirs if found
if(POPPLER_CPP_FOUND)
    target_include_directories(patx PRIVATE ${POPPLER_CPP_INCLUDE_DIRS})
endif()

# Static link
target_link_libraries(patx PRIVATE
    ${PATX_SQLITE_TARGET}
    ${PATX_WX_TARGETS}
    ${PATX_OPENXLSX_TARGET}
)

# MODULE-mode wxWidgets (macOS/homebrew) needs its -D flags propagated
if(NOT WIN32)
    set(WX_DEFS "")
    foreach(def ${wxWidgets_DEFINITIONS})
        string(REGEX REPLACE "^-D" "" def "${def}")
        list(APPEND WX_DEFS "-D${def}")
    endforeach()
    target_compile_options(patx PRIVATE ${WX_DEFS})
endif()

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

# ---------------------------------------------------------------------------
# Offline unit tests for the web-dossier OA merge rules (no GUI, no network)
# ---------------------------------------------------------------------------
option(PATX_BUILD_TESTS "Build unit tests" ON)
if(PATX_BUILD_TESTS)
    add_executable(patx_tests
        tests/test_web_dossier_rules.cpp
        src/cpp/database.cpp
        src/cpp/undo_manager.cpp
        src/cpp/web_dossier_rules.cpp
    )
    target_include_directories(patx_tests PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/cpp
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(patx_tests PRIVATE ${PATX_SQLITE_TARGET})
    if(MSVC)
        target_compile_options(patx_tests PRIVATE "/utf-8")
    endif()
    enable_testing()
    add_test(NAME web_dossier_rules COMMAND patx_tests)
endif()

message(STATUS "patX v${PROJECT_VERSION} - Full Patent Manager")
message(STATUS "Dependencies: SQLite3, wxWidgets, OpenXLSX")
