#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pw {

enum class ProcState : uint8_t { Running, Sleeping, Stopped, Zombie, Other };

// One process row from the sampler. Identity is (pid, start time): the daemon
// re-reads both before every signal.
struct ProcInfo {
    int32_t pid = 0;
    int32_t ppid = 0;
    int32_t pgid = 0;
    uint64_t start_sec = 0;
    uint64_t start_usec = 0;
    uint32_t status = 0;
    uint32_t tty = 0;
    char comm[32] = {};
    uint64_t footprint = 0;   // ri_phys_footprint (includes compressed pages)
    uint64_t rss = 0;         // pti_resident_size (upper-bound view)
    uint64_t cpu_time_ns = 0; // ri_user_time + ri_system_time
};

struct Snapshot {
    uint64_t t_usec = 0;
    std::vector<ProcInfo> procs;
};

ProcState proc_state_from_status(uint32_t status);
bool same_identity(const ProcInfo& p, int32_t pid, uint64_t start_sec,
                   uint64_t start_usec);
const ProcInfo* find_proc(const Snapshot& s, int32_t pid);

}  // namespace pw
