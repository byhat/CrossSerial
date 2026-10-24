#pragma once

#include <cstdint>

namespace cs {

/*
 * Enum values intentionally mirror QSerialPort / QSerialPortInfo so that code
 * written against Qt can be ported by renaming the namespace/class only.
 */

enum class DataBits : int {
    Data5 = 5,
    Data6 = 6,
    Data7 = 7,
    Data8 = 8
};

enum class Parity : int {
    NoParity    = 0,
    EvenParity  = 2,  // Qt has no value 1 here; keep Qt numbering
    OddParity   = 3,
    SpaceParity = 4,
    MarkParity  = 5
};

enum class StopBits : int {
    OneStop        = 1,
    TwoStop        = 2,
    OneAndHalfStop = 3
};

enum class FlowControl : int {
    NoFlowControl   = 0,
    HardwareControl = 1,
    SoftwareControl = 2
};

enum class Directions : int {
    Input         = 1,
    Output        = 2,
    AllDirections = Input | Output
};

enum class OpenMode : int {
    ReadOnly  = 1,
    WriteOnly = 2,
    ReadWrite = ReadOnly | WriteOnly
};

enum class SerialError : int {
    NoError                   = 0,
    DeviceNotFoundError,
    PermissionError,
    OpenError,
    ParityError,
    FramingError,
    BreakConditionError,
    WriteError,
    ReadError,
    ResourceError,
    UnsupportedOperationError,
    UnknownError,
    TimeoutError,
    NotOpenError
};

} // namespace cs

