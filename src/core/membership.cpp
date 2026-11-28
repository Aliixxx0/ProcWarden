#include "pw/core/membership.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace pw {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::regex& compiled(const std::string& pattern) {
    static std::map<std::string, std::regex> cache;
    auto it = cache.find(pattern);
    if (it != cache.end()) return it->second;
    auto inserted = cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript));
    return inserted.first->second;
}

}  // namespace

Phase phase_from_tag(const std::string& tag) {
    const std::string t = lower(tag);
    if (t == "plan") return Phase::Plan;
    if (t == "build") return Phase::Build;
    if (t == "test") return Phase::Test;
    if (t == "review") return Phase::Review;
    return Phase::None;
}

Phase phase_from_command(const std::string& cmdline, const PhaseRules& rules) {
    if (cmdline.empty()) return Phase::None;
    for (const auto& rule : rules.rules) {
        if (rule.pattern.empty()) continue;
        try {
            if (std::regex_search(cmdline, compiled(rule.pattern))) return rule.phase;
        } catch (const std::regex_error&) {
            // A malformed pattern is skipped; the next rule still applies.
        }
    }
    return Phase::None;
}

MembershipResult update_membership(
    const Session& s, const Snapshot& snap, const PhaseRules& rules,
    int idle_seconds, uint64_t now_usec,
    const std::function<bool(int32_t, TagInfo&)>& tag_lookup) {
    std::unordered_map<int32_t, const ProcInfo*> by_pid;
    std::unordered_map<int32_t, std::vector<int32_t>> children;
    by_pid.reserve(snap.procs.size());
    for (const auto& p : snap.procs) {
        by_pid[p.pid] = &p;
        children[p.ppid].push_back(p.pid);
    }

    std::unordered_map<int32_t, TagInfo> tags;
    auto tag_for = [&](int32_t pid) -> const TagInfo* {
        auto it = tags.find(pid);
        if (it != tags.end()) return &it->second;
        if (!tag_lookup) return nullptr;
        TagInfo info;
        if (!tag_lookup(pid, info)) return nullptr;
        return &tags.emplace(pid, std::move(info)).first->second;
    };

    // pid-reuse guard: only members with the same (pid, start) survive.
    std::unordered_map<int32_t, Phase> phase_of;
    std::unordered_set<int32_t> seeds;
    for (const auto& m : s.members) {
        auto it = by_pid.find(m.pid);
        if (it != by_pid.end() && it->second->start_sec == m.start_sec &&
            it->second->start_usec == m.start_usec) {
            seeds.insert(m.pid);
            phase_of[m.pid] = m.phase;
        }
    }

    // seeds: same process group, or carrying this session's PW_SESSION tag.
    const std::string session_tag = std::to_string(s.id);
    for (const auto& p : snap.procs) {
        if (s.pgid > 0 && p.pgid == s.pgid) seeds.insert(p.pid);
        if (tag_lookup) {
            const TagInfo* ti = tag_for(p.pid);
            if (ti != nullptr && ti->session == session_tag) seeds.insert(p.pid);
        }
    }

    // BFS over the ppid map to a fixpoint.
    std::unordered_set<int32_t> closure;
    std::unordered_map<int32_t, int32_t> parent_of;
    std::deque<int32_t> queue(seeds.begin(), seeds.end());
    while (!queue.empty()) {
        const int32_t pid = queue.front();
        queue.pop_front();
        if (!closure.insert(pid).second) continue;
        auto kids = children.find(pid);
        if (kids == children.end()) continue;
        for (int32_t kid : kids->second) {
            parent_of.emplace(kid, pid);
            queue.push_back(kid);
        }
    }

    // Phase attribution: PW_PHASE tag, then the first matching rule, then the
    // parent's phase.
    std::function<Phase(int32_t, int)> resolve = [&](int32_t pid, int depth) -> Phase {
        auto known = phase_of.find(pid);
        if (known != phase_of.end()) return known->second;
        Phase ph = Phase::None;
        if (const TagInfo* ti = tag_for(pid)) {
            ph = phase_from_tag(ti->phase);
            if (ph == Phase::None) ph = phase_from_command(ti->argv_joined, rules);
        }
        if (ph == Phase::None && depth < 64) {
            auto pa = parent_of.find(pid);
            if (pa != parent_of.end() && closure.count(pa->second) != 0)
                ph = resolve(pa->second, depth + 1);
        }
        phase_of[pid] = ph;
        return ph;
    };
    for (int32_t pid : closure) resolve(pid, 0);

    std::unordered_map<int32_t, const Member*> previous;
    for (const auto& m : s.members) previous[m.pid] = &m;

    MembershipResult out;
    out.members.reserve(closure.size());
    for (int32_t pid : closure) {
        auto it = by_pid.find(pid);
        if (it == by_pid.end()) continue;
        const ProcInfo& p = *it->second;

        Member m;
        m.pid = p.pid;
        m.ppid = p.ppid;
        m.pgid = p.pgid;
        m.start_sec = p.start_sec;
        m.start_usec = p.start_usec;
        m.phase = phase_of[pid];
        m.footprint = p.footprint;
        m.rss = p.rss;
        m.cpu_time_ns = p.cpu_time_ns;
        m.escaped = (s.pgid > 0 && p.pgid != s.pgid);

        auto prev = previous.find(pid);
        if (prev != previous.end() && prev->second->start_sec == p.start_sec &&
            prev->second->start_usec == p.start_usec) {
            if (prev->second->cpu_time_ns == p.cpu_time_ns) {
                m.idle_since_usec = prev->second->idle_since_usec != 0
                                        ? prev->second->idle_since_usec
                                        : s.last_seen_usec;
            }
        }

        const uint32_t phase_bit =
            m.phase == Phase::None ? 0u : (1u << (static_cast<uint32_t>(m.phase) - 1u));
        const bool lane_done = phase_bit != 0 && (s.done_phase_mask & phase_bit) != 0;
        m.idle = lane_done && m.idle_since_usec != 0 && now_usec >= m.idle_since_usec &&
                 now_usec - m.idle_since_usec >=
                     static_cast<uint64_t>(std::max(idle_seconds, 0)) * 1000000ull;

        if (m.escaped) out.escapees.push_back(m);
        if (p.ppid == 1) ++out.reparented;
        out.members.push_back(m);
    }
    return out;
}

}  // namespace pw
