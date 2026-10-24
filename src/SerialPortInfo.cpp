#include <cs/SerialPortInfo.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "detail/PortData.hpp"

// NB: __ANDROID__ must be checked first - bionic defines __linux__ too, and
// with it checked later the POSIX /dev/tty* branch would win on Android,
// where USB serial adapters never create tty nodes and the JNI bridge at the
// bottom of this file would never be reached (and would be gc-sections'd away
// by the linker, since nothing references androidEnumeratePorts).
#if defined(__ANDROID__)
#  include <cerrno>
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#  define CS_ENUM_POSIX 1
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <fstream>
#elif defined(_WIN32) || defined(_WIN64)
#  define CS_ENUM_WIN 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <setupapi.h>
#  include <initguid.h>
#  include <devpkey.h>
#elif defined(__ANDROID__)
#  include <cerrno>
#endif

namespace cs {

void SerialPortInfo::swap(SerialPortInfo& other) noexcept
{
    std::swap(portName_, other.portName_);
    std::swap(systemLocation_, other.systemLocation_);
    std::swap(description_, other.description_);
    std::swap(manufacturer_, other.manufacturer_);
    std::swap(serialNumber_, other.serialNumber_);
    std::swap(vid_, other.vid_);
    std::swap(pid_, other.pid_);
    std::swap(null_, other.null_);
}

std::vector<std::int32_t> SerialPortInfo::standardBaudRates()
{
    // Same list QSerialPortInfo::standardBaudRates() reports on desktop Linux.
    return {50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
            9600, 14400, 19200, 38400, 57600, 76800, 115200, 230400,
            460800, 500000, 576000, 921600, 1000000, 1152000, 1500000,
            2000000, 2500000, 3000000, 3500000, 4000000};
}

// ---- isBusy ---------------------------------------------------------------

bool SerialPortInfo::isBusy() const
{
    if (null_ || systemLocation_.empty()) return false;
#if defined(CS_ENUM_POSIX)
    if (systemLocation_.rfind("/dev/", 0) != 0) return false;  // Android-style name
    int fd = ::open(systemLocation_.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY);
    if (fd >= 0) { ::close(fd); return false; }
    return errno == EBUSY;
#elif defined(CS_ENUM_WIN)
    HANDLE h = ::CreateFileA(systemLocation_.c_str(),
                             GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); return false; }
    return true;  // best effort: cannot open == busy by definition here
#else
    return false;  // Android USB serial devices carry no host-side busy state
#endif
}

// ---- enumeration ----------------------------------------------------------

std::vector<SerialPortInfo> SerialPortInfo::availablePorts()
{
    std::vector<detail::RawPortInfo> raw = detail::enumeratePorts();

    std::vector<SerialPortInfo> out;
    out.reserve(raw.size());
    for (auto& r : raw)
        out.push_back(SerialPortInfo(std::move(r.portName), std::move(r.systemLocation),
                                     std::move(r.description), std::move(r.manufacturer),
                                     std::move(r.serialNumber), r.vendorId, r.productId));
    return out;
}

} // namespace cs

// ---- platform enumeration backends ----------------------------------------

#if defined(CS_ENUM_POSIX)

#include <sys/stat.h>

namespace cs {
namespace detail {

static bool readSysFile(const std::string& path, std::string& out)
{
    std::ifstream f(path.c_str());
    if (!f.is_open()) return false;
    std::getline(f, out);
    return true;
}

// USB attrs live on ancestor sysfs dirs; walk up a few levels looking for one.
static void findAttrUpwards(std::string dir, const char* attr, std::string& out)
{
    for (int i = 0; i < 10 && !dir.empty(); ++i) {
        if (readSysFile(dir + "/" + attr, out) && !out.empty()) return;
        std::size_t pos = dir.find_last_of('/');
        if (pos == std::string::npos) break;
        dir.resize(pos);
    }
    out.clear();
}

static int hexToInt(const std::string& s)
{
    if (s.empty()) return 0;
    try { return static_cast<int>(std::stoul(s, nullptr, 16)); }
    catch (...) { return 0; }
}

static bool looksSerial(const std::string& name)
{
    static const char* prefixes[] = {
        "ttyUSB", "ttyACM", "ttyS", "ttyAMA", "ttyXRUSB", "ttyMAX", "ttyHS",
        "ttyMSL", "ttyO", "ttyAT", "rfcomm", "ttyGS", "ttyMI", "ttySPI"
    };
    for (const char* p : prefixes)
        if (name.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

std::vector<RawPortInfo> enumeratePorts()
{
    std::vector<RawPortInfo> out;
    DIR* dir = ::opendir("/sys/class/tty");
    if (!dir) return out;

    while (dirent* ent = ::readdir(dir)) {
        std::string name = ent->d_name;
        if (name == "." || name == ".." || !looksSerial(name)) continue;

        // Follow the device symlink; entries without a real device (pure tty
        // helpers like ttyprintk) have none and are skipped.
        std::string link = "/sys/class/tty/" + name + "/device";
        char* rp = ::realpath(link.c_str(), nullptr);
        if (!rp) continue;
        std::string deviceDir = rp;
        std::free(rp);

        RawPortInfo info;
        info.portName       = name;                 // Qt style: short name
        info.systemLocation = "/dev/" + name;       // full path

        std::string vid, pid, manuf, product, serial, iface;
        findAttrUpwards(deviceDir, "idVendor", vid);
        findAttrUpwards(deviceDir, "idProduct", pid);
        findAttrUpwards(deviceDir, "manufacturer", manuf);
        findAttrUpwards(deviceDir, "product", product);
        findAttrUpwards(deviceDir, "serial", serial);
        findAttrUpwards(deviceDir, "interface", iface);

        info.description  = product.empty() ? iface : product;
        info.manufacturer = manuf;
        info.serialNumber = serial;
        if (!vid.empty() && !pid.empty()) {
            info.vendorId  = static_cast<std::uint16_t>(hexToInt(vid));
            info.productId = static_cast<std::uint16_t>(hexToInt(pid));
        }
        out.push_back(std::move(info));
    }
    ::closedir(dir);

    std::sort(out.begin(), out.end(),
              [](const RawPortInfo& a, const RawPortInfo& b) { return a.portName < b.portName; });
    return out;
}

} // namespace detail
} // namespace cs

#elif defined(CS_ENUM_WIN)

namespace cs {
namespace detail {

#ifndef GUID_DEVINTERFACE_COMPORT
DEFINE_GUID(GUID_DEVINTERFACE_COMPORT,
            0x86E0D1E0L, 0x8089, 0x11D0, 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73);
#endif

static std::string winNarrow(const wchar_t* w)
{
    if (!w || !*w) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

// "USB Serial Port (COM3)" -> "COM3"; empty when no COM<digit> token present.
static std::string extractComName(const wchar_t* w)
{
    if (!w) return {};
    for (std::size_t i = 0; w[i]; ++i) {
        if (w[i] == L'C' && w[i + 1] == L'O' && w[i + 2] == L'M' &&
            w[i + 3] >= L'0' && w[i + 3] <= L'9') {
            std::size_t e = i + 3;
            while (w[e] >= L'0' && w[e] <= L'9') ++e;
            std::wstring token(w + i, w + e);
            return winNarrow(token.c_str());
        }
    }
    return {};
}

static bool readRegStr(HDEVINFO hdev, PSP_DEVINFO_DATA did, DWORD prop,
                       wchar_t* buf, DWORD bufCch)
{
    buf[0] = L'\0';
    return ::SetupDiGetDeviceRegistryPropertyW(
               hdev, did, prop, nullptr,
               reinterpret_cast<PBYTE>(buf), bufCch * sizeof(wchar_t), nullptr) != 0;
}

static std::uint16_t extractHexId(const std::string& upper, const char* key)
{
    auto p = upper.find(key);
    if (p == std::string::npos) return 0;
    unsigned v = 0;
    if (::sscanf(upper.c_str() + p + std::strlen(key), "%4x", &v) != 1) return 0;
    return static_cast<std::uint16_t>(v);
}

std::vector<RawPortInfo> enumeratePorts()
{
    std::vector<RawPortInfo> out;
    HDEVINFO hdev = ::SetupDiGetClassDevs(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) return out;

    SP_DEVINFO_DATA devInfo{};
    devInfo.cbSize = sizeof(devInfo);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(hdev, i, &devInfo); ++i) {
        wchar_t friendly[512] = {0};
        wchar_t devDesc[512]  = {0};
        wchar_t mfg[512]      = {0};
        wchar_t hwid[1024]    = {0};

        readRegStr(hdev, &devInfo, SPDRP_FRIENDLYNAME, friendly, 512);
        readRegStr(hdev, &devInfo, SPDRP_DEVICEDESC, devDesc, 512);

        std::string comName = extractComName(friendly[0] ? friendly : devDesc);
        if (comName.empty()) continue;  // device exposes no openable COMx name

        RawPortInfo info;
        info.portName       = comName;                        // "COM3"
        info.systemLocation = "\\\\.\\" + comName;            // "\\.\COM3"
        info.description    = winNarrow(devDesc[0] ? devDesc : friendly);

        if (readRegStr(hdev, &devInfo, SPDRP_MFG, mfg, 512))
            info.manufacturer = winNarrow(mfg);

        if (readRegStr(hdev, &devInfo, SPDRP_HARDWAREID, hwid, 1024)) {
            std::string s = winNarrow(hwid);
            std::string upper;
            upper.reserve(s.size());
            for (char c : s)
                upper.push_back(static_cast<char>(::toupper(static_cast<unsigned char>(c))));
            info.vendorId  = extractHexId(upper, "VID_");
            info.productId = extractHexId(upper, "PID_");
        }
        out.push_back(std::move(info));
    }
    ::SetupDiDestroyDeviceInfoList(hdev);
    return out;
}

} // namespace detail
} // namespace cs

#elif defined(__ANDROID__)

namespace cs {
namespace detail {
// Implemented in SerialPort.cpp against the JNI bridge (usb-serial-for-android).
std::vector<RawPortInfo> androidEnumeratePorts();
std::vector<RawPortInfo> enumeratePorts() { return androidEnumeratePorts(); }
} // namespace detail
} // namespace cs

#else

namespace cs {
namespace detail {
std::vector<RawPortInfo> enumeratePorts() { return {}; }
} // namespace detail
} // namespace cs

#endif
