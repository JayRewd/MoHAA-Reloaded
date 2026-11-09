# Configure the .desktop entries with the arch suffix
configure_file(
    misc/linux/org.openmoh.openmohaa.desktop.in
    ${CMAKE_BINARY_DIR}/misc/linux/org.openmoh.openmohaa.desktop
    @ONLY
)
configure_file(
    misc/linux/org.openmoh.openmohaab.desktop.in
    ${CMAKE_BINARY_DIR}/misc/linux/org.openmoh.openmohaab.desktop
    @ONLY
)
configure_file(
    misc/linux/org.openmoh.openmohaas.desktop.in
    ${CMAKE_BINARY_DIR}/misc/linux/org.openmoh.openmohaas.desktop
    @ONLY
)

# Install .desktop entries
install(FILES ${CMAKE_BINARY_DIR}/misc/linux/org.openmoh.openmohaa.desktop  DESTINATION ${INSTALL_DATADIR_FULL}/applications)
install(FILES ${CMAKE_BINARY_DIR}/misc/linux/org.openmoh.openmohaab.desktop DESTINATION ${INSTALL_DATADIR_FULL}/applications)
install(FILES ${CMAKE_BINARY_DIR}/misc/linux/org.openmoh.openmohaas.desktop DESTINATION ${INSTALL_DATADIR_FULL}/applications)

install(FILES misc/linux/org.openmoh.openmohaa.metainfo.xml DESTINATION ${INSTALL_DATADIR_FULL}/metainfo)

# The games' icons, as MoHAA Launcher shows them (Allied Assault's the transparent gold star)
install(FILES launcher/assets/game-aa.svg DESTINATION ${INSTALL_DATADIR_FULL}/icons/hicolor/scalable/apps/ RENAME org.openmoh.openmohaa.svg)
install(FILES launcher/assets/game-sh.svg DESTINATION ${INSTALL_DATADIR_FULL}/icons/hicolor/scalable/apps/ RENAME org.openmoh.openmohaas.svg)
install(FILES launcher/assets/game-bt.svg DESTINATION ${INSTALL_DATADIR_FULL}/icons/hicolor/scalable/apps/ RENAME org.openmoh.openmohaab.svg)
