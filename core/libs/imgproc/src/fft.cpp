#include "printcmp/imgproc/fft.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <opencv2/imgproc.hpp>

namespace pc::fft {

namespace {

cv::Mat gray_f32(const cv::Mat& image) {
    cv::Mat g = image;
    if (image.channels() == 3) cv::cvtColor(image, g, cv::COLOR_RGB2GRAY);
    else if (image.channels() == 4) cv::cvtColor(image, g, cv::COLOR_RGBA2GRAY);
    cv::Mat f;
    g.convertTo(f, CV_32F);
    return f;
}

// frekvens (cykler/px) för index i i en DFT med längden n
inline double freq(int i, int n) { return (i <= n / 2 ? i : i - n) / double(n); }

double lowpass(double r, double c, Shape shape, int order) {
    if (c <= 0) return 0;
    switch (shape) {
        case Shape::Ideal: return r <= c ? 1.0 : 0.0;
        case Shape::Gaussian: return std::exp(-(r * r) / (2 * c * c));
        default: return 1.0 / (1.0 + std::pow(r / c, 2 * order));
    }
}

cv::Mat as_complex_gain(const cv::Mat& gain) {
    cv::Mat g2;
    cv::merge(std::vector<cv::Mat>{gain, gain}, g2);
    return g2;
}

// Filtrera en enkanalig flyttalsbild med en förstärkning (funktion av spektrumets storlek).
template <class GainFn>
cv::Mat filter_channel(const cv::Mat& ch_f32, GainFn gain_for) {
    Spectrum s = forward(ch_f32, Padding::Reflect);
    cv::Mat g = gain_for(s.complex.size());
    s.complex = s.complex.mul(as_complex_gain(g));
    return inverse(s);
}

template <class GainFn>
cv::Mat filter_image(const cv::Mat& image, GainFn gain_for) {
    std::vector<cv::Mat> ch;
    cv::split(image, ch);
    for (auto& c : ch) {
        cv::Mat f;
        c.convertTo(f, CV_32F);
        filter_channel(f, gain_for).convertTo(c, image.depth());
    }
    cv::Mat out;
    cv::merge(ch, out);
    return out;
}

}  // namespace

Spectrum forward(const cv::Mat& image, Padding pad) {
    cv::Mat f = gray_f32(image);
    int h = cv::getOptimalDFTSize(f.rows), w = cv::getOptimalDFTSize(f.cols);
    cv::Mat padded;
    cv::copyMakeBorder(f, padded, 0, h - f.rows, 0, w - f.cols, pad == Padding::Reflect ? cv::BORDER_REFLECT_101 : cv::BORDER_CONSTANT,
                       cv::Scalar::all(0));
    Spectrum s;
    s.image_size = f.size();
    cv::dft(padded, s.complex, cv::DFT_COMPLEX_OUTPUT);
    return s;
}

cv::Mat inverse(const Spectrum& s) {
    cv::Mat real;
    cv::idft(s.complex, real, cv::DFT_SCALE | cv::DFT_REAL_OUTPUT);
    return real(cv::Rect(0, 0, s.image_size.width, s.image_size.height)).clone();
}

void shift(cv::Mat& m) {
    int cx = m.cols / 2, cy = m.rows / 2;
    cv::Mat tmp;
    cv::Mat q0(m, cv::Rect(0, 0, cx, cy)), q1(m, cv::Rect(cx, 0, m.cols - cx, cy));
    cv::Mat q2(m, cv::Rect(0, cy, cx, m.rows - cy)), q3(m, cv::Rect(cx, cy, m.cols - cx, m.rows - cy));
    // för udda storlekar: rotera med cv::Mat-kopior i stället för att byta kvadranter
    if (m.cols % 2 || m.rows % 2) {
        cv::Mat out(m.size(), m.type());
        int sx = cx, sy = cy;   // som numpy.fft.fftshift: index i flyttas till (i + n/2) mod n
        for (int y = 0; y < m.rows; ++y)
            for (int x = 0; x < m.cols; ++x) {
                int ny = (y + sy) % m.rows, nx = (x + sx) % m.cols;
                std::memcpy(out.ptr(ny) + nx * m.elemSize(), m.ptr(y) + x * m.elemSize(), m.elemSize());
            }
        m = out;
        return;
    }
    q0.copyTo(tmp);
    q3.copyTo(q0);
    tmp.copyTo(q3);
    q1.copyTo(tmp);
    q2.copyTo(q1);
    tmp.copyTo(q2);
}

cv::Mat magnitude(const Spectrum& s, bool log, bool centered) {
    std::vector<cv::Mat> p;
    cv::split(s.complex, p);
    cv::Mat mag;
    cv::magnitude(p[0], p[1], mag);
    if (log) {
        mag += 1;
        cv::log(mag, mag);
    }
    if (centered) shift(mag);
    return mag;
}

cv::Mat phase(const Spectrum& s, bool centered) {
    std::vector<cv::Mat> p;
    cv::split(s.complex, p);
    cv::Mat ph;
    cv::phase(p[0], p[1], ph);
    if (centered) shift(ph);
    return ph;
}

// ------------------------------------------------------------------ frekvensfilter

cv::Mat frequency_mask(cv::Size size, const FrequencySpec& spec) {
    cv::Mat m(size, CV_32F);
    for (int y = 0; y < size.height; ++y) {
        double fy = freq(y, size.height);
        float* row = m.ptr<float>(y);
        for (int x = 0; x < size.width; ++x) {
            double fx = freq(x, size.width), r = std::sqrt(fx * fx + fy * fy);
            double lo = lowpass(r, spec.cutoff, spec.shape, spec.order), g;
            switch (spec.pass) {
                case Pass::Low: g = lo; break;
                case Pass::High: g = 1 - lo; break;
                case Pass::Band: g = lowpass(r, spec.cutoff2, spec.shape, spec.order) * (1 - lo); break;
                default: g = 1 - lowpass(r, spec.cutoff2, spec.shape, spec.order) * (1 - lo); break;
            }
            row[x] = float(g);
        }
    }
    return m;
}

cv::Mat filter_frequency(const cv::Mat& image, const FrequencySpec& spec) {
    return filter_image(image, [&](cv::Size size) { return frequency_mask(size, spec); });
}

imgproc::FilterPtr frequency_filter(const FrequencySpec& spec, int margin) {
    return imgproc::custom([spec](const cv::Mat& src) { return filter_frequency(src, spec); }, margin, "frequency");
}

// ------------------------------------------------------------------ periodiska mönster

std::vector<Peak> spectral_peaks(const cv::Mat& image, int n, double min_freq) {
    cv::Mat f = gray_f32(image);
    f -= cv::mean(f)[0];
    cv::Mat win;
    cv::createHanningWindow(win, f.size(), CV_32F);
    f = f.mul(win);
    Spectrum s = forward(f, Padding::Zero);
    cv::Mat mag = magnitude(s, false, false);
    int H = mag.rows, W = mag.cols;
    std::vector<Peak> cand;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double fy = freq(y, H), fx = freq(x, W);
            if (!(fy > 0 || (fy == 0 && fx > 0))) continue;   // ena halvan av det symmetriska spektrumet
            double r = std::hypot(fx, fy);
            if (r < min_freq) continue;
            float v = mag.at<float>(y, x);
            bool is_max = true;
            for (int dy = -1; dy <= 1 && is_max; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    if (mag.at<float>((y + dy + H) % H, (x + dx + W) % W) > v) {
                        is_max = false;
                        break;
                    }
                }
            if (!is_max) continue;
            Peak p;
            p.freq = {fx, fy};
            p.magnitude = v;
            p.period_px = 1.0 / r;
            p.angle_deg = std::atan2(fy, fx) * 180.0 / CV_PI;
            cand.push_back(p);
        }
    std::sort(cand.begin(), cand.end(), [](const Peak& a, const Peak& b) { return a.magnitude > b.magnitude; });
    if (int(cand.size()) > n) cand.resize(n);
    return cand;
}

cv::Mat notch(const cv::Mat& image, const std::vector<Peak>& peaks, double radius) {
    auto gain_for = [&](cv::Size size) {
        cv::Mat g(size, CV_32F);
        double r2 = 2 * radius * radius;
        for (int y = 0; y < size.height; ++y) {
            double fy = freq(y, size.height);
            float* row = g.ptr<float>(y);
            for (int x = 0; x < size.width; ++x) {
                double fx = freq(x, size.width), v = 1.0;
                for (auto& p : peaks)
                    for (int sgn : {1, -1}) {
                        double dx = fx - sgn * p.freq.x, dy = fy - sgn * p.freq.y;
                        v *= 1.0 - std::exp(-(dx * dx + dy * dy) / r2);
                    }
                row[x] = float(v);
            }
        }
        return g;
    };
    return filter_image(image, gain_for);
}

cv::Mat descreen(const cv::Mat& image, int n, double min_freq, double radius) {
    std::vector<cv::Mat> ch;
    cv::split(image, ch);
    for (auto& c : ch) c = notch(c, spectral_peaks(c, n, min_freq), radius);
    cv::Mat out;
    cv::merge(ch, out);
    return out;
}

double dominant_period(const cv::Mat& image, double min_freq) {
    auto peaks = spectral_peaks(image, 8, min_freq);
    if (peaks.empty()) return 0;
    // tydlig topp: klart starkare än de övriga lokala maxima
    double rest = 0;
    for (size_t i = 1; i < peaks.size(); ++i) rest += peaks[i].magnitude;
    rest /= std::max<size_t>(1, peaks.size() - 1);
    return peaks.size() == 1 || peaks[0].magnitude > 1.5 * rest ? peaks[0].period_px : 0.0;
}

// ------------------------------------------------------------------ faltning och korrelation

cv::Mat convolve(const cv::Mat& image, const cv::Mat& kernel_in) {
    cv::Mat k;
    kernel_in.convertTo(k, CV_32F);
    int kh = k.rows, kw = k.cols, ay = kh / 2, ax = kw / 2;
    std::vector<cv::Mat> ch, out;
    cv::split(image, ch);
    for (auto& c : ch) {
        cv::Mat f, P;
        c.convertTo(f, CV_32F);
        cv::copyMakeBorder(f, P, ay, kh - 1 - ay, ax, kw - 1 - ax, cv::BORDER_REFLECT_101);
        int N = cv::getOptimalDFTSize(P.rows), M = cv::getOptimalDFTSize(P.cols);
        cv::Mat Pp = cv::Mat::zeros(N, M, CV_32F), Kp = cv::Mat::zeros(N, M, CV_32F);
        P.copyTo(Pp(cv::Rect(0, 0, P.cols, P.rows)));
        k.copyTo(Kp(cv::Rect(0, 0, kw, kh)));
        cv::Mat FP, FK, FC, C;
        cv::dft(Pp, FP, cv::DFT_COMPLEX_OUTPUT);
        cv::dft(Kp, FK, cv::DFT_COMPLEX_OUTPUT);
        cv::mulSpectrums(FP, FK, FC, 0, true);   // korrelation: konjugerat
        cv::idft(FC, C, cv::DFT_SCALE | cv::DFT_REAL_OUTPUT);
        out.push_back(C(cv::Rect(0, 0, f.cols, f.rows)).clone());
    }
    cv::Mat dst;
    cv::merge(out, dst);
    return dst;
}

Shift phase_correlate(const cv::Mat& a_in, const cv::Mat& b_in, bool window) {
    cv::Mat a = gray_f32(a_in), b = gray_f32(b_in);
    cv::Size size(std::min(a.cols, b.cols), std::min(a.rows, b.rows));
    a = a(cv::Rect(0, 0, size.width, size.height));
    b = b(cv::Rect(0, 0, size.width, size.height));
    cv::Mat win;
    if (window) cv::createHanningWindow(win, size, CV_32F);
    Shift s;
    s.offset = cv::phaseCorrelate(a, b, win, &s.response);
    return s;
}

}  // namespace pc::fft
