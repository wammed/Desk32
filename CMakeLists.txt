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

if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=undefined")
    add_compile_definitions(DESK32_SANITIZE_UNDEFINED)
endif()

# Embed build version and auto-updated git commit hash for SessionStart observability
set(DESK32_GIT_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/git_version.hpp")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUTPUT_FILE=${DESK32_GIT_VERSION_HEADER}"
        -P "${CMAKE_SOURCE_DIR}/cmake/generate_git_version.cmake"
)
add_custom_target(desk32_git_version ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUTPUT_FILE=${DESK32_GIT_VERSION_HEADER}"
        -P "${CMAKE_SOURCE_DIR}/cmake/generate_git_version.cmake"
    BYPRODUCTS "${DESK32_GIT_VERSION_HEADER}"
    COMMENT "Checking Git revision for Desk32"
)
include_directories("${CMAKE_BINARY_DIR}/generated")
add_compile_definitions(
    DESK32_APP_VERSION="${PROJECT_VERSION}"
)

# Find Qt 6 Packages (Compatible with Ubuntu 24.04 LTS Qt 6.4.2+ and rolling Linux)
find_package(Qt6 6.4 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    LinguistTools
    Sql
)

find_package(ALSA REQUIRED)
find_package(Vulkan REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(JACK REQUIRED jack)
pkg_check_modules(SOXR REQUIRED soxr)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(TAGLIB QUIET taglib)
if(NOT TAGLIB_FOUND)
    message(FATAL_ERROR
        "TagLib development package ('taglib' via pkg-config) was not found.\n"
        "Please install the required development packages for your environment:\n"
        "  - Ubuntu / Debian (CI): sudo apt-get install -y libtag1-dev pkg-config\n"
        "  - CachyOS / Arch Linux: sudo pacman -S --needed taglib pkgconf"
    )
endif()
message(STATUS "Found TagLib: ${TAGLIB_VERSION} (libraries: ${TAGLIB_LIBRARIES})")

qt_standard_project_setup(REQUIRES 6.4)

# Primary Desk32 Executable
qt_add_executable(desk32
    src/main.cpp
    src/core/LanguageManager.hpp
    src/core/LanguageManager.cpp
    src/core/EngineController.hpp
    src/core/EngineController.cpp
    src/core/log/LogEvent.hpp
    src/core/log/LogRepository.hpp
    src/core/log/LogService.hpp
    src/core/log/LogService.cpp
    src/engine/backend/PipeWireAudioOutput.hpp
    src/engine/backend/PipeWireAudioOutput.cpp
    src/engine/decoder/AudioDecoder.hpp
    src/engine/decoder/FFmpegAudioDecoder.hpp
    src/engine/decoder/FFmpegAudioDecoder.cpp
    src/engine/decoder/DecoderWorker.hpp
    src/engine/decoder/DecoderWorker.cpp
    src/engine/session/FolderSession.hpp
    src/engine/session/FolderSession.cpp
    src/engine/deck/ReferenceDeck.hpp
    src/engine/deck/ReferenceDeck.cpp
    src/engine/deck/DeckManager.hpp
    src/engine/deck/DeckManager.cpp
    src/engine/insert/DelayCompensationBuffer.hpp
    src/engine/insert/ExternalInsertBridge.hpp
    src/engine/insert/ExternalInsertNode.hpp
    src/engine/preset/PresetManager.hpp
    src/engine/preset/PresetManager.cpp
    src/engine/render/WavFileWriter.hpp
    src/engine/render/RenderSequenceManager.hpp
    src/engine/render/RenderSequenceManager.cpp
    src/engine/render/RenderChain.hpp
    src/engine/render/RenderEngine.hpp
    src/engine/render/RenderEngine.cpp
    src/engine/metadata/TrackMetadata.hpp
    src/engine/metadata/TrackMetadata.cpp
    src/engine/workspace/WorkspaceManager.hpp
    src/engine/workspace/WorkspaceManager.cpp
)

# QML Module Setup
qt_add_qml_module(desk32
    URI Desk32
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/DspPreviewPanel.qml
        qml/RenderPanel.qml
        qml/WorkspacePanel.qml
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
        qml/DspPreviewPanel.qml
        qml/RenderPanel.qml
        qml/WorkspacePanel.qml
)

target_link_libraries(desk32 PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Vulkan::Headers
    ${PIPEWIRE_LIBRARIES}
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ALSA::ALSA
)

target_include_directories(desk32 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_dependencies(desk32 desk32_git_version)

target_include_directories(desk32 SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SOXR_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(desk32 PRIVATE ${TAGLIB_CFLAGS_OTHER})

# Testing support
enable_testing()
add_subdirectory(tests)
