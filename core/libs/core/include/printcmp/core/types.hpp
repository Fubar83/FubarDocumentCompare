// Grundtyper: JSON, mått, rutor, punkter och 3x3-matriser.
#pragma once

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace pc {

using json = nlohmann::ordered_json;
constexpr double MM_PER_INCH = 25.4;
constexpr double PT_PER_INCH = 72.0;
constexpr double PT_MM = MM_PER_INCH / PT_PER_INCH;

// Ruta (x0, y0, x1, y1), exklusiv höger/nederkant där det är pixlar.
struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
    double cx() const { return (x0 + x1) / 2; }
    double cy() const { return (y0 + y1) / 2; }
    double area() const { return std::max(0.0, w()) * std::max(0.0, h()); }
    bool empty() const { return w() <= 0 || h() <= 0; }
    Box unite(const Box& o) const {
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }
};

struct Pt {
    double x = 0, y = 0;
};

// 3x3-matris (radvis), för H: ref px -> prov px.
struct Mat3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double& operator()(int r, int c) { return m[r * 3 + c]; }
    double operator()(int r, int c) const { return m[r * 3 + c]; }
    Pt apply(Pt p) const {
        double d = m[6] * p.x + m[7] * p.y + m[8];
        return {(m[0] * p.x + m[1] * p.y + m[2]) / d, (m[3] * p.x + m[4] * p.y + m[5]) / d};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double s = 0;
                for (int k = 0; k < 3; ++k) s += (*this)(i, k) * o(k, j);
                r(i, j) = s;
            }
        return r;
    }
    Mat3 inverse() const;
};

}  // namespace pc
