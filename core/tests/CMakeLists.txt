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
)

foreach(test IN LISTS MAZECONNECT_TESTS)
    add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE mazeconnect-core Qt6::Test)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
