#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pw/core/session.hpp"
#include "pw/core/snapshot.hpp"

namespace pw {

// cleanup-rules.json phase patterns, in order. First match wins.
struct PhaseRule {
    std::string pattern;
    Phase phase = Phase::None;
};
struct PhaseRules {
    std::vector<PhaseRule> rules;
};

struct TagInfo {
    std::string session;   // PW_SESSION
    std::string phase;     // PW_PHASE
    std::string argv_joined;
};

struct MembershipResult {
    std::vector<Member> members;    // full closure
    std::vector<Member> escapees;   // pgid != session pgid
    int reparented = 0;             // members whose ppid is 1
};

// Pure function: snapshot in, membership out. No system calls. The caller
// supplies tag lookups lazily (cmdline/env is cached by the sampler).
MembershipResult update_membership(
    const Session& s, const Snapshot& snap, const PhaseRules& rules,
    int idle_seconds, uint64_t now_usec,
    const std::function<bool(int32_t, TagInfo&)>& tag_lookup);

Phase phase_from_tag(const std::string& tag);
Phase phase_from_command(const std::string& cmdline, const PhaseRules& rules);

}  // namespace pw
