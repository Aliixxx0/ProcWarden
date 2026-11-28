#pragma once

#include <cstddef>

#include "pw/result.hpp"

namespace pw {

// Lab 5: pipe() + fork(). The wrapper closes the ends the owner does not use
// and can set FD_CLOEXEC on both descriptors.
class Pipe {
public:
    Pipe() = default;
    ~Pipe();
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&& other) noexcept;
    Pipe& operator=(Pipe&& other) noexcept;

    static Result<Pipe> create(bool cloexec = true);

    int read_fd() const { return r_; }
    int write_fd() const { return w_; }

    void close_read();
    void close_write();
    void set_cloexec(bool on);

    // 0 bytes = EOF. EINTR is retried internally.
    Result<std::size_t> read_some(void* buf, std::size_t n);
    Result<std::size_t> write_all(const void* buf, std::size_t n);

private:
    int r_ = -1;
    int w_ = -1;
};

}  // namespace pw
