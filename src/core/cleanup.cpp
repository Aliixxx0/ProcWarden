#include "pw/core/cleanup.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "pw/core/sha256.hpp"

namespace pw {
namespace {

bool enabled(const std::vector<std::string>& include, const char* key) {
    return std::find(include.begin(), include.end(), key) != include.end();
}

bool cache_enabled(const std::vector<std::string>& include, const std::string& label) {
    for (const auto& key : include) {
        if (key == "sims" || key == "idle_agents" || key == "leftover_servers") continue;
        if (label == key) return true;
    }
    return false;
}

std::string phase_label(Phase p) {
    switch (p) {
        case Phase::Plan: return "Plan";
        case Phase::Build: return "Build";
        case Phase::Test: return "Test";
        case Phase::Review: return "Review";
        case Phase::None: break;
    }
    return "None";
}

std::string gb_string(uint64_t bytes) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f GB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

}  // namespace

CleanupPlan plan_cleanup(const CleanupInput& in) {
    CleanupPlan plan;
    std::unordered_set<int32_t> managed_pids;
    for (const auto& s : in.sessions)
        for (const auto& m : s.members) managed_pids.insert(m.pid);

    if (in.include_ram) {
        for (const auto& s : in.sessions) {
            if (s.state == SessionState::Done || s.state == SessionState::OwnerLost) {
                CleanupItem item;
                item.kind = ItemKind::Ram;
                item.label = "session " + s.project + " (" + session_state_name(s.state) + ")";
                item.detail = "terminate " + std::to_string(s.members.size()) + " processes";
                item.sess_id = s.id;
                for (const auto& m : s.members) {
                    item.pids.push_back(ProcRef{m.pid, m.start_sec, m.start_usec});
                    item.bytes += m.footprint;
                }
                if (!item.pids.empty()) plan.items.push_back(std::move(item));
                continue;
            }

            // Active/Detached: only idle lanes (done phase, no CPU for 30 s).
            for (Phase phase : {Phase::Plan, Phase::Build, Phase::Test, Phase::Review}) {
                CleanupItem item;
                item.kind = ItemKind::Ram;
                item.sess_id = s.id;
                for (const auto& m : s.members) {
                    if (m.phase != phase || !m.idle) continue;
                    item.pids.push_back(ProcRef{m.pid, m.start_sec, m.start_usec});
                    item.bytes += m.footprint;
                }
                if (item.pids.empty()) continue;
                item.label = "idle " + phase_label(phase) + " lane in " + s.project;
                item.detail = "terminate " + std::to_string(item.pids.size()) + " processes";
                plan.items.push_back(std::move(item));
            }
        }

        // Leftover dev servers: listening ports owned by nobody we manage.
        if (enabled(in.include, "leftover_servers")) {
            for (const auto& port : in.ports) {
                if (port.pid <= 1) continue;
                if (port.sess_id != 0) continue;
                if (managed_pids.count(port.pid) != 0) continue;
                CleanupItem item;
                item.kind = ItemKind::Ram;
                item.label = "leftover server :" + std::to_string(port.port);
                item.detail = port.project.empty() ? "listening process" : port.project;
                item.bytes = port.bytes;
                item.pids.push_back(ProcRef{port.pid, 0, 0});
                plan.items.push_back(std::move(item));
            }
        }
    }

    if (in.include_disk) {
        for (const auto& cache : in.caches) {
            if (cache.bytes == 0) continue;
            if (!cache_enabled(in.include, cache.label)) continue;
            CleanupItem item;
            item.kind = ItemKind::Disk;
            item.label = cache.label + " cache";
            item.detail = cache.path;
            item.bytes = cache.bytes;
            item.command = cache.command;
            plan.items.push_back(std::move(item));
        }
    }

    std::sort(plan.items.begin(), plan.items.end(), [](const CleanupItem& a, const CleanupItem& b) {
        if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        if (a.label != b.label) return a.label < b.label;
        return a.detail < b.detail;
    });

    for (const auto& item : plan.items) {
        if (item.kind == ItemKind::Ram)
            plan.ram_bytes += item.bytes;
        else
            plan.disk_bytes += item.bytes;
    }
    plan.hash = plan_hash(plan.items);
    return plan;
}

uint64_t plan_hash(const std::vector<CleanupItem>& items) {
    std::string canon;
    for (const auto& item : items) {
        canon += std::to_string(static_cast<int>(item.kind));
        canon += '|';
        canon += item.label;
        canon += '|';
        canon += item.detail;
        canon += '|';
        canon += std::to_string(item.bytes);
        canon += '|';
        canon += std::to_string(item.sess_id);
        canon += '|';
        for (const auto& ref : item.pids) {
            canon += std::to_string(ref.pid);
            canon += ':';
            canon += std::to_string(ref.start_sec);
            canon += ':';
            canon += std::to_string(ref.start_usec);
            canon += ',';
        }
        canon += '|';
        canon += item.command;
        canon += '\n';
    }

    const auto digest = sha256(canon.data(), canon.size());
    uint64_t h = 0;
    for (int i = 0; i < 8; ++i) h = (h << 8) | digest[static_cast<std::size_t>(i)];
    return h;
}

std::string plan_summary(const CleanupPlan& p) {
    return "RAM " + gb_string(p.ram_bytes) + ", disk " + gb_string(p.disk_bytes) + ", " +
           std::to_string(p.items.size()) + " items, hash " + [&] {
               char buf[24];
               std::snprintf(buf, sizeof(buf), "%016llx",
                             static_cast<unsigned long long>(p.hash));
               return std::string(buf);
           }();
}

void fill_status_cleanup(StatusCleanup& out, const CleanupPlan& plan) {
    out = StatusCleanup{};
    out.plan_hash = plan.hash;
    out.state = static_cast<uint8_t>(CleanupState::Ready);
    int n = 0;
    for (const auto& item : plan.items) {
        if (n >= kMaxCleanupItems) break;
        StatusCleanupItem& dst = out.items[n];
        dst.kind = static_cast<uint8_t>(item.kind);
        dst.status = static_cast<uint8_t>(ItemStatus::Ready);
        dst.bytes = item.bytes;
        status_set_str(dst.label, sizeof(dst.label), item.label);
        status_set_str(dst.detail, sizeof(dst.detail), item.detail);
        ++n;
    }
    out.count = static_cast<uint8_t>(n);
}

}  // namespace pw
