# macOS specific settings

if(NOT APPLE)
    return()
endif()

option(BUILD_MACOS_APP "Deploy as a macOS .app" ON)

# openal-soft is built from code/thirdparty (cmake/libraries/openal.cmake) and linked directly: the client calls
# it without function pointers, and the app carries its own copy in Contents/Frameworks (finish_macos_app)
set(USE_OPENAL_DLOPEN OFF CACHE INTERNAL "")

enable_language(OBJC)

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_osx.m)

list(APPEND COMMON_LIBRARIES "-framework Cocoa")

set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0)
set(CMAKE_OSX_ARCHITECTURES arm64)

if(BUILD_MACOS_APP)
    set(CLIENT_EXECUTABLE_OPTIONS MACOSX_BUNDLE)
    list(APPEND POST_CONFIGURE_FUNCTIONS finish_macos_app)
endif()

function(finish_macos_app)
    get_filename_component(MACOS_ICON_FILE ${MACOS_ICON_PATH} NAME)

    set(MACOS_APP_BUNDLE_NAME ${CLIENT_NAME})
    set(MACOS_APP_EXECUTABLE_NAME ${CLIENT_BINARY})
    set(MACOS_APP_GUI_IDENTIFIER ${MACOS_BUNDLE_ID})
    set(MACOS_APP_ICON_FILE ${MACOS_ICON_FILE})
    # The port's release version (identity.cmake); the game's own version stays PROJECT_VERSION (q_version.h)
    set(MACOS_APP_SHORT_VERSION_STRING ${PORT_RELEASE_VERSION})
    set(MACOS_APP_BUNDLE_VERSION ${PORT_RELEASE_VERSION})
    set(MACOS_APP_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET})
    set(MACOS_APP_COPYRIGHT ${COPYRIGHT})

    configure_file(${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
        ${CMAKE_BINARY_DIR}/Info.plist @ONLY)

    set_target_properties(${CLIENT_BINARY} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist)

    set(RESOURCES_DIR $<TARGET_FILE_DIR:${CLIENT_BINARY}>/../Resources)
    add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RESOURCES_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${MACOS_ICON_PATH} ${RESOURCES_DIR})

    # openal-soft goes into Contents/Frameworks under its install name (libopenal.1.dylib, found through the
    # client's rpath), with its license files in Contents/Resources/openal-soft: the LGPL, and the two notices
    # of code built into it (BSD-3Clause, LICENSE-pffft)
    if(TARGET OpenAL)
        set(FRAMEWORKS_DIR $<TARGET_FILE_DIR:${CLIENT_BINARY}>/../Frameworks)
        add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${FRAMEWORKS_DIR} ${RESOURCES_DIR}/openal-soft
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:OpenAL> ${FRAMEWORKS_DIR}/$<TARGET_SONAME_FILE_NAME:OpenAL>
            COMMAND ${CMAKE_COMMAND} -E copy ${INTERNAL_OPENAL_DIR}/COPYING ${INTERNAL_OPENAL_DIR}/BSD-3Clause
                ${INTERNAL_OPENAL_DIR}/LICENSE-pffft ${RESOURCES_DIR}/openal-soft)
        set_property(TARGET ${CLIENT_BINARY} APPEND PROPERTY INSTALL_RPATH @executable_path/../Frameworks)
    endif()

    # Seal the installed bundle with an ad-hoc signature (the Developer ID path below signs instead), and check
    # the seal (game.dylib and cgame.dylib inside it, basegame.cmake). No hardened runtime: its library validation
    # refuses ad-hoc-signed libraries (no team ID), which the app's own are.
    if("$ENV{APPLE_CERTIFICATE_ID}" STREQUAL "")
        install(CODE "
            set(APP \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${INSTALL_BINDIR_FULL}/${CLIENT_BINARY}.app\")
            execute_process(COMMAND codesign --force --sign - \"\${APP}\" COMMAND_ERROR_IS_FATAL ANY)
            execute_process(COMMAND codesign --verify --deep --strict \"\${APP}\" COMMAND_ERROR_IS_FATAL ANY)")
    endif()
endfunction()

if(NOT "$ENV{APPLE_CERTIFICATE_ID}" STREQUAL "")
    list(APPEND POST_CONFIGURE_FUNCTIONS codesign)

    function(codesign)
        set(DEV_ID "Developer ID Application")

        get_directory_property(INSTALL_TARGETS DIRECTORY
            ${CMAKE_SOURCE_DIR} BUILDSYSTEM_TARGETS)

        # Code sign everything that will be installed
        foreach(TARGET IN LISTS INSTALL_TARGETS)
            get_target_property(DESTINATION ${TARGET} INSTALL_DESTINATION)
            if(NOT DESTINATION)
                continue()
            endif()

            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND codesign --force --deep --options runtime
                    --entitlements ${CMAKE_SOURCE_DIR}/cmake/entitlements.plist
                    --sign "$ENV{APPLE_CERTIFICATE_ID}"
                    "$<TARGET_FILE:${TARGET}>"
                COMMENT "Code Signing for macOS: $<TARGET_FILE_BASE_NAME:${TARGET}>")
        endforeach()
    endfunction()
endif()

set(CPACK_GENERATOR "DragNDrop")

set(CPACK_DMG_VOLUME_NAME "${PROJECT_NAME} Installer")
set(CPACK_DMG_BACKGROUND_IMAGE "${CMAKE_SOURCE_DIR}/misc/macos/macos-dmg-background.png")
set(CPACK_DMG_SUBDIRECTORY "${CLIENT_NAME}")

configure_file(
  "${CMAKE_SOURCE_DIR}/misc/macos/macos-dmg-setup.applescript.in"
  "${CMAKE_BINARY_DIR}/macos-dmg-setup.applescript"
  @ONLY
)

set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${CMAKE_BINARY_DIR}/macos-dmg-setup.applescript")
