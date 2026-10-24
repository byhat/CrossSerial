#pragma once

#include <cs/Export.h>
#include <cs/SerialTypes.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cs {

/**
 * @brief Information about an available serial port (mirrors QSerialPortInfo).
 *
 * A plain value type: default construction yields a null info (isNull() == true).
 * The only non-inline parts are availablePorts(), isBusy() and standardBaudRates(),
 * which query the platform (sysfs on Linux, setupapi on Windows, USB via JNI on
 * Android).
 */
class CS_API SerialPortInfo {
public:
    SerialPortInfo() = default;
    SerialPortInfo(const SerialPortInfo&) = default;
    SerialPortInfo& operator=(const SerialPortInfo&) = default;
    SerialPortInfo(SerialPortInfo&&) noexcept = default;
    SerialPortInfo& operator=(SerialPortInfo&&) noexcept = default;
    ~SerialPortInfo() = default;

    /// Short port name: "ttyUSB0" (Linux), "COM3" (Windows), "/dev/bus/usb/001/002" (Android).
    std::string portName() const { return portName_; }
    /// Full system path: "/dev/ttyUSB0", "\\.\COM3", same as portName() on Android.
    std::string systemLocation() const { return systemLocation_; }
    std::string description() const { return description_; }
    std::string manufacturer() const { return manufacturer_; }
    std::string serialNumber() const { return serialNumber_; }

    std::uint16_t vendorIdentifier() const { return vid_; }
    std::uint16_t productIdentifier() const { return pid_; }
    bool hasVendorIdentifier() const { return vid_ != 0; }
    bool hasProductIdentifier() const { return pid_ != 0; }

    bool isNull() const { return null_; }
    void swap(SerialPortInfo& other) noexcept;

    /// True if the port is currently opened by someone (best effort on Windows).
    bool isBusy() const;

    /// Query the platform for available serial devices.
    static std::vector<SerialPortInfo> availablePorts();
    /// Baud rates the platform claims to support (same list as QSerialPortInfo).
    static std::vector<std::int32_t> standardBaudRates();

private:
    friend class SerialPort;

    SerialPortInfo(std::string portName, std::string systemLocation,
                   std::string description, std::string manufacturer,
                   std::string serialNumber, std::uint16_t vid, std::uint16_t pid)
        : portName_(std::move(portName)), systemLocation_(std::move(systemLocation)),
          description_(std::move(description)), manufacturer_(std::move(manufacturer)),
          serialNumber_(std::move(serialNumber)), vid_(vid), pid_(pid), null_(false) {}

    std::string   portName_;
    std::string   systemLocation_;
    std::string   description_;
    std::string   manufacturer_;
    std::string   serialNumber_;
    std::uint16_t vid_ = 0;
    std::uint16_t pid_ = 0;
    bool          null_ = true;
};

} // namespace cs

