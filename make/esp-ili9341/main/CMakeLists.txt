set(TINY386_ESP_SRC_ROOT "${TINY386HEN_SRC_ROOT}/esp/main")

set(TINY386_CORE_SOURCES
    "${TINY386_ESP_SRC_ROOT}/adlib.c"
    "${TINY386_REF_ROOT}/fpu.c"
    "${TINY386_REF_ROOT}/i386.c"
    "${TINY386_REF_ROOT}/i8042.c"
    "${TINY386_REF_ROOT}/i8254.c"
    "${TINY386_REF_ROOT}/i8259.c"
    "${TINY386_REF_ROOT}/ide.c"
    "${TINY386_REF_ROOT}/ini.c"
    "${TINY386_REF_ROOT}/pc.c"
    "${TINY386_REF_ROOT}/misc.c"
    "${TINY386_REF_ROOT}/pci.c"
    "${TINY386_REF_ROOT}/vga.c"
    "${TINY386_REF_ROOT}/ne2000.c"
    "${TINY386_REF_ROOT}/i8257.c"
    "${TINY386_REF_ROOT}/sb16.c"
    "${TINY386_REF_ROOT}/pcspk.c"
    "${EMU8950_REF_ROOT}/emu8950.c"
    "${EMU8950_REF_ROOT}/emuadpcm.c"
)

idf_component_register(
    SRCS
        "${TINY386_ESP_SRC_ROOT}/esp_main.c"
        "${TINY386_ESP_SRC_ROOT}/lcd_ili9341.c"
        "${TINY386_ESP_SRC_ROOT}/sdmmc_crc_wrap.c"
        "${TINY386_ESP_SRC_ROOT}/i2s.c"
        "${TINY386_ESP_SRC_ROOT}/storage.c"
        "${TINY386_ESP_SRC_ROOT}/usb_input.c"
        "${TINY386_ESP_SRC_ROOT}/wifi.c"
        ${TINY386_CORE_SOURCES}
    INCLUDE_DIRS
        "${TINY386_ESP_SRC_ROOT}"
        "${TINY386_REF_ROOT}"
        "${EMU8950_REF_ROOT}"
    LDFRAGMENTS
        linker.lf
)

message(STATUS "Building tiny386hen ESP overlay for BOARD=${BOARD}")
message(STATUS "Using ESP glue from ${TINY386_ESP_SRC_ROOT}")
message(STATUS "Using tiny386 refs from ${TINY386_REF_ROOT}")
message(STATUS "Using emu8950 refs from ${EMU8950_REF_ROOT}")

add_definitions(-include ${TINY386_ESP_SRC_ROOT}/board_${BOARD}.h)
add_definitions(-DI386_ENABLE_FPU)

target_link_options(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=sdmmc_init_spi_crc
    -Wl,--undefined=__wrap_sdmmc_init_spi_crc)

set_source_files_properties("${TINY386_ESP_SRC_ROOT}/adlib.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${EMU8950_REF_ROOT}/emu8950.c" PROPERTIES COMPILE_FLAGS "-Wno-all -fsingle-precision-constant")
set_source_files_properties("${EMU8950_REF_ROOT}/emuadpcm.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/fpu.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/i386.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("${TINY386_REF_ROOT}/i8042.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/vga.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("${TINY386_REF_ROOT}/pc.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("${TINY386_REF_ROOT}/ne2000.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/sb16.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/i8257.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
