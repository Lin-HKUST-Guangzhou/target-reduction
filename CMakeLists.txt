cmake_minimum_required(VERSION 3.18)

project(target_reduction LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(target-reduction
    src/main.cpp
    src/command_manager.cpp
    src/aig_manager.cpp
    src/target_reduction/target_reduction.cpp
    src/target_reduction/support.cpp
    src/utils/strash.cpp
    src/utils/miaig.cpp
    src/utils/packed_truthtable.cpp
)

target_include_directories(target-reduction PRIVATE
    src
    third_party/CLI11/include
    third_party/cli/include
)

target_compile_options(target-reduction PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-O3>
)

target_link_libraries(target-reduction PRIVATE pthread)
