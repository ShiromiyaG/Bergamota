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
    src/session.cpp
    src/loader.cpp
    src/async_gpu.cpp
    src/decomp.cpp
    src/interpreter.cpp
    src/cro.cpp
    src/gpu.cpp
    src/services/services.cpp
    src/services/fs.cpp
    src/services/cfg.cpp
    src/services/dsp.cpp
    src/services/ldr_ro.cpp
    src/services/gsp.cpp
    src/services/hid.cpp
    src/services/y2r.cpp
    src/audio.cpp
    src/pica/pica.cpp
    src/pica/shader.cpp
    src/pica/batch.cpp
    src/pica/vertex.cpp
    src/pica/lighting.cpp
    src/pica/sketch.cpp
    src/service_list.cpp
    src/sampler.cpp
)
# the runtime's own renderer on Vulkan (docs/vulkan-renderer.md): its two
# shaders compiled to SPIR-V here, never while a game runs
find_package(Vulkan COMPONENTS glslc shaderc_combined)
option(RUNTIME_VULKAN "the runtime's Vulkan renderer (needs the Vulkan SDK)" ${Vulkan_FOUND})
if(RUNTIME_VULKAN)
    set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${SHADER_OUT})
    foreach(shader pica.vert pica_gpu.vert pica.frag present.vert present.frag)
        add_custom_command(
            OUTPUT ${SHADER_OUT}/${shader}.inc
            COMMAND Vulkan::glslc -O -mfmt=num -o ${SHADER_OUT}/${shader}.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/${shader}
            DEPENDS src/vk/${shader}
            VERBATIM)
    endforeach()
    # the vertex shader's text as well: the frame each translated program
    # goes in (src/vk/translate.cpp)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/pica_gpu.vert.txt
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/vk/pica_gpu.vert -DOUT=${SHADER_OUT}/pica_gpu.vert.txt
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS src/vk/pica_gpu.vert cmake/embed.cmake
        VERBATIM)
    # the fragment shader again for the w buffer, the only one writing the
    # depth itself (which costs the GPU its early depth test)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/pica_wbuffer.frag.inc
        COMMAND Vulkan::glslc -O -mfmt=num -DW_BUFFER -o ${SHADER_OUT}/pica_wbuffer.frag.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/pica.frag
        DEPENDS src/vk/pica.frag
        VERBATIM)
    # and for the shadow output, writing a shadow map through atomics
    add_custom_command(
        OUTPUT ${SHADER_OUT}/pica_shadow.frag.inc
        COMMAND Vulkan::glslc -O -mfmt=num -DSHADOW -o ${SHADER_OUT}/pica_shadow.frag.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/pica.frag
        DEPENDS src/vk/pica.frag
        VERBATIM)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/depth.comp.inc
        COMMAND Vulkan::glslc -O -mfmt=num -o ${SHADER_OUT}/depth.comp.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/vk/depth.comp
        DEPENDS src/vk/depth.comp
        VERBATIM)
    target_sources(recomp_runtime PRIVATE src/vk/renderer.cpp src/vk/device.cpp src/vk/surfaces.cpp
        src/vk/textures.cpp src/vk/draw.cpp src/vk/between.cpp src/vk/screens.cpp src/vk/translate.cpp src/vk/present.cpp src/pica/texture.cpp
        ${SHADER_OUT}/pica.vert.inc ${SHADER_OUT}/pica_gpu.vert.inc ${SHADER_OUT}/pica_gpu.vert.txt ${SHADER_OUT}/pica.frag.inc
        ${SHADER_OUT}/pica_wbuffer.frag.inc ${SHADER_OUT}/pica_shadow.frag.inc ${SHADER_OUT}/present.vert.inc ${SHADER_OUT}/present.frag.inc
        ${SHADER_OUT}/depth.comp.inc)
    target_include_directories(recomp_runtime PRIVATE ${SHADER_OUT})
    # the game's vertex programs compiled while it runs (shaderc, from the SDK, linked in)
    target_link_libraries(recomp_runtime PUBLIC Vulkan::Vulkan Vulkan::shaderc_combined)
    target_compile_definitions(recomp_runtime PUBLIC RUNTIME_VULKAN)
endif()

# third_party/recomp.h: 3dsrecomp's interface to the code it generates (MIT)
target_include_directories(recomp_runtime PUBLIC include third_party)
if(WIN32)
    target_link_libraries(recomp_runtime PUBLIC winmm dbghelp)
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
add_executable(program_test tests/program_test.cpp)
target_link_libraries(program_test PRIVATE recomp_runtime)
add_test(NAME program COMMAND program_test)
if(RUNTIME_VULKAN)
    add_executable(between_test tests/between_test.cpp)
    target_link_libraries(between_test PRIVATE recomp_runtime)
    add_test(NAME between COMMAND between_test)
    add_executable(render_test tests/render_test.cpp)
    target_link_libraries(render_test PRIVATE recomp_runtime)
    add_test(NAME render COMMAND render_test)
endif()

# boots a game on the runtime and reports how far it gets
add_executable(boot tools/boot.cpp)
target_link_libraries(boot PRIVATE recomp_runtime)

# a game in a window: SDL3 for the window, the input and (later) the sound
option(RUNTIME_PLAYER "build the player (fetches SDL3)" ON)
if(RUNTIME_PLAYER)
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
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static Vulkan::Vulkan)
    add_executable(player frontend/player.cpp frontend/settings.cpp frontend/launcher.cpp frontend/overlay.cpp frontend/scenes.cpp
                          frontend/layouts.cpp frontend/battle_ui.cpp)
    target_link_libraries(player PRIVATE recomp_runtime SDL3::SDL3-static imgui tomlplusplus::tomlplusplus)
    # boot's shots of the single screen, composed as the player shows it
    if(RUNTIME_VULKAN)
        target_sources(boot PRIVATE frontend/layouts.cpp frontend/scenes.cpp frontend/battle_ui.cpp)
        target_link_libraries(boot PRIVATE tomlplusplus::tomlplusplus imgui SDL3::SDL3-static)
        target_compile_definitions(boot PRIVATE BOOT_LAYOUTS=1)
    endif()
endif()
