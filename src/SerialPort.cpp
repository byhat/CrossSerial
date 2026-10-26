#include <cs/SerialPort.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#if defined(_WIN32)
// ~SerialPort destroys the unique_ptr<WinRxState> member: the type must be
// complete in this TU on Windows.
#include "detail/WinRxState.hpp"
#endif

namespace cs {

namespace {
constexpr std::size_t kReadChunk  = 4096;  // initial read arena (doubles on demand)
constexpr std::size_t kCompactMin = 4096;  // never memmove less than this
} // namespace

// ==== lifecycle =============================================================

SerialPort::SerialPort()
#ifndef __ANDROID__
    : port_(io_)
#endif
{}

SerialPort::SerialPort(const std::string& portName) : SerialPort()
{
    portName_ = portName;
}

SerialPort::SerialPort(const SerialPortInfo& info) : SerialPort()
{
    portName_ = info.portName();
}

SerialPort::~SerialPort()
{
    close();
}

void SerialPort::setPortName(const std::string& name) { portName_ = name; }
std::string SerialPort::portName() const { return portName_; }

// ==== read buffer ===========================================================

// bufMx_ held. Appends n bytes, growing the arena only when the tail is full.
void SerialPort::appendLocked(const std::uint8_t* data, std::size_t n)
{
    if (buf_.size() - end_ < n) {
        compactLocked();
        if (buf_.size() - end_ < n)
            buf_.resize(end_ + n);
    }
    std::memcpy(buf_.data() + end_, data, n);
    end_ += n;
    readCv_.notify_all();
}

// bufMx_ held. Drops consumed bytes with one memmove, but only when it pays.
void SerialPort::compactLocked()
{
    if (head_ == 0) return;
    if (head_ == end_) { head_ = end_ = 0; return; }
    if (head_ >= kCompactMin && head_ * 2 >= end_) {
        std::memmove(buf_.data(), buf_.data() + head_, end_ - head_);
        end_ -= head_;
        head_ = 0;
    }
}

std::int64_t SerialPort::read(void* out, std::size_t maxSize)
{
    if (!out) return 0;
    std::lock_guard<std::mutex> lk(bufMx_);
    std::size_t n = std::min(maxSize, end_ - head_);
    if (n) {
        std::memcpy(out, buf_.data() + head_, n);
        head_ += n;
        // head_ == end_ means "empty"; the worker normalizes the layout back
        // to offset 0 when it arms the next read (never here: a read operation
        // may be pending into the buffer tail at this very moment).
    }
    return static_cast<std::int64_t>(n);
}

std::vector<std::uint8_t> SerialPort::readAll()
{
    std::lock_guard<std::mutex> lk(bufMx_);
    std::vector<std::uint8_t> out(buf_.begin() + head_, buf_.begin() + end_);
    head_ = end_;   // mark consumed; layout normalization is worker-side
    return out;
}

std::int64_t SerialPort::bytesAvailable() const
{
    std::lock_guard<std::mutex> lk(bufMx_);
    return static_cast<std::int64_t>(end_ - head_);
}

std::int64_t SerialPort::bytesToWrite() const { return 0; }  // writes are synchronous

// ==== configuration =========================================================

bool SerialPort::setBaudRate(std::int32_t rate, Directions)
{
    baudRate_ = rate;
#if defined(__ANDROID__)
    return true;  // applied when open() passes it to the driver
#else
    if (!port_.is_open()) return true;
    boost::system::error_code ec;
    port_.set_option(boost::asio::serial_port::baud_rate(static_cast<unsigned>(rate)), ec);
    if (ec) { fail(SerialError::OpenError, "setBaudRate: " + ec.message()); return false; }
    return true;
#endif
}

std::int32_t SerialPort::baudRate(Directions) const { return baudRate_; }

bool SerialPort::setDataBits(DataBits b)
{
    dataBits_ = b;
#if defined(__ANDROID__)
    return true;
#else
    if (!port_.is_open()) return true;
    boost::system::error_code ec;
    port_.set_option(boost::asio::serial_port::character_size(static_cast<unsigned>(b)), ec);
    if (ec) { fail(SerialError::OpenError, "setDataBits: " + ec.message()); return false; }
    return true;
#endif
}

DataBits SerialPort::dataBits() const { return dataBits_; }

bool SerialPort::setParity(Parity p)
{
    parity_ = p;
#if defined(__ANDROID__)
    return true;
#else
    if (port_.is_open()) {
        applyOptions();      // asio covers none/even/odd; space/mark need native mode
        applyNativeMode();
    }
    return true;
#endif
}

Parity SerialPort::parity() const { return parity_; }

bool SerialPort::setStopBits(StopBits s)
{
    stopBits_ = s;
#if !defined(__ANDROID__)
    if (port_.is_open()) applyOptions();
#endif
    return true;
}

StopBits SerialPort::stopBits() const { return stopBits_; }

bool SerialPort::setFlowControl(FlowControl f)
{
    flowControl_ = f;
#if !defined(__ANDROID__)
    if (port_.is_open()) applyOptions();
#endif
    return true;
}

FlowControl SerialPort::flowControl() const { return flowControl_; }

// ==== errors, callbacks, waits ==============================================

void SerialPort::setError(SerialError e, std::string msg)
{
    std::lock_guard<std::mutex> lk(errMx_);
    errorCode_ = e;
    errorText_ = std::move(msg);
}

void SerialPort::fail(SerialError e, std::string msg)
{
    setError(e, std::move(msg));
    if (onError_) onError_(e);
}

SerialError SerialPort::error() const
{
    std::lock_guard<std::mutex> lk(errMx_);
    return errorCode_;
}

std::string SerialPort::errorString() const
{
    std::lock_guard<std::mutex> lk(errMx_);
    return errorText_;
}

void SerialPort::clearError()
{
    std::lock_guard<std::mutex> lk(errMx_);
    errorCode_ = SerialError::NoError;
    errorText_.clear();
}

void SerialPort::registerOnReadyRead(std::function<void()> callback)
{
    onReadyRead_ = std::move(callback);
}

void SerialPort::registerOnBytesWritten(std::function<void(std::int64_t)> callback)
{
    onBytesWritten_ = std::move(callback);
}

void SerialPort::registerOnError(std::function<void(SerialError)> callback)
{
    onError_ = std::move(callback);
}

bool SerialPort::waitForReadyRead(std::int64_t ms)
{
    std::unique_lock<std::mutex> lk(bufMx_);
    bool signaled = readCv_.wait_for(lk, std::chrono::milliseconds(ms), [this] {
        return end_ > head_ || !reading_.load();
    });
    return signaled && end_ > head_;
}

bool SerialPort::waitForBytesWritten(std::int64_t)
{
    return isOpen();  // write() is synchronous: nothing is ever in flight
}

void SerialPort::setPermissionRequestEnabled(bool enabled)
{
#if defined(__ANDROID__)
    requestPermission_ = enabled;
#else
    (void)enabled;
#endif
}

std::int64_t SerialPort::write(const std::vector<std::uint8_t>& data)
{
    return data.empty() ? 0 : write(data.data(), data.size());
}

} // namespace cs
