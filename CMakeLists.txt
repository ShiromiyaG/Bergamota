cmake_minimum_required(VERSION 3.25)
project(recomp_runtime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# a lean 3DS system for the recompiled ports: the syscalls and services
# USUM, XY and ORAS ask for (Solstice's docs/lean-runtime.md)
add_library(recomp_runtime STATIC
    src/kernel.cpp
    src/svc.cpp
    src/memory.cpp
    src/system.cpp
    src/loader.cpp
    src/interpreter.cpp
    src/services/services.cpp
    src/services.cpp
)
# third_party/recomp.h: 3dsrecomp's interface to the code it generates (MIT)
target_include_directories(recomp_runtime PUBLIC include third_party)

enable_testing()
add_executable(kernel_test tests/kernel_test.cpp)
target_link_libraries(kernel_test PRIVATE recomp_runtime)
add_test(NAME kernel COMMAND kernel_test)
add_executable(system_test tests/system_test.cpp)
target_link_libraries(system_test PRIVATE recomp_runtime)
add_test(NAME system COMMAND system_test)

# boots a game on the runtime and reports how far it gets
add_executable(boot tools/boot.cpp)
target_link_libraries(boot PRIVATE recomp_runtime)
