#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pw/core/snapshot.hpp"
#include "pw/ipc/status_table.hpp"

namespace pw {

// One tracked process. Identity = (pid, start time).
struct Member {
    int32_t pid = 0;
    int32_t ppid = 0;
    int32_t pgid = 0;
    uint64_t start_sec = 0;
    uint64_t start_usec = 0;
    Phase phase = Phase::None;
    bool escaped = false;       // pgid differs from the session pgid
    bool idle = false;
    uint64_t idle_since_usec = 0;
    uint64_t footprint = 0;
    uint64_t rss = 0;
    uint64_t cpu_time_ns = 0;
};

enum class DetachPolicy { Stop, Keep, StopAfter10m };

struct Session {
    int32_t id = 0;
    std::string tool = "opencode";
    std::string project;
    std::string project_path;
    int32_t pgid = 0;
    int32_t root_pid = 0;
    uint64_t root_start_sec = 0;
    uint64_t root_start_usec = 0;
    int32_t owner_pid = 0;  // launcher; 0 when adopted/discovered
    uint64_t owner_start_sec = 0;
    uint64_t owner_start_usec = 0;
    uint64_t budget_bytes = 0;
    DetachPolicy on_detach = DetachPolicy::Keep;
    SessionState state = SessionState::Active;
    bool pausable = false;
    uint32_t done_phase_mask = 0;
    std::vector<Member> members;    // full closure (includes escapees)
    std::vector<Member> escapees;   // members whose pgid differs
    uint64_t last_seen_usec = 0;
};

}  // namespace pw
