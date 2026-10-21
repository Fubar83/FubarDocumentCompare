// Trycksimulering: raster, punktförstoring, passningsfel, tryckfel. Allt slumpas från en given Rng.
#include "printcmp/testgen/sim.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pc::testgen::sim {

const cv::Vec3f PAPER(0.955f, 0.950f, 0.925f);

json PrintParams::to_json() const {
    const char* n = "CMYK";
    json g, d, m;
    for (int i = 0; i < 4; ++i) {
        std::string k(1, n[i]);
        g[k] = dot_gain[i];
        d[k] = density[i];
        m[k] = {misreg_mm[i][0], misreg_mm[i][1]};
    }
    return {{"halftone", halftone}, {"lpcm", lpcm}, {"dot_gain", g}, {"density", d}, {"misreg_mm", m}};
}

namespace {

const double ANGLES[4] = {15.0, 75.0, 0.0, 45.0};   // C, M, Y, K

// plåtar ur RGB (float): C, M, Y, K
std::array<cv::Mat, 4> rgb_to_cmyk(const cv::Mat& rgb) {
    std::array<cv::Mat, 4> p;
    for (auto& m : p) m.create(rgb.size(), CV_32F);
    cv::parallel_for_(cv::Range(0, rgb.rows), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y) {
            const auto* s = rgb.ptr<cv::Vec3f>(y);
            float *c = p[0].ptr<float>(y), *m = p[1].ptr<float>(y), *ye = p[2].ptr<float>(y), *k = p[3].ptr<float>(y);
            for (int x = 0; x < rgb.cols; ++x) {
                float kk = 1.f - std::max({s[x][0], s[x][1], s[x][2]});
                float d = std::max(1.f - kk, 1e-4f);
                c[x] = (1.f - s[x][0] - kk) / d;
                m[x] = (1.f - s[x][1] - kk) / d;
                ye[x] = (1.f - s[x][2] - kk) / d;
                k[x] = kk;
            }
        }
    });
    return p;
}

void halftone(cv::Mat& t, double dpi, double lpcm, double angle_deg, double ph0, double ph1) {
    double period = dpi / (lpcm * 2.54), a = angle_deg * M_PI / 180, ca = std::cos(a), sa = std::sin(a);
    cv::parallel_for_(cv::Range(0, t.rows), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y) {
            float* row = t.ptr<float>(y);
            for (int x = 0; x < t.cols; ++x) {
                double u = (x * ca + y * sa) / period + ph0, v = (-x * sa + y * ca) / period + ph1;
                double spot = 0.04 + 0.92 * (0.5 + 0.25 * (std::cos(2 * M_PI * u) + std::cos(2 * M_PI * v)));
                row[x] = float(std::clamp((row[x] - spot) * 12 + 0.5, 0.0, 1.0));
            }
        }
    });
}

void clip01(cv::Mat& m) {
    cv::max(m, 0, m);
    cv::min(m, 1, m);
}

}  // namespace

cv::Mat print_label(const cv::Mat& rgb_hi_u8, double dpi_hi, double dpi_out, const PrintParams& p, Rng& rng) {
    cv::Mat rgb;
    rgb_hi_u8.convertTo(rgb, CV_32FC3, 1.0 / 255);
    auto plates = rgb_to_cmyk(rgb);
    for (int n = 0; n < 4; ++n) {
        cv::Mat& t = plates[n];
        // punktförstoring: t + 4g·t(1−t)
        t *= p.density[n];
        cv::Mat one_minus = 1 - t;
        t = t + 4 * p.dot_gain[n] * t.mul(one_minus);
        clip01(t);
        double dx = p.misreg_mm[n][0] * dpi_hi / 25.4, dy = p.misreg_mm[n][1] * dpi_hi / 25.4;
        if (std::fabs(dx) > 1e-3 || std::fabs(dy) > 1e-3) {
            cv::Matx23f M(1, 0, float(dx), 0, 1, float(dy));
            cv::warpAffine(t, t, M, t.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        }
        if (p.halftone) {
            double ph0 = rng.random(), ph1 = rng.random();
            halftone(t, dpi_hi, p.lpcm, ANGLES[n], ph0, ph1);
        }
    }
    cv::Mat img(rgb.size(), CV_32FC3);
    cv::parallel_for_(cv::Range(0, img.rows), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y) {
            const float *c = plates[0].ptr<float>(y), *m = plates[1].ptr<float>(y), *ye = plates[2].ptr<float>(y),
                        *k = plates[3].ptr<float>(y);
            auto* o = img.ptr<cv::Vec3f>(y);
            for (int x = 0; x < img.cols; ++x) {
                float kk = 1 - k[x];
                o[x] = cv::Vec3f((1 - c[x]) * kk * PAPER[0], (1 - m[x]) * kk * PAPER[1], (1 - ye[x]) * kk * PAPER[2]);
            }
        }
    });
    if (p.halftone) {
        // Pressen antas vara kalibrerad mot provtrycket: medelfärgen (lågpass) ska ligga nära PDF:ens
        // färg med en kontrollerad tonavvikelse, medan rastret, passningsfel och kantstruktur behålls.
        cv::Vec3f gam(float(1 + p.dot_gain[0] * 0.6), float(1 + p.dot_gain[1] * 0.6), float(1 + p.dot_gain[2] * 0.6));
        cv::Mat target(rgb.size(), CV_32FC3);
        cv::parallel_for_(cv::Range(0, rgb.rows), [&](const cv::Range& r) {
            for (int y = r.start; y < r.end; ++y) {
                const auto* s = rgb.ptr<cv::Vec3f>(y);
                auto* o = target.ptr<cv::Vec3f>(y);
                for (int x = 0; x < rgb.cols; ++x)
                    for (int ch = 0; ch < 3; ++ch)
                        o[x][ch] = PAPER[ch] * std::pow(std::clamp(s[x][ch], 1e-4f, 1.f), gam[ch]);
            }
        });
        double sig = 1.5 * dpi_hi / (p.lpcm * 2.54);
        cv::Mat bt, bi, corr;
        cv::GaussianBlur(target, bt, cv::Size(0, 0), sig);
        cv::GaussianBlur(img, bi, cv::Size(0, 0), sig);
        cv::max(bi, 1e-3, bi);
        cv::divide(bt, bi, corr);
        cv::max(corr, 0.2, corr);
        cv::min(corr, 5.0, corr);
        img = img.mul(corr);
        clip01(img);
        // Multiplikativ korrektion kan inte lyfta kanaler där rastret täcker helt (värde 0), så
        // restfelet i lågpass läggs till additivt; två varv tar hand om det som klipps vid 0/1.
        for (int k = 0; k < 2; ++k) {
            cv::GaussianBlur(img, bi, cv::Size(0, 0), sig);
            img = img + (bt - bi);
            clip01(img);
        }
    }
    double s = dpi_out / dpi_hi;
    cv::Mat out;
    cv::resize(img, out, cv::Size(), s, s, cv::INTER_AREA);
    return out;
}

std::optional<PrintDefect> add_print_defect(cv::Mat& img, double ppm, const std::string& kind, Rng& rng,
                                            const std::array<double, 4>& region, const std::vector<Rect>& avoid) {
    int h = img.rows, w = img.cols;
    for (int attempt = 0; attempt < 30; ++attempt) {
        double rx, ry;
        if (kind == "spot") {
            rx = rng.uniform(0.12, 0.8);
            ry = rng.uniform(0.12, 0.8);
        } else if (kind == "streak") {
            rx = rng.uniform(0.05, 0.15);
            ry = rng.uniform(2.0, 9.0);
        } else {   // hickey
            rx = ry = rng.uniform(0.35, 0.9);
        }
        double cx = rng.uniform(region[0] + rx + 0.5, region[2] - rx - 0.5);
        double cy = rng.uniform(region[1] + ry + 0.5, region[3] - ry - 0.5);
        Rect box{cx - rx, cy - ry, 2 * rx, 2 * ry};
        bool clash = false;
        for (auto& a : avoid)
            clash |= !(box[0] > a[0] + a[2] + 1 || a[0] > box[0] + box[2] + 1 || box[1] > a[1] + a[3] + 1 || a[1] > box[1] + box[3] + 1);
        if (clash) continue;
        cv::Mat m = cv::Mat::zeros(h, w, CV_32F);
        cv::Point center(int(cx * ppm * 16), int(cy * ppm * 16));
        cv::Size axes(std::max(1, int(rx * ppm * 16)), std::max(1, int(ry * ppm * 16)));
        if (kind == "hickey") {
            cv::ellipse(m, center, axes, 0, 0, 360, 1.0, std::max(16, int(0.12 * ppm * 16)), cv::LINE_AA, 4);
            cv::circle(m, center, std::max(1, int(0.12 * ppm * 16)), 1.0, -1, cv::LINE_AA, 4);
        } else {
            cv::ellipse(m, center, axes, 0, 0, 360, 1.0, -1, cv::LINE_AA, 4);
        }
        cv::GaussianBlur(m, m, cv::Size(0, 0), 0.6);
        bool dark = kind != "spot" || rng.random() < 0.6;
        if (!dark) {
            // en ljus fläck syns bara på tryckt yta – på vitt papper finns inget att ta bort
            cv::Mat sel = m > 0.5;
            double bg = cv::countNonZero(sel) ? cv::mean(img, sel)[0] / 3 + cv::mean(img, sel)[1] / 3 + cv::mean(img, sel)[2] / 3 : 1.0;
            if (bg > 0.8) dark = true;
        }
        cv::Vec3f col;
        double strength;
        std::string polarity;
        if (dark) {
            col = rng.random() < 0.7 ? cv::Vec3f(0.18f, 0.17f, 0.2f) : cv::Vec3f(0.1f, 0.25f, 0.55f);
            strength = rng.uniform(0.55, 0.95);
            polarity = "dark";
        } else {
            col = PAPER;
            strength = rng.uniform(0.6, 0.95);
            polarity = "light";
        }
        for (int y = 0; y < h; ++y) {
            const float* mr = m.ptr<float>(y);
            auto* ir = img.ptr<cv::Vec3f>(y);
            for (int x = 0; x < w; ++x) {
                float a = float(strength) * mr[x];
                if (a > 0) ir[x] = ir[x] * (1 - a) + col * a;
            }
        }
        return PrintDefect{box, polarity, strength};
    }
    return std::nullopt;
}

cv::Mat rounded_rect_mask(int w, int h, double r_px) {
    cv::Mat m = cv::Mat::zeros(h, w, CV_8U);
    int r = int(r_px);
    cv::rectangle(m, cv::Point(r, 0), cv::Point(w - 1 - r, h - 1), 1, -1);
    cv::rectangle(m, cv::Point(0, r), cv::Point(w - 1, h - 1 - r), 1, -1);
    for (auto c : {cv::Point(r, r), cv::Point(w - 1 - r, r), cv::Point(r, h - 1 - r), cv::Point(w - 1 - r, h - 1 - r)})
        cv::circle(m, c, r, 1, -1, cv::LINE_AA);
    return m;
}

cv::Mat fold_v1(const cv::Mat& img, cv::Point2d p0, cv::Point2d p1, double w_px, double strength, double amp_px) {
    int h = img.rows, wd = img.cols;
    double dx = p1.x - p0.x, dy = p1.y - p0.y, n = std::hypot(dx, dy), nx = -dy / n, ny = dx / n;
    cv::Mat mx(h, wd, CV_32F), my(h, wd, CV_32F), shade(h, wd, CV_32F);
    cv::parallel_for_(cv::Range(0, h), [&](const cv::Range& r) {
        for (int y = r.start; y < r.end; ++y)
            for (int x = 0; x < wd; ++x) {
                double s = (x - p0.x) * nx + (y - p0.y) * ny, d = amp_px * std::tanh(s / (3 * w_px));
                mx.at<float>(y, x) = float(x - d * nx);
                my.at<float>(y, x) = float(y - d * ny);
                double sg = s > 0 ? 1 : s < 0 ? -1 : 0;
                shade.at<float>(y, x) = float(1 - strength * std::exp(-(s / w_px) * (s / w_px)) * sg -
                                              0.35 * strength * std::exp(-(s / (0.4 * w_px)) * (s / (0.4 * w_px))));
            }
    });
    cv::Mat out;
    cv::remap(img, out, mx, my, cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    std::vector<cv::Mat> ch;
    cv::split(out, ch);
    for (auto& c : ch) c = c.mul(shade);
    cv::merge(ch, out);
    clip01(out);
    return out;
}

cv::Mat scanner_v1(const cv::Mat& img, Rng& rng, std::array<double, 2> mtf, double noise_sigma, double color_jitter) {
    cv::Mat out;
    cv::GaussianBlur(img, out, cv::Size(0, 0), mtf[0], mtf[1]);
    cv::Matx33f M = cv::Matx33f::eye();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) M(i, j) += float(rng.normal(0, color_jitter));
    cv::transform(out, out, M);
    clip01(out);
    cv::Mat noise(out.size(), CV_32FC3);
    cv::RNG cvr(rng.next());
    cvr.fill(noise, cv::RNG::NORMAL, 0.0, noise_sigma);
    out = out * 255 + noise + cv::Scalar::all(0.5);
    cv::Mat u8;
    out.convertTo(u8, CV_8UC3);   // mättar vid 0/255; +0.5 ovan ger avrundning som astype efter clip
    return u8;
}

void emboss(cv::Mat& img, const std::vector<cv::Point2d>& pts, double r_px, double strength, cv::Point2d light) {
    if (pts.empty()) return;
    int h = img.rows, w = img.cols, pad = int(std::ceil(2 * r_px)) + 2;
    double mnx = 1e18, mny = 1e18, mxx = -1e18, mxy = -1e18;
    for (auto& p : pts) mnx = std::min(mnx, p.x), mny = std::min(mny, p.y), mxx = std::max(mxx, p.x), mxy = std::max(mxy, p.y);
    int x0 = std::max(0, int(mnx) - pad), y0 = std::max(0, int(mny) - pad);
    int x1 = std::min(w, int(mxx) + pad), y1 = std::min(h, int(mxy) + pad);
    if (x1 <= x0 || y1 <= y0) return;
    cv::Mat H = cv::Mat::zeros(y1 - y0, x1 - x0, CV_32F);
    for (auto& p : pts) {
        int ax = std::max(x0, int(p.x - r_px) - 1), bx = std::min(x1, int(p.x + r_px) + 2);
        int ay = std::max(y0, int(p.y - r_px) - 1), by = std::min(y1, int(p.y + r_px) + 2);
        for (int y = ay; y < by; ++y)
            for (int x = ax; x < bx; ++x) {
                double d = std::hypot(x - p.x, y - p.y) / r_px;
                if (d < 1) {
                    double c = std::cos(M_PI / 2 * d);
                    H.at<float>(y - y0, x - x0) += float(c * c);
                }
            }
    }
    // np.gradient: centrala differenser, ensidiga vid kanten
    auto grad = [&](int y, int x, bool along_y) {
        int n = along_y ? H.rows : H.cols, i = along_y ? y : x;
        auto at = [&](int k) { return along_y ? H.at<float>(k, x) : H.at<float>(y, k); };
        if (n < 2) return 0.0;
        if (i == 0) return double(at(1) - at(0));
        if (i == n - 1) return double(at(n - 1) - at(n - 2));
        return (at(i + 1) - at(i - 1)) / 2.0;
    };
    double norm = M_PI / (2 * r_px);
    for (int y = 0; y < H.rows; ++y) {
        auto* row = img.ptr<cv::Vec3f>(y + y0);
        for (int x = 0; x < H.cols; ++x) {
            double shade = (grad(y, x, false) * light.x + grad(y, x, true) * light.y) / norm;
            float f = float(1 + strength * std::clamp(shade, -1.5, 1.5));
            auto& v = row[x + x0];
            for (int c = 0; c < 3; ++c) v[c] = std::clamp(v[c] * f, 0.f, 1.f);
        }
    }
}

}  // namespace pc::testgen::sim
