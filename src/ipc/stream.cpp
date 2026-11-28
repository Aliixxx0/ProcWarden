#include "pw/ipc/stream.hpp"

#include <nlohmann/json.hpp>

namespace pw {
namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr double kMiB = 1024.0 * 1024.0;

const char* item_status_name(ItemStatus s) {
    switch (s) {
        case ItemStatus::Ready: return "ready";
        case ItemStatus::Clearing: return "clearing";
        case ItemStatus::Freed: return "freed";
        case ItemStatus::Failed: return "failed";
    }
    return "ready";
}

const char* agent_status_name(uint8_t s) {
    switch (s) {
        case 1: return "idle";
        case 2: return "done";
        default: return "active";
    }
}

}  // namespace

std::string status_to_json(const StatusTable& t) {
    using nlohmann::json;

    json j;
    j["t"] = static_cast<double>(t.header.t_usec) / 1e6;

    json ram;
    ram["used_gb"] = static_cast<double>(t.sys.used) / kGiB;
    ram["total_gb"] = static_cast<double>(t.sys.total) / kGiB;
    j["ram"] = std::move(ram);

    json swap;
    swap["used_mb"] = static_cast<double>(t.sys.swap_used) / kMiB;
    j["swap"] = std::move(swap);

    j["processes"] = t.sys.processes;
    j["pressure"] = pressure_name(t.sys.pressure_level);
    j["band"] = band_name(static_cast<Band>(t.sys.band));

    json nr;
    nr["low_gb"] = t.normal.low_gb;
    nr["high_gb"] = t.normal.high_gb;
    j["normal"] = std::move(nr);

    json agents = json::array();
    for (int i = 0; i < kMaxAgents; ++i) {
        const StatusAgent& a = t.agents[i];
        if (a.sess_id == 0) continue;
        json o;
        o["phase"] = phase_name(static_cast<Phase>(a.phase));
        o["gb"] = static_cast<double>(a.footprint) / kGiB;
        o["idle_gb"] = static_cast<double>(a.idle_bytes) / kGiB;
        o["status"] = agent_status_name(a.status);
        agents.push_back(std::move(o));
    }
    j["agents"] = std::move(agents);

    json sessions = json::array();
    for (int i = 0; i < kMaxSessions; ++i) {
        const StatusSession& s = t.sessions[i];
        if (s.id == 0) continue;
        json o;
        o["tool"] = std::string(s.tool, strnlen(s.tool, sizeof(s.tool)));
        o["project"] = std::string(s.project, strnlen(s.project, sizeof(s.project)));
        o["gb"] = static_cast<double>(s.footprint) / kGiB;
        o["usual_gb"] = s.usual_gb;
        o["ratio"] = s.ratio;
        o["state"] = session_state_name(static_cast<SessionState>(s.state));
        sessions.push_back(std::move(o));
    }
    j["sessions"] = std::move(sessions);

    json chart = json::array();
    for (int i = 0; i < kRingPoints; ++i) {
        const StatusRing& r = t.ring[i];
        if (r.t_usec == 0) continue;
        json point = json::array();
        point.push_back(static_cast<double>(r.t_usec) / 1e6);
        point.push_back(static_cast<double>(r.ram) / kGiB);
        point.push_back(static_cast<double>(r.swap) / kGiB);
        chart.push_back(std::move(point));
    }
    j["chart"] = std::move(chart);

    json cleanup;
    cleanup["ram_gb"] = 0.0;
    cleanup["disk_gb"] = 0.0;
    json items = json::array();
    for (int i = 0; i < t.cleanup.count && i < kMaxCleanupItems; ++i) {
        const StatusCleanupItem& it = t.cleanup.items[i];
        json o;
        o["label"] = std::string(it.label, strnlen(it.label, sizeof(it.label)));
        o["bytes"] = it.bytes;
        o["kind"] = it.kind == static_cast<uint8_t>(ItemKind::Disk) ? "disk" : "ram";
        o["status"] = item_status_name(static_cast<ItemStatus>(it.status));
        items.push_back(std::move(o));
        if (it.kind == static_cast<uint8_t>(ItemKind::Disk))
            cleanup["disk_gb"] = cleanup["disk_gb"].get<double>() +
                                 static_cast<double>(it.bytes) / kGiB;
        else
            cleanup["ram_gb"] = cleanup["ram_gb"].get<double>() +
                                static_cast<double>(it.bytes) / kGiB;
    }
    cleanup["items"] = std::move(items);
    j["cleanup"] = std::move(cleanup);

    return j.dump();
}

}  // namespace pw
