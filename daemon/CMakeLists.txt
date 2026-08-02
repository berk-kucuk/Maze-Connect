find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(mazeconnectd
    src/main.cpp
)

# Matches ExecStart= in the systemd unit. Not in bin/: this is a background
# service the user is not meant to invoke by hand.
install(TARGETS mazeconnectd
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/mazeconnect"
)

target_link_libraries(mazeconnectd
    PRIVATE
        mazeconnect-core
        Qt6::Core
        Qt6::Network
)
