#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pw/config.hpp"
#include "pw/core/registry.hpp"
#include "pw/core/session.hpp"
#include "pw/ipc/status_table.hpp"

namespace pw {

struct CacheInfo {
    std::string label;    // "npm", "pnpm", "bun", "biome"
    std::string path;
    uint64_t bytes = 0;
    std::string command;  // tool's own clear command, if any
};

struct PortInfo {
    uint16_t port = 0;
    int32_t pid = 0;
    int32_t sess_id = 0;
    uint64_t bytes = 0;
    std::string project;
};

struct CleanupItem {
    ItemKind kind = ItemKind::Ram;
    std::string label;
    std::string detail;
    uint64_t bytes = 0;
    int32_t sess_id = 0;
    std::vector<ProcRef> pids;
    std::string command;  // disk items: run this (or empty -> safe rm -rf)
};

struct CleanupInput {
    std::vector<Session> sessions;
    std::vector<PortInfo> ports;
    std::vector<CacheInfo> caches;
    std::vector<std::string> include;
    bool include_ram = true;
    bool include_disk = true;
};

struct CleanupPlan {
    uint64_t hash = 0;
    std::vector<CleanupItem> items;
    uint64_t ram_bytes = 0;
    uint64_t disk_bytes = 0;
};

// Pure planner: which processes/caches would be cleaned, how much each frees.
// Only Done/OwnerLost sessions and idle lanes of active sessions are RAM
// candidates; active work is never included.
CleanupPlan plan_cleanup(const CleanupInput& in);

// SHA-256 over a canonical serialization; first 8 bytes as a uint64.
uint64_t plan_hash(const std::vector<CleanupItem>& items);

std::string plan_summary(const CleanupPlan& p);

// Copies the plan into the shared-memory cleanup block (labels truncated).
void fill_status_cleanup(StatusCleanup& out, const CleanupPlan& plan);

}  // namespace pw
