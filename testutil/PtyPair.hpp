// Minimal POSIX pty pair helper for tests and benchmarks: data written to the
// master fd surfaces on the slave as if a device had sent it.
#ifndef CROSSSERIAL_PTYPair_HPP
#define CROSSSERIAL_PTYPair_HPP

#if defined(__unix__) || defined(__APPLE__)

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

class PtyPair {
public:
    PtyPair() {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) throw std::runtime_error("posix_openpt failed");
        if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            ::close(master_);
            throw std::runtime_error("grantpt/unlockpt failed");
        }
        char* name = ::ptsname(master_);
        if (!name) {
            ::close(master_);
            throw std::runtime_error("ptsname failed");
        }
        slavePath_ = name;
    }

    ~PtyPair() {
        if (master_ >= 0) ::close(master_);
    }

    PtyPair(const PtyPair&)            = delete;
    PtyPair& operator=(const PtyPair&) = delete;

    int master() const { return master_; }
    const std::string& slavePath() const { return slavePath_; }

    static void writeAll(int fd, const void* data, std::size_t size) {
        auto* p = static_cast<const char*>(data);
        while (size) {
            ssize_t n = ::write(fd, p, size);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("pty write failed");
            }
            p += n;
            size -= static_cast<std::size_t>(n);
        }
    }

    static bool pollReadable(int fd, int ms) {
        pollfd p{fd, POLLIN, 0};
        int r = ::poll(&p, 1, ms);
        return r > 0 && (p.revents & POLLIN);
    }

private:
    int         master_ = -1;
    std::string slavePath_;
};

#endif // POSIX

#endif // CROSSSERIAL_PTYPair_HPP
