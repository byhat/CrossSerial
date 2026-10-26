// Desktop (Linux/Windows) implementation of cs::SerialPort on boost::asio.
// Only compiled when NOT building for Android.

#include <cs/SerialPort.hpp>

#if !defined(__ANDROID__)

#include <cerrno>
#include <cstring>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <termios.h>
#  include <unistd.h>
#else
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace cs {

namespace {
constexpr std::size_t kReadChunk = 4096;  // initial read arena (doubles on demand)

SerialError mapOpenError(const boost::system::error_code& ec)
{
    if (!ec) return SerialError::NoError;
    const int v = ec.value();
#if defined(_WIN32)
    if (v == 2L)  return SerialError::DeviceNotFoundError;  // ERROR_FILE_NOT_FOUND
    if (v == 5L)  return SerialError::PermissionError;      // ERROR_ACCESS_DENIED
    if (v == 32L) return SerialError::ResourceError;        // ERROR_SHARING_VIOLATION
#else
    if (v == ENOENT || v == ENXIO) return SerialError::DeviceNotFoundError;
    if (v == EACCES || v == EPERM) return SerialError::PermissionError;
    if (v == EBUSY)                return SerialError::ResourceError;
#endif
    return SerialError::UnknownError;
}
} // namespace

// ---- name resolution -------------------------------------------------------

std::string SerialPort::resolveDeviceName(const std::string& portName)
{
    if (portName.empty()) return portName;
#if defined(_WIN32)
    if (portName.compare(0, 4, "\\\\.\\") == 0) return portName;  // already \\.\COM3
    // COM names beyond COM9 require the \\.\ prefix; it is valid for all of them.
    if (portName.size() >= 4 &&
        (portName[0] == 'C' || portName[0] == 'c') &&
        (portName[1] == 'O' || portName[1] == 'o') &&
        (portName[2] == 'M' || portName[2] == 'm') &&
        portName[3] >= '0' && portName[3] <= '9')
        return "\\\\.\\" + portName;
    return portName;
#else
    if (portName[0] == '/') return portName;
    return "/dev/" + portName;
#endif
}

// ---- open / close ----------------------------------------------------------

bool SerialPort::open(OpenMode mode)
{
    openMode_ = mode;
    if (port_.is_open()) { fail(SerialError::OpenError, "open: port is already open"); return false; }
    if (portName_.empty()) { fail(SerialError::DeviceNotFoundError, "open: no port name set"); return false; }

    const std::string dev = resolveDeviceName(portName_);
    boost::system::error_code ec;
    port_.open(dev, ec);
    if (ec) {
        fail(mapOpenError(ec), "open " + dev + ": " + ec.message());
        return false;
    }

    clearError();
    applyOptions();
    applyNativeMode();

    // Purge bytes that arrived while the port was closed (and any the driver
    // still holds): delivering them first would front-run fresh data with
    // stale bytes. Must happen before the read loop is armed - once a read is
    // in flight the kernel hands it those bytes and no flush can recall them.
    // QSerialPort purges on open for the same reason.
#if defined(_WIN32)
    ::PurgeComm(port_.native_handle(), PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
    ::tcflush(port_.native_handle(), TCIOFLUSH);
#endif

    {
        std::lock_guard<std::mutex> lk(bufMx_);
        head_ = end_ = 0;
    }

    closing_ = false;
    reading_ = true;
    work_ = std::make_unique<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(io_));
    if (!readThread_.joinable())
        readThread_ = std::thread([this] { io_.run(); });

    {
        std::lock_guard<std::mutex> lk(bufMx_);
        armRead();
    }
    return true;
}

void SerialPort::close()
{
    if (!port_.is_open()) return;

    // Stop the io loop first; once run() has returned nothing touches port_.
    closing_ = true;
    reading_ = false;
    work_.reset();
    io_.stop();
    if (readThread_.joinable()) readThread_.join();
    io_.restart();

    {
        boost::system::error_code ec;
        port_.cancel(ec);
        port_.close(ec);
    }
    {
        std::lock_guard<std::mutex> lk(bufMx_);
        head_ = end_ = 0;
        buf_.clear();          // keeps capacity for the next open
        readCv_.notify_all();
    }
    closing_ = false;
}

bool SerialPort::isOpen() const { return port_.is_open(); }

// ---- options ---------------------------------------------------------------

void SerialPort::applyOptions()
{
    if (!port_.is_open()) return;
    boost::system::error_code ec;
    port_.set_option(boost::asio::serial_port::baud_rate(static_cast<unsigned>(baudRate_)), ec);
    port_.set_option(boost::asio::serial_port::character_size(static_cast<unsigned>(dataBits_)), ec);

    using Par = boost::asio::serial_port::parity;
    switch (parity_) {
        case Parity::NoParity:   port_.set_option(Par(Par::none), ec); break;
        case Parity::EvenParity: port_.set_option(Par(Par::even), ec); break;
        case Parity::OddParity:  port_.set_option(Par(Par::odd),  ec); break;
        default: break;  // space/mark are not in asio; done natively below
    }

    using Stop = boost::asio::serial_port::stop_bits;
    switch (stopBits_) {
        case StopBits::OneStop:        port_.set_option(Stop(Stop::one), ec); break;
        case StopBits::TwoStop:        port_.set_option(Stop(Stop::two), ec); break;
        case StopBits::OneAndHalfStop: port_.set_option(Stop(Stop::onepointfive), ec); break;
    }

    using Flow = boost::asio::serial_port::flow_control;
    switch (flowControl_) {
        case FlowControl::NoFlowControl:   port_.set_option(Flow(Flow::none), ec); break;
        case FlowControl::HardwareControl: port_.set_option(Flow(Flow::hardware), ec); break;
        case FlowControl::SoftwareControl: port_.set_option(Flow(Flow::software), ec); break;
    }
}

void SerialPort::applyNativeMode()
{
    if (!port_.is_open()) return;
#if defined(_WIN32)
    HANDLE h = port_.native_handle();
    COMMTIMEOUTS to{};
    // Interval timeout: an overlapped read completes 1 ms after the last byte
    // (or when the buffer fills). asio cannot WaitCommEvent(EV_RXCHAR) like
    // QSerialPort, so this timeout is the only completion trigger for short
    // bursts; keep it minimal or every request-response round trip eats it as
    // fixed latency. MAXDWORD is not an option: with zero total timeouts it
    // makes ReadFile return immediately, and a zero-byte completion would be
    // re-armed instantly, spinning the io_context thread.
    to.ReadIntervalTimeout         = 1;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;
    ::SetCommTimeouts(h, &to);
    if (parity_ == Parity::SpaceParity || parity_ == Parity::MarkParity) {
        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        if (::GetCommState(h, &dcb)) {
            dcb.fParity = TRUE;
            dcb.Parity  = (parity_ == Parity::SpaceParity) ? SPACEPARITY : MARKPARITY;
            ::SetCommState(h, &dcb);
        }
    }
#else
    const int fd = port_.native_handle();
    struct termios t{};
    if (::tcgetattr(fd, &t) != 0) return;
    // Raw mode: without it the line discipline mangles the payload (CR/LF
    // translation, 0x04 = EOF killing the read loop, 0x03/0x1A = signals,
    // 0x13/0x11 = XOFF/XON). Binary-clean transport requires raw mode.
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL
                   | IXON | IXOFF | IXANY);
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_oflag &= ~OPOST;
    // VMIN=1: with VMIN=0 a non-blocking read on a raw tty returns 0 = EOF,
    // which asio's speculative read would misread as "device gone" and kill
    // the read loop. VMIN=1 makes the empty case return EAGAIN instead, which
    // is what an epoll-driven reactor needs.
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
#  ifdef CMSPAR
    if (parity_ == Parity::SpaceParity) {
        t.c_cflag |= (PARENB | CMSPAR);
        t.c_cflag &= static_cast<tcflag_t>(~PARODD);
    } else if (parity_ == Parity::MarkParity) {
        t.c_cflag |= (PARENB | CMSPAR | PARODD);
    }
#  endif
    ::tcsetattr(fd, TCSANOW, &t);
#endif
}

// ---- read loop -------------------------------------------------------------

// bufMx_ held. Called ONLY from the worker between completions, i.e. when no
// read operation is pending. This is the single place allowed to move data
// around or resize: the armed async read captured its buffer position at post
// time, so consumer threads must never change the layout (they only advance
// head_). Here we first normalize the consumed prefix away, then grow the
// arena if full, then arm the next read at the new end_.
void SerialPort::armRead()
{
    if (!reading_.load() || !port_.is_open()) return;

    if (head_ == end_) {
        head_ = end_ = 0;                     // fully drained: restart at offset 0
    } else if (head_ >= kReadChunk && head_ * 2 >= end_) {
        std::memmove(buf_.data(), buf_.data() + head_, end_ - head_);
        end_ -= head_;
        head_ = 0;
    }
    if (end_ == buf_.size())
        buf_.resize(buf_.empty() ? kReadChunk : buf_.size() * 2);

    const std::size_t space = buf_.size() - end_;
    port_.async_read_some(
        boost::asio::buffer(buf_.data() + end_, space),
        [this](const boost::system::error_code& ec, std::size_t n) { handleRead(ec, n); });
}

void SerialPort::handleRead(const boost::system::error_code& ec, std::size_t n)
{
    if (ec || !reading_.load()) {
        // Errors while we tear the port down ourselves are silent; anything
        // else on a live port means the device is gone.
        if (reading_.load() && !closing_.load()) {
            reading_ = false;
            fail(SerialError::ResourceError, "read: " + ec.message());
        } else {
            reading_ = false;
        }
        return;
    }

    {
        // One lock crossing per chunk: publish the arrived bytes, then arm the
        // next read right away. The user callback runs after the unlock and
        // only touches [head_, end_), which never conflicts with the armed
        // tail region.
        std::lock_guard<std::mutex> lk(bufMx_);
        end_ += n;   // bytes are already in place: the kernel wrote the tail
        readCv_.notify_all();
        armRead();
    }
    if (onReadyRead_) onReadyRead_();   // outside bufMx_: user may call read() here
}

// ---- write / flush / clear -------------------------------------------------

std::int64_t SerialPort::write(const void* data, std::size_t size)
{
    if (size == 0) return 0;
    if (!port_.is_open()) { fail(SerialError::NotOpenError, "write: port not open"); return -1; }

    std::lock_guard<std::mutex> lk(writeMx_);
    boost::system::error_code ec;
    const std::size_t n = boost::asio::write(port_, boost::asio::buffer(data, size), ec);
    if (ec) { fail(SerialError::WriteError, "write: " + ec.message()); return -1; }
    if (onBytesWritten_) onBytesWritten_(static_cast<std::int64_t>(n));
    return static_cast<std::int64_t>(n);
}

bool SerialPort::flush()
{
    if (!port_.is_open()) return false;
#if defined(_WIN32)
    return ::FlushFileBuffers(port_.native_handle()) != 0;
#else
    return ::tcdrain(port_.native_handle()) == 0;
#endif
}

bool SerialPort::clear(Directions d)
{
    if (!port_.is_open()) return false;
#if defined(_WIN32)
    // No PURGE_*ABORT flags: aborting the pending overlapped read would look
    // like an error to the read loop.
    DWORD flags = 0;
    if (static_cast<int>(d) & static_cast<int>(Directions::Input))  flags |= PURGE_RXCLEAR;
    if (static_cast<int>(d) & static_cast<int>(Directions::Output)) flags |= PURGE_TXCLEAR;
    if (!::PurgeComm(port_.native_handle(), flags)) return false;
#else
    int q = 0;
    if (static_cast<int>(d) & static_cast<int>(Directions::Input))  q |= TCIFLUSH;
    if (static_cast<int>(d) & static_cast<int>(Directions::Output)) q |= TCOFLUSH;
    ::tcflush(port_.native_handle(), q);
#endif
    std::lock_guard<std::mutex> lk(bufMx_);
    head_ = end_;   // mark consumed (an async read may be armed into the tail)
    return true;
}

} // namespace cs

#endif // !__ANDROID__
