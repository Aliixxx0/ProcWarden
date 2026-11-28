#include "pw/ipc/status_table.hpp"

#include <cstring>

namespace pw {

void status_set_str(char* dst, std::size_t n, const std::string& s) {
    if (n == 0) return;
    std::memset(dst, 0, n);
    const std::size_t len = s.size() < n - 1 ? s.size() : n - 1;
    std::memcpy(dst, s.data(), len);
    dst[len] = '\0';
}

const char* band_name(Band b) {
    switch (b) {
        case Band::Green: return "GREEN";
        case Band::Yellow: return "YELLOW";
        case Band::Orange: return "ORANGE";
        case Band::Red: return "RED";
    }
    return "GREEN";
}

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Plan: return "Plan";
        case Phase::Build: return "Build";
        case Phase::Test: return "Test";
        case Phase::Review: return "Review";
        case Phase::None: break;
    }
    return "None";
}

const char* session_state_name(SessionState s) {
    switch (s) {
        case SessionState::Active: return "active";
        case SessionState::Detached: return "detached";
        case SessionState::OwnerLost: return "owner_lost";
        case SessionState::Done: return "done";
    }
    return "active";
}

const char* pressure_name(uint32_t level) {
    if (level >= 4) return "critical";
    if (level >= 2) return "warning";
    return "normal";
}

}  // namespace pw
