cmake_minimum_required(VERSION 3.16)

set(DISPLAY_VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION.txt")
if(EXISTS "${DISPLAY_VERSION_FILE}")
    file(READ "${DISPLAY_VERSION_FILE}" DISPLAY_VERSION_RAW)
    string(STRIP "${DISPLAY_VERSION_RAW}" DISPLAY_VERSION)
endif()

find_package(Git QUIET)
if(DEFINED DISPLAY_VERSION AND NOT DISPLAY_VERSION STREQUAL "")
    set(PROJECT_VER "${DISPLAY_VERSION}")
elseif(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_DESCRIBE_VERSION)
        set(PROJECT_VER "${GIT_DESCRIBE_VERSION}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(game_console_idf61_ili9341)
