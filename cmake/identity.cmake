set(PROJECT_NAME openmohaa)
set(PROJECT_VERSION 0.83.0)

set(SERVER_NAME omohaaded)
set(CLIENT_NAME openmohaa)

set(BASEGAME)

set(CGAME_MODULE cgame)
set(GAME_MODULE game)
set(UI_MODULE ui)

# The Metal port's icon (misc/macos/openmohaa-metal.svg, built with port/makeicon.swift)
set(MACOS_ICON_PATH ${CMAKE_SOURCE_DIR}/misc/macos/openmohaa-metal.icns)
# The Metal port's own app identity (its first release, 2026-10-20): the bundle ID, and the release version in
# the Info.plist and the release zip's name. PROJECT_VERSION, the version the game shows and sends, stays upstream's.
set(MACOS_BUNDLE_ID io.github.pstngh.openmohaa-metal)
set(PORT_RELEASE_VERSION 1.0.0)

set(COPYRIGHT "OpenMoHAA Copyright © 2025 OpenMoHAA Team, Inc. All rights reserved.")

set(CONTACT_EMAIL "")
