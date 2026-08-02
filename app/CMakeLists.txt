find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2)
find_package(KF6WindowSystem REQUIRED)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(maze-connect
    src/main.cpp
    src/Backend.cpp
    src/Backend.h
    src/DeviceListModel.cpp
    src/DeviceListModel.h
    src/ActivityLog.cpp
    src/ActivityLog.h
    src/TransferModel.cpp
    src/TransferModel.h
    src/WindowEffects.cpp
    src/WindowEffects.h
    src/SingleInstance.cpp
    src/SingleInstance.h
    src/Tray.cpp
    src/Tray.h
    src/Autostart.cpp
    src/Autostart.h
)

# `pragma Singleton` alone isn't enough for qt_add_qml_module to emit a
# `singleton Theme ...` line in the generated qmldir — without this property
# it registers Theme as a plain type, and every unqualified `Theme.foo`
# reference in QML silently resolves to `undefined` instead of failing.
set_source_files_properties(qml/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(maze-connect
    URI MazeConnect.App
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/components/GlassPanel.qml
        qml/components/MazeButton.qml
        qml/components/MazeIcon.qml
        qml/components/Meter.qml
        qml/components/NavButton.qml
        qml/components/Pill.qml
        qml/components/SectionLabel.qml
        qml/components/TitleBar.qml
        qml/components/VerificationCode.qml
        qml/components/WindowButton.qml
        qml/views/ActivityView.qml
        qml/views/CommandsView.qml
        qml/views/DashboardView.qml
        qml/views/DevicesView.qml
        qml/views/GuardView.qml
        qml/views/FileOfferOverlay.qml
        qml/views/PairingOverlay.qml
        qml/views/SettingsView.qml
        qml/views/TransfersView.qml
)

target_include_directories(maze-connect PRIVATE src)

install(TARGETS maze-connect
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES packaging/maze-connect.desktop
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
    OPTIONAL
)

target_link_libraries(maze-connect
    PRIVATE
        mazeconnect-core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        KF6::WindowSystem
)
