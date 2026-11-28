#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "daemon.hpp"
#include "pw/os/proc.hpp"

namespace pw {
namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

}  // namespace

void publish_locked(DaemonState& st, const Snapshot& snap, const MemSample& mem,
                    const Decision& decision) {
    status_begin_write(st.table);
    StatusTable& t = *st.table;

    t.header.magic = kStatusMagic;
    t.header.version = kStatusVersion;
    t.header.sample_no = ++st.sample_no;
    t.header.t_usec = mem.t_usec;

    t.sys.total = mem.total;
    t.sys.used = mem.used;
    t.sys.phys_free = mem.phys_free;
    t.sys.compressed = mem.compressed;
    t.sys.swap_used = mem.swap_used;
    t.sys.swap_total = mem.swap_total;
    t.sys.pressure_level = mem.pressure_level;
    t.sys.processes = static_cast<uint32_t>(snap.procs.size());
    t.sys.band = static_cast<uint8_t>(decision.band);
    t.sys.enforce = st.cfg.enforce ? 1 : 0;

    t.normal.low_gb = st.normal.low_gb;
    t.normal.high_gb = st.normal.high_gb;
    t.normal.mode = st.cfg.normal_mode == NormalMode::Manual ? 1 : 0;

    std::memset(t.sessions, 0, sizeof(t.sessions));
    std::memset(t.agents, 0, sizeof(t.agents));
    std::memset(t.procs, 0, sizeof(t.procs));

    std::map<int32_t, std::pair<int32_t, uint8_t>> managed;
    std::set<int32_t> escaped;
    for (const auto& s : st.sessions) {
        for (const auto& m : s.members) {
            managed[m.pid] = {s.id, static_cast<uint8_t>(m.phase)};
            if (m.escaped) escaped.insert(m.pid);
        }
    }

    int session_index = 0;
    for (const auto& s : st.sessions) {
        if (s.id == 0 || session_index >= kMaxSessions) continue;
        StatusSession& row = t.sessions[session_index++];
        row.id = s.id;
        status_set_str(row.tool, sizeof(row.tool), s.tool);
        status_set_str(row.project, sizeof(row.project), s.project);
        row.phase_mask = s.phase_mask;
        row.state = static_cast<uint8_t>(s.state);
        row.members = static_cast<uint32_t>(s.members.size());

        uint64_t footprint = 0;
        uint64_t rss = 0;
        for (const auto& m : s.members) {
            footprint += m.footprint;
            rss += m.rss;
        }
        row.footprint = footprint;
        row.rss_upper = rss;

        auto usual = st.usual_gb.find(s.id);
        row.usual_gb = usual == st.usual_gb.end() ? 0.0 : usual->second;
        const double gb = static_cast<double>(footprint) / kGiB;
        row.ratio = row.usual_gb > 0.0 ? gb / row.usual_gb : 0.0;
        row.owner_alive =
            (s.owner_pid == 0 ||
             identity_alive(snap, s.owner_pid, s.owner_start_sec, s.owner_start_usec))
                ? 1
                : 0;
    }

    int agent_index = 0;
    for (const auto& s : st.sessions) {
        for (int ph = 1; ph <= 4; ++ph) {
            uint64_t footprint = 0;
            uint64_t idle_bytes = 0;
            int members = 0;
            for (const auto& m : s.members) {
                if (static_cast<int>(m.phase) != ph) continue;
                ++members;
                footprint += m.footprint;
                if (m.idle) idle_bytes += m.footprint;
            }
            if (members == 0 || agent_index >= kMaxAgents) continue;

            StatusAgent& row = t.agents[agent_index++];
            row.sess_id = s.id;
            row.phase = static_cast<uint8_t>(ph);
            row.footprint = footprint;
            row.idle_bytes = idle_bytes;
            row.status = idle_bytes == footprint ? 2 : (idle_bytes > 0 ? 1 : 0);
        }
    }

    std::vector<const ProcInfo*> procs;
    procs.reserve(snap.procs.size());
    for (const auto& p : snap.procs) procs.push_back(&p);
    std::sort(procs.begin(), procs.end(), [](const ProcInfo* a, const ProcInfo* b) {
        return a->footprint > b->footprint;
    });
    if (static_cast<int>(procs.size()) > kMaxProcs) procs.resize(kMaxProcs);

    for (int i = 0; i < static_cast<int>(procs.size()); ++i) {
        const ProcInfo& p = *procs[static_cast<std::size_t>(i)];
        StatusProc& row = t.procs[i];
        row.pid = p.pid;
        row.ppid = p.ppid;
        row.pgid = p.pgid;
        row.footprint = p.footprint;
        row.rss = p.rss;
        row.state = static_cast<uint8_t>(proc_state_from_status(p.status));
        row.escaped = escaped.count(p.pid) != 0 ? 1 : 0;

        auto it = managed.find(p.pid);
        if (it != managed.end()) {
            row.sess_id = it->second.first;
            row.phase = it->second.second;
        }
    }

    const std::size_t start =
        st.mem_ring.size() > static_cast<std::size_t>(kRingPoints)
            ? st.mem_ring.size() - static_cast<std::size_t>(kRingPoints)
            : 0;
    int ring_index = 0;
    for (std::size_t i = start; i < st.mem_ring.size() && ring_index < kRingPoints; ++i) {
        const MemSample& sample = st.mem_ring[i];
        StatusRing& row = t.ring[ring_index++];
        row.t_usec = static_cast<int64_t>(sample.t_usec);
        row.ram = sample.used;
        row.swap = sample.swap_used;
    }

    // Preserve per-item cleanup statuses across publishes; only refill when a
    // new plan becomes visible.
    if (st.cleanup_visible_hash != 0 && t.cleanup.plan_hash != st.cleanup_visible_hash)
        fill_status_cleanup(t.cleanup, st.pending.plan);

    status_end_write(st.table);
}

}  // namespace pw
