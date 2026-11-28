#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pw/core/snapshot.hpp"
#include "pw/result.hpp"

namespace pw {

struct ProcRef {
    int32_t pid = 0;
    uint64_t start_sec = 0;
    uint64_t start_usec = 0;
};

struct RegistryEntry {
    int32_t sess_id = 0;
    int32_t pgid = 0;
    int32_t root_pid = 0;
    int32_t owner_pid = 0;
    uint64_t root_start_sec = 0;
    uint64_t root_start_usec = 0;
    uint64_t owner_start_sec = 0;
    uint64_t owner_start_usec = 0;
    uint64_t last_seen_usec = 0;
    uint64_t budget_bytes = 0;
    std::string tool;
    std::string project;
    std::string project_path;
    std::string state;
    std::string on_detach;
};

struct Registry {
    std::vector<RegistryEntry> entries;

    static Result<Registry> load(const std::string& path);
    std::optional<Error> save(const std::string& path) const;
};

// Identity check: pid and start time both match.
bool identity_alive(const Snapshot& snap, int32_t pid, uint64_t start_sec,
                    uint64_t start_usec);

}  // namespace pw
