#include "pw/os/thread.hpp"

#include <utility>

namespace pw {

PosixThread::~PosixThread() {
    if (joinable_) join();
}

PosixThread::PosixThread(PosixThread&& other) noexcept
    : handle_(other.handle_), joinable_(other.joinable_) {
    other.joinable_ = false;
}

PosixThread& PosixThread::operator=(PosixThread&& other) noexcept {
    if (this != &other) {
        if (joinable_) join();
        handle_ = other.handle_;
        joinable_ = other.joinable_;
        other.joinable_ = false;
    }
    return *this;
}

// Lab 4: pthread_create, context struct through void*.
bool PosixThread::start(Fn fn, void* arg) {
    if (joinable_) return false;
    const int rc = pthread_create(&handle_, nullptr, fn, arg);
    if (rc != 0) return false;
    joinable_ = true;
    return true;
}

// Lab 4: pthread_join returns the worker's heap result to the joiner.
void* PosixThread::join() {
    if (!joinable_) return nullptr;
    void* out = nullptr;
    pthread_join(handle_, &out);
    joinable_ = false;
    return out;
}

}  // namespace pw
