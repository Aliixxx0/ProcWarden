#include "doctest.h"

#include <vector>

#include "pw/core/baseline.hpp"

TEST_CASE("percentile interpolates and clamps") {
    const std::vector<double> v{1, 2, 3, 4};
    CHECK(pw::percentile(v, 0) == doctest::Approx(1.0));
    CHECK(pw::percentile(v, 50) == doctest::Approx(2.5));
    CHECK(pw::percentile(v, 100) == doctest::Approx(4.0));
    CHECK(pw::percentile({}, 50) == doctest::Approx(0.0));
}

TEST_CASE("normal range falls back to manual before 24 h of history") {
    pw::Config cfg;
    cfg.low_gb = 5.0;
    cfg.high_gb = 9.0;

    std::vector<double> samples(100, 7.0);
    const auto r = pw::normal_range(samples, cfg);
    CHECK_FALSE(r.have_data);
    CHECK(r.low_gb == doctest::Approx(5.0));
    CHECK(r.high_gb == doctest::Approx(9.0));
    CHECK(r.samples == 100);
}

TEST_CASE("normal range uses p10/p90 once enough history exists") {
    pw::Config cfg;
    cfg.baseline_min_hours = 24;

    std::vector<double> samples;
    samples.reserve(24 * 360);
    for (int i = 0; i < 24 * 360; ++i) samples.push_back(5.0 + static_cast<double>(i % 100) / 50.0);

    const auto r = pw::normal_range(samples, cfg);
    CHECK(r.have_data);
    CHECK(r.low_gb >= 5.0);
    CHECK(r.high_gb <= 7.0);
    CHECK(r.high_gb > r.low_gb);
    CHECK(pw::in_range(6.0, r));
    CHECK_FALSE(pw::in_range(9.0, r));
}

TEST_CASE("manual mode ignores learned data") {
    pw::Config cfg;
    cfg.normal_mode = pw::NormalMode::Manual;
    cfg.low_gb = 4.0;
    cfg.high_gb = 8.0;

    std::vector<double> samples(24 * 360, 6.0);
    const auto r = pw::normal_range(samples, cfg);
    CHECK_FALSE(r.have_data);
    CHECK(r.low_gb == doctest::Approx(4.0));
    CHECK(r.high_gb == doctest::Approx(8.0));
}

TEST_CASE("usual footprint is median and p90") {
    const auto u = pw::usual_footprint({1.0, 2.0, 3.0, 4.0});
    CHECK(u.median_gb == doctest::Approx(2.5));
    CHECK(u.p90_gb == doctest::Approx(3.7));
    CHECK(u.samples == 4);

    const auto empty = pw::usual_footprint({});
    CHECK(empty.samples == 0);
    CHECK(empty.median_gb == doctest::Approx(0.0));
}
