// Normalisering efter registrering: raster, skärpa, belysning, färg.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <random>

#include "printcmp/core/numeric.hpp"
#include "printcmp/image/image.hpp"
#include "printcmp/scan/normalize.hpp"

namespace pc::scan {

std::optional<double> estimate_screen_period_px(const cv::Mat& sample_gray, const cv::Mat& ref_gray, const cv::Mat& valid,
                                                int n) {
    int h = ref_gray.rows, w = ref_gray.cols;
    if (std::min(h, w) < n) return std::nullopt;
    cv::Mat win(n, n, CV_32F), band(n, n, CV_8U), rmap(n, n, CV_64F);
    std::vector<double> hann(n);
    for (int i = 0; i < n; ++i) hann[i] = 0.5 - 0.5 * std::cos(2 * CV_PI * i / (n - 1));   // np.hanning
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            win.at<float>(y, x) = (float)(hann[y] * hann[x]);
            double r = std::hypot(y - n / 2, x - n / 2);
            rmap.at<double>(y, x) = r;
            band.at<uchar>(y, x) = (r > n / 14.0 && r < n / 2.3) ? 1 : 0;
        }
    std::vector<std::pair<double, double>> found;
    for (int y0 = 0; y0 + n <= h && found.size() < 6; y0 += n / 2)
        for (int x0 = 0; x0 + n <= w; x0 += n / 2) {
            cv::Rect r(x0, y0, n, n);
            if (cv::countNonZero(valid(r)) != n * n) continue;
            cv::Scalar mu, sd;
            cv::meanStdDev(ref_gray(r), mu, sd);
            if (sd[0] > 0.015 || !(0.08 < mu[0] && mu[0] < 0.9)) continue;
            cv::Mat tile;
            sample_gray(r).convertTo(tile, CV_32F);
            tile = (tile - cv::mean(tile)[0]).mul(win);
            cv::Mat planes[] = {cv::Mat_<float>(tile), cv::Mat::zeros(tile.size(), CV_32F)}, cx, mag;
            cv::merge(planes, 2, cx);
            cv::dft(cx, cx);
            cv::split(cx, planes);
            cv::magnitude(planes[0], planes[1], mag);
            // fftshift
            int c2 = n / 2;
            cv::Mat sh(n, n, CV_32F);
            for (int yy = 0; yy < n; ++yy)
                for (int xx = 0; xx < n; ++xx) sh.at<float>((yy + c2) % n, (xx + c2) % n) = mag.at<float>(yy, xx);
            double best = -1;
            int bi = 0;
            std::vector<double> inband;
            for (int i = 0; i < n * n; ++i) {
                double v = sh.at<float>(i / n, i % n) * band.at<uchar>(i / n, i % n);
                if (v > best) {
                    best = v;
                    bi = i;
                }
                if (band.at<uchar>(i / n, i % n)) inband.push_back(sh.at<float>(i / n, i % n));
            }
            double strength = best / (median(inband) + 1e-9);
            if (strength > 12) found.push_back({n / rmap.at<double>(bi / n, bi % n), strength});
            if (found.size() >= 6) break;
        }
    if (found.empty()) return std::nullopt;
    std::vector<double> p;
    for (auto& f : found) p.push_back(f.first);
    return median(p);
}

double estimate_extra_blur(const cv::Mat& ref_f, const cv::Mat& smp_f, const cv::Mat& valid) {
    cv::Mat gr = image::luma(ref_f), gs = image::luma(smp_f);
    cv::Mat grad, edge;
    cv::morphologyEx(gr, grad, cv::MORPH_GRADIENT, cv::Mat::ones(3, 3, CV_8U));
    edge = valid & (grad > 0.1);
    cv::dilate(edge, edge, cv::Mat::ones(5, 5, CV_8U));
    edge &= valid;
    if (cv::countNonZero(edge) < 500) return 0.0;
    auto norm = [&](const cv::Mat& lap) {
        auto v = image::masked_values(lap, edge);
        double m = 0, s = 0;
        for (float x : v) m += x;
        m /= v.size();
        for (float x : v) s += (x - m) * (x - m);
        s = std::sqrt(s / v.size()) + 1e-9;
        std::vector<double> out(v.size());
        for (size_t i = 0; i < v.size(); ++i) out[i] = (v[i] - m) / s;
        return out;
    };
    cv::Mat ls_;
    cv::Laplacian(gs, ls_, CV_32F, 3);
    auto ls = norm(ls_);
    double best = -2.0, best_s = 0.0;
    for (double s : {0.0, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.6, 2.0}) {
        cv::Mat b = gr, lr_;
        if (s > 0) cv::GaussianBlur(gr, b, cv::Size(), s);
        cv::Laplacian(b, lr_, CV_32F, 3);
        auto lr = norm(lr_);
        double ncc = 0;
        for (size_t i = 0; i < lr.size(); ++i) ncc += lr[i] * ls[i];
        ncc /= lr.size();
        if (ncc > best) {
            best = ncc;
            best_s = s;
        }
    }
    return best_s;
}

cv::Mat descreen(const cv::Mat& img_f, double sigma_px) {
    if (sigma_px <= 0.3) return img_f;
    cv::Mat out;
    cv::GaussianBlur(img_f, out, cv::Size(), sigma_px);
    return out;
}

cv::Mat flatten_illumination(const cv::Mat& sample_f, const cv::Mat& ref_f, const cv::Mat& valid, double ppm,
                             double sigma_mm) {
    int ds = std::max(1, (int)pyround(ppm * 0.5));
    cv::Mat ys = image::luma(sample_f), yr = image::luma(ref_f);
    int hh = (ys.rows + ds - 1) / ds, ww = (ys.cols + ds - 1) / ds;
    cv::Mat ratio(hh, ww, CV_32F);
    std::vector<double> finite;
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < ww; ++x) {
            float r = yr.at<float>(y * ds, x * ds);
            bool v = valid.at<uchar>(y * ds, x * ds) && r > 0.25f;
            float q = v ? ys.at<float>(y * ds, x * ds) / std::max(r, 1e-3f) : NAN;
            ratio.at<float>(y, x) = q;
            if (v) finite.push_back(q);
        }
    float fill = finite.empty() ? 1.0f : (float)median(finite);
    ratio.forEach<float>([&](float& v, const int*) { v = std::isnan(v) ? fill : std::clamp(v, 0.0f, 4.0f); });
    cv::Mat med, sm, full;
    cv::medianBlur(ratio, med, 5);
    cv::GaussianBlur(med, sm, cv::Size(), sigma_mm / 0.5 / 2);
    cv::resize(sm, full, sample_f.size(), 0, 0, cv::INTER_LINEAR);
    cv::min(cv::max(full, 0.5), 2.0, full);
    cv::Mat f3, out;
    cv::merge(std::vector<cv::Mat>{full, full, full}, f3);
    cv::divide(sample_f, f3, out);
    cv::min(cv::max(out, 0.0), 1.0, out);
    return out;
}

static void features(const float* p, double* f) {
    double r = p[0], g = p[1], b = p[2];
    f[0] = r; f[1] = g; f[2] = b; f[3] = 1;
    f[4] = r * r; f[5] = g * g; f[6] = b * b; f[7] = r * g; f[8] = r * b; f[9] = g * b;
}

std::vector<std::vector<double>> ColorModel::M() const {
    std::vector<std::vector<double>> m(3, std::vector<double>(4));
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 4; ++k) m[c][k] = B.at<double>(k, c);
    return m;
}

ColorModel fit_color(const cv::Mat& sample_f, const cv::Mat& ref_f, const cv::Mat& valid, int iters) {
    const int step = 3;
    cv::Mat g = image::luma(ref_f), gs = image::luma(sample_f), gr1, gr2;
    cv::Mat k = cv::Mat::ones(3, 3, CV_8U);
    cv::morphologyEx(g, gr1, cv::MORPH_GRADIENT, k);
    cv::morphologyEx(gs, gr2, cv::MORPH_GRADIENT, k);
    cv::Mat m = valid & (gr1 < 0.08) & (gr2 < 0.08);
    cv::Mat B0 = cv::Mat::zeros(10, 3, CV_64F);
    B0.at<double>(0, 0) = B0.at<double>(1, 1) = B0.at<double>(2, 2) = 1.0;
    std::vector<cv::Vec3f> S, R;
    for (int y = 0; y < m.rows; y += step)
        for (int x = 0; x < m.cols; x += step)
            if (m.at<uchar>(y, x)) {
                S.push_back(sample_f.at<cv::Vec3f>(y, x));
                R.push_back(ref_f.at<cv::Vec3f>(y, x));
            }
    if (S.size() < 200) return {B0};
    if (S.size() > 200000) {
        std::mt19937 rng(0);
        std::vector<size_t> idx(S.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::shuffle(idx.begin(), idx.end(), rng);
        idx.resize(200000);
        std::vector<cv::Vec3f> S2, R2;
        for (size_t i : idx) {
            S2.push_back(S[i]);
            R2.push_back(R[i]);
        }
        S.swap(S2);
        R.swap(R2);
    }
    // svärteankare: mörkaste percentilen i respektive bild mappas på varandra
    bool have_anchor = false;
    cv::Vec3d s_black, r_black;
    {
        std::vector<double> ls, lr;
        std::vector<cv::Vec3f> ps, pr;
        for (int y = 0; y < valid.rows; y += 2)
            for (int x = 0; x < valid.cols; x += 2)
                if (valid.at<uchar>(y, x)) {
                    ls.push_back(gs.at<float>(y, x));
                    lr.push_back(g.at<float>(y, x));
                    ps.push_back(sample_f.at<cv::Vec3f>(y, x));
                    pr.push_back(ref_f.at<cv::Vec3f>(y, x));
                }
        if (ls.size() > 1000) {
            double qs = percentile(ls, 0.3), qr = percentile(lr, 0.3);
            if (qr < 0.25) {
                cv::Vec3d a(0, 0, 0), b(0, 0, 0);
                int na = 0, nb = 0;
                for (size_t i = 0; i < ls.size(); ++i) {
                    if (ls[i] <= qs) {
                        a += cv::Vec3d(ps[i]);
                        ++na;
                    }
                    if (lr[i] <= qr) {
                        b += cv::Vec3d(pr[i]);
                        ++nb;
                    }
                }
                if (na && nb) {
                    s_black = a / na;
                    r_black = b / nb;
                    have_anchor = true;
                }
            }
        }
    }
    size_t N = S.size();
    cv::Mat X(N, 10, CV_64F), Y(N, 3, CV_64F);
    for (size_t i = 0; i < N; ++i) {
        features(&S[i][0], X.ptr<double>((int)i));
        for (int c = 0; c < 3; ++c) Y.at<double>((int)i, c) = R[i][c];
    }
    cv::Mat Xa(1, 10, CV_64F), ra(1, 3, CV_64F);
    if (have_anchor) {
        float sb[3] = {(float)s_black[0], (float)s_black[1], (float)s_black[2]};
        features(sb, Xa.ptr<double>(0));
        for (int c = 0; c < 3; ++c) ra.at<double>(0, c) = r_black[c];
    }
    std::vector<double> wts(N, 1.0);
    cv::Mat B = B0.clone();
    const double lamv[10] = {0.003, 0.003, 0.003, 0.003, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01};
    for (int it = 0; it < iters; ++it) {
        double n_eff = 0;
        for (double w : wts) n_eff += w;
        cv::Mat A = cv::Mat::zeros(10, 10, CV_64F), Bm = cv::Mat::zeros(10, 3, CV_64F);
        for (size_t i = 0; i < N; ++i) {
            const double* x = X.ptr<double>((int)i);
            double w = wts[i];
            for (int a = 0; a < 10; ++a) {
                double wa = w * x[a];
                for (int b = 0; b < 10; ++b) A.at<double>(a, b) += wa * x[b];
                for (int c = 0; c < 3; ++c) Bm.at<double>(a, c) += wa * Y.at<double>((int)i, c);
            }
        }
        for (int a = 0; a < 10; ++a) {
            A.at<double>(a, a) += lamv[a] * n_eff;
            for (int c = 0; c < 3; ++c) Bm.at<double>(a, c) += lamv[a] * n_eff * B0.at<double>(a, c);
        }
        if (have_anchor) {
            double wa = 0.10 * n_eff;
            A += wa * Xa.t() * Xa;
            Bm += wa * Xa.t() * ra;
        }
        cv::solve(A, Bm, B, cv::DECOMP_LU);
        cv::Mat P = X * B;
        std::vector<double> res(N);
        for (size_t i = 0; i < N; ++i) {
            double s = 0;
            for (int c = 0; c < 3; ++c) {
                double d = P.at<double>((int)i, c) - Y.at<double>((int)i, c);
                s += d * d;
            }
            res[i] = std::sqrt(s);
        }
        // Huber-gräns minst 0,06 (≈ ΔE 5–6), annars dominerar papperet
        double c = std::max(2.0 * median(res), 0.06);
        for (size_t i = 0; i < N; ++i) wts[i] = res[i] < c ? 1.0 : c / std::max(res[i], 1e-9);
    }
    return {B};
}

cv::Mat apply_color(const cv::Mat& sample_f, const ColorModel& m) {
    cv::Mat out(sample_f.size(), CV_32FC3);
    const cv::Mat& B = m.B;
    sample_f.forEach<cv::Vec3f>([&](const cv::Vec3f& p, const int* pos) {
        double f[10];
        features(&p[0], f);
        cv::Vec3f o;
        for (int c = 0; c < 3; ++c) {
            double s = 0;
            for (int k = 0; k < 10; ++k) s += f[k] * B.at<double>(k, c);
            o[c] = (float)std::clamp(s, 0.0, 1.0);
        }
        out.at<cv::Vec3f>(pos[0], pos[1]) = o;
    });
    return out;
}

}  // namespace pc::scan
