#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace pw {

// Lab 6 shared-memory status table: one writer (daemon), many readers.
// < 1 MB, well under the default kern.sysv.shmmax of 4 MB.
inline constexpr uint32_t kStatusMagic = 0x50575442;  // "PWTB"
inline constexpr uint32_t kStatusVersion = 1;
inline constexpr int kMaxSessions = 64;
inline constexpr int kMaxAgents = 16;
inline constexpr int kMaxProcs = 512;
inline constexpr int kRingPoints = 120;
inline constexpr int kMaxCleanupItems = 16;

enum class Band : uint8_t { Green = 0, Yellow = 1, Orange = 2, Red = 3 };
enum class Phase : uint8_t { None = 0, Plan = 1, Build = 2, Test = 3, Review = 4 };
enum class SessionState : uint8_t { Active = 0, Detached = 1, OwnerLost = 2, Done = 3 };
enum class CleanupState : uint8_t { None = 0, Ready = 1, Clearing = 2, Freed = 3, Failed = 4 };
enum class ItemKind : uint8_t { Ram = 0, Disk = 1 };
enum class ItemStatus : uint8_t { Ready = 0, Clearing = 1, Freed = 2, Failed = 3 };

struct StatusHeader {
    uint32_t magic = kStatusMagic;
    uint32_t version = kStatusVersion;
    uint32_t seq = 0;        // seqlock: odd = write in progress, even = stable
    uint32_t sample_no = 0;
    uint64_t t_usec = 0;
};

struct StatusSys {
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t phys_free = 0;
    uint64_t compressed = 0;
    uint64_t swap_used = 0;
    uint64_t swap_total = 0;
    uint32_t pressure_level = 1;  // 1 normal, 2 warning, 4 critical
    uint32_t processes = 0;
    uint8_t band = 0;
    uint8_t enforce = 0;
    uint16_t pad = 0;
};

struct StatusNormal {
    double low_gb = 5.0;
    double high_gb = 9.0;
    uint8_t mode = 0;  // 0 auto, 1 manual
    uint8_t pad[7] = {};
};

struct StatusSession {
    int32_t id = 0;
    char tool[24] = {};
    char project[48] = {};
    uint32_t phase_mask = 0;
    uint8_t state = 0;
    uint8_t pad[3] = {};
    uint32_t members = 0;
    uint64_t footprint = 0;
    uint64_t rss_upper = 0;
    double usual_gb = 0.0;
    double ratio = 0.0;
    uint8_t owner_alive = 1;
    uint8_t pad2[7] = {};
};

struct StatusAgent {
    int32_t sess_id = 0;
    uint8_t phase = 0;
    uint8_t status = 0;  // 0 active, 1 idle, 2 done
    uint8_t pad[2] = {};
    uint64_t footprint = 0;
    uint64_t idle_bytes = 0;
};

struct StatusProc {
    int32_t pid = 0;
    int32_t ppid = 0;
    int32_t pgid = 0;
    int32_t sess_id = 0;  // 0 = unmanaged
    uint8_t phase = 0;
    uint8_t state = 0;
    uint8_t escaped = 0;
    uint8_t pad = 0;
    uint64_t footprint = 0;
    uint64_t rss = 0;
};

struct StatusRing {
    int64_t t_usec = 0;
    uint64_t ram = 0;
    uint64_t swap = 0;
};

struct StatusCleanupItem {
    uint8_t kind = 0;
    uint8_t status = 0;
    uint16_t pad = 0;
    char label[40] = {};
    char detail[96] = {};
    uint64_t bytes = 0;
};

struct StatusCleanup {
    uint64_t plan_hash = 0;
    uint8_t state = 0;  // CleanupState
    uint8_t count = 0;
    uint16_t pad = 0;
    StatusCleanupItem items[kMaxCleanupItems] = {};
};

struct StatusTable {
    StatusHeader header;
    StatusSys sys;
    StatusNormal normal;
    StatusSession sessions[kMaxSessions];
    StatusAgent agents[kMaxAgents];
    StatusProc procs[kMaxProcs];
    StatusRing ring[kRingPoints];
    StatusCleanup cleanup;
};

static_assert(std::is_trivially_copyable_v<StatusTable>, "StatusTable must be memcpy-able");
static_assert(sizeof(StatusTable) < 1024 * 1024, "status table must stay under 1 MB");

// Seqlock accessors. Writer: begin (odd) -> mutate -> end (even).
inline void status_begin_write(StatusTable* t) {
    std::atomic_ref<uint32_t> seq(t->header.seq);
    seq.fetch_add(1, std::memory_order_acq_rel);
}
inline void status_end_write(StatusTable* t) {
    std::atomic_ref<uint32_t> seq(t->header.seq);
    seq.fetch_add(1, std::memory_order_acq_rel);
}

// Consistent copy for readers: retry while seq is odd or changed.
inline bool status_copy(const StatusTable* t, StatusTable* out) {
    std::atomic_ref<uint32_t> seq(const_cast<uint32_t&>(t->header.seq));
    for (int attempt = 0; attempt < 200; ++attempt) {
        uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::memcpy(out, t, sizeof(StatusTable));
        uint32_t s2 = seq.load(std::memory_order_acquire);
        if (s1 == s2) return true;
    }
    return false;
}

void status_set_str(char* dst, std::size_t n, const std::string& s);
const char* band_name(Band b);
const char* phase_name(Phase p);
const char* session_state_name(SessionState s);
const char* pressure_name(uint32_t level);

}  // namespace pw
