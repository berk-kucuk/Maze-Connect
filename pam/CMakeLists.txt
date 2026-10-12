# pam_mazeconnect: approve sudo/polkit with a paired phone. Root runs this
# code, so it is C with no dependency beyond libpam and libcrypto, built with
# every warning an error and the usual hardening (_FORTIFY_SOURCE comes from
# the distribution's own flags, which makepkg sets).
find_package(OpenSSL REQUIRED)
find_library(PAM_LIBRARY pam REQUIRED)
find_path(PAM_INCLUDE_DIR security/pam_modules.h REQUIRED)
# libsystemd for sd_session_is_remote(): an SSH session is told apart by
# logind, not by environment variables the caller controls.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

set(MAZECONNECT_PAM_DIR "lib/security"
    CACHE STRING "Install dir for PAM modules, relative to the prefix")

set(PAM_HARDENING
    -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wconversion -Wno-sign-conversion
    -fstack-protector-strong -fvisibility=hidden)

add_library(mazeconnect-approval-core STATIC approval_core.c)
set_target_properties(mazeconnect-approval-core PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(mazeconnect-approval-core PRIVATE ${PAM_HARDENING})
target_include_directories(mazeconnect-approval-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mazeconnect-approval-core PUBLIC OpenSSL::Crypto)

add_library(pam_mazeconnect MODULE pam_mazeconnect.c)
set_target_properties(pam_mazeconnect PROPERTIES PREFIX "" C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(pam_mazeconnect PRIVATE ${PAM_HARDENING})
target_include_directories(pam_mazeconnect PRIVATE ${PAM_INCLUDE_DIR})
target_link_libraries(pam_mazeconnect PRIVATE mazeconnect-approval-core ${PAM_LIBRARY}
    PkgConfig::SYSTEMD)
target_link_options(pam_mazeconnect PRIVATE -Wl,-z,relro,-z,now -Wl,--no-undefined)
install(TARGETS pam_mazeconnect LIBRARY DESTINATION "${MAZECONNECT_PAM_DIR}")

# The enrolment tool. Qt Core for JSON, and the same Approval.cpp the app
# uses so that the codes it prints are the codes the phone and the app show.
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(maze-connect-approvers approvers.cpp ../core/src/crypto/Approval.cpp)
target_include_directories(maze-connect-approvers PRIVATE ../core/include)
target_link_libraries(maze-connect-approvers PRIVATE Qt6::Core OpenSSL::Crypto)
include(GNUInstallDirs)
install(TARGETS maze-connect-approvers RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTS)
    add_executable(test_approval_core tests/test_approval_core.c)
    set_target_properties(test_approval_core PROPERTIES C_STANDARD 11)
    target_compile_options(test_approval_core PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_approval_core PRIVATE mazeconnect-approval-core)
    add_test(NAME test_approval_core COMMAND test_approval_core)
endif()

if(BUILD_TESTS)
    # The module through real libpam: pam_start_confdir() loads a service
    # file from the test's own directory, so nothing in /etc is touched.
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(tst_pam tests/tst_pam.cpp)
    target_include_directories(tst_pam PRIVATE ${PAM_INCLUDE_DIR})
    target_link_libraries(tst_pam PRIVATE mazeconnect-core Qt6::Test OpenSSL::Crypto ${PAM_LIBRARY})
    target_compile_definitions(tst_pam PRIVATE
        PAM_MODULE_PATH="$<TARGET_FILE:pam_mazeconnect>")
    add_dependencies(tst_pam pam_mazeconnect)
    add_test(NAME tst_pam COMMAND tst_pam)
    set_tests_properties(tst_pam PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
