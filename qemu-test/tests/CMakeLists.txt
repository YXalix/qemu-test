cmake_minimum_required(VERSION 3.10)
project(qemu-e2e-tests C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static -O2 -Wall")
set(CMAKE_EXE_LINKER_FLAGS "-static")

include_directories(src)

# CACHECOW Tests
add_executable(test-cachecow-basic
    src/main.c
    src/test_common.c
    src/test_cachecow_basic.c
)
set_target_properties(test-cachecow-basic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(test-cachecow-mmap
    src/main.c
    src/test_common.c
    src/test_cachecow_mmap.c
)
set_target_properties(test-cachecow-mmap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
