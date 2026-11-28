#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "daemon.hpp"
#include "pw/core/membership.hpp"
#include "pw/log.hpp"
#include "pw/os/proc.hpp"

namespace pw {
namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr int kHistoryCap = 14 * 8640;  // 10 s samples over 14 days

uint64_t bucket_of(uint64_t usec) {
    const time_t sec = static_cast<time_t>(usec / 1000000ull);
    struct tm tm {};
    localtime_r(&sec, &tm);
    return static_cast<uint64_t>(tm.tm_wday) * 24ull + static_cast<uint64_t>(tm.tm_hour);
}

std::string date_of(uint64_t usec) {
    const time_t sec = static_cast<time_t>(usec / 1000000ull);
    struct tm tm {};
    localtime_r(&sec, &tm);
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void append_line(const std::string& path, const std::string& line) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(path, std::ios::app);
    if (out) out << line << "\n";
}

PhaseRules default_rules() {
    PhaseRules r;
    r.rules.push_back({"opencode.*(plan|Plan)|--phase[= ]?Plan", Phase::Plan});
    r.rules.push_back(
        {"tsc|vite build|next build|webpack|esbuild|--phase[= ]?Build", Phase::Build});
    r.rules.push_back(
        {"bun test|vitest|jest|pytest|pw-sim|--phase[= ]?Test", Phase::Test});
    r.rules.push_back({"review|git diff|--phase[= ]?Review", Phase::Review});
    return r;
}

}  // namespace

PhaseRules rules_for_project(const std::string& project_path) {
    if (!project_path.empty()) {
        std::ifstream in(project_path + "/.procwarden/cleanup-rules.json");
        if (in) {
            try {
                nlohmann::json j;
                in >> j;
                auto it = j.find("phase_rules");
                if (it != j.end() && it->is_array()) {
                    PhaseRules r;
                    for (const auto& e : *it) {
                        const std::string pattern = e.value("pattern", std::string());
                        const Phase phase = phase_from_tag(e.value("phase", std::string()));
                        if (!pattern.empty() && phase != Phase::None)
                            r.rules.push_back({pattern, phase});
                    }
                    if (!r.rules.empty()) return r;
                }
            } catch (const std::exception&) {
                // Fall through to the defaults.
            }
        }
    }
    return default_rules();
}

void load_history(DaemonState& st) {
    std::error_code ec;
    std::filesystem::create_directories(st.history_dir, ec);
    if (ec) return;

    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(st.history_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() > 6 && name.compare(name.size() - 6, 6, ".jsonl") == 0)
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    const std::size_t keep = static_cast<std::size_t>(std::max(st.cfg.baseline_days, 1));
    if (files.size() > keep) files.erase(files.begin(), files.end() - keep);

    for (const auto& file : files) {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            try {
                const auto j = nlohmann::json::parse(line);
                const double used = j.value("used_gb", 0.0);
                const uint64_t t = static_cast<uint64_t>(j.value("t", 0.0) * 1e6);
                if (t > 0 && used > 0.0) st.system_history.emplace_back(t, used);

                auto it = j.find("sessions");
                if (it != j.end() && it->is_array()) {
                    for (const auto& s : *it) {
                        const double gb = s.value("gb", 0.0);
                        if (gb <= 0.0) continue;
                        const std::string key = s.value("tool", std::string("opencode")) + "|" +
                                                s.value("project", std::string());
                        st.usual_by_key[key].push_back(gb);
                    }
                }
            } catch (const std::exception&) {
                // A corrupt line does not invalidate the rest of the history.
            }
        }
    }

    while (st.system_history.size() > static_cast<std::size_t>(kHistoryCap))
        st.system_history.pop_front();
    for (auto& [key, values] : st.usual_by_key) {
        (void)key;
        if (values.size() > static_cast<std::size_t>(kHistoryCap))
            values.erase(values.begin(), values.end() - kHistoryCap);
    }

    PW_LOG(LogLevel::Info, "history", "-", "-", "loaded %zu system samples, %zu keys",
           st.system_history.size(), st.usual_by_key.size());
}

void write_history_locked(DaemonState& st, const Snapshot& snap, const MemSample& mem) {
    (void)snap;
    nlohmann::json j;
    j["t"] = static_cast<double>(mem.t_usec) / 1e6;
    j["used_gb"] = static_cast<double>(mem.used) / kGiB;
    j["swap_gb"] = static_cast<double>(mem.swap_used) / kGiB;

    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& s : st.sessions) {
        if (s.state == SessionState::Done) continue;
        uint64_t fp = 0;
        for (const auto& m : s.members) fp += m.footprint;
        nlohmann::json o;
        o["id"] = s.id;
        o["tool"] = s.tool;
        o["project"] = s.project;
        o["gb"] = static_cast<double>(fp) / kGiB;
        sessions.push_back(std::move(o));
    }
    j["sessions"] = std::move(sessions);
    const std::string line = j.dump();
    const std::string date = date_of(mem.t_usec);

    append_line(st.history_dir + "/" + date + ".jsonl", line);
    for (const auto& s : st.sessions) {
        if (s.project_path.empty() || s.state == SessionState::Done) continue;
        append_line(s.project_path + "/.procwarden/history/" + date + ".jsonl", line);
    }
}

void* monitor_main(void* arg) {
    auto& st = *static_cast<DaemonState*>(arg);

    while (true) {
        {
            LockGuard guard(st.mu);
            if (st.stop) break;
        }
        sleep_ms(st.cfg.sample_interval_ms);

        auto snap_r = snapshot();
        auto mem_r = mem_sample();
        if (!snap_r.ok() || !mem_r.ok()) {
            PW_LOG(LogLevel::Warn, "monitor", "-", "-", "sample failed");
            continue;
        }
        Snapshot snap = std::move(snap_r.value());
        MemSample mem = mem_r.value();
        mem.processes = static_cast<uint32_t>(snap.procs.size());

        LockGuard guard(st.mu);
        if (st.stop) break;

        const uint64_t now = snap.t_usec;
        const uint64_t idle_us =
            static_cast<uint64_t>(std::max(st.cfg.idle_seconds, 0)) * 1000000ull;

        st.mem_ring.push_back(mem);
        if (static_cast<int>(st.mem_ring.size()) > kRingPoints)
            st.mem_ring.erase(st.mem_ring.begin());

        std::unordered_map<int32_t, const ProcInfo*> by_pid;
        by_pid.reserve(snap.procs.size());
        for (const auto& p : snap.procs) by_pid[p.pid] = &p;

        std::map<int32_t, double> footprint_gb;

        for (auto& s : st.sessions) {
            if (s.state == SessionState::Done) continue;

            auto& rules = st.rules_cache[s.project_path];
            if (rules.rules.empty()) rules = rules_for_project(s.project_path);

            auto lookup = [&](int32_t pid, TagInfo& out) {
                auto it = by_pid.find(pid);
                if (it == by_pid.end()) return false;
                auto ce = cmdline_env(pid, it->second->start_sec, it->second->start_usec);
                if (!ce.ok()) return false;
                out.session = ce.value().get("PW_SESSION");
                out.phase = ce.value().get("PW_PHASE");
                out.argv_joined = ce.value().argv_joined();
                return true;
            };

            auto res = update_membership(s, snap, rules, st.cfg.idle_seconds, now, lookup);
            s.members = std::move(res.members);
            s.escapees = std::move(res.escapees);
            s.last_seen_usec = now;

            // A lane is done when every member in it has stopped consuming CPU
            // for idle_seconds. Members are re-marked with that mask.
            uint32_t done_mask = 0;
            for (int ph = 1; ph <= 4; ++ph) {
                bool present = false;
                bool all_idle = true;
                uint64_t min_since = now;
                for (const auto& m : s.members) {
                    if (static_cast<int>(m.phase) != ph) continue;
                    present = true;
                    if (m.idle_since_usec == 0)
                        all_idle = false;
                    else
                        min_since = std::min(min_since, m.idle_since_usec);
                }
                if (present && all_idle && now >= min_since && now - min_since >= idle_us)
                    done_mask |= 1u << (ph - 1);
            }
            s.done_phase_mask = done_mask;

            for (auto& m : s.members) {
                const uint32_t bit =
                    m.phase == Phase::None
                        ? 0u
                        : (1u << (static_cast<uint32_t>(m.phase) - 1u));
                m.idle = bit != 0 && (done_mask & bit) != 0 && m.idle_since_usec != 0 &&
                         now >= m.idle_since_usec && now - m.idle_since_usec >= idle_us;
            }

            s.phase_mask = 0;
            uint64_t fp = 0;
            for (const auto& m : s.members) {
                if (m.phase != Phase::None)
                    s.phase_mask |= 1u << (static_cast<uint32_t>(m.phase) - 1u);
                fp += m.footprint;
            }
            footprint_gb[s.id] = static_cast<double>(fp) / kGiB;

            // Owner (launcher) death: apply the detach policy.
            if (s.owner_pid > 0 &&
                !identity_alive(snap, s.owner_pid, s.owner_start_sec, s.owner_start_usec)) {
                if (s.state == SessionState::Active) {
                    s.state = s.on_detach == DetachPolicy::Keep ? SessionState::Detached
                                                                : SessionState::OwnerLost;
                    s.pausable = s.on_detach != DetachPolicy::Keep;
                    PW_LOG(LogLevel::Warn, "monitor", s.project.c_str(), "owner",
                           "launcher %d gone (policy %s)", s.owner_pid, detach_name(s.on_detach));
                    if (s.on_detach == DetachPolicy::Stop) {
                        Job job;
                        job.kind = Job::Kind::Pause;
                        job.sess_id = s.id;
                        enqueue_locked(st, job);
                    } else if (s.on_detach == DetachPolicy::StopAfter10m) {
                        st.detach_deadline[s.id] = now + 600000000ull;
                    }
                }
            }

            auto deadline = st.detach_deadline.find(s.id);
            if (deadline != st.detach_deadline.end() && now >= deadline->second) {
                Job job;
                job.kind = Job::Kind::Pause;
                job.sess_id = s.id;
                enqueue_locked(st, job);
                st.detach_deadline.erase(deadline);
            }
        }

        // Growth history (1 s samples, last 120 s).
        for (auto& s : st.sessions) {
            if (s.state == SessionState::Done) continue;
            auto& hist = st.session_growth[s.id];
            hist.emplace_back(now, footprint_gb[s.id]);
            if (hist.size() > 120) hist.pop_front();
        }

        const uint64_t history_interval =
            static_cast<uint64_t>(std::max(st.cfg.history_interval_s, 1)) * 1000000ull;
        if (now >= st.last_history_write + history_interval) {
            st.last_history_write = now;

            st.system_history.emplace_back(now, static_cast<double>(mem.used) / kGiB);
            while (st.system_history.size() > static_cast<std::size_t>(kHistoryCap))
                st.system_history.pop_front();

            for (auto& s : st.sessions) {
                if (s.state == SessionState::Done) continue;
                const std::string key = s.tool + "|" + s.project;
                auto& values = st.usual_by_key[key];
                values.push_back(footprint_gb[s.id]);
                if (values.size() > static_cast<std::size_t>(kHistoryCap))
                    values.erase(values.begin(), values.end() - kHistoryCap);
                st.usual_gb[s.id] =
                    values.size() >= 3 ? percentile(values, 50.0) : 0.0;
            }

            write_history_locked(st, snap, mem);

            // Learned normal range for the same weekday/hour bucket when there
            // is enough data, otherwise all history (normal_range falls back
            // to the manual range under 24 h).
            std::vector<double> bucket;
            const uint64_t current_bucket = bucket_of(now);
            for (const auto& [t, v] : st.system_history)
                if (bucket_of(t) == current_bucket) bucket.push_back(v);
            if (bucket.size() <
                static_cast<std::size_t>(st.cfg.baseline_min_hours * 360)) {
                bucket.clear();
                for (const auto& [t, v] : st.system_history) {
                    (void)t;
                    bucket.push_back(v);
                }
            }
            st.normal = normal_range(bucket, st.cfg);
        }

        DecisionInput din;
        din.total = mem.total;
        din.used = mem.used;
        din.pressure_level = mem.pressure_level;
        din.enforce = st.cfg.enforce;
        if (st.mem_ring.size() >= 2) {
            din.swapout_this_sample =
                st.mem_ring.back().swapouts > st.mem_ring[st.mem_ring.size() - 2].swapouts;
        }

        for (const auto& s : st.sessions) {
            if (s.state == SessionState::Done) continue;
            SessionLoad load;
            load.id = s.id;
            load.label = s.tool + ":" + s.project;
            load.footprint_gb = footprint_gb[s.id];
            load.usual_gb = st.usual_gb[s.id];
            load.pausable = s.pausable;
            load.owner_alive =
                s.owner_pid == 0 ||
                identity_alive(snap, s.owner_pid, s.owner_start_sec, s.owner_start_usec);

            bool all_idle = !s.members.empty();
            for (const auto& m : s.members)
                if (!m.idle) all_idle = false;
            load.idle = all_idle;

            const auto& hist = st.session_growth[s.id];
            std::vector<std::pair<uint64_t, double>> points;
            const int take = std::min<int>(30, static_cast<int>(hist.size()));
            for (int i = static_cast<int>(hist.size()) - take; i < static_cast<int>(hist.size());
                 ++i)
                points.push_back(hist[static_cast<std::size_t>(i)]);
            load.growing = growth_stats(points, st.cfg).growing;

            load.over_budget = s.budget_bytes > 0 &&
                               static_cast<uint64_t>(load.footprint_gb * kGiB) > s.budget_bytes;

            const bool ratio_condition =
                load.usual_gb > 0.0 && load.footprint_gb > load.usual_gb * st.cfg.ratio_threshold;
            if (ratio_condition) {
                auto it = st.ratio_since.find(s.id);
                if (it == st.ratio_since.end()) st.ratio_since[s.id] = now;
                load.ratio_high = now >= st.ratio_since[s.id] &&
                                  now - st.ratio_since[s.id] >=
                                      static_cast<uint64_t>(st.cfg.ratio_hold_s) * 1000000ull;
            } else {
                st.ratio_since.erase(s.id);
                load.ratio_high = false;
            }

            din.sessions.push_back(std::move(load));
        }

        const Decision decision = decide(st.decision, din, st.cfg);
        for (const auto& action : decision.actions) {
            Job job;
            job.kind = action.kind == ActionKind::Terminate ? Job::Kind::Terminate
                                                            : Job::Kind::Pause;
            job.sess_id = action.sess_id;
            st.jobs.push_back(job);
            PW_LOG(LogLevel::Warn, "decision", action.label.c_str(), "enforce",
                   "%s (%s)", action.kind == ActionKind::Terminate ? "terminate" : "pause",
                   action.reason.c_str());
        }
        if (!decision.actions.empty()) st.job_cv.signal();

        publish_locked(st, snap, mem, decision);
    }
    return nullptr;
}

}  // namespace pw
