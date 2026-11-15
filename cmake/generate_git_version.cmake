# Generates git_version.hpp at build time and updates it only when values change.
# Usage: cmake -DSOURCE_DIR=<src> -DOUTPUT_FILE=<out> -P generate_git_version.cmake

find_package(Git QUIET)

set(_git_commit "unknown")
set(_git_dirty 0)

if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _git_rev
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_rev)
        set(_git_commit "${_git_rev}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_status)
        set(_git_dirty 1)
    endif()
endif()

get_filename_component(_out_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

set(_tmp_file "${OUTPUT_FILE}.tmp")
file(WRITE "${_tmp_file}"
"#pragma once
#define DESK32_GIT_COMMIT \"${_git_commit}\"
#define DESK32_GIT_DIRTY ${_git_dirty}
")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tmp_file}" "${OUTPUT_FILE}"
)
file(REMOVE "${_tmp_file}")
