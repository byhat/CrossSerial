#pragma once

#include <cs/SerialPort.hpp>

#include <QObject>
#include <QString>
#include <memory>

// QML-facing wrapper around cs::SerialPort. Library callbacks arrive on the
// worker thread and are marshaled into the GUI thread with QueuedConnection.
class SerialController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString portName READ portName NOTIFY connectedChanged)

public:
    explicit SerialController(QObject* parent = nullptr);
    ~SerialController() override;

    Q_INVOKABLE void connectPort(const QString& portName, int baud);
    Q_INVOKABLE void disconnectPort();
    Q_INVOKABLE void send(const QString& text);

    bool connected() const;
    QString portName() const;

signals:
    void connectedChanged();
    void dataReceived(const QString& text);
    void errorOccurred(const QString& message);

private:
    std::unique_ptr<cs::SerialPort> port_;
    QString name_;
};
