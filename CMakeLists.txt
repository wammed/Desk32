cmake_minimum_required(VERSION 3.22)

project(desk32
    VERSION 0.1.0
    DESCRIPTION "Audiophile Studio-Console-Style Hi-Fi Music Player"
    HOMEPAGE_URL "https://github.com/wammed/Desk32"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warning flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
    )
endif()

# Find Qt 6 Packages
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    LinguistTools
    Sql
)

find_package(ALSA REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(JACK REQUIRED jack)
pkg_check_modules(SOXR REQUIRED soxr)

qt_standard_project_setup(REQUIRES 6.5)

# Primary Desk32 Executable
qt_add_executable(desk32
    src/main.cpp
    src/core/LanguageManager.hpp
    src/core/LanguageManager.cpp
)

# QML Module Setup
qt_add_qml_module(desk32
    URI Desk32
    VERSION 1.0
    QML_FILES
        qml/Main.qml
    RESOURCES
        images/Desk32-Icon.svg
)

# Translations (Full Bilingual Support)
qt_add_translations(desk32
    TS_FILES
        i18n/desk32_en.ts
        i18n/desk32_ja.ts
    SOURCES
        src/main.cpp
        qml/Main.qml
)

target_link_libraries(desk32 PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

target_include_directories(desk32 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Testing support
enable_testing()
add_subdirectory(tests)
