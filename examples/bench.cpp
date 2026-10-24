// cs_bench: measures CrossSerial against QSerialPort (if Qt6 is available) and
// against a raw boost::asio loop, all over a pty pair - so the numbers compare
// library overhead, not hardware.
//
//   cs_bench [total_mb=8] [impl=all|cs|qt|raw]
//
// Two modes per implementation:
//   throughput - writer thread pushes `total` bytes in 4096-byte chunks as fast
//                as the pty accepts them; the reader counts until everything
//                arrived. Reports MB/s.
//   latency    - the "4096 bytes every 400 ms" telemetry shape: 25 chunks, the
//                arrival time of each chunk is recorded. Reports avg/max ms.

#include <cs/SerialPort.hpp>

#include <PtyPair.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifdef CS_BENCH_HAVE_QT
#  include <QCoreApplication>
#  include <QMetaObject>
#  include <QSerialPort>
#endif

#include <boost/asio.hpp>

using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kChunk        = 4096;
constexpr int         kLatencyRounds = 25;
constexpr auto        kLatencyPeriod = std::chrono::milliseconds(400);
constexpr int         kTimeoutSec    = 120;

struct Stats {
    double seconds = 0;
    std::vector<double> latencyMs;
};

struct Progress {
    std::mutex m;
    std::condition_variable cv;
    std::size_t got = 0;                 // total bytes received
    std::vector<Clock::time_point> sentAt;   // [round] guarded by m
    std::vector<Clock::time_point> recvAt;   // [round] guarded by m

    // Called by the reader for every n-byte batch. In latency mode records the
    // receive timestamp of each fully-arrived chunk.
    void addBytes(std::size_t n, bool latencyMode)
    {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lk(m);
        if (latencyMode) {
            const std::size_t before = got;
            got += n;
            for (std::size_t k = before / kChunk + 1; k <= got / kChunk; ++k)
                recvAt.push_back(now);
        } else {
            got += n;
        }
        cv.notify_all();
    }

    void noteSent()
    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(m);
        sentAt.push_back(now);
    }

    void waitAll(std::size_t target)
    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(kTimeoutSec),
                         [&] { return got >= target; }))
            throw std::runtime_error("timed out waiting for data");
    }

    bool done(std::size_t target)
    {
        std::lock_guard<std::mutex> lk(m);
        return got >= target;
    }

    Stats finish(Clock::time_point t0, Clock::time_point t1, bool latencyMode)
    {
        Stats st;
        st.seconds = std::chrono::duration<double>(t1 - t0).count();
        if (latencyMode)
            for (std::size_t i = 0; i < sentAt.size() && i < recvAt.size(); ++i)
                st.latencyMs.push_back(
                    std::chrono::duration<double, std::milli>(recvAt[i] - sentAt[i]).count());
        return st;
    }
};

std::vector<std::uint8_t> makePattern()
{
    std::vector<std::uint8_t> p(kChunk);
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = static_cast<std::uint8_t>(i & 0xff);
    return p;
}

void writeRounds(int master, const std::vector<std::uint8_t>& pattern,
                 std::size_t rounds, Progress& pr, bool latencyMode)
{
    for (std::size_t i = 0; i < rounds; ++i) {
        pr.noteSent();
        PtyPair::writeAll(master, pattern.data(), pattern.size());
        if (latencyMode && i + 1 < rounds)
            std::this_thread::sleep_for(kLatencyPeriod);
    }
}

// ---- CrossSerial -----------------------------------------------------------

Stats runCs(std::size_t total, bool latencyMode)
{
    PtyPair pty;
    const auto pattern = makePattern();
    const std::size_t rounds = latencyMode ? kLatencyRounds : total / kChunk;

    cs::SerialPort port;
    port.setPortName(pty.slavePath());
    port.setBaudRate(115200);

    Progress pr;
    port.registerOnReadyRead([&] {
        std::uint8_t buf[16384];
        std::size_t n = 0;
        std::int64_t r;
        while ((r = port.read(buf, sizeof buf)) > 0) n += static_cast<std::size_t>(r);
        if (n) pr.addBytes(n, latencyMode);
    });

    if (!port.open()) throw std::runtime_error("cs open failed: " + port.errorString());

    std::thread writer(writeRounds, pty.master(), std::cref(pattern), rounds,
                       std::ref(pr), latencyMode);

    const Clock::time_point t0 = Clock::now();
    pr.waitAll(rounds * kChunk);
    const Clock::time_point t1 = Clock::now();

    writer.join();
    port.close();
    return pr.finish(t0, t1, latencyMode);
}

// ---- raw boost::asio -------------------------------------------------------

Stats runRaw(std::size_t total, bool latencyMode)
{
    PtyPair pty;
    const auto pattern = makePattern();
    const std::size_t rounds = latencyMode ? kLatencyRounds : total / kChunk;

    boost::asio::io_context io;
    boost::asio::serial_port sp(io, pty.slavePath());

    Progress pr;
    std::vector<unsigned char> buf(16384);
    std::function<void(const boost::system::error_code&, std::size_t)> handler =
        [&](const boost::system::error_code& ec, std::size_t n) {
            if (ec) return;
            if (n) pr.addBytes(n, latencyMode);
            sp.async_read_some(boost::asio::buffer(buf), handler);
        };
    sp.async_read_some(boost::asio::buffer(buf), handler);

    auto guard = boost::asio::make_work_guard(io);
    std::thread ioThread([&] { io.run(); });

    std::thread writer(writeRounds, pty.master(), std::cref(pattern), rounds,
                       std::ref(pr), latencyMode);

    const Clock::time_point t0 = Clock::now();
    pr.waitAll(rounds * kChunk);
    const Clock::time_point t1 = Clock::now();

    writer.join();
    guard.reset();
    io.stop();
    ioThread.join();
    return pr.finish(t0, t1, latencyMode);
}

#ifdef CS_BENCH_HAVE_QT

Stats runQt(QCoreApplication* app, std::size_t total, bool latencyMode)
{
    PtyPair pty;
    const auto pattern = makePattern();
    const std::size_t rounds = latencyMode ? kLatencyRounds : total / kChunk;

    QSerialPort sp;
    sp.setPortName(QString::fromStdString(pty.slavePath()));
    sp.setBaudRate(115200);
    if (!sp.open(QIODevice::ReadWrite))
        throw std::runtime_error("qt open failed: " + sp.errorString().toStdString());

    Progress pr;
    QObject::connect(&sp, &QSerialPort::readyRead, [&] {
        std::size_t n = 0;
        while (sp.bytesAvailable() > 0) {
            const QByteArray a = sp.readAll();
            if (a.isEmpty()) break;
            n += static_cast<std::size_t>(a.size());
        }
        if (n) {
            pr.addBytes(n, latencyMode);
            if (pr.done(rounds * kChunk))
                QMetaObject::invokeMethod(app, &QCoreApplication::quit,
                                          Qt::QueuedConnection);
        }
    });

    std::thread writer(writeRounds, pty.master(), std::cref(pattern), rounds,
                       std::ref(pr), latencyMode);

    const Clock::time_point t0 = Clock::now();
    app->exec();
    const Clock::time_point t1 = Clock::now();

    writer.join();
    sp.close();
    return pr.finish(t0, t1, latencyMode);
}

#endif // CS_BENCH_HAVE_QT

void report(const char* impl, const char* mode, const Stats& st, std::size_t bytes)
{
    std::cout << std::left << std::setw(6) << impl << std::setw(12) << mode
              << std::fixed << std::setprecision(2);
    if (st.latencyMs.empty()) {
        std::cout << std::setw(9) << (bytes / st.seconds / (1024.0 * 1024.0))
                  << " MB/s  (" << st.seconds << " s)\n";
    } else {
        const double avg = std::accumulate(st.latencyMs.begin(), st.latencyMs.end(), 0.0)
                           / static_cast<double>(st.latencyMs.size());
        const double maxv = *std::max_element(st.latencyMs.begin(), st.latencyMs.end());
        std::cout << "avg " << std::setw(7) << avg << " ms   max " << std::setw(7)
                  << maxv << " ms\n";
    }
    std::cout.flush();
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t totalMb = 8;
    std::string impl = "all";
    if (argc > 1) totalMb = static_cast<std::size_t>(std::atoi(argv[1]));
    if (argc > 2) impl = argv[2];
    const std::size_t total = totalMb * 1024 * 1024;

#ifdef CS_BENCH_HAVE_QT
    QCoreApplication app(argc, argv);
#endif

    std::cout << "pty loopback: throughput " << totalMb << " MB in " << kChunk
              << "-byte chunks + latency " << kLatencyRounds << "x" << kChunk
              << "B @400ms\n\n";

    try {
        if (impl == "all" || impl == "cs")
            report("cs", "throughput", runCs(total, false), total);
#ifdef CS_BENCH_HAVE_QT
        if (impl == "all" || impl == "qt")
            report("qt", "throughput", runQt(&app, total, false), total);
#endif
        if (impl == "all" || impl == "raw")
            report("raw", "throughput", runRaw(total, false), total);

        const std::size_t latBytes = kLatencyRounds * kChunk;
        if (impl == "all" || impl == "cs")
            report("cs", "latency", runCs(latBytes, true), latBytes);
#ifdef CS_BENCH_HAVE_QT
        if (impl == "all" || impl == "qt")
            report("qt", "latency", runQt(&app, latBytes, true), latBytes);
#endif
        if (impl == "all" || impl == "raw")
            report("raw", "latency", runRaw(latBytes, true), latBytes);
    } catch (const std::exception& e) {
        std::cerr << "bench failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
