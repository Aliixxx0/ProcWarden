#include "doctest.h"

#include <initializer_list>
#include <map>
#include <string>

#include "pw/core/membership.hpp"

namespace {

pw::ProcInfo P(int pid, int ppid, int pgid, uint64_t start = 1, uint64_t cpu = 100) {
    pw::ProcInfo p;
    p.pid = pid;
    p.ppid = ppid;
    p.pgid = pgid;
    p.start_sec = start;
    p.start_usec = 0;
    p.cpu_time_ns = cpu;
    p.footprint = 1024;
    return p;
}

pw::Snapshot S(std::initializer_list<pw::ProcInfo> procs) {
    pw::Snapshot s;
    s.t_usec = 40'000'000;
    for (const auto& p : procs) s.procs.push_back(p);
    return s;
}

pw::PhaseRules rules() {
    pw::PhaseRules r;
    r.rules.push_back({"opencode.*plan", pw::Phase::Plan});
    r.rules.push_back({"tsc|vite build|next build", pw::Phase::Build});
    r.rules.push_back({"bun test|vitest|jest|pw-sim", pw::Phase::Test});
    r.rules.push_back({"review|diff", pw::Phase::Review});
    return r;
}

}  // namespace

TEST_CASE("closure follows children to a fixpoint") {
    pw::Session s;
    s.id = 1;
    s.pgid = 100;

    const auto snap = S({P(100, 1, 100), P(101, 100, 100), P(102, 101, 100), P(103, 100, 999)});
    auto r = pw::update_membership(s, snap, rules(), 30, snap.t_usec, nullptr);

    CHECK(r.members.size() == 4);
    REQUIRE(r.escapees.size() == 1);
    CHECK(r.escapees[0].pid == 103);
    CHECK(r.escapees[0].escaped);
}

TEST_CASE("pid reuse drops the stale member") {
    pw::Session s;
    s.id = 1;
    s.pgid = 100;

    pw::Member old;
    old.pid = 101;
    old.start_sec = 1;
    old.phase = pw::Phase::Build;
    s.members.push_back(old);

    // pid 101 was reused (new start time, different group); 200 is the live child.
    const auto snap = S({P(100, 1, 100), P(101, 1, 999, /*start=*/2), P(200, 100, 100)});
    auto r = pw::update_membership(s, snap, rules(), 30, snap.t_usec, nullptr);

    for (const auto& m : r.members) CHECK(m.pid != 101);
    bool has_200 = false;
    for (const auto& m : r.members) has_200 = has_200 || m.pid == 200;
    CHECK(has_200);
}

TEST_CASE("environment tag adopts an out-of-group process") {
    pw::Session s;
    s.id = 7;
    s.pgid = 100;

    const auto snap = S({P(100, 1, 100), P(300, 1, 500, /*start=*/5)});
    std::map<int, pw::TagInfo> tags;
    tags[300] = pw::TagInfo{"7", "", ""};

    auto lookup = [&](int32_t pid, pw::TagInfo& out) {
        auto it = tags.find(pid);
        if (it == tags.end()) return false;
        out = it->second;
        return true;
    };
    auto r = pw::update_membership(s, snap, rules(), 30, snap.t_usec, lookup);

    bool adopted = false;
    for (const auto& m : r.members) adopted = adopted || m.pid == 300;
    CHECK(adopted);
    REQUIRE(r.escapees.size() == 1);
    CHECK(r.escapees[0].pid == 300);
}

TEST_CASE("phase: tag wins, then rule, then parent") {
    pw::Session s;
    s.id = 1;
    s.pgid = 100;

    const auto snap = S({P(100, 1, 100), P(101, 100, 100), P(102, 101, 100)});
    std::map<int, pw::TagInfo> tags;
    tags[100] = pw::TagInfo{"1", "Build", ""};
    tags[101] = pw::TagInfo{"", "", "tsc --noEmit"};

    auto lookup = [&](int32_t pid, pw::TagInfo& out) {
        auto it = tags.find(pid);
        if (it == tags.end()) return false;
        out = it->second;
        return true;
    };
    auto r = pw::update_membership(s, snap, rules(), 30, snap.t_usec, lookup);

    for (const auto& m : r.members) CHECK(m.phase == pw::Phase::Build);
}

TEST_CASE("phase rules match the first pattern") {
    CHECK(pw::phase_from_tag("test") == pw::Phase::Test);
    CHECK(pw::phase_from_tag("nope") == pw::Phase::None);
    CHECK(pw::phase_from_command("vite build --mode prod", rules()) == pw::Phase::Build);
    CHECK(pw::phase_from_command("bun test ./x", rules()) == pw::Phase::Test);
    CHECK(pw::phase_from_command("vim notes.txt", rules()) == pw::Phase::None);
}

TEST_CASE("idle detection needs a done lane and 30 unchanged seconds") {
    pw::Session s;
    s.id = 1;
    s.pgid = 100;
    s.last_seen_usec = 10'000'000;  // previous sample
    s.done_phase_mask = 1u << (static_cast<uint32_t>(pw::Phase::Test) - 1u);

    pw::Member m;
    m.pid = 101;
    m.start_sec = 1;
    m.phase = pw::Phase::Test;
    m.cpu_time_ns = 500;
    s.members.push_back(m);

    const auto snap = S({P(100, 1, 100), P(101, 100, 100, 1, 500)});
    auto r = pw::update_membership(s, snap, rules(), 30, 40'000'000, nullptr);

    bool saw = false;
    for (const auto& x : r.members) {
        if (x.pid != 101) continue;
        saw = true;
        CHECK(x.idle);
        CHECK(x.idle_since_usec == 10'000'000);
    }
    CHECK(saw);

    // CPU moved -> no longer idle.
    const auto snap2 = S({P(100, 1, 100), P(101, 100, 100, 1, 900)});
    auto r2 = pw::update_membership(s, snap2, rules(), 30, 40'000'000, nullptr);
    for (const auto& x : r2.members)
        if (x.pid == 101) CHECK_FALSE(x.idle);
}

TEST_CASE("idle does not trigger before the lane is done") {
    pw::Session s;
    s.id = 1;
    s.pgid = 100;
    s.last_seen_usec = 10'000'000;
    s.done_phase_mask = 0;

    pw::Member m;
    m.pid = 101;
    m.start_sec = 1;
    m.phase = pw::Phase::Test;
    m.cpu_time_ns = 500;
    s.members.push_back(m);

    const auto snap = S({P(100, 1, 100), P(101, 100, 100, 1, 500)});
    auto r = pw::update_membership(s, snap, rules(), 30, 40'000'000, nullptr);
    for (const auto& x : r.members)
        if (x.pid == 101) CHECK_FALSE(x.idle);
}
