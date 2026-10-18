# Native Metal renderer (macOS on Apple Silicon), the only renderer: linked into the client (client.cmake).
# A copy of upstream's GL1 renderer whose GL calls go to a Metal backend (code/renderer_metal/tr_mtl.h).

if(NOT BUILD_CLIENT)
    return()
endif()

if(NOT APPLE)
    message(FATAL_ERROR "The Metal renderer is macOS only")
endif()

include(utils/set_output_dirs)
include(renderer_common)

set(RENDERER_METAL_DIR ${SOURCE_DIR}/renderer_metal)

# The window: the native AppKit one (code/macos)
set(RENDERER_METAL_WINDOW_SOURCE ${SOURCE_DIR}/macos/macos_window.m)
list(APPEND RENDERER_LIBRARIES "-framework CoreGraphics")

set(RENDERER_METAL_SOURCES
    ${RENDERER_METAL_DIR}/tr_backend.c
    ${RENDERER_METAL_DIR}/tr_bsp.c
    ${RENDERER_METAL_DIR}/tr_cmds.c
    ${RENDERER_METAL_DIR}/tr_curve.c
    ${RENDERER_METAL_DIR}/tr_draw.c
    ${RENDERER_METAL_DIR}/tr_flares.c
    ${RENDERER_METAL_DIR}/tr_font.cpp
    ${RENDERER_METAL_DIR}/tr_ghost.cpp
    ${RENDERER_METAL_DIR}/tr_image.c
    ${RENDERER_METAL_DIR}/tr_init.c
    ${RENDERER_METAL_DIR}/tr_light.c
    ${RENDERER_METAL_DIR}/tr_main.c
    ${RENDERER_METAL_DIR}/tr_marks_permanent.c
    ${RENDERER_METAL_DIR}/tr_marks.c
    ${RENDERER_METAL_DIR}/tr_model.cpp
    ${RENDERER_METAL_DIR}/tr_mtl.m
    ${RENDERER_METAL_DIR}/tr_scene.c
    ${RENDERER_METAL_DIR}/tr_shade_calc.c
    ${RENDERER_METAL_DIR}/tr_shade.c
    ${RENDERER_METAL_DIR}/tr_shader.c
    ${RENDERER_METAL_DIR}/tr_shadows.c
    ${RENDERER_METAL_DIR}/tr_sky_portal.cpp
    ${RENDERER_METAL_DIR}/tr_sky.c
    ${RENDERER_METAL_DIR}/tr_sphere_shade.cpp
    ${RENDERER_METAL_DIR}/tr_sprite.c
    ${RENDERER_METAL_DIR}/tr_staticmodels.cpp
    ${RENDERER_METAL_DIR}/tr_sun_flare.cpp
    ${RENDERER_METAL_DIR}/tr_surface.c
    ${RENDERER_METAL_DIR}/tr_swipe.cpp
    ${RENDERER_METAL_DIR}/tr_terrain.c
    ${RENDERER_METAL_DIR}/tr_util.cpp
    ${RENDERER_METAL_DIR}/tr_vis.cpp
    ${RENDERER_METAL_DIR}/tr_world.c
    ${RENDERER_METAL_WINDOW_SOURCE}
)

set(RENDERER_METAL_SHADERS
    ${RENDERER_METAL_DIR}/shaders/tr_mtl_final.metal
    ${RENDERER_METAL_DIR}/shaders/tr_mtl_stage.metal
)

# Offline shader compilation with the Metal Toolchain (mounted read-only by port/env.sh's omm_mount_metal),
# embedded in the binary as a C array.
if(DEFINED ENV{OMM_METAL_BIN})
    set(METAL_TOOLCHAIN_BIN_DEFAULT $ENV{OMM_METAL_BIN})
else()
    set(METAL_TOOLCHAIN_BIN_DEFAULT /Volumes/MetalToolchainCryptex/Metal.xctoolchain/usr/bin)
endif()
set(METAL_TOOLCHAIN_BIN ${METAL_TOOLCHAIN_BIN_DEFAULT} CACHE PATH "Folder holding the metal and metallib tools")

set(RENDERER_METAL_GENERATED_DIR ${CMAKE_BINARY_DIR}/renderer_metal)
set(RENDERER_METAL_METALLIB_HEADER ${RENDERER_METAL_GENERATED_DIR}/tr_mtl_metallib.h)
string(REPLACE ";" "|" RENDERER_METAL_SHADER_LIST "${RENDERER_METAL_SHADERS}")

add_custom_command(
    OUTPUT ${RENDERER_METAL_METALLIB_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DMETAL_BIN=${METAL_TOOLCHAIN_BIN}
        -DSOURCES=${RENDERER_METAL_SHADER_LIST}
        -DWORK_DIR=${RENDERER_METAL_GENERATED_DIR}
        -DSYMBOL=tr_mtl_metallib
        -DOUTPUT=${RENDERER_METAL_METALLIB_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/utils/metallib.cmake
    DEPENDS ${RENDERER_METAL_SHADERS} ${CMAKE_SOURCE_DIR}/cmake/utils/metallib.cmake
    COMMENT "Compiling Metal shaders"
    VERBATIM)

list(APPEND RENDERER_METAL_BINARY_SOURCES
    ${RENDERER_COMMON_SOURCES}
    ${RENDERER_METAL_SOURCES}
    ${RENDERER_LIBRARY_SOURCES}
    ${RENDERER_METAL_METALLIB_HEADER})

list(APPEND RENDERER_INCLUDE_DIRS ${RENDERER_METAL_GENERATED_DIR})
list(APPEND RENDERER_LIBRARIES "-framework Metal" "-framework QuartzCore")

set_source_files_properties(
    ${RENDERER_METAL_DIR}/tr_mtl.m
    ${RENDERER_METAL_WINDOW_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
