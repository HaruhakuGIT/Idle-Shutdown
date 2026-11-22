enable_language(C)

find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets DBus Network)
find_package(KF6IdleTime REQUIRED)
find_package(LayerShellQt REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Wayland REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WaylandProtocols REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(IDLE_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml")
if(NOT EXISTS "${IDLE_PROTOCOL}")
    message(FATAL_ERROR "The installed wayland-protocols package does not provide ext-idle-notify-v1.xml.")
endif()

set(IDLE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-client.h")
set(IDLE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-protocol.c")
add_custom_command(
    OUTPUT "${IDLE_HEADER}" "${IDLE_SOURCE}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${IDLE_PROTOCOL}" "${IDLE_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${IDLE_PROTOCOL}" "${IDLE_SOURCE}"
    DEPENDS "${IDLE_PROTOCOL}"
    VERBATIM
)

add_executable(shutdown-warning main.cpp "${IDLE_HEADER}" "${IDLE_SOURCE}")
target_include_directories(shutdown-warning PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/app/common"
)
target_compile_features(shutdown-warning PRIVATE cxx_std_20)
target_compile_options(shutdown-warning PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(shutdown-warning PRIVATE
    Qt6::Widgets Qt6::DBus Qt6::Network
    KF6::IdleTime LayerShellQt::Interface PkgConfig::Wayland
)

include(GNUInstallDirs)
configure_file(shutdown-warning.service shutdown-warning.service @ONLY)
install(TARGETS shutdown-warning DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES shutdown-warning.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES shutdown-warning.svg DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/shutdown-warning.service"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")

if(BUILD_TESTING)
    add_test(NAME linux-render COMMAND shutdown-warning --self-test)
    set_tests_properties(linux-render PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endif()
