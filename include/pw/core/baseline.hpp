#pragma once

#include <vector>

#include "pw/config.hpp"

namespace pw {

struct NormalRange {
    double low_gb = 0.0;
    double high_gb = 0.0;
    bool have_data = false;
    int samples = 0;
};

struct UsualFootprint {
    double median_gb = 0.0;
    double p90_gb = 0.0;
    int samples = 0;
};

// p in [0, 100], linear interpolation between ranks. Empty input -> 0.
double percentile(std::vector<double> values, double p);

// p10/p90 of the system "used" series, rounded outward to 0.1 GB. With fewer
// than baseline_min_hours * 360 samples (10 s history) it falls back to the
// configured manual range and sets have_data=false.
NormalRange normal_range(const std::vector<double>& used_gb, const Config& cfg);

// Median and p90 of one session's footprint history.
UsualFootprint usual_footprint(const std::vector<double>& footprint_gb);

bool in_range(double used_gb, const NormalRange& r);

}  // namespace pw
