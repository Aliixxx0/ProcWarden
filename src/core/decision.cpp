#include "pw/core/decision.hpp"

#include <algorithm>
#include <cmath>

namespace pw {
namespace {

constexpr double kGreenAvail = 25.0;
constexpr double kYellowAvail = 15.0;
constexpr double kOrangeAvail = 10.0;

double threshold_for_rank(int rank) {
    switch (rank) {
        case 0: return kGreenAvail;
        case 1: return kYellowAvail;
        case 2: return kOrangeAvail;
        default: return 0.0;
    }
}

}  // namespace

int band_rank(Band b) {
    switch (b) {
        case Band::Green: return 0;
        case Band::Yellow: return 1;
        case Band::Orange: return 2;
        case Band::Red: return 3;
    }
    return 0;
}

Band band_from_rank(int rank) {
    switch (std::clamp(rank, 0, 3)) {
        case 0: return Band::Green;
        case 1: return Band::Yellow;
        case 2: return Band::Orange;
        default: return Band::Red;
    }
}

Band band_from_available(double available_pct) {
    if (available_pct >= kGreenAvail) return Band::Green;
    if (available_pct >= kYellowAvail) return Band::Yellow;
    if (available_pct >= kOrangeAvail) return Band::Orange;
    return Band::Red;
}

GrowthStats growth_stats(const std::vector<std::pair<uint64_t, double>>& points_gb,
                         const Config& cfg) {
    GrowthStats g;
    if (points_gb.size() < 3) return g;

    // Least-squares slope in GB/s using the sample timestamps.
    const double t0 = static_cast<double>(points_gb.front().first) / 1e6;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double n = static_cast<double>(points_gb.size());
    for (const auto& p : points_gb) {
        const double x = static_cast<double>(p.first) / 1e6 - t0;
        const double y = p.second;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double denom = n * sxx - sx * sx;
    if (denom > 1e-12) {
        const double slope_gb_s = (n * sxy - sx * sy) / denom;
        g.mb_per_s = slope_gb_s * 1024.0;
    }

    g.rise_mb = (points_gb.back().second - points_gb.front().second) * 1024.0;

    int positive = 0;
    for (std::size_t i = 1; i < points_gb.size(); ++i)
        if (points_gb[i].second > points_gb[i - 1].second) ++positive;
    g.positive_fraction =
        static_cast<double>(positive) / static_cast<double>(points_gb.size() - 1);

    g.growing = g.mb_per_s >= cfg.growth_mb_s && g.rise_mb >= cfg.growth_rise_mb &&
                g.positive_fraction >= cfg.growth_positive;
    return g;
}

Decision decide(DecisionState& state, const DecisionInput& in, const Config& cfg) {
    Decision out;

    const double total_gb = static_cast<double>(in.total) / (1024.0 * 1024.0 * 1024.0);
    const double used_gb = static_cast<double>(in.used) / (1024.0 * 1024.0 * 1024.0);
    const double available_pct =
        total_gb > 0.0 ? std::max(0.0, 100.0 * (total_gb - used_gb) / total_gb) : 100.0;

    Band candidate = band_from_available(available_pct);
    std::string reason = "available " + std::to_string(static_cast<int>(available_pct)) + "%";

    // Kernel pressure can only make the band worse.
    if (in.pressure_level >= 4) {
        candidate = Band::Red;
        reason = "kernel pressure critical";
    } else if (in.pressure_level >= 2 && band_rank(candidate) < band_rank(Band::Orange)) {
        candidate = Band::Orange;
        reason = "kernel pressure warning";
    }

    // Swap-out rule: N samples with page-outs inside the last W samples.
    state.swapouts.push_back(in.swapout_this_sample);
    while (static_cast<int>(state.swapouts.size()) > cfg.swapout_window)
        state.swapouts.pop_front();
    const int hits = static_cast<int>(std::count(state.swapouts.begin(), state.swapouts.end(), true));
    if (hits >= cfg.swapout_hits && band_rank(candidate) < band_rank(Band::Orange)) {
        candidate = Band::Orange;
        reason = "swap-outs " + std::to_string(hits) + "/" +
                 std::to_string(cfg.swapout_window);
    }

    // Hysteresis: 3 worse samples to drop one band, 10 better samples (with a
    // 5-point margin over the boundary) to recover one band.
    int current = band_rank(state.band);
    const int cand_rank = band_rank(candidate);
    if (cand_rank > current) {
        state.better_count = 0;
        if (++state.worse_count >= cfg.band_worse_samples) {
            current = std::min(current + 1, 3);
            state.worse_count = 0;
        }
    } else if (cand_rank < current) {
        state.worse_count = 0;
        const double needed = threshold_for_rank(current - 1) + cfg.band_margin_points;
        if (available_pct + 1e-9 >= needed) {
            if (++state.better_count >= cfg.band_better_samples) {
                current = std::max(current - 1, 0);
                state.better_count = 0;
            }
        } else {
            state.better_count = 0;
        }
    } else {
        state.worse_count = 0;
        state.better_count = 0;
    }

    // Critical pressure is immediate, not hysteretic.
    if (in.pressure_level >= 4) current = 3;

    state.band = band_from_rank(current);
    out.band = state.band;
    out.reason = reason;

    // Per-session cooldown: the same victim is not chosen again for N samples.
    for (auto it = state.cooldown.begin(); it != state.cooldown.end();) {
        if (--it->second <= 0)
            it = state.cooldown.erase(it);
        else
            ++it;
    }

    // Victim choice only when the band is Orange or Red.
    if (band_rank(state.band) < band_rank(Band::Orange)) return out;

    struct Candidate {
        const SessionLoad* load;
        double excess;
        bool terminate;
    };
    std::vector<Candidate> candidates;
    for (const auto& s : in.sessions) {
        if (s.id == 0) continue;
        const bool eligible = s.idle || s.growing || s.over_budget || s.ratio_high;
        if (!eligible) continue;
        if (state.cooldown.count(s.id) != 0) continue;

        double reference = s.usual_gb;
        if (s.budget_bytes > 0) {
            const double budget_gb =
                static_cast<double>(s.budget_bytes) / (1024.0 * 1024.0 * 1024.0);
            reference = std::max(reference, budget_gb);
        }
        const double excess = s.footprint_gb - reference;
        if (excess <= 0.0) continue;

        // Idle lanes are terminated (frees memory); active/pausable sessions
        // are paused (stops growth without losing work).
        candidates.push_back({&s, excess, s.idle || !s.pausable});
    }
    if (candidates.empty()) return out;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.excess > b.excess; });
    const Candidate& chosen = candidates.front();

    Action preview;
    preview.sess_id = chosen.load->id;
    preview.label = chosen.load->label;
    preview.bytes = static_cast<uint64_t>(chosen.load->footprint_gb * 1024.0 * 1024.0 * 1024.0);
    preview.reason = out.reason + "; footprint " +
                     std::to_string(static_cast<int>(chosen.load->footprint_gb * 10.0) / 10.0) +
                     " GB vs usual " +
                     std::to_string(static_cast<int>(chosen.load->usual_gb * 10.0) / 10.0) + " GB";

    if (chosen.terminate) {
        preview.kind = ActionKind::WouldTerminate;
        out.would.push_back(preview);
        if (in.enforce) {
            Action a = preview;
            a.kind = ActionKind::Terminate;
            out.actions.push_back(a);
        }
    } else {
        preview.kind = ActionKind::WouldPause;
        out.would.push_back(preview);
        if (in.enforce) {
            Action a = preview;
            a.kind = ActionKind::Pause;
            out.actions.push_back(a);
        }
    }

    if (in.enforce && !out.actions.empty()) {
        state.cooldown[chosen.load->id] = cfg.victim_cooldown_samples;
    }
    return out;
}

}  // namespace pw
