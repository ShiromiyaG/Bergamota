cmake_minimum_required(VERSION 3.25)
project(recomp_runtime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# a lean 3DS system for the recompiled ports: the syscalls and services
# USUM, XY and ORAS ask for (Solstice's docs/lean-runtime.md)
add_library(recomp_runtime STATIC
    src/kernel.cpp
    src/services.cpp
)
target_include_directories(recomp_runtime PUBLIC include)
