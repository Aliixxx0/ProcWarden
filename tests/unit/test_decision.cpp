#include "doctest.h"

#include <cstdint>
#include <vector>

#include "pw/core/decision.hpp"

namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

pw::DecisionInput input(double total_gb, double used_gb, uint32_t pressure = 1) {
    pw::DecisionInput in;
    in.total = static_cast<uint64_t>(total_gb * kGiB);
    in.used = static_cast<uint64_t>(used_gb * kGiB);
    in.pressure_level = pressure;
    return in;
}

pw::SessionLoad load(int id, double footprint, double usual, bool idle) {
    pw::SessionLoad s;
    s.id = id;
    s.label = "sess-" + std::to_string(id);
    s.footprint_gb = footprint;
    s.usual_gb = usual;
    s.idle = idle;
    s.pausable = false;
    return s;
}

}  // namespace

TEST_CASE("band boundaries follow the available-percent table") {
    CHECK(pw::band_from_available(40.0) == pw::Band::Green);
    CHECK(pw::band_from_available(25.0) == pw::Band::Green);
    CHECK(pw::band_from_available(24.9) == pw::Band::Yellow);
    CHECK(pw::band_from_available(15.0) == pw::Band::Yellow);
    CHECK(pw::band_from_available(14.9) == pw::Band::Orange);
    CHECK(pw::band_from_available(10.0) == pw::Band::Orange);
    CHECK(pw::band_from_available(9.9) == pw::Band::Red);
}

TEST_CASE("kernel pressure warning bumps one band, critical forces red") {
    pw::Config cfg;
    pw::DecisionState state;

    // 30% available would be GREEN; warning pressure makes it ORANGE.
    for (int i = 0; i < 2 * cfg.band_worse_samples; ++i)
        pw::decide(state, input(16, 11.2, 2), cfg);
    CHECK(state.band == pw::Band::Orange);

    pw::DecisionState critical;
    const auto d = pw::decide(critical, input(16, 8, 4), cfg);
    CHECK(d.band == pw::Band::Red);
    CHECK(d.reason.find("critical") != std::string::npos);
}

TEST_CASE("swap-out rule escalates to orange") {
    pw::Config cfg;
    pw::DecisionState state;
    state.band = pw::Band::Green;
    // Four of the last four samples already swapped out.
    for (int i = 0; i < 4; ++i) state.swapouts.push_back(true);

    for (int i = 0; i < 2 * cfg.band_worse_samples; ++i) {
        auto in = input(16, 11, 1);  // 31% available -> green candidate
        in.swapout_this_sample = true;
        pw::decide(state, in, cfg);
    }
    CHECK(state.band == pw::Band::Orange);
}

TEST_CASE("hysteresis needs 3 worse and 10 better samples") {
    pw::Config cfg;
    pw::DecisionState state;

    // 20% available -> YELLOW candidate, but stay GREEN for two samples.
    CHECK(pw::decide(state, input(16, 12.8), cfg).band == pw::Band::Green);
    CHECK(pw::decide(state, input(16, 12.8), cfg).band == pw::Band::Green);
    CHECK(pw::decide(state, input(16, 12.8), cfg).band == pw::Band::Yellow);

    // 31% available -> GREEN candidate, but only after 10 samples and >30%.
    for (int i = 0; i < cfg.band_better_samples - 1; ++i)
        CHECK(pw::decide(state, input(16, 11.0), cfg).band == pw::Band::Yellow);
    CHECK(pw::decide(state, input(16, 11.0), cfg).band == pw::Band::Green);
}

TEST_CASE("recovery needs the 5-point margin") {
    pw::Config cfg;
    pw::DecisionState state;
    state.band = pw::Band::Yellow;

    // 26% available is better than the 25% boundary but below 25+5.
    for (int i = 0; i < 20; ++i)
        CHECK(pw::decide(state, input(16, 11.84), cfg).band == pw::Band::Yellow);

    // 31% clears the margin; 10 samples recover.
    for (int i = 0; i < cfg.band_better_samples - 1; ++i)
        CHECK(pw::decide(state, input(16, 11.0), cfg).band == pw::Band::Yellow);
    CHECK(pw::decide(state, input(16, 11.0), cfg).band == pw::Band::Green);
}

TEST_CASE("observe mode only previews, enforce mode acts") {
    pw::Config cfg;
    pw::DecisionState state;
    state.band = pw::Band::Red;

    pw::DecisionInput in = input(16, 15);  // 6% available
    in.sessions.push_back(load(2, 8.0, 4.5, /*idle=*/true));

    const auto observe = pw::decide(state, in, cfg);
    REQUIRE(observe.would.size() == 1);
    CHECK(observe.would[0].kind == pw::ActionKind::WouldTerminate);
    CHECK(observe.would[0].sess_id == 2);
    CHECK(observe.actions.empty());

    pw::DecisionInput enforcing = in;
    enforcing.enforce = true;
    const auto act = pw::decide(state, enforcing, cfg);
    REQUIRE(act.actions.size() == 1);
    CHECK(act.actions[0].kind == pw::ActionKind::Terminate);
}

TEST_CASE("victim choice uses excess over usual and respects cooldown") {
    pw::Config cfg;
    pw::DecisionState state;
    state.band = pw::Band::Red;

    pw::DecisionInput in = input(16, 15);
    in.enforce = true;
    in.sessions.push_back(load(2, 8.0, 4.5, true));
    in.sessions.push_back(load(3, 2.0, 1.9, true));

    auto d = pw::decide(state, in, cfg);
    REQUIRE(d.actions.size() == 1);
    CHECK(d.actions[0].sess_id == 2);  // larger excess

    // During the cooldown the same victim is skipped; the other session can act.
    d = pw::decide(state, in, cfg);
    REQUIRE(d.actions.size() == 1);
    CHECK(d.actions[0].sess_id == 3);

    // With both victims cooling down, no action fires.
    d = pw::decide(state, in, cfg);
    CHECK(d.actions.empty());

    // After the cooldown expires, the largest excess is eligible again.
    state.cooldown.clear();
    d = pw::decide(state, in, cfg);
    REQUIRE(d.actions.size() == 1);
    CHECK(d.actions[0].sess_id == 2);
}

TEST_CASE("no victim in green/yellow") {
    pw::Config cfg;
    pw::DecisionState state;
    state.band = pw::Band::Green;

    pw::DecisionInput in = input(16, 10);  // 37% available
    in.enforce = true;
    in.sessions.push_back(load(2, 8.0, 4.5, true));

    const auto d = pw::decide(state, in, cfg);
    CHECK(d.actions.empty());
    CHECK(d.would.empty());
}

TEST_CASE("budget alone can make a session a victim") {
    pw::Config cfg;
    pw::DecisionState state;
    state.band = pw::Band::Orange;

    pw::SessionLoad s = load(4, 6.0, 0.0, false);
    s.over_budget = true;
    s.budget_bytes = static_cast<uint64_t>(2.0 * kGiB);
    s.pausable = true;

    pw::DecisionInput in = input(16, 14);
    in.enforce = true;
    in.sessions.push_back(s);

    const auto d = pw::decide(state, in, cfg);
    REQUIRE(d.actions.size() == 1);
    CHECK(d.actions[0].kind == pw::ActionKind::Pause);
    CHECK(d.actions[0].sess_id == 4);
}

TEST_CASE("growth rule needs slope, rise and a positive trend") {
    pw::Config cfg;
    std::vector<std::pair<uint64_t, double>> points;
    for (int i = 0; i < 6; ++i)
        points.emplace_back(static_cast<uint64_t>(i) * 1'000'000ull, 1.0 + 0.1 * i);

    const auto g = pw::growth_stats(points, cfg);
    CHECK(g.mb_per_s > cfg.growth_mb_s);
    CHECK(g.rise_mb >= cfg.growth_rise_mb);
    CHECK(g.positive_fraction == doctest::Approx(1.0));
    CHECK(g.growing);

    std::vector<std::pair<uint64_t, double>> flat;
    for (int i = 0; i < 6; ++i)
        flat.emplace_back(static_cast<uint64_t>(i) * 1'000'000ull, 2.0);
    CHECK_FALSE(pw::growth_stats(flat, cfg).growing);
}
