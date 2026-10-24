#include "SerialController.h"

#include <QByteArray>
#include <QMetaObject>

SerialController::SerialController(QObject* parent) : QObject(parent), port_(std::make_unique<cs::SerialPort>())
{
    port_->registerOnReadyRead([this] {
        const auto data = port_->readAll();
        if (data.empty()) return;
        const auto text = QString::fromUtf8(
            QByteArray(reinterpret_cast<const char*>(data.data()),
                       static_cast<qsizetype>(data.size())));
        // Worker thread -> GUI thread.
        QMetaObject::invokeMethod(
            this, [this, text] { emit dataReceived(text); }, Qt::QueuedConnection);
    });
    port_->registerOnError([this](cs::SerialError e) {
        const auto msg = QString::fromStdString(port_->errorString());
        QMetaObject::invokeMethod(
            this, [this, msg, e] {
                // An error (e.g. the USB device was yanked) may have closed the
                // port behind our back: re-notify QML so `connected` and every
                // binding that depends on it (Connect/Disconnect button, port
                // combo, ...) re-evaluate. Emit before errorOccurred so the
                // error text ends up as the final status message.
                emit connectedChanged();
                emit errorOccurred(msg + QStringLiteral(" (") + QString::number(static_cast<int>(e)) + QStringLiteral(")"));
            },
            Qt::QueuedConnection);
    });
}

SerialController::~SerialController()
{
    port_->close();
}

bool SerialController::connected() const
{
    return port_ && port_->isOpen();
}

QString SerialController::portName() const
{
    return port_->isOpen() ? name_ : QString();
}

void SerialController::connectPort(const QString& portName, int baud)
{
    if (port_->isOpen()) return;
    name_ = portName;
    port_->setPortName(portName.toStdString());
    port_->setBaudRate(baud);
    if (!port_->open()) {
        emit errorOccurred(QStringLiteral("open failed: ")
                           + QString::fromStdString(port_->errorString()));
        return;
    }
    emit connectedChanged();
}

void SerialController::disconnectPort()
{
    if (!port_->isOpen()) return;
    port_->close();
    emit connectedChanged();
}

void SerialController::send(const QString& text)
{
    if (!port_->isOpen()) return;
    const QByteArray raw = text.toUtf8();
    port_->write(raw.constData(), static_cast<std::size_t>(raw.size()));
}
