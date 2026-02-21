cmake_minimum_required(VERSION 3.16)

find_package(Git QUIET)
if(GIT_FOUND)
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
project(stem_framework_idf61_lcd)

# Pack generated MJPEG files into the "spiffs" data partition for runtime playback.
# Prefer single-asset packaging to avoid SPIFFS overflow from unrelated files.
set(PRIMARY_MJPEG "")
if(EXISTS "${CMAKE_SOURCE_DIR}/mjpeg_out/space_total.mjpeg")
    set(PRIMARY_MJPEG "${CMAKE_SOURCE_DIR}/mjpeg_out/space_total.mjpeg")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/MJPEG_OUT/space_total.mjpeg")
    set(PRIMARY_MJPEG "${CMAKE_SOURCE_DIR}/MJPEG_OUT/space_total.mjpeg")
endif()

if(PRIMARY_MJPEG)
    # Keep headroom for SPIFFS metadata/pages.
    file(SIZE "${PRIMARY_MJPEG}" PRIMARY_MJPEG_SIZE)
    set(SPIFFS_LIMIT_BYTES 2023424)  # 0x1EE000 from partitions.csv
    set(SPIFFS_SAFE_PAYLOAD 1900000)
    if(PRIMARY_MJPEG_SIZE GREATER SPIFFS_SAFE_PAYLOAD)
        message(FATAL_ERROR
            "space_total.mjpeg too large for current SPIFFS partition. "
            "file=${PRIMARY_MJPEG_SIZE} bytes, safe_limit=${SPIFFS_SAFE_PAYLOAD}, partition=${SPIFFS_LIMIT_BYTES}. "
            "Compress further or enlarge spiffs in partitions.csv.")
    endif()

    set(SPIFFS_STAGE_DIR "${CMAKE_BINARY_DIR}/spiffs_payload")
    file(REMOVE_RECURSE "${SPIFFS_STAGE_DIR}")
    file(MAKE_DIRECTORY "${SPIFFS_STAGE_DIR}")
    file(COPY "${PRIMARY_MJPEG}" DESTINATION "${SPIFFS_STAGE_DIR}")
    spiffs_create_partition_image(spiffs "${SPIFFS_STAGE_DIR}" FLASH_IN_PROJECT)
else()
    if(EXISTS "${CMAKE_SOURCE_DIR}/MJPEG_OUT")
        set(SOLAR_MJPEG_DIR "MJPEG_OUT")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/mjpeg_out")
        set(SOLAR_MJPEG_DIR "mjpeg_out")
    else()
        set(SOLAR_MJPEG_DIR "MJPEG_OUT")
    endif()
    spiffs_create_partition_image(spiffs ${SOLAR_MJPEG_DIR} FLASH_IN_PROJECT)
endif()
