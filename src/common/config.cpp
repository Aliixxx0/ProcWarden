#include "pw/config.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>

namespace pw {
namespace {

constexpr const char* kDefaultTool = "opencode";

const char* storage_name(Storage s) { return s == Storage::Shared ? "shared" : "local"; }
const char* mode_name(NormalMode m) { return m == NormalMode::Manual ? "manual" : "auto"; }
const char* alerts_name(Alerts a) {
    switch (a) {
        case Alerts::Quiet: return "quiet";
        case Alerts::Proactive: return "proactive";
        case Alerts::Balanced: break;
    }
    return "balanced";
}

Result<nlohmann::json> read_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return nlohmann::json::object();  // missing file = no overrides
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object())
            return Result<nlohmann::json>::err(EINVAL, path + ": top level must be an object");
        return j;
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::err(EINVAL, path + ": " + e.what());
    }
}

const nlohmann::json* sub_object(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return nullptr;
    return &*it;
}

void set_bool(const nlohmann::json& j, const char* key, bool& v) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) v = it->get<bool>();
}
void set_int(const nlohmann::json& j, const char* key, int& v) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) v = it->get<int>();
}
void set_double(const nlohmann::json& j, const char* key, double& v) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) v = it->get<double>();
}
void set_string(const nlohmann::json& j, const char* key, std::string& v) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) v = it->get<std::string>();
}

}  // namespace

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h != nullptr && h[0] != '\0') return h;
    if (const struct passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr)
        return pw->pw_dir;
    return ".";
}

std::string procwarden_home() { return home_dir() + "/.procwarden"; }

Result<Config> Config::load_files(const std::string& home_config,
                                  const std::string& project_settings) {
    Config c;

    if (!home_config.empty()) {
        auto j = read_json_file(home_config);
        if (!j.ok()) return j.error();
        c.apply_json(j.value());
    }
    if (!project_settings.empty()) {
        auto j = read_json_file(project_settings);
        if (!j.ok()) return j.error();
        c.apply_json(j.value());
    }
    if (auto e = c.validate()) return *e;
    return c;
}

Result<Config> Config::load(const std::string& project_dir) {
    std::string home_cfg = procwarden_home() + "/config.json";
    std::string proj_cfg;
    if (!project_dir.empty()) proj_cfg = project_dir + "/.procwarden/settings.json";
    return load_files(home_cfg, proj_cfg);
}

void Config::apply_json(const nlohmann::json& j) {
    std::string s;

    s.clear();
    set_string(j, "storage", s);
    if (s == "local") storage = Storage::Local;
    else if (s == "shared") storage = Storage::Shared;

    if (const auto* w = sub_object(j, "watch")) {
        set_bool(*w, "opencode", watch_opencode);
        set_bool(*w, "sims", watch_sims);
        set_bool(*w, "dev_servers", watch_dev_servers);
        set_bool(*w, "package_caches", watch_package_caches);
    }

    if (const auto* nr = sub_object(j, "normal_range")) {
        s.clear();
        set_string(*nr, "mode", s);
        if (s == "auto") normal_mode = NormalMode::Auto;
        else if (s == "manual") normal_mode = NormalMode::Manual;
        set_double(*nr, "low_gb", low_gb);
        set_double(*nr, "high_gb", high_gb);
    }

    s.clear();
    set_string(j, "alerts", s);
    if (s == "quiet") alerts = Alerts::Quiet;
    else if (s == "balanced") alerts = Alerts::Balanced;
    else if (s == "proactive") alerts = Alerts::Proactive;

    if (const auto* c = sub_object(j, "cleanup")) {
        set_bool(*c, "always_ask", cleanup_always_ask);
        auto it = c->find("include");
        if (it != c->end() && it->is_array()) {
            std::vector<std::string> v;
            for (const auto& e : *it)
                if (e.is_string()) v.push_back(e.get<std::string>());
            if (!v.empty()) cleanup_include = std::move(v);
        }
    }

    set_bool(j, "enforce", enforce);
    set_int(j, "sample_interval_ms", sample_interval_ms);
    set_int(j, "grace_ms", grace_ms);
    set_int(j, "poll_ms", poll_ms);
    set_int(j, "heartbeat_ms", heartbeat_ms);
    set_int(j, "history_interval_s", history_interval_s);
    set_int(j, "history_days", history_days);
    set_int(j, "idle_seconds", idle_seconds);
    set_double(j, "ratio_threshold", ratio_threshold);
    set_int(j, "ratio_hold_s", ratio_hold_s);
    set_double(j, "growth_mb_s", growth_mb_s);
    set_double(j, "growth_rise_mb", growth_rise_mb);
    set_double(j, "growth_positive", growth_positive);
    set_int(j, "band_worse_samples", band_worse_samples);
    set_int(j, "band_better_samples", band_better_samples);
    set_double(j, "band_margin_points", band_margin_points);
    set_int(j, "swapout_window", swapout_window);
    set_int(j, "swapout_hits", swapout_hits);
    set_int(j, "victim_cooldown_samples", victim_cooldown_samples);
    set_int(j, "budget_cap_percent", budget_cap_percent);
    set_int(j, "baseline_days", baseline_days);
    set_int(j, "baseline_min_hours", baseline_min_hours);
    set_int(j, "max_active_sessions", max_active_sessions);
    set_int(j, "ipc_reply_timeout_ms", ipc_reply_timeout_ms);
    set_int(j, "ipc_retry_sleep_ms", ipc_retry_sleep_ms);
    set_double(j, "demo_target_gb", demo_target_gb);
    set_int(j, "demo_processes", demo_processes);

    (void)kDefaultTool;
}

std::optional<Error> Config::validate() const {
    if (!cleanup_always_ask)
        return Error{EINVAL, "cleanup.always_ask cannot be false"};
    if (low_gb <= 0.0 || high_gb <= low_gb)
        return Error{EINVAL, "normal_range must satisfy 0 < low_gb < high_gb"};
    if (sample_interval_ms < 100 || sample_interval_ms > 60000)
        return Error{EINVAL, "sample_interval_ms out of range"};
    if (grace_ms < 0 || poll_ms < 10)
        return Error{EINVAL, "grace_ms/poll_ms out of range"};
    if (band_worse_samples < 1 || band_better_samples < 1)
        return Error{EINVAL, "band hysteresis sample counts must be >= 1"};
    if (budget_cap_percent <= 0 || budget_cap_percent > 100)
        return Error{EINVAL, "budget_cap_percent out of range"};
    if (swapout_window < 1 || swapout_hits < 1 || swapout_hits > swapout_window)
        return Error{EINVAL, "swapout window/hits out of range"};
    if (cleanup_include.empty())
        return Error{EINVAL, "cleanup.include must name at least one item class"};
    return std::nullopt;
}

nlohmann::json Config::to_json() const {
    nlohmann::json j;
    j["version"] = 1;
    j["storage"] = storage_name(storage);

    nlohmann::json w;
    w["opencode"] = watch_opencode;
    w["sims"] = watch_sims;
    w["dev_servers"] = watch_dev_servers;
    w["package_caches"] = watch_package_caches;
    j["watch"] = std::move(w);

    nlohmann::json nr;
    nr["mode"] = mode_name(normal_mode);
    nr["low_gb"] = low_gb;
    nr["high_gb"] = high_gb;
    j["normal_range"] = std::move(nr);

    j["alerts"] = alerts_name(alerts);

    nlohmann::json c;
    c["always_ask"] = cleanup_always_ask;
    c["include"] = cleanup_include;
    j["cleanup"] = std::move(c);

    j["enforce"] = enforce;
    j["sample_interval_ms"] = sample_interval_ms;
    j["grace_ms"] = grace_ms;
    j["poll_ms"] = poll_ms;
    j["heartbeat_ms"] = heartbeat_ms;
    j["history_interval_s"] = history_interval_s;
    j["history_days"] = history_days;
    j["idle_seconds"] = idle_seconds;
    j["ratio_threshold"] = ratio_threshold;
    j["ratio_hold_s"] = ratio_hold_s;
    j["growth_mb_s"] = growth_mb_s;
    j["growth_rise_mb"] = growth_rise_mb;
    j["growth_positive"] = growth_positive;
    j["band_worse_samples"] = band_worse_samples;
    j["band_better_samples"] = band_better_samples;
    j["band_margin_points"] = band_margin_points;
    j["swapout_window"] = swapout_window;
    j["swapout_hits"] = swapout_hits;
    j["victim_cooldown_samples"] = victim_cooldown_samples;
    j["budget_cap_percent"] = budget_cap_percent;
    j["baseline_days"] = baseline_days;
    j["baseline_min_hours"] = baseline_min_hours;
    j["max_active_sessions"] = max_active_sessions;
    j["ipc_reply_timeout_ms"] = ipc_reply_timeout_ms;
    j["ipc_retry_sleep_ms"] = ipc_retry_sleep_ms;
    j["demo_target_gb"] = demo_target_gb;
    j["demo_processes"] = demo_processes;
    return j;
}

}  // namespace pw
