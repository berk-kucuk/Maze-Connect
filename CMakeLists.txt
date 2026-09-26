cmake_minimum_required(VERSION 3.25)
project(MazeConnect
    VERSION 1.2.0
    DESCRIPTION "Maze Linux device-link app"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(BUILD_DAEMON "Build the mazeconnectd background daemon" ON)
option(BUILD_GUI "Build the QML pairing/settings UI" ON)
option(BUILD_TESTS "Build the mazeconnect-core test suite" ON)

if(BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

if(BUILD_DAEMON)
    add_subdirectory(daemon)
endif()

if(BUILD_GUI)
    add_subdirectory(app)
endif()

include(GNUInstallDirs)

# The systemd unit goes to the *user* unit directory, never the system one.
# Maze Connect needs no privileges, and installing this among system units
# would run a LAN-facing parser as root — see the header of the unit file.
# Relative on purpose: a cached absolute path baked from CMAKE_INSTALL_PREFIX
# at configure time would ignore a later `cmake --install --prefix`, and the
# unit would land under the wrong prefix.
set(MAZECONNECT_USER_UNIT_DIR "lib/systemd/user"
    CACHE STRING "Install dir for the systemd --user unit, relative to the prefix")

if(BUILD_DAEMON)
    install(FILES daemon/systemd/mazeconnectd.service
        DESTINATION "${MAZECONNECT_USER_UNIT_DIR}"
    )
endif()

# ---- Icons ------------------------------------------------------------------
# Two forms, because they are asked to do different jobs.
#
# The hicolor set is the app icon `maze-connect.desktop` names, and it carries
# its own black ground: a launcher or task switcher puts it against whatever
# the user's wallpaper happens to be.
#
# The mark in pixmaps/ is the same artwork with the ground taken away, for the
# sidebar and the corner watermark. Those are drawn *on* the panel, where a
# black square would read as a hole punched in it.
foreach(size 48 64 128 256 512)
    install(FILES "packaging/icons/maze-connect-${size}.png"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps"
        RENAME maze-connect.png
    )
endforeach()

install(FILES packaging/icons/maze-connect-mark.png
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/pixmaps"
)

# ---- firewalld --------------------------------------------------------------
# Installing the definition only teaches firewalld what "maze-connect" means;
# it does not open anything. Adding it to a zone is the package scriptlet's
# job (packaging/maze-connect.install), so a plain `cmake --install` into a
# staging root never touches a live firewall.
#
# Relative for the same reason as the systemd unit above: firewalld reads
# /usr/lib/firewalld, and a path baked from CMAKE_INSTALL_PREFIX at configure
# time would ignore a later `--prefix`.
set(MAZECONNECT_FIREWALLD_DIR "lib/firewalld/services"
    CACHE STRING "Install dir for the firewalld service, relative to the prefix")

install(FILES packaging/firewalld/maze-connect.xml
    DESTINATION "${MAZECONNECT_FIREWALLD_DIR}"
)
