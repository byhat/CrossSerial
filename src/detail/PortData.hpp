#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs {
namespace detail {

/** Raw platform-collected port info (turned into cs::SerialPortInfo). */
struct RawPortInfo {
    std::string   portName;        // Qt-style short name
    std::string   systemLocation;  // full device path
    std::string   description;
    std::string   manufacturer;
    std::string   serialNumber;
    std::uint16_t vendorId  = 0;
    std::uint16_t productId = 0;
};

std::vector<RawPortInfo> enumeratePorts();

} // namespace detail
} // namespace cs

