// Line-based serial terminal: stdin lines -> port, port -> stdout.
//
//   cs_terminal <port> [baud=115200] [--hex] [--eol=lf|crlf|cr|none]
//
// The transport is binary-clean (raw mode under the hood); the EOL option only
// controls what gets appended to each outgoing line you type.

#include <cs/SerialPort.hpp>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_alive{true};

void printChunk(const std::uint8_t* data, std::size_t n, bool hex)
{
    if (hex) {
        static const char* kDigits = "0123456789abcdef";
        for (std::size_t i = 0; i < n; ++i) {
            char b[4] = {kDigits[data[i] >> 4], kDigits[data[i] & 0xf], ' ', 0};
            std::cout.write(b, 3);
            if ((i & 15) == 15) std::cout.put('\n');
        }
    } else {
        std::cout.write(reinterpret_cast<const char*>(data),
                        static_cast<std::streamsize>(n));
    }
    std::cout.flush();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <port> [baud=115200] [--hex] [--eol=lf|crlf|cr|none]\n";
        return 1;
    }

    const std::string portName = argv[1];
    int baud = 115200;
    bool hex = false;
    std::string eol = "\n";

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hex") hex = true;
        else if (arg.rfind("--eol=", 0) == 0) {
            const std::string v = arg.substr(6);
            if      (v == "lf")   eol = "\n";
            else if (v == "crlf") eol = "\r\n";
            else if (v == "cr")   eol = "\r";
            else if (v == "none") eol.clear();
            else { std::cerr << "unknown eol: " << v << "\n"; return 1; }
        }
        else baud = std::atoi(arg.c_str());
    }

    cs::SerialPort port;
    port.setPortName(portName);
    port.setBaudRate(baud);

    port.registerOnReadyRead([&port, hex] {
        // Worker thread: read() is safe from callbacks by contract.
        std::uint8_t chunk[8192];
        std::int64_t n;
        while ((n = port.read(chunk, sizeof chunk)) > 0)
            printChunk(chunk, static_cast<std::size_t>(n), hex);
    });
    port.registerOnError([&port](cs::SerialError) {
        std::cerr << "\n[error " << static_cast<int>(port.error()) << "] "
                  << port.errorString() << "\n";
        g_alive = false;
    });

    if (!port.open()) {
        std::cerr << "open failed: " << port.errorString() << "\n";
        return 1;
    }
    std::cerr << "connected to " << portName << " @ " << baud
              << " (type lines; ctrl-d quits)\n";

    std::string line;
    while (g_alive && std::getline(std::cin, line)) {
        line += eol;
        if (port.write(line.data(), line.size()) < 0) break;
    }

    port.close();
    return 0;
}
