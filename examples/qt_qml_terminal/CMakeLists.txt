set(CMAKE_AUTOMOC ON)

qt_add_executable(cs_qml_terminal
    main.cpp
    SerialController.cpp
    PortListModel.cpp
)

qt_add_qml_module(cs_qml_terminal
    URI AppQml
    VERSION 1.0
    QML_FILES Main.qml
)

target_link_libraries(cs_qml_terminal PRIVATE
    Qt6::Quick
    Qt6::Qml
    crossserial
)

if(ANDROID)
    # Custom manifest on top of Qt's template: declares the usb-host feature
    # (not required, so the app still installs anywhere) - needed for serial
    # USB adapters to be usable/enumerable on device.
    # NB: the property name is QT_ANDROID_PACKAGE_SOURCE_DIR in Qt 6.9 (no
    # ECTORY suffix) - that's what the deployment-settings genex reads.
    set_target_properties(cs_qml_terminal PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
else()
    set_target_properties(cs_qml_terminal PROPERTIES
        MACOSX_BUNDLE TRUE
        WIN32_EXECUTABLE TRUE
    )
endif()
