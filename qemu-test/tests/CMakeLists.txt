cmake_minimum_required(VERSION 3.10)
project(hugetlb-tests C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static -O2 -Wall")
set(CMAKE_EXE_LINKER_FLAGS "-static")

include_directories(src)

add_executable(test-hugetlb
    src/main.c
    src/test_common.c
)

set_target_properties(test-hugetlb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
