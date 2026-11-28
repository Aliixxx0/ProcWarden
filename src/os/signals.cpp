#include "pw/os/signals.hpp"

namespace pw {

SignalFlag* volatile SignalFlag::slots_[64] = {};

SignalFlag::~SignalFlag() {
    if (sig_ > 0 && sig_ < 64 && slots_[sig_] == this) {
        signal(sig_, SIG_DFL);
        slots_[sig_] = nullptr;
    }
}

// Lab 5: signal() as taught. The handler only sets the flag.
bool SignalFlag::install(int sig) {
    if (sig <= 0 || sig >= 64) return false;
    slots_[sig] = this;
    sig_ = sig;
    signal(sig, &SignalFlag::handler);
    return true;
}

void SignalFlag::ignore(int sig) { signal(sig, SIG_IGN); }

void SignalFlag::handler(int sig) {
    if (sig > 0 && sig < 64 && slots_[sig] != nullptr) slots_[sig]->flag_ = 1;
}

}  // namespace pw
