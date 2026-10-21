#include "printcmp/imgproc/transform.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pc::imgproc {

namespace {

int flags_of(Interpolation i, bool shrinking) {
    switch (i) {
        case Interpolation::Nearest: return cv::INTER_NEAREST;
        case Interpolation::Linear: return cv::INTER_LINEAR;
        case Interpolation::Cubic: return cv::INTER_CUBIC;
        case Interpolation::Area: return cv::INTER_AREA;
        default: return shrinking ? cv::INTER_AREA : cv::INTER_CUBIC;
    }
}

// Area finns inte för warpAffine/warpPerspective: linjär i stället
int warp_flags(Interpolation i) {
    int f = flags_of(i, false);
    return f == cv::INTER_AREA ? cv::INTER_LINEAR : f;
}

cv::Scalar fill_for(const cv::Mat& m, const cv::Scalar& fill) {
    if (m.channels() == 4 && fill == cv::Scalar::all(255)) return cv::Scalar(255, 255, 255, 0);   // genomskinligt
    return fill;
}

}  // namespace

cv::Mat rotate(const cv::Mat& src, double deg, bool expand, const cv::Scalar& fill, Interpolation interp) {
    if (std::abs(std::fmod(deg, 90.0)) < 1e-9) {
        int q = (int)std::lround(deg / 90.0);
        if (expand || (q % 2 == 0) || src.cols == src.rows) return rotate90(src, q);
    }
    cv::Point2d c((src.cols - 1) / 2.0, (src.rows - 1) / 2.0);
    cv::Mat m = cv::getRotationMatrix2D(c, deg, 1.0);
    cv::Size size = src.size();
    if (expand) {
        double a = deg * CV_PI / 180, ca = std::abs(std::cos(a)), sa = std::abs(std::sin(a));
        size = cv::Size((int)std::ceil(src.cols * ca + src.rows * sa), (int)std::ceil(src.cols * sa + src.rows * ca));
        m.at<double>(0, 2) += (size.width - 1) / 2.0 - c.x;
        m.at<double>(1, 2) += (size.height - 1) / 2.0 - c.y;
    }
    cv::Mat out;
    cv::warpAffine(src, out, m, size, warp_flags(interp), cv::BORDER_CONSTANT, fill_for(src, fill));
    return out;
}

cv::Mat rotate90(const cv::Mat& src, int q) {
    q = ((q % 4) + 4) % 4;
    if (q == 0) return src.clone();
    cv::Mat out;
    cv::rotate(src, out, q == 1 ? cv::ROTATE_90_COUNTERCLOCKWISE : q == 2 ? cv::ROTATE_180 : cv::ROTATE_90_CLOCKWISE);
    return out;
}

cv::Mat flip(const cv::Mat& src, bool horizontal, bool vertical) {
    if (!horizontal && !vertical) return src.clone();
    cv::Mat out;
    cv::flip(src, out, horizontal && vertical ? -1 : horizontal ? 1 : 0);
    return out;
}

cv::Mat scale(const cv::Mat& src, double fx, double fy, Interpolation interp) {
    if (!(fx > 0) || !(fy > 0)) throw std::invalid_argument("skalan måste vara positiv");
    return resize_to(src, cv::Size(std::max(1, (int)std::lround(src.cols * fx)), std::max(1, (int)std::lround(src.rows * fy))), interp);
}

cv::Mat resize_to(const cv::Mat& src, cv::Size size, Interpolation interp) {
    if (size.width <= 0 || size.height <= 0) throw std::invalid_argument("storleken måste vara positiv");
    cv::Mat out;
    bool shrinking = size.width < src.cols || size.height < src.rows;
    cv::resize(src, out, size, 0, 0, flags_of(interp, shrinking));
    return out;
}

cv::Mat crop(const cv::Mat& src, const cv::Rect& rect) {
    cv::Rect r = rect & cv::Rect(0, 0, src.cols, src.rows);
    if (r.empty()) throw std::invalid_argument("utsnittet ligger utanför bilden");
    return src(r).clone();
}

cv::Mat warp_affine(const cv::Mat& src, const cv::Matx23d& m, cv::Size size, const cv::Scalar& fill, Interpolation interp) {
    cv::Mat out;
    cv::warpAffine(src, out, cv::Mat(m), size, warp_flags(interp), cv::BORDER_CONSTANT, fill_for(src, fill));
    return out;
}

cv::Mat warp_quad(const cv::Mat& src, const std::array<cv::Point2d, 4>& corners, cv::Size size, Interpolation interp) {
    std::vector<cv::Point2f> from, to{{0, 0}, {(float)size.width - 1, 0}, {(float)size.width - 1, (float)size.height - 1}, {0, (float)size.height - 1}};
    for (auto& c : corners) from.emplace_back((float)c.x, (float)c.y);
    cv::Mat h = cv::getPerspectiveTransform(from, to), out;
    cv::warpPerspective(src, out, h, size, warp_flags(interp), cv::BORDER_REPLICATE);
    return out;
}

namespace {

Skew skew_of_gray(const cv::Mat& g, double max_deg);

}  // namespace

Skew estimate_skew(const cv::Mat& src, double max_deg) {
    // arbetsbild: grå, högst ~1200 px (snabbt, och fint raster ger inte riktningar)
    cv::Mat g = src;
    if (src.channels() == 3) cv::cvtColor(src, g, cv::COLOR_RGB2GRAY);
    else if (src.channels() == 4) cv::cvtColor(src, g, cv::COLOR_RGBA2GRAY);
    if (g.depth() != CV_8U) g.convertTo(g, CV_8U);
    double f = std::min(1.0, 1200.0 / std::max(g.cols, g.rows));
    if (f < 1) cv::resize(g, g, cv::Size(), f, f, cv::INTER_AREA);
    cv::GaussianBlur(g, g, cv::Size(0, 0), 1.0);
    Skew first = skew_of_gray(g, max_deg);
    if (first.confidence <= 0) return first;
    // Förfining med projektionsprofiler: kantkartan vrids i små steg kring den grova vinkeln, och den
    // vinkel där rad- och kolumnsummorna är skarpast (störst varians) vinner. Gradientriktningen ensam
    // dras mot axlarna av trappstegen i interpolerade kanter.
    cv::Mat gx, gy, mag;
    cv::Scharr(g, gx, CV_32F, 1, 0);
    cv::Scharr(g, gy, CV_32F, 0, 1);
    cv::magnitude(gx, gy, mag);
    double fr = std::min(1.0, 800.0 / std::max(mag.cols, mag.rows));
    if (fr < 1) cv::resize(mag, mag, cv::Size(), fr, fr, cv::INTER_AREA);
    cv::Point2d c((mag.cols - 1) / 2.0, (mag.rows - 1) / 2.0);
    auto sharpness = [&](double deg) {
        cv::Mat r, rows, cols;
        cv::warpAffine(mag, r, cv::getRotationMatrix2D(c, -deg, 1.0), mag.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, 0);
        cv::reduce(r, rows, 1, cv::REDUCE_SUM, CV_64F);
        cv::reduce(r, cols, 0, cv::REDUCE_SUM, CV_64F);
        cv::Scalar mr, sr, mc, sc;
        cv::meanStdDev(rows, mr, sr);
        cv::meanStdDev(cols, mc, sc);
        return sr[0] * sr[0] / std::max(1, mag.cols) + sc[0] * sc[0] / std::max(1, mag.rows);
    };
    constexpr double step = 0.05, span = 1.0;
    double best = first.angle_deg, best_v = -1;
    std::vector<std::pair<double, double>> samples;
    for (double d = -span; d <= span + 1e-9; d += step) {
        double a = first.angle_deg + d;
        if (std::abs(a) > max_deg + 1e-9) continue;
        double v = sharpness(a);
        samples.push_back({a, v});
        if (v > best_v) best_v = v, best = a;
    }
    // parabel genom toppen och grannarna
    for (size_t i = 1; i + 1 < samples.size(); ++i)
        if (samples[i].first == best) {
            double l = samples[i - 1].second, r = samples[i + 1].second, den = l - 2 * best_v + r;
            if (std::abs(den) > 1e-12) best += std::clamp(0.5 * (l - r) / den, -0.5, 0.5) * step;
        }
    return {best, first.confidence};
}

namespace {

Skew skew_of_gray(const cv::Mat& g, double max_deg) {
    cv::Mat gx, gy;
    cv::Scharr(g, gx, CV_32F, 1, 0);   // Scharr: jämnare i alla riktningar än Sobel 3×3
    cv::Scharr(g, gy, CV_32F, 0, 1);

    // histogram över kantriktningen modulo 90° (0,05° per fack), viktat med kantstyrkan i kvadrat
    constexpr double step = 0.05;
    const int bins = (int)std::lround(90 / step);
    std::vector<double> hist(bins, 0.0);
    double total = 0;
    std::vector<float> mags;
    mags.reserve(g.total());
    for (int y = 0; y < g.rows; ++y)
        for (int x = 0; x < g.cols; ++x) mags.push_back(std::hypot(gx.at<float>(y, x), gy.at<float>(y, x)));
    std::vector<float> sorted = mags;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 9 / 10, sorted.end());
    float thr = std::max(8.0f, sorted[sorted.size() * 9 / 10]);   // de starkaste 10 % av kanterna
    for (int y = 0, i = 0; y < g.rows; ++y)
        for (int x = 0; x < g.cols; ++x, ++i) {
            float m = mags[i];
            if (m < thr) continue;
            double a = std::atan2((double)gy.at<float>(y, x), (double)gx.at<float>(y, x)) * 180 / CV_PI;   // gradientens riktning
            a = std::fmod(a + 360.0 * 4, 90.0);                                                                // 0..90
            int b = std::min(bins - 1, (int)(a / step));
            hist[b] += (double)m * m;
            total += (double)m * m;
        }
    if (total <= 0) return {};
    // utjämna (cirkulärt), hitta toppen inom ±max_deg
    std::vector<double> smooth(bins, 0.0);
    const int w = (int)std::lround(0.3 / step);
    for (int i = 0; i < bins; ++i)
        for (int k = -w; k <= w; ++k) smooth[i] += hist[((i + k) % bins + bins) % bins] * std::exp(-0.5 * (k * k) / (w * w / 4.0));
    int best = -1;
    double best_v = -1;
    for (int i = 0; i < bins; ++i) {
        double a = i * step + step / 2;
        double d = a > 45 ? a - 90 : a;   // -45..45
        if (std::abs(d) > max_deg + 1e-9) continue;
        if (smooth[i] > best_v) best_v = smooth[i], best = i;
    }
    if (best < 0) return {};
    double l = smooth[(best - 1 + bins) % bins], r = smooth[(best + 1) % bins], den = l - 2 * best_v + r;
    double off = std::abs(den) > 1e-12 ? 0.5 * (l - r) / den : 0;
    double a = (best + 0.5 + off) * step;
    double d = a > 45 ? a - 90 : a;
    double mean = 0;
    for (double v : smooth) mean += v;
    mean /= bins;
    Skew s;
    // gradientens vinkel i bildkoordinater (y nedåt) är medurs; innehållets vridning moturs är det motsatta
    s.angle_deg = -d;
    s.confidence = std::clamp(1.0 - mean / best_v, 0.0, 1.0);
    return s;
}

}  // namespace

cv::Mat deskew(const cv::Mat& src, double max_deg, Skew* found, bool expand, const cv::Scalar& fill) {
    Skew s = estimate_skew(src, max_deg);
    if (found) *found = s;
    if (std::abs(s.angle_deg) < 0.01) return src.clone();
    return rotate(src, -s.angle_deg, expand, fill);
}

}  // namespace pc::imgproc
