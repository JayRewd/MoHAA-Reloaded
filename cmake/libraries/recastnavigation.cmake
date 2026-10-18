# Recast navigation
# EXCLUDE_FROM_ALL: built only as game's dependencies, and their install rules (headers, static libraries)
# stay out of the install prefix
add_subdirectory(${SOURCE_DIR}/thirdparty/recastnavigation/Detour recastnav_detour EXCLUDE_FROM_ALL)
add_subdirectory(${SOURCE_DIR}/thirdparty/recastnavigation/DetourCrowd recastnav_crowd EXCLUDE_FROM_ALL)
add_subdirectory(${SOURCE_DIR}/thirdparty/recastnavigation/Recast recacstnav_recast EXCLUDE_FROM_ALL)
# Enable position independant code on recast navigation libraries.
# Otherwise linking will fail relocating some functions on Linux
set_property(TARGET Detour DetourCrowd Recast PROPERTY POSITION_INDEPENDENT_CODE ON)

set_target_properties(Detour DetourCrowd Recast PROPERTIES LINKER_LANGUAGE CXX)
