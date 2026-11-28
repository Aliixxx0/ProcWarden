#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pw/config.hpp"
#include "pw/ipc/status_table.hpp"

namespace pw {

// Growth rule: least-squares slope over the recent window, total rise, and the
// fraction of positive steps. All three must pass before a session counts as
// "growing".
struct GrowthStats {
    double mb_per_s = 0.0;
    double rise_mb = 0.0;
    double positive_fraction = 0.0;
    bool growing = false;
};

GrowthStats growth_stats(const std::vector<std::pair<uint64_t, double>>& points_gb,
                         const Config& cfg);

struct SessionLoad {
    int32_t id = 0;
    std::string label;
    double footprint_gb = 0.0;
    double usual_gb = 0.0;
    bool idle = false;
    bool pausable = false;
    bool owner_alive = true;
    bool growing = false;
    bool over_budget = false;
    bool ratio_high = false;
    uint64_t budget_bytes = 0;
    double excess_gb = 0.0;  // footprint - reference (usual * ratio, or budget)
};

struct DecisionState {
    Band band = Band::Green;
    int worse_count = 0;
    int better_count = 0;
    std::deque<bool> swapouts;
    std::map<int32_t, int> cooldown;  // session id -> samples left
};

struct DecisionInput {
    uint64_t total = 0;
    uint64_t used = 0;
    uint32_t pressure_level = 1;
    bool swapout_this_sample = false;
    bool enforce = false;
    std::vector<SessionLoad> sessions;
};

enum class ActionKind { Terminate, Pause, Renice, WouldTerminate, WouldPause };

struct Action {
    ActionKind kind = ActionKind::WouldTerminate;
    int32_t sess_id = 0;
    std::string label;
    std::string reason;
    uint64_t bytes = 0;
};

struct Decision {
    Band band = Band::Green;
    std::string reason;
    std::vector<Action> actions;  // executed now (empty in observe mode)
    std::vector<Action> would;    // preview of what would happen
};

int band_rank(Band b);
Band band_from_rank(int rank);
Band band_from_available(double available_pct);

// Pure decision engine. Hysteresis, pressure override, swap-out rule and the
// victim choice all live here; the daemon only executes the returned actions.
Decision decide(DecisionState& state, const DecisionInput& in, const Config& cfg);

}  // namespace pw
