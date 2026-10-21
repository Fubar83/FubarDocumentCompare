// Deterministisk slumpgenerator för testgeneratorn (xoshiro256**, frö via splitmix64).
// Egna fördelningar i stället för <random>: standardbibliotekets fördelningar skiljer mellan
// implementationer, och samma frö ska ge samma testfall på alla plattformar.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace pc::testgen {

class Rng {
public:
    explicit Rng(uint64_t seed) {
        uint64_t z = seed;
        for (auto& s : s_) s = splitmix(z);
    }

    uint64_t next() {
        uint64_t r = rotl(s_[1] * 5, 7) * 9, t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return r;
    }

    // [0, 1)
    double random() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * random(); }
    // [lo, hi) som numpy integers(lo, hi); integers(n) = [0, n)
    int64_t integers(int64_t lo, int64_t hi) {
        if (hi <= lo) throw std::invalid_argument("Rng::integers: tomt intervall");
        uint64_t n = uint64_t(hi - lo), lim = UINT64_MAX - UINT64_MAX % n, x;
        do x = next(); while (x >= lim);
        return lo + int64_t(x % n);
    }
    int64_t integers(int64_t n) { return integers(0, n); }
    int index(size_t n) { return int(integers(int64_t(n))); }
    bool chance(double p) { return random() < p; }
    double sign() { return random() < 0.5 ? -1.0 : 1.0; }

    double normal(double mu = 0, double sigma = 1) {
        if (has_spare_) {
            has_spare_ = false;
            return mu + sigma * spare_;
        }
        double u, v, s;
        do {
            u = 2 * random() - 1;
            v = 2 * random() - 1;
            s = u * u + v * v;
        } while (s >= 1 || s == 0);
        double f = std::sqrt(-2 * std::log(s) / s);
        spare_ = v * f;
        has_spare_ = true;
        return mu + sigma * u * f;
    }

    int poisson(double lambda) {
        double l = std::exp(-lambda), p = 1;
        int k = 0;
        do {
            ++k;
            p *= random();
        } while (p > l);
        return k - 1;
    }

    template <class T>
    const T& choice(const std::vector<T>& v) {
        return v.at(index(v.size()));
    }
    template <class T>
    void shuffle(std::vector<T>& v) {
        for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[size_t(integers(int64_t(i)))]);
    }
    std::vector<int> permutation(int n) {
        std::vector<int> p(n);
        std::iota(p.begin(), p.end(), 0);
        shuffle(p);
        return p;
    }
    // k olika tal ur [0, n)
    std::vector<int> sample(int n, int k) {
        auto p = permutation(n);
        p.resize(std::min(k, n));
        return p;
    }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static uint64_t splitmix(uint64_t& z) {
        uint64_t r = (z += 0x9E3779B97F4A7C15ull);
        r = (r ^ (r >> 30)) * 0xBF58476D1CE4E5B9ull;
        r = (r ^ (r >> 27)) * 0x94D049BB133111EBull;
        return r ^ (r >> 31);
    }
    uint64_t s_[4];
    bool has_spare_ = false;
    double spare_ = 0;
};

}  // namespace pc::testgen
