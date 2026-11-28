#pragma once

#include <pthread.h>

namespace pw {

// std::mutex is not allowed by the brief; these wrap pthread primitives.
class Mutex {
public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock();
    void unlock();
    pthread_mutex_t* native() { return &mu_; }

private:
    pthread_mutex_t mu_;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_;
};

class CondVar {
public:
    CondVar();
    ~CondVar();
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    void wait(Mutex& m);
    void signal();
    void broadcast();

private:
    pthread_cond_t cv_;
};

}  // namespace pw
