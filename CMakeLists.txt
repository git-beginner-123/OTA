cmake_minimum_required(VERSION 3.16)

find_package(Git QUIET)
if((NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "") AND GIT_FOUND)
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
