#include "PortListModel.h"

PortListModel::PortListModel(QObject* parent) : QAbstractListModel(parent)
{
    refresh();
}

int PortListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(ports_.size());
}

QVariant PortListModel::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.row() >= static_cast<int>(ports_.size()))
        return {};
    const auto& p = ports_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case PortNameRole:     return QString::fromStdString(p.portName());
        case DescriptionRole:  return QString::fromStdString(p.description());
        case ManufacturerRole: return QString::fromStdString(p.manufacturer());
        case VidRole:          return p.vendorIdentifier();
        case PidRole:          return p.productIdentifier();
    }
    return {};
}

QHash<int, QByteArray> PortListModel::roleNames() const
{
    return {{PortNameRole, "portName"},
            {DescriptionRole, "description"},
            {ManufacturerRole, "manufacturer"},
            {VidRole, "vid"},
            {PidRole, "pid"}};
}

void PortListModel::refresh()
{
    beginResetModel();
    ports_ = cs::SerialPortInfo::availablePorts();
    endResetModel();
}
