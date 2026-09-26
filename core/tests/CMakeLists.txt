find_package(Qt6 REQUIRED COMPONENTS Test)

# Each test is its own QTEST_MAIN binary, registered with ctest.
set(MAZECONNECT_TESTS
    tst_version
    tst_crypto
    tst_framing
    tst_replaywindow
    tst_pathsanitizer
    tst_transport
    tst_filetransfer
    tst_statusprovider
    tst_commandrunner
    tst_aibridge
    tst_guardbridge
    tst_devicemanager
    tst_interop
    tst_phonelink
)

foreach(test IN LISTS MAZECONNECT_TESTS)
    add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE mazeconnect-core Qt6::Test)
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# The media bridge talks to MPRIS players over D-Bus, so its test registers a
# fake player on a bus of its own. dbus-run-session gives it one — private to
# the test, never the user's desktop session — and without it the bus-backed
# cases skip rather than fail.
add_executable(tst_mediabridge tst_mediabridge.cpp)
target_link_libraries(tst_mediabridge PRIVATE mazeconnect-core Qt6::Test Qt6::DBus)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME tst_mediabridge COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_mediabridge>)
else()
    add_test(NAME tst_mediabridge COMMAND tst_mediabridge)
endif()
