#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pw/result.hpp"

namespace pw {

enum class Storage { Local, Shared };
enum class NormalMode { Auto, Manual };
enum class Alerts { Quiet, Balanced, Proactive };

// Every tunable lives here, with a default and a JSON key. The loader applies
// defaults, then ~/.procwarden/config.json, then the project
// .procwarden/settings.json (spec section 3.1).
struct Config {
    // Storage / watch list
    Storage storage = Storage::Local;
    bool watch_opencode = true;
    bool watch_sims = true;
    bool watch_dev_servers = true;
    bool watch_package_caches = false;

    // Normal range and alerts
    NormalMode normal_mode = NormalMode::Auto;
    double low_gb = 5.0;
    double high_gb = 9.0;
    Alerts alerts = Alerts::Balanced;

    // Cleanup permission flow. always_ask cannot be false.
    bool cleanup_always_ask = true;
    std::vector<std::string> cleanup_include{
        "sims", "idle_agents", "leftover_servers", "npm", "pnpm", "bun", "biome"};
    bool enforce = false;  // observe mode is the default

    // Tunables (no magic numbers in the logic)
    int sample_interval_ms = 1000;
    int grace_ms = 5000;
    int poll_ms = 250;
    int heartbeat_ms = 2000;
    int history_interval_s = 10;
    int history_days = 30;
    int idle_seconds = 30;
    double ratio_threshold = 2.0;
    int ratio_hold_s = 30;
    double growth_mb_s = 2.0;
    double growth_rise_mb = 64.0;
    double growth_positive = 0.70;
    int band_worse_samples = 3;
    int band_better_samples = 10;
    double band_margin_points = 5.0;
    int swapout_window = 10;
    int swapout_hits = 5;
    int victim_cooldown_samples = 5;
    int budget_cap_percent = 25;
    int baseline_days = 14;
    int baseline_min_hours = 24;
    int max_active_sessions = 4;
    int ipc_reply_timeout_ms = 2000;
    int ipc_retry_sleep_ms = 50;
    double demo_target_gb = 14.2;
    int demo_processes = 412;

    static Config defaults() { return Config{}; }

    // Loads defaults -> ~/.procwarden/config.json -> <project>/.procwarden/settings.json.
    static Result<Config> load(const std::string& project_dir);
    // Testable variant with explicit paths ("" means "skip this file").
    static Result<Config> load_files(const std::string& home_config,
                                     const std::string& project_settings);

    // Returns an Error when a setting is invalid (e.g. always_ask=false).
    std::optional<Error> validate() const;

    nlohmann::json to_json() const;
    void apply_json(const nlohmann::json& j);
};

std::string home_dir();
std::string procwarden_home();

}  // namespace pw
