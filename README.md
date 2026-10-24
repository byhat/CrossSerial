# CrossSerial (`cs`)

Cross-platform serial port library for **Linux / Windows / Android** with a
QSerialPort-flavoured API and **no Qt dependency**. Signals/slots are replaced
by `std::function` callbacks.

```
cs::SerialPort port;
port.setPortName("ttyUSB0");          // Qt-style short name (or "/dev/ttyUSB0")
port.setBaudRate(115200);
port.registerOnReadyRead([&] {        // fires on the worker thread
    auto data = port.readAll();
    ...
});
port.registerOnError([&](cs::SerialError e) { ... });
port.open();
port.write(cmd.data(), cmd.size());   // synchronous, zero-copy
```

## Design

| | |
|---|---|
| Backend | Linux/Windows: **boost::asio** (worker thread + async read loop). Android: **JNI into usb-serial-for-android** (embedded `classes.dex`, no Gradle/Java work for the consumer). |
| pimpl | **None.** Platform members sit directly in the class. On desktop the public header includes `<boost/asio.hpp>`, so consumers need Boost headers (the CMake target propagates `Boost::headers`). |
| Data type | `std::vector<std::uint8_t>` / raw `void*` buffers. |
| write() | Synchronous, **zero-copy**: writes straight from the caller's buffer and returns the byte count (POSIX `write` semantics). `bytesToWrite()` is always 0. |
| Read buffer | Plain linear `std::vector` arena - **no ring buffer**. The kernel writes directly into the arena tail (zero intermediate copies); `read()` copies out once; `readAll()` copies out once. The worker compacts the arena only when it clearly pays off. |
| Callbacks | `registerOnReadyRead(std::function<void()>)`, `registerOnBytesWritten(std::function<void(int64_t)>)`, `registerOnError(std::function<void(SerialError)>)`. Register before `open()`; do not re-register from another thread while open. |
| Threading | One worker thread per port delivers `onReadyRead` / `onError`. `onBytesWritten` fires on the thread that called `write()`. Marshal to your GUI thread yourself (see `examples/qt_qml_terminal`). |

### Why not pimpl

Every call through a pimpl is an extra indirection and, more importantly, the
usual alternatives (fast-pimpl storage, virtual backends) add complexity for
zero benefit in a per-platform shared library. Since an Android `.so` never
needs boost and a desktop one always has it, the members are simply
`#if defined(__ANDROID__)`-switched in the class itself.

### API surface (QSerialPort-compatible where it makes sense)

`cs::SerialPort`: `setPortName/portName`, `open(OpenMode)/close/isOpen`,
`write(const void*, size_t)`, `write(vector)`, `read(void*, max)`, `readAll`,
`setBaudRate/baudRate`, `setDataBits/dataBits`, `setParity/parity`,
`setStopBits/stopBits`, `setFlowControl/flowControl`, `clear(Directions)`,
`flush`, `bytesAvailable`, `bytesToWrite`, `error`, `errorString`,
`clearError`, `waitForReadyRead(ms)`, `waitForBytesWritten(ms)`.

Enum values match Qt numerically (`Parity::EvenParity == 2`,
`StopBits::OneStop == 1`, `StopBits::OneAndHalfStop == 3`, full
`QSerialPort::SerialPortError` list).

`cs::SerialPortInfo`: `portName` (short, Qt-style: `ttyUSB0` / `COM3`),
`systemLocation` (full path), `description`, `manufacturer`, `serialNumber`,
`vendorIdentifier/productIdentifier`, `isBusy`, `isNull`,
`availablePorts()`, `standardBaudRates()`.

Not ported from Qt: DTR/RTS line control, `setReadBufferSize` (the arena grows
on demand; consume it or it grows unbounded, same as QSerialPort without a cap).

### Binary cleanliness

The port is forced into raw mode on Linux (`termios`) and binary mode on
Windows: `\r`/`\n` arrive untouched, `0x00`-`0xFF` passes through, and the
classic cooked-mode killers (`0x04` = EOF, `0x03`/`0x1A` = signals,
`0x11`/`0x13` = XON/XOFF) are disabled - in **both** directions. This is
covered by `tests/test_serial.cpp` (`testBinaryClean`).

## Performance

`cs_bench` (pty loopback, 16 MB in 4 KiB chunks) on the reference machine:

```
cs    throughput  220-270 MB/s     qt (QSerialPort)  211-218 MB/s
raw   (asio loop, no library)      224-227 MB/s
cs    latency     avg 0.2 ms       (4096 B every 400 ms telemetry shape)
```

CrossSerial is on par with a raw asio loop and consistently **faster than
QSerialPort**, because the hot path is one mutex crossing per chunk, one kernel
copy, and zero allocations (writes come from the caller's buffer; the read arena
is reused). The overheads that made the predecessor slow were removed
specifically: no `vector::erase` from the front, no per-write heap allocation,
no double copying.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # needs boost headers
cmake --build build
ctest --test-dir build                           # Linux/macOS: pty-based tests
```

Options: `CROSSSERIAL_BUILD_SHARED` (default ON), `CROSSSERIAL_BUILD_EXAMPLES`
(ON), `CROSSSERIAL_BUILD_TESTS` (ON), `CROSSSERIAL_ANDROID_MIN_API` (21).

Linux deps: `gcc`, `cmake >= 3.16`, `boost` (headers). Windows (MSVC or
mingw): same plus nothing else (`setupapi` is linked automatically). Android:
see **docs/android.md** (NDK + SDK platform + d8 + JDK).

### Using from another CMake project

```sh
cmake --install build --prefix /opt/crossserial
```

```cmake
find_package(crossserial 1.0 REQUIRED)
target_link_libraries(myapp PRIVATE crossserial::crossserial)
```

Boost headers are pulled in automatically on desktop platforms (the public
header includes `<boost/asio.hpp>`). Works for Qt and non-Qt projects alike;
no C ABI is exposed.

## Examples

* `examples/list_ports` - table of all ports with VID/PID/busy state.
* `examples/terminal` - line-based terminal (`cs_terminal <port> [baud] [--hex] [--eol=...]`).
* `examples/qt_qml_terminal` - QML terminal showing Qt integration and callback
  marshaling into the GUI thread (built when Qt6 Quick is found).
* `examples/bench.cpp` - the benchmark above (`cs_bench [total_mb] [all|cs|qt|raw]`;
  the Qt comparison is enabled when Qt6 SerialPort is found).

## Android in one paragraph

The usb-serial-for-android driver plus a small bridge class are compiled into a
`classes.dex` and **embedded into `libcrossserial.so`**; at runtime it is loaded
via `DexClassLoader` - the app bundles only the `.so`. Call
`cs::initAndroid(nullptr, context)` once (JNI_OnLoad captures the JavaVM), then
use the same `cs::SerialPort` API. By default `open()` auto-requests USB
permission with the system dialog (waits up to 30 s);
`setPermissionRequestEnabled(false)` switches to fail-fast `PermissionError`.
Details: **docs/android.md**.

## License

LGPL-3.0-or-later (see LICENSE). The vendored usb-serial-for-android on Android
is Apache-2.0 (see NOTICE).
