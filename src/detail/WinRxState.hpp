#pragma once

// Internal Windows-only state for the event-driven reader of cs::SerialPort.
// Lives behind a forward declaration in the public header so windows.h stays
// out of it; defined here for SerialPort.cpp (complete type in ~SerialPort)
// and SerialPortDesktop.cpp (implementation).

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <vector>

namespace cs {
namespace detail {

struct WinRxState {
    OVERLAPPED waitOv{};   // in-flight WaitCommEvent
    OVERLAPPED readOv{};   // in-flight ReadFile (drain after EV_RXCHAR)
    HANDLE waitEvent = nullptr;  // auto-reset: signalled when waitOv completes
    HANDLE readEvent = nullptr;  // auto-reset: signalled when readOv completes
    std::vector<std::uint8_t> scratch;  // ReadFile landing zone (copied into buf_)

    WinRxState();
    ~WinRxState();

    WinRxState(const WinRxState&)            = delete;
    WinRxState& operator=(const WinRxState&) = delete;
};

} // namespace detail
} // namespace cs
