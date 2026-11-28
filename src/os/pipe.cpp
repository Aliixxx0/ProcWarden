#include "pw/os/pipe.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace pw {

Pipe::~Pipe() {
    close_read();
    close_write();
}

Pipe::Pipe(Pipe&& other) noexcept : r_(other.r_), w_(other.w_) {
    other.r_ = -1;
    other.w_ = -1;
}

Pipe& Pipe::operator=(Pipe&& other) noexcept {
    if (this != &other) {
        close_read();
        close_write();
        r_ = other.r_;
        w_ = other.w_;
        other.r_ = -1;
        other.w_ = -1;
    }
    return *this;
}

// Lab 5: pipe() with both ends close-on-exec by default.
Result<Pipe> Pipe::create(bool cloexec) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return Result<Pipe>::err(errno, "pipe");

    Pipe p;
    p.r_ = fds[0];
    p.w_ = fds[1];
    if (cloexec) p.set_cloexec(true);
    return p;
}

void Pipe::close_read() {
    if (r_ >= 0) {
        close(r_);
        r_ = -1;
    }
}

void Pipe::close_write() {
    if (w_ >= 0) {
        close(w_);
        w_ = -1;
    }
}

void Pipe::set_cloexec(bool on) {
    const int flags = on ? FD_CLOEXEC : 0;
    if (r_ >= 0) fcntl(r_, F_SETFD, flags);
    if (w_ >= 0) fcntl(w_, F_SETFD, flags);
}

// Lab 5: blocking read, retried on EINTR.
Result<std::size_t> Pipe::read_some(void* buf, std::size_t n) {
    if (r_ < 0) return Result<std::size_t>::err(EBADF, "pipe read: closed");
    for (;;) {
        const ssize_t got = read(r_, buf, n);
        if (got < 0) {
            if (errno == EINTR) continue;
            return Result<std::size_t>::err(errno, "read");
        }
        return static_cast<std::size_t>(got);
    }
}

// Lab 5: blocking write, retried on EINTR and short writes.
Result<std::size_t> Pipe::write_all(const void* buf, std::size_t n) {
    if (w_ < 0) return Result<std::size_t>::err(EBADF, "pipe write: closed");
    const char* p = static_cast<const char*>(buf);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t put = write(w_, p + done, n - done);
        if (put < 0) {
            if (errno == EINTR) continue;
            return Result<std::size_t>::err(errno, "write");
        }
        done += static_cast<std::size_t>(put);
    }
    return done;
}

}  // namespace pw
