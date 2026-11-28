#pragma once

#include <cstdint>

#include "pw/result.hpp"

namespace pw {

struct MemSample {
    uint64_t t_usec = 0;
    uint64_t total = 0;
    uint64_t used = 0;       // (active + wired + compressor) * page_size
    uint64_t phys_free = 0;
    uint64_t active = 0;
    uint64_t wired = 0;
    uint64_t compressed = 0;
    uint64_t swap_used = 0;
    uint64_t swap_total = 0;
    uint32_t pressure_level = 1;  // 1 normal, 2 warning, 4 critical
    uint64_t page_size = 0;
    uint64_t swapouts = 0;        // cumulative page swap-outs
    uint64_t disk_free = 0;
    uint64_t disk_total = 0;
    uint32_t processes = 0;       // filled by the monitor from the snapshot
};

// "Used" is an estimate: (active + wired + compressor) * page_size, which
// roughly matches Activity Monitor's "Memory Used".
Result<MemSample> mem_sample();

}  // namespace pw
