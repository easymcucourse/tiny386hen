cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED BOARD)
    if(DEFINED ENV{BOARD})
        set(BOARD $ENV{BOARD})
    else()
        set(BOARD "ili9341")
    endif()
endif()

if(NOT BOARD STREQUAL "ili9341")
    message(FATAL_ERROR "This ESP overlay currently supports BOARD=ili9341 only")
endif()

set(TINY386HEN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(TINY386HEN_SRC_ROOT "${TINY386HEN_ROOT}/src")
set(TINY386_REF_ROOT "${TINY386HEN_ROOT}/refs/tiny386")
set(EMU8950_REF_ROOT "${TINY386HEN_ROOT}/refs/emu8950")

if(NOT EXISTS "${TINY386_REF_ROOT}/i386.c")
    message(FATAL_ERROR "refs/tiny386 is missing. Run git submodule update --init --recursive")
endif()
if(NOT EXISTS "${EMU8950_REF_ROOT}/emu8950.c")
    message(FATAL_ERROR "refs/emu8950 is missing. Run git submodule update --init --recursive")
endif()

set(BOARD_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${BOARD}")
if(EXISTS "${BOARD_SDKCONFIG}")
    file(COPY_FILE "${BOARD_SDKCONFIG}" "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" ONLY_IF_DIFFERENT)
endif()
set(SDKCONFIG_DEFAULTS "${BOARD_SDKCONFIG}")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tiny386hen_ili9341)
