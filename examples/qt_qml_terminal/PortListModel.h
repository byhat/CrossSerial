#pragma once

#include <cs/SerialPortInfo.hpp>

#include <QAbstractListModel>
#include <QHash>
#include <vector>

// List model over cs::SerialPortInfo::availablePorts() for the port combo box.
class PortListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        PortNameRole = Qt::UserRole + 1,
        DescriptionRole,
        ManufacturerRole,
        VidRole,
        PidRole
    };

    explicit PortListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();

private:
    std::vector<cs::SerialPortInfo> ports_;
};
