#include "pw/core/snapshot.hpp"

#include <cstring>

namespace pw {

ProcState proc_state_from_status(uint32_t status) {
    switch (status) {
        case 1: return ProcState::Other;     // SIDL
        case 2: return ProcState::Running;   // SRUN
        case 3: return ProcState::Sleeping;  // SSLEEP
        case 4: return ProcState::Stopped;   // SSTOP
        case 5: return ProcState::Zombie;    // SZOMB
        default: return ProcState::Other;
    }
}

bool same_identity(const ProcInfo& p, int32_t pid, uint64_t start_sec,
                   uint64_t start_usec) {
    return p.pid == pid && p.start_sec == start_sec && p.start_usec == start_usec;
}

const ProcInfo* find_proc(const Snapshot& s, int32_t pid) {
    for (const auto& p : s.procs)
        if (p.pid == pid) return &p;
    return nullptr;
}

}  // namespace pw
