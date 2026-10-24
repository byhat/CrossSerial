#include "PortListModel.h"
#include "SerialController.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#ifdef Q_OS_ANDROID
#include <cs/SerialPort.hpp>
#include <QCoreApplication>
#include <QJniEnvironment>
#endif

int main(int argc, char* argv[])
{
#ifdef Q_OS_ANDROID
    // Hands the Android Context to the embedded Java bridge before the first
    // cs::SerialPort is touched; without it open() cannot raise the USB
    // permission dialog and enumeration returns nothing.
    cs::initAndroid(QJniEnvironment::javaVM(),
                    QNativeInterface::QAndroidApplication::context().object());
#endif

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("crossserial terminal"));

    qmlRegisterType<PortListModel>("CrossSerial", 1, 0, "PortListModel");
    qmlRegisterType<SerialController>("CrossSerial", 1, 0, "SerialController");

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/AppQml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
