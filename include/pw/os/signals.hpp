#pragma once

#include <csignal>

namespace pw {

// Lab 5: signal() + handlers. The handler only sets a volatile sig_atomic_t;
// the main loop reads it. One flag per signal (a handler cannot capture).
class SignalFlag {
public:
    SignalFlag() = default;
    ~SignalFlag();
    SignalFlag(const SignalFlag&) = delete;
    SignalFlag& operator=(const SignalFlag&) = delete;

    bool install(int sig);
    bool raised() const { return flag_ != 0; }
    void clear() { flag_ = 0; }

    static void ignore(int sig);

private:
    static void handler(int sig);
    static SignalFlag* volatile slots_[64];

    volatile sig_atomic_t flag_ = 0;
    int sig_ = 0;
};

}  // namespace pw
