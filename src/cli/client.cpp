#include "cli.hpp"

#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

namespace pw {
namespace {

uint64_t now_usec() {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ull +
           static_cast<uint64_t>(tv.tv_usec);
}

const char* band_str(Band b) {
    switch (b) {
        case Band::Green: return "green";
        case Band::Yellow: return "yellow";
        case Band::Orange: return "orange";
        case Band::Red: return "red";
    }
    return "?";
}

}  // namespace

Result<Client> Client::connect(bool need_shm) {
    auto keys = ipc_keys();
    if (!keys.ok()) return Result<Client>::err(keys.error().code, keys.error().context);

    auto q = MsgQueue::open(keys.value().queue_key);
    if (!q.ok()) return Result<Client>::err(ENOENT, "daemon not running (msgget)");

    Client c;
    c.queue = std::move(q.value());

    if (need_shm) {
        auto s = ShmSegment::open(keys.value().shm_key);
        if (!s.ok()) return Result<Client>::err(ENOENT, "daemon not running (shmget)");
        c.shm = std::move(s.value());
        auto mapped = c.shm.attach(true);
        if (!mapped.ok()) return Result<Client>::err(mapped.error().code, "shmat");
        c.table = static_cast<StatusTable*>(mapped.value());
        if (c.table->header.magic != kStatusMagic)
            return Result<Client>::err(EINVAL, "status table not initialised");
    }
    return c;
}

Result<PwMsg> Client::request(PwMsg msg, int timeout_ms) {
    // Drop replies from a previous timed-out request.
    PwMsg stale {};
    for (int i = 0; i < 16; ++i) {
        auto got = queue.recv(&stale, sizeof(stale), static_cast<long>(getpid()), IPC_NOWAIT);
        if (!got.ok()) break;
    }

    msg.client_pid = static_cast<int32_t>(getpid());
    msg.mtype = kMtypeNormal;
    auto sent = queue.send(&msg, sizeof(msg), msg.mtype, IPC_NOWAIT);
    if (!sent.ok()) return Result<PwMsg>::err(sent.error().code, sent.error().context);

    const uint64_t deadline =
        now_usec() + static_cast<uint64_t>(std::max(timeout_ms, 1)) * 1000ull;
    while (now_usec() < deadline) {
        PwMsg reply {};
        auto got = queue.recv(&reply, sizeof(reply), static_cast<long>(getpid()), IPC_NOWAIT);
        if (got.ok()) {
            if (reply.op == OP_REPLY) return reply;
            continue;
        }
        if (got.error().code != ENOMSG)
            return Result<PwMsg>::err(got.error().code, got.error().context);
        usleep(2000);
    }
    return Result<PwMsg>::err(ETIMEDOUT, "no reply from procwardend");
}

bool Client::refresh(StatusTable& out) const {
    if (table == nullptr) return false;
    if (!status_copy(table, &out)) return false;
    return out.header.magic == kStatusMagic && out.header.version == kStatusVersion;
}

std::string fmt_bytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f GiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::string fmt_gb(uint64_t bytes) { return fmt_bytes(bytes); }

std::string hash_hex(uint64_t hash) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

std::string status_to_json(const StatusTable& t) {
    nlohmann::json j;
    j["sample_no"] = t.header.sample_no;
    j["t_usec"] = t.header.t_usec;
    j["band"] = band_str(static_cast<Band>(t.sys.band));
    j["enforce"] = t.sys.enforce != 0;
    j["pressure"] = pressure_name(t.sys.pressure_level);
    j["total"] = t.sys.total;
    j["used"] = t.sys.used;
    j["swap_used"] = t.sys.swap_used;
    j["swap_total"] = t.sys.swap_total;
    j["processes"] = t.sys.processes;
    j["normal"] = {{"low_gb", t.normal.low_gb},
                   {"high_gb", t.normal.high_gb},
                   {"mode", t.normal.mode == 0 ? "auto" : "manual"}};

    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& s : t.sessions) {
        if (s.id == 0) continue;
        sessions.push_back({{"id", s.id},
                            {"tool", s.tool},
                            {"project", s.project},
                            {"state", session_state_name(static_cast<SessionState>(s.state))},
                            {"members", s.members},
                            {"footprint", s.footprint},
                            {"rss_upper", s.rss_upper},
                            {"usual_gb", s.usual_gb},
                            {"ratio", s.ratio},
                            {"owner_alive", s.owner_alive != 0}});
    }
    j["sessions"] = std::move(sessions);

    nlohmann::json agents = nlohmann::json::array();
    for (const auto& a : t.agents) {
        if (a.sess_id == 0) continue;
        agents.push_back({{"sess_id", a.sess_id},
                          {"phase", phase_name(static_cast<Phase>(a.phase))},
                          {"status", a.status == 0 ? "active" : (a.status == 1 ? "idle" : "done")},
                          {"footprint", a.footprint},
                          {"idle_bytes", a.idle_bytes}});
    }
    j["agents"] = std::move(agents);

    j["cleanup"] = {{"hash", hash_hex(t.cleanup.plan_hash)},
                    {"state", static_cast<int>(t.cleanup.state)},
                    {"count", t.cleanup.count}};
    return j.dump();
}

void print_status_line(const StatusTable& t) {
    const double avail =
        t.sys.total > 0
            ? 100.0 * (static_cast<double>(t.sys.total) - static_cast<double>(t.sys.used)) /
                  static_cast<double>(t.sys.total)
            : 0.0;
    std::printf("band=%s mode=%s used=%s/%s swap=%s procs=%u avail=%.0f%%\n",
                band_str(static_cast<Band>(t.sys.band)), t.sys.enforce ? "enforce" : "observe",
                fmt_bytes(t.sys.used).c_str(), fmt_bytes(t.sys.total).c_str(),
                fmt_bytes(t.sys.swap_used).c_str(), t.sys.processes, avail);
}

void draw_dashboard(const StatusTable& t) {
    std::printf("\x1b[2J\x1b[H");
    print_status_line(t);
    std::printf("pressure=%s normal=[%.1f, %.1f] GB (%s)\n", pressure_name(t.sys.pressure_level),
                t.normal.low_gb, t.normal.high_gb, t.normal.mode == 0 ? "auto" : "manual");

    std::printf("\nSessions\n");
    int rows = 0;
    for (const auto& s : t.sessions) {
        if (s.id == 0) continue;
        std::printf("  #%-2d %-9s %-16s %-9s %8s  usual %4.1f  ratio %4.2f  owner %s\n", s.id,
                    s.tool, s.project,
                    session_state_name(static_cast<SessionState>(s.state)),
                    fmt_bytes(s.footprint).c_str(), s.usual_gb, s.ratio,
                    s.owner_alive ? "ok" : "lost");
        ++rows;
    }
    if (rows == 0) std::printf("  (none)\n");

    std::printf("\nLanes\n");
    rows = 0;
    for (const auto& a : t.agents) {
        if (a.sess_id == 0) continue;
        std::printf("  #%-2d %-7s %-6s %8s (idle %s)\n", a.sess_id,
                    phase_name(static_cast<Phase>(a.phase)),
                    a.status == 0 ? "active" : (a.status == 1 ? "idle" : "done"),
                    fmt_bytes(a.footprint).c_str(), fmt_bytes(a.idle_bytes).c_str());
        ++rows;
    }
    if (rows == 0) std::printf("  (none)\n");

    if (t.cleanup.state != 0) {
        std::printf("\nCleanup %s  hash %s\n",
                    t.cleanup.state == static_cast<uint8_t>(CleanupState::Ready)
                        ? "ready"
                        : (t.cleanup.state == static_cast<uint8_t>(CleanupState::Clearing)
                               ? "clearing"
                               : (t.cleanup.state == static_cast<uint8_t>(CleanupState::Freed)
                                      ? "freed"
                                      : "failed")),
                    hash_hex(t.cleanup.plan_hash).c_str());
        for (int i = 0; i < t.cleanup.count; ++i) {
            const auto& item = t.cleanup.items[i];
            std::printf("  %-6s %-40s %8s  %s\n",
                        item.kind == static_cast<uint8_t>(ItemKind::Ram) ? "RAM" : "DISK",
                        item.label, fmt_bytes(item.bytes).c_str(),
                        item.status == 0 ? "ready"
                                         : (item.status == 1 ? "clearing"
                                                             : (item.status == 2 ? "freed" : "failed")));
        }
    }
    std::fflush(stdout);
}

}  // namespace pw
