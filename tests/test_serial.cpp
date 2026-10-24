// Functional tests for cs::SerialPort / cs::SerialPortInfo over pty pairs.
// Run via ctest (cs_tests) on Linux/macOS.

#include <cs/SerialPort.hpp>
#include <cs/SerialPortInfo.hpp>

#include <PtyPair.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

using Bytes = std::vector<std::uint8_t>;

Bytes pattern(std::size_t n, std::uint8_t seed = 0)
{
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>((i * 31 + seed) & 0xff);
    return b;
}

// Reads whatever is pending on the master fd (non-blocking after poll).
Bytes readMaster(int fd, std::size_t atLeast, int timeoutMs = 3000)
{
    Bytes out;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (out.size() < atLeast) {
        if (!PtyPair::pollReadable(fd, 50)) {
            if (std::chrono::steady_clock::now() > deadline) break;
            continue;
        }
        std::uint8_t buf[16384];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

// Wait until port buffered >= n bytes (or timeout); returns bytes available.
bool waitHas(cs::SerialPort& p, std::size_t n, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (p.bytesAvailable() < static_cast<std::int64_t>(n)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        p.waitForReadyRead(50);
    }
    return true;
}

// ---- tests -----------------------------------------------------------------

void testOpenClose()
{
    std::printf("testOpenClose\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());
    CHECK(p.isOpen());
    p.close();
    CHECK(!p.isOpen());
    CHECK(p.open());   // reopen on the same object must work
    CHECK(p.isOpen());
    p.close();
}

void testOpenErrors()
{
    std::printf("testOpenErrors\n");
    cs::SerialPort p;
    p.setPortName("/dev/does-not-exist-xyz");
    CHECK(!p.open());
    CHECK(p.error() == cs::SerialError::DeviceNotFoundError);
    CHECK(!p.errorString().empty());
    p.clearError();
    CHECK(p.error() == cs::SerialError::NoError);

    // double open
    PtyPair pty;
    cs::SerialPort q;
    q.setPortName(pty.slavePath());
    CHECK(q.open());
    CHECK(!q.open());  // already open
    CHECK(q.error() == cs::SerialError::OpenError);
    q.close();

    // write to closed port
    cs::SerialPort r;
    const char x = 'x';
    CHECK(r.write(&x, 1) == -1);
    CHECK(r.error() == cs::SerialError::NotOpenError);
}

void testWriteToMaster()
{
    std::printf("testWriteToMaster\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    const auto pat = pattern(10000, 7);
    CHECK(p.write(pat.data(), pat.size())
          == static_cast<std::int64_t>(pat.size()));
    const Bytes got = readMaster(pty.master(), pat.size());
    CHECK(got.size() == pat.size());
    CHECK(std::equal(got.begin(), got.end(), pat.begin()));

    p.close();
}

void testReadFromMaster()
{
    std::printf("testReadFromMaster\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    const auto pat = pattern(5000, 3);
    PtyPair::writeAll(pty.master(), pat.data(), pat.size());
    CHECK(waitHas(p, pat.size()));

    Bytes got(pat.size());
    CHECK(p.read(got.data(), got.size()) == static_cast<std::int64_t>(pat.size()));
    CHECK(std::equal(got.begin(), got.end(), pat.begin()));
    CHECK(p.bytesAvailable() == 0);

    // partial reads
    const auto p2 = pattern(100, 9);
    PtyPair::writeAll(pty.master(), p2.data(), p2.size());
    CHECK(waitHas(p, p2.size()));
    std::uint8_t first[10];
    CHECK(p.read(first, 10) == 10);
    const auto rest = p.readAll();
    CHECK(rest.size() == p2.size() - 10);
    CHECK(std::equal(rest.begin(), rest.end(), p2.begin() + 10));
    CHECK(p.readAll().empty());

    p.close();
}

// Binary cleanliness: every byte value plus the classic cooked-mode killers
// (\r\n translation, 0x04 EOF, 0x03/0x1A signals, 0x11/0x13 XON/XOFF) must
// pass through untouched in both directions.
void testBinaryClean()
{
    std::printf("testBinaryClean\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    Bytes all(256);
    for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    const Bytes msg = []{
        Bytes b;
        b.insert(b.end(), 8, 0);            // NULs
        b.push_back('\r'); b.push_back('\n'); b.push_back('\n'); b.push_back('\r');
        b.push_back(0x04);                  // EOF in cooked mode
        b.push_back(0x03);                  // SIGINT in cooked mode
        b.push_back(0x1a);                  // SUSP in cooked mode
        b.push_back(0x11); b.push_back(0x13);  // XON/XOFF
        return b;
    }();
    Bytes tx = all;
    tx.insert(tx.end(), msg.begin(), msg.end());

    // slave -> master
    CHECK(p.write(tx.data(), tx.size()) == static_cast<std::int64_t>(tx.size()));
    const Bytes m1 = readMaster(pty.master(), tx.size());
    CHECK(m1 == tx);

    // master -> slave
    PtyPair::writeAll(pty.master(), tx.data(), tx.size());
    CHECK(waitHas(p, tx.size()));
    const Bytes m2 = p.readAll();
    CHECK(m2 == tx);

    p.close();
}

void testCallbacks()
{
    std::printf("testCallbacks\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());

    std::atomic<int> readyCalls{0};
    std::atomic<std::int64_t> writtenCb{0};
    std::atomic<int> errCalls{0};
    p.registerOnReadyRead([&] { ++readyCalls; });
    p.registerOnBytesWritten([&](std::int64_t n) { writtenCb = n; });
    p.registerOnError([&](cs::SerialError) { ++errCalls; });

    CHECK(p.open());
    const auto pat = pattern(300, 1);
    CHECK(p.write(pat.data(), pat.size()) == static_cast<std::int64_t>(pat.size()));

    // bytesWritten fires from the writer thread (this thread)
    CHECK(writtenCb.load() == static_cast<std::int64_t>(pat.size()));

    PtyPair::writeAll(pty.master(), pat.data(), pat.size());
    for (int i = 0; i < 100 && readyCalls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(readyCalls.load() > 0);
    CHECK(waitHas(p, pat.size()));
    CHECK(p.readAll().size() == pat.size());
    CHECK(errCalls.load() == 0);  // no errors on a healthy port

    // replacing a callback works
    std::atomic<int> second{0};
    p.registerOnReadyRead([&] { ++second; });
    PtyPair::writeAll(pty.master(), pat.data(), pat.size());
    for (int i = 0; i < 100 && second.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(second.load() > 0);

    p.close();
}

void testWaitForReadyRead()
{
    std::printf("testWaitForReadyRead\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    // nothing pending -> times out
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!p.waitForReadyRead(150));
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(dt >= 100);  // actually waited, not a busy return

    // data arrives after 120 ms
    const auto pat = pattern(64, 5);
    std::thread late([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        PtyPair::writeAll(pty.master(), pat.data(), pat.size());
    });
    CHECK(p.waitForReadyRead(2000));
    late.join();
    CHECK(p.bytesAvailable() >= 1);

    // waitForBytesWritten is trivially true on an open port
    CHECK(p.waitForBytesWritten(100));
    p.close();
}

void testLargeTransfer()
{
    std::printf("testLargeTransfer\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    constexpr std::size_t kTotal = 4u << 20;   // 4 MiB
    constexpr std::size_t kChunk = 4096;
    std::atomic<std::size_t> received{0};
    p.registerOnReadyRead([&] {
        std::uint8_t buf[16384];
        std::int64_t n;
        while ((n = p.read(buf, sizeof buf)) > 0)
            received += static_cast<std::size_t>(n);
    });

    std::thread writer([&] {
        for (std::size_t off = 0; off < kTotal; off += kChunk) {
            const auto pat = pattern(kChunk, static_cast<std::uint8_t>(off >> 12));
            PtyPair::writeAll(pty.master(), pat.data(), pat.size());
        }
    });
    writer.join();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (received.load() < kTotal) {
        if (std::chrono::steady_clock::now() > deadline) break;
        p.waitForReadyRead(50);
    }
    CHECK(received.load() == kTotal);   // no drops, no dups: consumer counts every byte
    p.close();
}

void testByteByByte()
{
    std::printf("testByteByByte\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    const auto pat = pattern(2000, 11);
    for (std::uint8_t b : pat)
        PtyPair::writeAll(pty.master(), &b, 1);

    CHECK(waitHas(p, pat.size(), 5000));
    const Bytes got = p.readAll();
    CHECK(got.size() == pat.size());
    CHECK(std::equal(got.begin(), got.end(), pat.begin()));
    p.close();
}

void testClearAndFlush()
{
    std::printf("testClearAndFlush\n");
    PtyPair pty;
    cs::SerialPort p;
    p.setPortName(pty.slavePath());
    CHECK(p.open());

    const auto pat = pattern(500, 2);
    PtyPair::writeAll(pty.master(), pat.data(), pat.size());
    CHECK(waitHas(p, 1));
    CHECK(p.clear());
    CHECK(p.bytesAvailable() == 0);
    CHECK(p.flush());

    // port still functional after clear
    PtyPair::writeAll(pty.master(), pat.data(), pat.size());
    CHECK(waitHas(p, pat.size()));
    CHECK(p.readAll().size() == pat.size());
    p.close();
}

void testConfig()
{
    std::printf("testConfig\n");
    // config before open is stored
    cs::SerialPort p;
    CHECK(p.setBaudRate(115200));
    CHECK(p.baudRate() == 115200);
    CHECK(p.setDataBits(cs::DataBits::Data7));
    CHECK(p.dataBits() == cs::DataBits::Data7);
    CHECK(p.setParity(cs::Parity::EvenParity));
    CHECK(p.parity() == cs::Parity::EvenParity);
    CHECK(p.setStopBits(cs::StopBits::TwoStop));
    CHECK(p.stopBits() == cs::StopBits::TwoStop);
    CHECK(p.setFlowControl(cs::FlowControl::HardwareControl));
    CHECK(p.flowControl() == cs::FlowControl::HardwareControl);

    // applied at open (pty accepts any sane termios)
    PtyPair pty;
    p.setPortName(pty.slavePath());
    CHECK(p.open());
    CHECK(p.setBaudRate(9600));
    CHECK(p.baudRate() == 9600);
    CHECK(p.setParity(cs::Parity::NoParity));
    CHECK(p.parity() == cs::Parity::NoParity);

    // data still flows after reconfig
    const auto pat = pattern(100, 4);
    CHECK(p.write(pat.data(), pat.size()) == static_cast<std::int64_t>(pat.size()));
    const Bytes got = readMaster(pty.master(), pat.size());
    CHECK(got.size() == pat.size());
    p.close();
}

void testPortInfo()
{
    std::printf("testPortInfo\n");
    cs::SerialPortInfo null;
    CHECK(null.isNull());
    CHECK(null.portName().empty());

    // enumeration should at least not crash; a pty slave is usually NOT listed
    // (only real tty devices are), so no presence assertions here.
    const auto ports = cs::SerialPortInfo::availablePorts();
    for (const auto& pi : ports) {
        CHECK(!pi.isNull());
        CHECK(!pi.portName().empty());
        CHECK(!pi.systemLocation().empty());
        CHECK(pi.portName() != pi.systemLocation());  // Qt-style short vs full
    }

    const auto rates = cs::SerialPortInfo::standardBaudRates();
    CHECK(!rates.empty());
    CHECK(std::find(rates.begin(), rates.end(), 9600) != rates.end());

    // info -> port ctor
    cs::SerialPort fromInfo(null);
    CHECK(fromInfo.portName().empty());
}

void testDestructionWhileActive()
{
    std::printf("testDestructionWhileActive\n");
    PtyPair pty;
    {
        cs::SerialPort p;
        p.setPortName(pty.slavePath());
        p.registerOnReadyRead([&] { p.readAll(); });
        CHECK(p.open());
        const auto pat = pattern(4096, 13);
        PtyPair::writeAll(pty.master(), pat.data(), pat.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // dtor must close() safely with a live worker thread and pending data
    }
    CHECK(true);
}

} // namespace

int main()
{
    testOpenClose();
    testOpenErrors();
    testWriteToMaster();
    testReadFromMaster();
    testBinaryClean();
    testCallbacks();
    testWaitForReadyRead();
    testLargeTransfer();
    testByteByByte();
    testClearAndFlush();
    testConfig();
    testPortInfo();
    testDestructionWhileActive();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
