find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)

# Headers are listed alongside the sources so AUTOMOC picks up the Q_OBJECT
# ones: they live under include/ rather than next to their .cpp, which is
# outside AUTOMOC's same-basename-same-directory heuristic.
add_library(mazeconnect-core
    src/Version.cpp
    src/DeviceManager.cpp
    src/crypto/Identity.cpp
    src/crypto/Sas.cpp
    src/protocol/Framing.cpp
    src/protocol/ReplayWindow.cpp
    src/protocol/Capabilities.cpp
    src/protocol/Messages.cpp
    src/filetransfer/PathSanitizer.cpp
    src/filetransfer/FileTransfer.cpp
    src/plugins/AiBridge.cpp
    src/plugins/CommandRunner.cpp
    src/plugins/GuardBridge.cpp
    src/plugins/StatusProvider.cpp
    src/storage/DeviceStore.cpp
    src/discovery/Beacon.cpp
    src/transport/Connection.cpp
    src/transport/Server.cpp

    include/mazeconnect/core/Version.h
    include/mazeconnect/core/DeviceManager.h
    include/mazeconnect/core/Limits.h
    include/mazeconnect/core/Identity.h
    include/mazeconnect/core/Sas.h
    include/mazeconnect/core/Framing.h
    include/mazeconnect/core/ReplayWindow.h
    include/mazeconnect/core/Capabilities.h
    include/mazeconnect/core/Messages.h
    include/mazeconnect/core/PathSanitizer.h
    include/mazeconnect/core/FileTransfer.h
    include/mazeconnect/core/AiBridge.h
    include/mazeconnect/core/CommandRunner.h
    include/mazeconnect/core/GuardBridge.h
    include/mazeconnect/core/StatusProvider.h
    include/mazeconnect/core/DeviceStore.h
    include/mazeconnect/core/Beacon.h
    include/mazeconnect/core/Connection.h
    include/mazeconnect/core/Server.h
)

target_include_directories(mazeconnect-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(mazeconnect-core
    PUBLIC
        Qt6::Core
        Qt6::Network
    PRIVATE
        OpenSSL::Crypto
)

target_compile_features(mazeconnect-core PUBLIC cxx_std_20)

# Where StatusProvider looks for the status helper. Baked from the install
# prefix so the helper is found beside mazeconnectd wherever the package
# landed; $MAZECONNECT_STATUS_HELPER overrides it for tests and build trees.
include(GNUInstallDirs)
target_compile_definitions(mazeconnect-core
    PRIVATE
        MAZECONNECT_STATUS_HELPER_PATH="${CMAKE_INSTALL_FULL_LIBDIR}/mazeconnect/maze-connect-status"
)

# The helper is what makes the dashboard work at all, so it installs with the
# library rather than with the GUI: the daemon serves statusRequest too.
install(PROGRAMS ../packaging/maze-connect-status
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/mazeconnect"
)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
