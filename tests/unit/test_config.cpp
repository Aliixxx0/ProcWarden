#include "doctest.h"

#include <filesystem>
#include <fstream>

#include "pw/config.hpp"

namespace {

std::string write_temp(const std::string& name, const std::string& body) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    out << body;
    return path.string();
}

}  // namespace

TEST_CASE("config defaults match the brief") {
    const pw::Config c = pw::Config::defaults();
    CHECK(c.storage == pw::Storage::Local);
    CHECK(c.watch_opencode);
    CHECK(c.watch_sims);
    CHECK(c.watch_dev_servers);
    CHECK_FALSE(c.watch_package_caches);
    CHECK(c.normal_mode == pw::NormalMode::Auto);
    CHECK(c.low_gb == doctest::Approx(5.0));
    CHECK(c.high_gb == doctest::Approx(9.0));
    CHECK(c.cleanup_always_ask);
    CHECK_FALSE(c.enforce);
    CHECK(c.cleanup_include.size() == 7);
    CHECK(c.grace_ms == 5000);
    CHECK(c.budget_cap_percent == 25);
}

TEST_CASE("config precedence: defaults < home < project") {
    const std::string home = write_temp("pw_home_cfg.json",
        R"({"sample_interval_ms":2000,"enforce":true,"storage":"shared"})");
    const std::string proj = write_temp("pw_proj_cfg.json",
        R"({"watch":{"opencode":false},"normal_range":{"low_gb":6,"high_gb":10},
            "cleanup":{"include":["sims","npm"]}})");

    auto r = pw::Config::load_files(home, proj);
    REQUIRE(r.ok());
    const pw::Config& c = r.value();
    CHECK(c.sample_interval_ms == 2000);   // home
    CHECK(c.enforce);                      // home
    CHECK(c.storage == pw::Storage::Shared);
    CHECK_FALSE(c.watch_opencode);         // project wins
    CHECK(c.low_gb == doctest::Approx(6.0));
    CHECK(c.high_gb == doctest::Approx(10.0));
    REQUIRE(c.cleanup_include.size() == 2);
    CHECK(c.cleanup_include[0] == "sims");
}

TEST_CASE("always_ask=false is rejected") {
    pw::Config c;
    nlohmann::json j = nlohmann::json::parse(R"({"cleanup":{"always_ask":false}})");
    c.apply_json(j);
    auto e = c.validate();
    REQUIRE(e.has_value());
    CHECK(e->code == EINVAL);
}

TEST_CASE("invalid normal range is rejected") {
    pw::Config c;
    c.low_gb = 9.0;
    c.high_gb = 5.0;
    auto e = c.validate();
    REQUIRE(e.has_value());
}

TEST_CASE("settings JSON round trip") {
    pw::Config a;
    a.storage = pw::Storage::Shared;
    a.enforce = true;
    a.low_gb = 7.0;
    a.high_gb = 11.5;
    a.alerts = pw::Alerts::Proactive;
    a.sample_interval_ms = 750;

    const nlohmann::json j = a.to_json();
    pw::Config b;
    b.apply_json(j);

    CHECK(b.storage == pw::Storage::Shared);
    CHECK(b.enforce);
    CHECK(b.low_gb == doctest::Approx(7.0));
    CHECK(b.high_gb == doctest::Approx(11.5));
    CHECK(b.alerts == pw::Alerts::Proactive);
    CHECK(b.sample_interval_ms == 750);
    CHECK(b.cleanup_include == a.cleanup_include);
}

TEST_CASE("missing config files fall back to defaults") {
    auto r = pw::Config::load_files("", "");
    REQUIRE(r.ok());
    CHECK(r.value().sample_interval_ms == 1000);
}
