// Numerik som i Python/numpy: avrundning, median, percentil, medel.
#pragma once

#include <charconv>
#include <cmath>
#include <vector>

namespace pc {

// Pythons round(v, n): avrundar det exakta binära värdet (2.675 -> 2.67, 0.125 -> 0.12), inte v * 10^n.
inline double round_to(double v, int decimals) {
    if (!std::isfinite(v)) return v;
    char buf[400];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, decimals);
    if (r.ec != std::errc()) {
        double p = std::pow(10.0, decimals);
        return std::nearbyint(v * p) / p;
    }
    double out = 0;
    std::from_chars(buf, r.ptr, out);
    return out;
}
// Pythons round(): till närmaste, lika långt -> jämnt (som standardläget för nearbyint).
inline long pyround(double v) { return static_cast<long>(std::nearbyint(v)); }

double median(std::vector<double> v);
double percentile(std::vector<double> v, double q);   // numpy "linear"
double mean(const std::vector<double>& v);

}  // namespace pc
