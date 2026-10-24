// Lists every serial port the platform reports, QSerialPortInfo-style.

#include <cs/SerialPortInfo.hpp>

#include <iomanip>
#include <iostream>

int main()
{
    const auto ports = cs::SerialPortInfo::availablePorts();
    if (ports.empty()) {
        std::cout << "no serial ports found\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(16) << "PORT"
              << std::setw(8)  << "VID"
              << std::setw(8)  << "PID"
              << std::setw(28) << "DESCRIPTION"
              << std::setw(20) << "MANUFACTURER"
              << std::setw(16) << "BUSY"
              << "LOCATION\n"
              << std::string(110, '-') << '\n';

    for (const auto& p : ports) {
        std::cout << std::left
                  << std::setw(16) << p.portName()
                  << std::setw(8)  << (p.hasVendorIdentifier()
                                         ? std::to_string(p.vendorIdentifier()) : "-")
                  << std::setw(8)  << (p.hasProductIdentifier()
                                         ? std::to_string(p.productIdentifier()) : "-")
                  << std::setw(28) << (p.description().substr(0, 27))
                  << std::setw(20) << (p.manufacturer().substr(0, 19))
                  << std::setw(16) << (p.isBusy() ? "yes" : "no")
                  << p.systemLocation() << '\n';
    }
    return 0;
}
