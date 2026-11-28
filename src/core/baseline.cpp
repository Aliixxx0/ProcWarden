#include "pw/core/baseline.hpp"

#include <algorithm>
#include <cmath>

namespace pw {

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    if (p <= 0.0) return values.front();
    if (p >= 100.0) return values.back();

    const double rank = (static_cast<double>(values.size()) - 1.0) * p / 100.0;
    const std::size_t lo = static_cast<std::size_t>(std::floor(rank));
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return values[lo] + (values[hi] - values[lo]) * frac;
}

NormalRange normal_range(const std::vector<double>& used_gb, const Config& cfg) {
    NormalRange r;
    r.samples = static_cast<int>(used_gb.size());

    const int min_samples = cfg.baseline_min_hours * 360;  // 10 s history
    if (cfg.normal_mode == NormalMode::Manual || r.samples < min_samples) {
        r.low_gb = cfg.low_gb;
        r.high_gb = cfg.high_gb;
        r.have_data = false;
        return r;
    }

    const double low = percentile(used_gb, 10.0);
    const double high = percentile(used_gb, 90.0);
    r.low_gb = std::floor(low * 10.0) / 10.0;
    r.high_gb = std::ceil(high * 10.0) / 10.0;
    if (r.high_gb <= r.low_gb) r.high_gb = r.low_gb + 0.1;
    r.have_data = true;
    return r;
}

UsualFootprint usual_footprint(const std::vector<double>& footprint_gb) {
    UsualFootprint u;
    u.samples = static_cast<int>(footprint_gb.size());
    if (footprint_gb.empty()) return u;
    u.median_gb = percentile(footprint_gb, 50.0);
    u.p90_gb = percentile(footprint_gb, 90.0);
    return u;
}

bool in_range(double used_gb, const NormalRange& r) {
    return used_gb >= r.low_gb && used_gb <= r.high_gb;
}

}  // namespace pw
