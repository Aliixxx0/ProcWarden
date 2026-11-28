#pragma once

#include <pthread.h>

namespace pw {

// Lab 4: RAII around pthread_create/pthread_join. The worker receives its
// context through void* and returns a heap-allocated result owned by the
// joiner. No std::thread anywhere.
class PosixThread {
public:
    using Fn = void* (*)(void*);

    PosixThread() = default;
    ~PosixThread();

    PosixThread(const PosixThread&) = delete;
    PosixThread& operator=(const PosixThread&) = delete;
    PosixThread(PosixThread&& other) noexcept;
    PosixThread& operator=(PosixThread&& other) noexcept;

    bool start(Fn fn, void* arg);
    bool joinable() const { return joinable_; }
    void* join();  // returns the heap result; caller owns it

private:
    pthread_t handle_ {};
    bool joinable_ = false;
};

}  // namespace pw
