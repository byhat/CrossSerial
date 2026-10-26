#pragma once

#include <cs/Export.h>
#include <cs/SerialTypes.hpp>
#include <cs/SerialPortInfo.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
// Android backend talks to usb-serial-for-android over JNI; everything it needs
// lives in the .cpp behind plain members below. No boost on this platform.
#else
#  include <boost/asio.hpp>
#endif

namespace cs {

namespace detail { struct AndroidAccess; }
namespace detail { struct WinRxState; }

#if defined(__ANDROID__)
/** Android only: one-time init. Call once before first use, passing the
 *  JavaVM (may be null if the library was loaded with System.loadLibrary,
 *  JNI_OnLoad captures it) and the android Context (as jobject). */
CS_API SerialError initAndroid(void* java_vm, void* android_context);
#endif

/**
 * @brief Cross-platform serial port with a QSerialPort-like API and no Qt
 *        dependency (Linux / Windows / Android).
 *
 * Desktop platforms are implemented directly on boost::asio: an internal worker
 * thread runs an async read loop that appends into a linear byte buffer; write()
 * is synchronous and copies nothing (it writes straight from the caller's
 * buffer, returning the number of bytes handed to the OS). Android is
 * implemented over JNI into usb-serial-for-android (see docs/android.md).
 *
 * There is deliberately no pimpl of any kind: the platform members sit right in
 * this class. On desktop that means <boost/asio.hpp> is part of the public
 * header, so consumers of the shared library need Boost headers available
 * (CMake target propagates Boost::headers).
 *
 * Threading: onReadyRead / onError fire on the internal worker thread (on
 * Android: the Java I/O manager thread); onBytesWritten fires on the thread
 * that called write(). Register callbacks before open() and do not re-register
 * while the port is open from another thread.
 */
class CS_API SerialPort {
public:
    SerialPort();
    explicit SerialPort(const std::string& portName);
    explicit SerialPort(const SerialPortInfo& info);
    ~SerialPort();

    SerialPort(const SerialPort&)            = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    // Not movable: the class owns an I/O thread (desktop) / a JNI registry
    // entry keyed by its address (Android). Use std::unique_ptr<SerialPort> to
    // move ownership around.

    void setPortName(const std::string& name);
    std::string portName() const;

    bool open(OpenMode mode = OpenMode::ReadWrite);
    void close();
    bool isOpen() const;

    /// Synchronous, zero-copy write from the caller's buffer.
    /// Returns bytes written or -1 on error.
    std::int64_t write(const void* data, std::size_t size);
    std::int64_t write(const std::vector<std::uint8_t>& data);

    /// Reads up to maxSize buffered bytes into buf (never blocks).
    /// Returns bytes read (0 if none) or -1 on error.
    std::int64_t read(void* buf, std::size_t maxSize);
    /// Returns and consumes everything currently buffered.
    std::vector<std::uint8_t> readAll();

    bool setBaudRate(std::int32_t rate, Directions d = Directions::AllDirections);
    std::int32_t baudRate(Directions d = Directions::AllDirections) const;
    bool setDataBits(DataBits b);
    DataBits dataBits() const;
    bool setParity(Parity p);
    Parity parity() const;
    bool setStopBits(StopBits s);
    StopBits stopBits() const;
    bool setFlowControl(FlowControl f);
    FlowControl flowControl() const;

    bool clear(Directions d = Directions::AllDirections);
    bool flush();
    std::int64_t bytesAvailable() const;
    /// Always 0: writes are synchronous, so nothing is ever pending.
    std::int64_t bytesToWrite() const;

    SerialError error() const;
    std::string errorString() const;
    void clearError();

    /// Blocks until buffered data is available or ms milliseconds elapse.
    bool waitForReadyRead(std::int64_t ms = 30000);
    /// Writes are synchronous, so this merely checks the port is open.
    bool waitForBytesWritten(std::int64_t ms = 30000);

    /// Android only: when enabled (default), open() auto-requests USB
    /// permission with a system dialog and waits (up to 30 s) for the grant.
    /// When disabled, open() fails immediately with PermissionError if the
    /// user has not granted permission yet. No-op on desktop platforms.
    void setPermissionRequestEnabled(bool enabled);

    /// Replace the "new data is buffered" callback (worker thread).
    void registerOnReadyRead(std::function<void()> callback);
    /// Replace the "N bytes handed to the OS" callback (writer's thread).
    void registerOnBytesWritten(std::function<void(std::int64_t)> callback);
    /// Replace the error callback (worker thread / writer's thread).
    void registerOnError(std::function<void(SerialError)> callback);

private:
    friend struct detail::AndroidAccess;  // delivers JNI read/error events (Android)

    // ---- shared buffer helpers (bufMx_ must be held) ----
    void appendLocked(const std::uint8_t* data, std::size_t n);
    void compactLocked();
    void notifyReadLocked();

    void setError(SerialError e, std::string msg);
    void fail(SerialError e, std::string msg);  // setError + onError callback
    static std::string resolveDeviceName(const std::string& portName);

    // ---- configuration (applied on open, live when open) ----
    std::string   portName_;
    std::int32_t  baudRate_ = 9600;
    DataBits      dataBits_ = DataBits::Data8;
    Parity        parity_   = Parity::NoParity;
    StopBits      stopBits_ = StopBits::OneStop;
    FlowControl   flowControl_ = FlowControl::NoFlowControl;
    OpenMode      openMode_ = OpenMode::ReadWrite;

    // ---- callbacks (set before open; not re-registered while open) ----
    std::function<void()>                    onReadyRead_;
    std::function<void(std::int64_t)>        onBytesWritten_;
    std::function<void(SerialError)>         onError_;

    // ---- last error ----
    mutable std::mutex errMx_;
    SerialError       errorCode_ = SerialError::NoError;
    std::string       errorText_;

    // ---- linear read buffer: data lives in [head_, end_) of buf_ ----
    // The worker appends at end_ (growing capacity as needed); consumers take
    // from head_. No ring buffer by design: at ~4 KB per 0.4 s a plain vector
    // with occasional compaction beats a ring on simplicity and speed.
    mutable std::mutex        bufMx_;
    std::condition_variable   readCv_;
    std::vector<std::uint8_t> buf_;
    std::size_t               head_ = 0;
    std::size_t               end_  = 0;
    std::atomic<bool>         reading_{false};  // read source alive (worker/Java)

#if defined(__ANDROID__)
    // ---- Android state (JNI machinery itself lives in the .cpp) ----
    std::atomic<bool> androidOpen_{false};
    bool              requestPermission_ = true;

    void androidHandleData(const std::uint8_t* data, std::size_t n);
    void androidHandleError(SerialError e);
    bool androidOpenPort();
    void androidClosePort();
#else
    // ---- desktop state: boost::asio ----
    void applyOptions();
    void applyNativeMode();   // termios raw mode / COMMTIMEOUTS + CMSPAR/DCB extras
    void armRead();           // post the next async_read_some (bufMx_ held)
    void handleRead(const boost::system::error_code& ec, std::size_t n);

    boost::asio::io_context   io_;
    boost::asio::serial_port  port_;   // bound to io_ in every constructor
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> work_;
    std::thread         readThread_;
    std::atomic<bool>   closing_{false};  // our own teardown in progress
    std::mutex          writeMx_;         // serializes concurrent write() calls

#if defined(_WIN32)
    // Windows reader, QSerialPort-style: WaitCommEvent(EV_RXCHAR) wakes the
    // thread the moment bytes land, then a drain ReadFile takes everything the
    // driver holds. asio has no WaitCommEvent, and its plain overlapped
    // ReadFile completes only on buffer-full or COMMTIMEOUTS - whose driver
    // side granularity (~10 ms) showed up as latency after every burst.
    // io_/work_/readThread_ stay idle on Windows (write() is synchronous and
    // needs no io_context::run).
    void eventLoop();  // runs on evThread_
    std::thread evThread_;
    std::unique_ptr<detail::WinRxState> winRx_;
#endif
#endif
};

} // namespace cs

