cmake_minimum_required(VERSION 3.20)
project(patX VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static runtime on MSVC (matches vcpkg x64-windows-static triplet)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
endif()

option(PATX_BUILD_GUI "Build the wxWidgets GUI executable" ON)
option(PATX_BUILD_TESTS "Build unit tests and register them with CTest" ON)

# ---------------------------------------------------------------------------
# Single version source: generated header consumed by GUI, logs and About box
# ---------------------------------------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/patx/version.h.in
    ${CMAKE_BINARY_DIR}/generated/include/patx/version.h
    @ONLY
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# SQLite3: vcpkg exposes unofficial-sqlite3; homebrew/system expose SQLite3.
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(PATX_SQLITE_TARGET unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    set(PATX_SQLITE_TARGET SQLite::SQLite3)
endif()

# nlohmann/json: vcpkg/homebrew package first, else download the single header
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    set(PATX_JSON_TARGET nlohmann_json::nlohmann_json)
else()
    set(JSON_HEADER ${CMAKE_BINARY_DIR}/generated/include/nlohmann/json.hpp)
    if(NOT EXISTS ${JSON_HEADER})
        message(STATUS "Downloading nlohmann/json single header (v3.11.3)")
        file(DOWNLOAD
            https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
            ${JSON_HEADER}
            SHOW_PROGRESS
            STATUS json_dl_status)
        list(GET json_dl_status 0 json_dl_rc)
        if(NOT json_dl_rc EQUAL 0)
            message(FATAL_ERROR "Failed to download nlohmann/json header (code ${json_dl_rc}). Install nlohmann-json via vcpkg/brew or retry with network access.")
        endif()
    endif()
    add_library(nlohmann_json_header INTERFACE)
    target_include_directories(nlohmann_json_header INTERFACE ${CMAKE_BINARY_DIR}/generated/include)
    set(PATX_JSON_TARGET nlohmann_json_header)
endif()

# OpenXLSX: vcpkg package first, else FetchContent from upstream
find_package(OpenXLSX CONFIG QUIET)
if(NOT OpenXLSX_FOUND)
    include(FetchContent)
    FetchContent_Declare(OpenXLSX
        GIT_REPOSITORY https://github.com/troldal/OpenXLSX.git
        GIT_TAG v0.5.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(OpenXLSX)
endif()

# ---------------------------------------------------------------------------
# Core library: database, IO, web dossier rules. No wxWidgets dependency,
# so it builds and tests on macOS/Linux/CI without a GUI toolchain.
# ---------------------------------------------------------------------------
add_library(patx_core STATIC
    src/cpp/database.cpp
    src/cpp/undo_manager.cpp
    src/cpp/core/migration.cpp
    src/cpp/core/log.cpp
    src/cpp/database/schema_migrations.cpp
    src/cpp/deadline_engine.cpp
    src/cpp/io/excel_io.cpp
    src/cpp/web_dossier_rules.cpp
)

target_include_directories(patx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated/include
)

target_link_libraries(patx_core PUBLIC
    ${PATX_SQLITE_TARGET}
    ${PATX_JSON_TARGET}
    OpenXLSX::OpenXLSX
)

if(MSVC)
    target_compile_options(patx_core PRIVATE /utf-8 /W4)
else()
    target_compile_options(patx_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# GUI executable (wxWidgets). Skipped gracefully when wx is unavailable,
# e.g. on headless CI macOS/Linux core-only builds.
# ---------------------------------------------------------------------------
if(PATX_BUILD_GUI)
    # vcpkg exposes targets (wx::core), homebrew exposes wxWidgets_LIBRARIES
    find_package(wxWidgets COMPONENTS core base adv aui html core QUIET)
    if(wxWidgets_FOUND)
        # Module-style results (homebrew/system) provide wxWidgets_USE_FILE
        # that adds include dirs; config-style (vcpkg) only provides targets.
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        add_executable(patx
            src/cpp/main_gui.cpp
            src/cpp/ui/edit_dialogs.cpp
            src/cpp/web_dossier.cpp
            src/cpp/ui/web_dossier_dialogs.cpp
            src/cpp/ui/epo_family_dialog.cpp
        )
        if(WIN32)
            set_target_properties(patx PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        target_include_directories(patx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp)
        target_link_libraries(patx PRIVATE patx_core ${wxWidgets_LIBRARIES})
        target_compile_definitions(patx PRIVATE PATX_GUI=1)
        if(MSVC)
            target_compile_options(patx PRIVATE /utf-8)
        endif()
        message(STATUS "patX GUI enabled (wxWidgets found)")
    else()
        message(STATUS "wxWidgets not found - building core library and tests only")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PATX_BUILD_TESTS)
    enable_testing()
    add_executable(patx_tests
        tests/test_database.cpp
        tests/test_excel.cpp
        tests/test_main.cpp
    )
    target_link_libraries(patx_tests PRIVATE patx_core)
    target_include_directories(patx_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME patx_tests COMMAND patx_tests)

    # Deadline calculation base (calendar arithmetic + CN rules JSON)
    add_executable(patx_deadline_tests tests/test_deadline_engine.cpp)
    target_link_libraries(patx_deadline_tests PRIVATE patx_core)
    target_include_directories(patx_deadline_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME patx_deadline_tests COMMAND patx_deadline_tests)

    # Web dossier OA merge rules (standalone main; links the core rules unit)
    add_executable(patx_web_dossier_tests tests/test_web_dossier_rules.cpp)
    target_link_libraries(patx_web_dossier_tests PRIVATE patx_core)
    target_include_directories(patx_web_dossier_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp
    )
    if(MSVC)
        target_compile_options(patx_web_dossier_tests PRIVATE /utf-8)
    endif()
    add_test(NAME patx_web_dossier_tests COMMAND patx_web_dossier_tests)
endif()

message(STATUS "patX v${PROJECT_VERSION} - core library, optional GUI, tests")
