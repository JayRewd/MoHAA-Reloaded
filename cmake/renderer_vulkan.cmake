if(NOT BUILD_CLIENT OR NOT BUILD_RENDERER_VULKAN)
    return()
endif()

if(NOT USE_RENDERER_DLOPEN)
    message(FATAL_ERROR "The Vulkan renderer requires USE_RENDERER_DLOPEN")
endif()

include(utils/set_output_dirs)
include(renderer_common)

set(RENDERER_VULKAN_SOURCES
    ${SOURCE_DIR}/renderervk/tr_backend.c
    ${SOURCE_DIR}/renderervk/tr_bsp.c
    ${SOURCE_DIR}/renderervk/tr_cmds.c
    ${SOURCE_DIR}/renderervk/tr_curve.c
    ${SOURCE_DIR}/renderervk/tr_draw.c
    ${SOURCE_DIR}/renderervk/tr_flares.c
    ${SOURCE_DIR}/renderervk/tr_font.cpp
    ${SOURCE_DIR}/renderervk/tr_ghost.cpp
    ${SOURCE_DIR}/renderervk/tr_image.c
    ${SOURCE_DIR}/renderervk/tr_immediate.c
    ${SOURCE_DIR}/renderervk/tr_init.c
    ${SOURCE_DIR}/renderervk/tr_light.c
    ${SOURCE_DIR}/renderervk/tr_main.c
    ${SOURCE_DIR}/renderervk/tr_marks_permanent.c
    ${SOURCE_DIR}/renderervk/tr_marks.c
    ${SOURCE_DIR}/renderervk/tr_model.cpp
    ${SOURCE_DIR}/renderervk/tr_scene.c
    ${SOURCE_DIR}/renderervk/tr_shade_calc.c
    ${SOURCE_DIR}/renderervk/tr_shade.c
    ${SOURCE_DIR}/renderervk/tr_shader.c
    ${SOURCE_DIR}/renderervk/tr_shadows.c
    ${SOURCE_DIR}/renderervk/tr_sky_portal.cpp
    ${SOURCE_DIR}/renderervk/tr_sky.c
    ${SOURCE_DIR}/renderervk/tr_sphere_shade.cpp
    ${SOURCE_DIR}/renderervk/tr_sprite.c
    ${SOURCE_DIR}/renderervk/tr_staticmodels.cpp
    ${SOURCE_DIR}/renderervk/tr_sun_flare.cpp
    ${SOURCE_DIR}/renderervk/tr_surface.c
    ${SOURCE_DIR}/renderervk/tr_swipe.cpp
    ${SOURCE_DIR}/renderervk/tr_terrain.c
    ${SOURCE_DIR}/renderervk/tr_util.cpp
    ${SOURCE_DIR}/renderervk/tr_vis.cpp
    ${SOURCE_DIR}/renderervk/tr_world.c
    ${SOURCE_DIR}/renderervk/vk.c
)

# No OpenGL: the window is created by sdl_vkimp.c instead of sdl_glimp.c
set(SDL_RENDERER_VULKAN_SOURCES
    ${SOURCE_DIR}/sdl/sdl_gamma.c
    ${SOURCE_DIR}/sdl/sdl_vkimp.c
)

set(RENDERER_VULKAN_BASENAME renderer_vulkan)
set(RENDERER_VULKAN_BINARY ${RENDERER_VULKAN_BASENAME})

list(APPEND RENDERER_VULKAN_BINARY_SOURCES
    ${RENDERER_COMMON_SOURCES}
    ${RENDERER_VULKAN_SOURCES}
    ${SDL_RENDERER_VULKAN_SOURCES}
    ${RENDERER_LIBRARY_SOURCES}
    ${DYNAMIC_RENDERER_SOURCES})

add_library(${RENDERER_VULKAN_BINARY} SHARED ${RENDERER_VULKAN_BINARY_SOURCES})

target_link_libraries(      ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_LIBRARIES})
target_include_directories( ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_INCLUDE_DIRS})
target_compile_definitions( ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_DEFINITIONS} USE_VULKAN)
target_compile_options(     ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_COMPILE_OPTIONS})
target_link_options(        ${RENDERER_VULKAN_BINARY} PRIVATE ${RENDERER_LINK_OPTIONS})

set_output_dirs(${RENDERER_VULKAN_BINARY})

INSTALL(TARGETS ${RENDERER_VULKAN_BINARY} DESTINATION ${INSTALL_LIBDIR_FULL})

if(MSVC)
    INSTALL(FILES $<TARGET_PDB_FILE:${RENDERER_VULKAN_BINARY}> DESTINATION ${INSTALL_LIBDIR_FULL} OPTIONAL)
endif()
