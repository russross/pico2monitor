cmake_minimum_required(VERSION 3.20)
set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/deps/sdk" CACHE PATH "Pico SDK")
set(PICO_TOOLCHAIN_PATH "${CMAKE_CURRENT_LIST_DIR}/deps/toolchain" CACHE PATH "RISC-V tools")
set(picotool_DIR "${CMAKE_CURRENT_LIST_DIR}/deps/picotool" CACHE PATH "picotool")
set(PICO_BOARD pico2 CACHE STRING "Board")
set(PICO_PLATFORM rp2350-riscv CACHE STRING "Architecture")
set(TINYUSB_OPT_OS OPT_OS_NONE CACHE STRING "TinyUSB scheduling")
include(deps/sdk/external/pico_sdk_import.cmake)
project(pico_monitor C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
if(PICO_NO_FLASH)
    message(FATAL_ERROR "SRAM-executing monitor builds are not supported; use the flash build")
endif()

# The emulator target is deliberately separate from the protected board image.
foreach(target monitor monitor_emu)
    add_executable(${target} src/main.c src/memory.c src/launch.c src/map.c src/program.c src/trap.c src/entry.S src/pmp.c src/io.c src/shell.c
        src/elf.c src/loader.c src/transfer.c src/wire.c src/usb_descriptors.c src/debug.c src/disasm.c)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_compile_definitions(${target} PRIVATE PICO_STACK_SIZE=4096 PICO_CORE1_STACK_SIZE=0
        PICO_HEAP_SIZE=256
        PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1 PICO_STDIO_USB=0 PICO_STDIO_UART=0
        PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow -Wformat=2 -fstack-usage
        -march=rv32imac_zicsr_zifencei_zbs -mabi=ilp32)
    target_link_options(${target} PRIVATE
        -march=rv32imac_zicsr_zifencei_zbs -mabi=ilp32)
    target_link_libraries(${target} pico_stdlib tinyusb_device hardware_riscv_platform_timer
        hardware_watchdog pico_bootrom pico_unique_id)
    # Compiler bit operations avoid the SDK assembly's unconditional Zbb/Zbkb use.
    pico_set_bit_ops_implementation(${target} compiler)
    pico_set_float_implementation(${target} compiler)
    pico_set_double_implementation(${target} compiler)
    pico_set_mem_ops_implementation(${target} compiler)
    pico_set_linker_script(${target} ${CMAKE_CURRENT_LIST_DIR}/src/monitor.ld)
    pico_add_linker_script_override_path(${target} ${CMAKE_CURRENT_LIST_DIR}/src/linker
        FILES default_text_excludes.incl section_extra_post_data.incl)
    if(target STREQUAL monitor_emu)
        target_compile_definitions(${target} PRIVATE MONITOR_EMULATOR=1)
    endif()
    pico_add_extra_outputs(${target})
endforeach()
