cmake_minimum_required(VERSION 3.25)
# optimized unless asked otherwise: a Debug runtime runs the games several
# times slower
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
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
    src/decomp.cpp
    src/interpreter.cpp
    src/cro.cpp
    src/gpu.cpp
    src/services/services.cpp
    src/services/fs.cpp
    src/audio.cpp
    src/pica/pica.cpp
    src/pica/shader.cpp
    src/pica/batch.cpp
    src/pica/vertex.cpp
    src/pica/lighting.cpp
    src/pica/sketch.cpp
    src/services.cpp
)
# the runtime's own renderer on Vulkan (docs/vulkan-renderer.md): its two
# shaders compiled to SPIR-V here, never while a game runs
find_package(Vulkan COMPONENTS glslc)
option(RUNTIME_VULKAN "the runtime's Vulkan renderer (needs the Vulkan SDK)" ${Vulkan_FOUND})
if(RUNTIME_VULKAN)
    set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${SHADER_OUT})
    foreach(stage vert frag)
        add_custom_command(
            OUTPUT ${SHADER_OUT}/pica.${stage}.inc
            COMMAND Vulkan::glslc -O -mfmt=num -o ${SHADER_OUT}/pica.${stage}.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/pica.${stage}
            DEPENDS src/vk/pica.${stage}
            VERBATIM)
    endforeach()
    add_custom_command(
        OUTPUT ${SHADER_OUT}/depth.comp.inc
        COMMAND Vulkan::glslc -O -mfmt=num -o ${SHADER_OUT}/depth.comp.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/depth.comp
        DEPENDS src/vk/depth.comp
        VERBATIM)
    target_sources(recomp_runtime PRIVATE src/vk/renderer.cpp src/pica/texture.cpp
        ${SHADER_OUT}/pica.vert.inc ${SHADER_OUT}/pica.frag.inc ${SHADER_OUT}/depth.comp.inc)
    target_include_directories(recomp_runtime PRIVATE ${SHADER_OUT})
    target_link_libraries(recomp_runtime PUBLIC Vulkan::Vulkan)
    target_compile_definitions(recomp_runtime PUBLIC RUNTIME_VULKAN)
endif()

# third_party/recomp.h: 3dsrecomp's interface to the code it generates (MIT)
target_include_directories(recomp_runtime PUBLIC include third_party)
if(WIN32)
    target_link_libraries(recomp_runtime PUBLIC winmm)
endif()

# drawing with Zakuro's renderer until the native one exists: a Rust static
# library, bridge/zakuro-gpu-c, built with cargo
option(RUNTIME_ZAKURO_GPU "draw with Zakuro's renderer (needs cargo)" ON)
if(RUNTIME_ZAKURO_GPU)
    set(ZGPU_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bridge/zakuro-gpu-c)
    set(ZGPU_LIB ${ZGPU_DIR}/target/release/${CMAKE_STATIC_LIBRARY_PREFIX}zakuro_gpu_c${CMAKE_STATIC_LIBRARY_SUFFIX})
    add_custom_target(zakuro_gpu_c
        COMMAND cargo build --release --manifest-path ${ZGPU_DIR}/Cargo.toml
        BYPRODUCTS ${ZGPU_LIB}
        WORKING_DIRECTORY ${ZGPU_DIR})
    add_dependencies(recomp_runtime zakuro_gpu_c)
    target_compile_definitions(recomp_runtime PUBLIC RUNTIME_ZAKURO_GPU)
    target_include_directories(recomp_runtime PRIVATE bridge)
    target_link_libraries(recomp_runtime PUBLIC ${ZGPU_LIB})
    if(WIN32)
        # what Rust's standard library needs from Windows
        target_link_libraries(recomp_runtime PUBLIC ws2_32 userenv ntdll bcrypt advapi32 kernel32 synchronization ole32 oleaut32 shell32)
    endif()
endif()

enable_testing()
add_executable(kernel_test tests/kernel_test.cpp)
target_link_libraries(kernel_test PRIVATE recomp_runtime)
add_test(NAME kernel COMMAND kernel_test)
add_executable(system_test tests/system_test.cpp)
target_link_libraries(system_test PRIVATE recomp_runtime)
add_test(NAME system COMMAND system_test)
add_executable(lighting_test tests/lighting_test.cpp)
target_link_libraries(lighting_test PRIVATE recomp_runtime)
add_test(NAME lighting COMMAND lighting_test)
add_executable(decomp_test tests/decomp_test.cpp)
target_link_libraries(decomp_test PRIVATE recomp_runtime)
add_test(NAME decomp COMMAND decomp_test)

# boots a game on the runtime and reports how far it gets
add_executable(boot tools/boot.cpp)
target_link_libraries(boot PRIVATE recomp_runtime)

# a game in a window: SDL3 for the window, the input and (later) the sound
option(RUNTIME_PLAYER "build the player (fetches SDL3)" ON)
if(RUNTIME_PLAYER AND RUNTIME_ZAKURO_GPU)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    # the settings file, and the launcher and overlay drawn with Dear ImGui
    FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
    add_executable(player frontend/player.cpp frontend/settings.cpp frontend/launcher.cpp frontend/overlay.cpp)
    target_link_libraries(player PRIVATE recomp_runtime SDL3::SDL3-static imgui tomlplusplus::tomlplusplus)
endif()
