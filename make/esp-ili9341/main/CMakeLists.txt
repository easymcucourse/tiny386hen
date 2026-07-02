set(TINY386_ESP_SRC_ROOT "${TINY386HEN_SRC_ROOT}/esp/main")
set(TINY386_OVERLAY_ROOT "${TINY386HEN_SRC_ROOT}/tiny386")
set(EMU8950_SRC_ROOT "${TINY386HEN_SRC_ROOT}/emu8950")

set(TINY386_CORE_SOURCES
    "${TINY386_ESP_SRC_ROOT}/adlib.c"
    "${TINY386_REF_ROOT}/fpu.c"
    "${TINY386_OVERLAY_ROOT}/i386.c"
    "${TINY386_ESP_SRC_ROOT}/jit_lx7.c"
    "${TINY386_ESP_SRC_ROOT}/jit_selftest.c"
    "${TINY386_REF_ROOT}/i8042.c"
    "${TINY386_REF_ROOT}/i8254.c"
    "${TINY386_REF_ROOT}/i8259.c"
    "${TINY386_REF_ROOT}/ide.c"
    "${TINY386_REF_ROOT}/ini.c"
    "${TINY386_OVERLAY_ROOT}/pc.c"
    "${TINY386_REF_ROOT}/misc.c"
    "${TINY386_REF_ROOT}/pci.c"
    "${TINY386_ESP_SRC_ROOT}/vga.c"
    "${TINY386_REF_ROOT}/ne2000.c"
    "${TINY386_REF_ROOT}/i8257.c"
    "${TINY386_REF_ROOT}/sb16.c"
    "${TINY386_REF_ROOT}/pcspk.c"
    "${EMU8950_SRC_ROOT}/emu8950.c"
    "${EMU8950_SRC_ROOT}/emuadpcm.c"
)

idf_component_register(
    SRCS
        "${TINY386_ESP_SRC_ROOT}/esp_main.c"
        "${TINY386_ESP_SRC_ROOT}/debugcon.c"
        "${TINY386_ESP_SRC_ROOT}/flash_stdio.c"
        "${TINY386_ESP_SRC_ROOT}/lcd_ili9341.c"
        "${TINY386_ESP_SRC_ROOT}/sdmmc_crc_wrap.c"
        "${TINY386_ESP_SRC_ROOT}/i2s.c"
        "${TINY386_ESP_SRC_ROOT}/storage.c"
        "${TINY386_ESP_SRC_ROOT}/startup_splash.c"
        "${TINY386_ESP_SRC_ROOT}/timestamp_stdio.c"
        "${TINY386_ESP_SRC_ROOT}/usb_input.c"
        "${TINY386_ESP_SRC_ROOT}/wifi.c"
        ${TINY386_CORE_SOURCES}
    INCLUDE_DIRS
        "${TINY386_ESP_SRC_ROOT}"
        "${TINY386_OVERLAY_ROOT}"
        "${TINY386_REF_ROOT}"
        "${EMU8950_SRC_ROOT}"
    REQUIRES
        driver
        esp_mm
        esp_lcd
        esp_partition
        esp_psram
        esp_system
        esp_timer
        fatfs
    LDFRAGMENTS
        linker.lf
)

message(STATUS "Building tiny386hen ESP overlay for BOARD=${BOARD}")
message(STATUS "Using ESP glue from ${TINY386_ESP_SRC_ROOT}")
message(STATUS "Using tiny386 overlays from ${TINY386_OVERLAY_ROOT}")
message(STATUS "Using tiny386 refs from ${TINY386_REF_ROOT}")
message(STATUS "Using emu8950 sources from ${EMU8950_SRC_ROOT}")

add_definitions(-include ${TINY386_ESP_SRC_ROOT}/board_${BOARD}.h)
add_definitions(-DI386_ENABLE_FPU)

# JIT: enable on ESP32 only via per-source compile flags.
set(TINY386_JIT_LEVEL "3" CACHE STRING "JIT opcode enable level")
set(TINY386_JIT_ENABLE_JMP "1" CACHE STRING "Enable JIT unconditional JMP blocks")
set(TINY386_JIT_TRACE "0" CACHE STRING "Enable verbose JIT translate/execute trace")
set(TINY386_JIT_STATS_PERIOD "2048" CACHE STRING "Dump JIT stats every N attempts; 0 disables")
set(TINY386_JIT_UNSUPPORTED_HIST "1" CACHE STRING "Track top unsupported JIT opcodes in stats dumps")
set(TINY386_BENCH_PROFILE "0" CACHE STRING "Benchmark profile: 0=normal boot, 1=selftest, 2=DOS microbench")
set(TINY386_JIT_ENABLE_LINKING "1" CACHE STRING "Enable JIT block linking exit stubs")
set(TINY386_JIT_ENABLE_MEM_HELPERS "1" CACHE STRING "Enable helper-call memory operand JIT actions")
set(TINY386_JIT_ENABLE_CMPTEST_JCC "1" CACHE STRING "Enable CMP/TEST plus Jcc fusion actions")
set(TINY386_JIT_ENABLE_SMC_BITMAP "1" CACHE STRING "Enable translated-code page bitmap before SMC scans")
set(TINY386_JIT_SELFTEST_ONLY "0" CACHE STRING "Run only the JIT board selftest")
set(TINY386_JIT_SELFTEST_AT_BOOT "0" CACHE STRING "Run the JIT board selftest during normal boot")
set(TINY386_JIT_COMPILE_FLAGS
    "-DTINY386_ENABLE_JIT -DTINY386_JIT_LEVEL=${TINY386_JIT_LEVEL} -DTINY386_JIT_ENABLE_JMP=${TINY386_JIT_ENABLE_JMP} -DTINY386_JIT_TRACE=${TINY386_JIT_TRACE} -DTINY386_JIT_STATS_PERIOD=${TINY386_JIT_STATS_PERIOD} -DTINY386_JIT_UNSUPPORTED_HIST=${TINY386_JIT_UNSUPPORTED_HIST} -DTINY386_BENCH_PROFILE=${TINY386_BENCH_PROFILE} -DTINY386_JIT_ENABLE_LINKING=${TINY386_JIT_ENABLE_LINKING} -DTINY386_JIT_ENABLE_MEM_HELPERS=${TINY386_JIT_ENABLE_MEM_HELPERS} -DTINY386_JIT_ENABLE_CMPTEST_JCC=${TINY386_JIT_ENABLE_CMPTEST_JCC} -DTINY386_JIT_ENABLE_SMC_BITMAP=${TINY386_JIT_ENABLE_SMC_BITMAP} -DTINY386_JIT_SELFTEST_ONLY=${TINY386_JIT_SELFTEST_ONLY} -DTINY386_JIT_SELFTEST_AT_BOOT=${TINY386_JIT_SELFTEST_AT_BOOT}")
add_compile_definitions(
    TINY386_BENCH_PROFILE=${TINY386_BENCH_PROFILE}
    TINY386_JIT_SELFTEST_ONLY=${TINY386_JIT_SELFTEST_ONLY}
    TINY386_JIT_SELFTEST_AT_BOOT=${TINY386_JIT_SELFTEST_AT_BOOT})

target_link_options(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=sdmmc_init_spi_crc
    -Wl,--undefined=__wrap_sdmmc_init_spi_crc
    -Wl,--wrap=sdmmc_read_sectors
    -Wl,--undefined=__wrap_sdmmc_read_sectors
    -Wl,--wrap=sdmmc_write_sectors
    -Wl,--undefined=__wrap_sdmmc_write_sectors
    -Wl,--wrap=fopen
    -Wl,--wrap=fread
    -Wl,--wrap=fwrite
    -Wl,--wrap=fseeko
    -Wl,--wrap=ftello
    -Wl,--wrap=fclose)

set_source_files_properties("${TINY386_ESP_SRC_ROOT}/adlib.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${EMU8950_SRC_ROOT}/emu8950.c" PROPERTIES COMPILE_FLAGS "-Wno-all -fsingle-precision-constant")
set_source_files_properties("${EMU8950_SRC_ROOT}/emuadpcm.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/fpu.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_OVERLAY_ROOT}/i386.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3 ${TINY386_JIT_COMPILE_FLAGS}")
set_source_files_properties("${TINY386_ESP_SRC_ROOT}/jit_lx7.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3 ${TINY386_JIT_COMPILE_FLAGS}")
set_source_files_properties("${TINY386_ESP_SRC_ROOT}/jit_selftest.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3 ${TINY386_JIT_COMPILE_FLAGS}")
set_source_files_properties("${TINY386_REF_ROOT}/i8042.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_ESP_SRC_ROOT}/vga.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("${TINY386_OVERLAY_ROOT}/pc.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("${TINY386_REF_ROOT}/ne2000.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/sb16.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("${TINY386_REF_ROOT}/i8257.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
