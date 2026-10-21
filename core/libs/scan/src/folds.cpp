// Veck: långa raka residualband, förfining, stöd och zonbredd.
#include <algorithm>
#include <cmath>
#include <map>
#include <opencv2/imgproc.hpp>

#include "printcmp/image/image.hpp"
#include "printcmp/scan/folds.hpp"

namespace pc::scan {

namespace {

struct Prep {
    cv::Mat r, s, v, mag, edges;
    int ds;
};

Prep prepare(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm) {
    Prep p;
    p.ds = std::max(1, (int)pyround(ppm / 6));
    cv::Size sz(ref_L.cols / p.ds, ref_L.rows / p.ds);
    cv::resize(ref_L, p.r, sz, 0, 0, cv::INTER_AREA);
    cv::resize(smp_L, p.s, sz, 0, 0, cv::INTER_AREA);
    cv::resize(valid, p.v, sz, 0, 0, cv::INTER_NEAREST);
    cv::Mat res, lp;
    cv::GaussianBlur(p.s - p.r, res, cv::Size(), 1.0);
    cv::GaussianBlur(res, lp, cv::Size(), 12);
    p.mag = cv::abs(res - lp);
    cv::Mat r8;
    cv::Mat(p.r * 2.55).convertTo(r8, CV_8U);   // klipps till 0..255
    cv::Mat e;
    cv::Canny(r8, e, 30, 90);
    cv::dilate(e, p.edges, cv::Mat::ones(3, 3, CV_8U));
    return p;
}

double robust_sigma(const cv::Mat& mag, const cv::Mat& flat) {
    if (!cv::countNonZero(flat)) return 1.0;
    return 1.4826 * image::masked_median(mag, flat);
}

}  // namespace

double point_line_dist(Pt p, Pt a, Pt b) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / std::max(dx * dx + dy * dy, 1e-9);
    return std::hypot(a.x + t * dx - p.x, a.y + t * dy - p.y);
}

double angle_diff(Pt a0, Pt a1, Pt b0, Pt b1) {
    auto ang = [](Pt p, Pt q) {
        double a = std::atan2(q.y - p.y, q.x - p.x) * 180 / CV_PI;
        return std::fmod(std::fmod(a, 180) + 180, 180);
    };
    double d = std::abs(ang(a0, a1) - ang(b0, b1));
    return std::min(d, 180 - d);
}

void draw_extended_band(cv::Mat& img, const Fold& f) {
    double dx = f.p1.x - f.p0.x, dy = f.p1.y - f.p0.y, n = std::hypot(dx, dy);
    if (n == 0) n = 1;
    double ux = dx / n, uy = dy / n, L = 2.0 * (img.cols + img.rows);
    cv::line(img, cv::Point((int)(f.p0.x - ux * L), (int)(f.p0.y - uy * L)),
             cv::Point((int)(f.p1.x + ux * L), (int)(f.p1.y + uy * L)), 1, std::max(1, (int)(2 * f.band_px)));
}

std::vector<Fold> detect_folds(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm, double band_mm) {
    Prep P = prepare(ref_L, smp_L, valid, ppm);
    int ds = P.ds;
    cv::Mat flat = (P.v > 0) & ~P.edges;
    double sigma = robust_sigma(P.mag, flat);
    double t_cand = std::max(2.5, 4.0 * sigma), t_line = std::max(2.0, 3.0 * sigma);
    cv::Mat cand = (P.mag > t_cand) & flat;
    cv::morphologyEx(cand, cand, cv::MORPH_OPEN, cv::Mat::ones(2, 2, CV_8U));
    int H_ = cand.rows, W_ = cand.cols;
    int min_len = (int)(0.45 * std::min(H_, W_));
    if (min_len < 20) return {};
    std::vector<cv::Vec4i> lines;
    // skimage probabilistic_hough_line(threshold=10, line_length, line_gap) ~ HoughLinesP
    cv::HoughLinesP(cand, lines, 1, CV_PI / 180, 10, min_len, std::max(4, min_len / 6));
    std::vector<Fold> out;
    double bw = band_mm * ppm;
    std::vector<cv::Point> vpts;
    cv::findNonZero(P.v, vpts);
    cv::Rect vb = vpts.empty() ? cv::Rect(0, 0, W_, H_) : cv::boundingRect(vpts);
    double vbx0 = vb.x, vby0 = vb.y, vbx1 = vb.x + vb.width - 1, vby1 = vb.y + vb.height - 1;
    double edge_tol = 3.0 * ppm / ds;
    cv::Mat good = (P.mag > t_line) & flat;
    std::vector<cv::Point> gpts;
    cv::findNonZero(good, gpts);
    for (auto& ln : lines) {
        double x0 = ln[0], y0 = ln[1], x1 = ln[2], y1 = ln[3];
        int n = (int)std::hypot(x1 - x0, y1 - y0);
        if (n < min_len) continue;
        double ang = std::fmod(std::atan2(y1 - y0, x1 - x0) * 180 / CV_PI + 180, 180);
        if (std::min(ang, 180 - ang) < 5 && std::min(std::abs((y0 + y1) / 2 - vby0), std::abs((y0 + y1) / 2 - vby1)) < edge_tol) continue;
        if (std::abs(ang - 90) < 5 && std::min(std::abs((x0 + x1) / 2 - vbx0), std::abs((x0 + x1) / 2 - vbx1)) < edge_tol) continue;
        double dx = (x1 - x0) / n, dy = (y1 - y0) / n;
        int L = (int)std::hypot(W_, H_);
        std::vector<int> xs, ys;
        for (int t = -L; t < L; ++t) {
            int xi = (int)pyround(x0 + t * dx), yi = (int)pyround(y0 + t * dy);
            if (xi >= 0 && xi < W_ && yi >= 0 && yi < H_ && P.v.at<uchar>(yi, xi)) {
                xs.push_back(xi);
                ys.push_back(yi);
            }
        }
        if ((int)xs.size() < min_len) continue;
        int n_bright = 0, n_hit = 0;
        for (size_t k = 0; k < xs.size(); ++k) {
            float m = 0;
            for (int o = -2; o <= 2; ++o) {
                m = std::max(m, P.mag.at<float>(std::clamp(ys[k] + o, 0, H_ - 1), xs[k]));
                m = std::max(m, P.mag.at<float>(ys[k], std::clamp(xs[k] + o, 0, W_ - 1)));
            }
            bool bright = P.r.at<float>(ys[k], xs[k]) > 25 && !P.edges.at<uchar>(ys[k], xs[k]);
            if (bright) {
                ++n_bright;
                n_hit += m > t_line;
            }
        }
        if ((double)n_bright / xs.size() < 0.25) continue;   // längs tryckt yta: troligen ett riktigt fel
        if (n_bright == 0 || (double)n_hit / n_bright < 0.6) continue;
        // förfina linjen med PCA på residualpixlar nära den förlängda linjen
        if (gpts.size() > 50) {
            cv::Point2d a0(x0, y0), dv(dx, dy), nv(-dy, dx);
            bool ok = true;
            for (int it = 0; it < 3 && ok; ++it) {
                std::vector<cv::Point2d> nearby;
                for (auto& q : gpts) {
                    cv::Point2d d(q.x - a0.x, q.y - a0.y);
                    if (std::abs(d.dot(nv)) < 3.0) nearby.push_back(q);
                }
                if (nearby.size() < 30) break;
                cv::PCA pca(cv::Mat(nearby).reshape(1), cv::Mat(), cv::PCA::DATA_AS_ROW);
                a0 = {pca.mean.at<double>(0), pca.mean.at<double>(1)};
                dv = {pca.eigenvectors.at<double>(0, 0), pca.eigenvectors.at<double>(0, 1)};
                nv = {-dv.y, dv.x};
            }
            double pmin = 1e18, pmax = -1e18;
            int cnt = 0;
            for (auto& q : gpts) {
                cv::Point2d d(q.x - a0.x, q.y - a0.y);
                if (std::abs(d.dot(nv)) < 3.0) {
                    double pr = d.dot(dv);
                    pmin = std::min(pmin, pr);
                    pmax = std::max(pmax, pr);
                    ++cnt;
                }
            }
            if (cnt > 30) {
                x0 = a0.x + pmin * dv.x;
                y0 = a0.y + pmin * dv.y;
                x1 = a0.x + pmax * dv.x;
                y1 = a0.y + pmax * dv.y;
            }
        }
        Fold f{{x0 * ds, y0 * ds}, {x1 * ds, y1 * ds}, bw};
        bool merged = false;
        for (auto& g : out)
            if (point_line_dist(f.p0, g.p0, g.p1) < 2 * bw && angle_diff(f.p0, f.p1, g.p0, g.p1) < 5) merged = true;
        if (!merged) out.push_back(f);
    }
    return out;
}

Fold refine_fold(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm, const Fold& prior,
                 double search_mm) {
    Prep P = prepare(ref_L, smp_L, valid, ppm);
    int ds = P.ds;
    cv::Mat flat = (P.v > 0) & ~P.edges;
    double sigma = robust_sigma(P.mag, flat);
    double t_line = std::max(2.0, 3.0 * sigma);
    std::vector<cv::Point> pts;
    cv::findNonZero((P.mag > t_line) & flat, pts);
    if (pts.size() < 30) return prior;
    cv::Point2d a0(prior.p0.x / ds, prior.p0.y / ds);
    cv::Point2d d(prior.p1.x / ds - a0.x, prior.p1.y / ds - a0.y);
    d /= std::max(cv::norm(d), 1e-9);
    cv::Point2d n(-d.y, d.x);
    double lim = search_mm * ppm / ds;
    for (int it = 0; it < 4; ++it) {
        std::vector<cv::Point2d> nearby;
        for (auto& q : pts) {
            cv::Point2d v(q.x - a0.x, q.y - a0.y);
            if (std::abs(v.dot(n)) < (it == 0 ? lim : 3.0)) nearby.push_back(q);
        }
        if (nearby.size() < 30) return prior;
        cv::PCA pca(cv::Mat(nearby).reshape(1), cv::Mat(), cv::PCA::DATA_AS_ROW);
        cv::Point2d mu(pca.mean.at<double>(0), pca.mean.at<double>(1));
        cv::Point2d e(pca.eigenvectors.at<double>(0, 0), pca.eigenvectors.at<double>(0, 1));
        if (std::abs(e.dot(d)) < 0.995) {   // håll vinkeln nära den förväntade
            a0 = mu;
            continue;
        }
        d = e.dot(d) > 0 ? e : -e;
        n = {-d.y, d.x};
        a0 = mu;
    }
    double L = 2.0 * (ref_L.cols + ref_L.rows) / ds;
    return {{(a0.x - d.x * L) * ds, (a0.y - d.y * L) * ds}, {(a0.x + d.x * L) * ds, (a0.y + d.y * L) * ds}, prior.band_px};
}

ResidualMap residual_map(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm) {
    Prep P = prepare(ref_L, smp_L, valid, ppm);
    ResidualMap rm;
    rm.mag = P.mag;
    cv::Mat ve = (P.v > 0) & ~P.edges;
    rm.flat = ve & (P.r > 25);
    rm.t = std::max(2.0, 3.0 * robust_sigma(P.mag, ve));
    rm.ds = P.ds;
    return rm;
}

std::pair<double, int> fold_support(const ResidualMap& rm, const Fold& f) {
    int H_ = rm.mag.rows, W_ = rm.mag.cols;
    cv::Point2d a(f.p0.x / rm.ds, f.p0.y / rm.ds), b(f.p1.x / rm.ds, f.p1.y / rm.ds), d = b - a;
    double n = cv::norm(d);
    if (n < 1) return {0.0, 0};
    d /= n;
    cv::Point2d c(W_ / 2.0, H_ / 2.0);
    cv::Point2d foot = a + (c - a).dot(d) * d;
    int L = (int)std::hypot(W_, H_);
    int total = 0, hit = 0;
    for (int t = -L; t < L; ++t) {
        int xi = (int)pyround(foot.x + t * d.x), yi = (int)pyround(foot.y + t * d.y);
        if (xi < 0 || xi >= W_ || yi < 0 || yi >= H_ || !rm.flat.at<uchar>(yi, xi)) continue;
        float m = 0;
        for (int o = -2; o <= 2; ++o) {
            m = std::max(m, rm.mag.at<float>(std::clamp(yi + o, 0, H_ - 1), xi));
            m = std::max(m, rm.mag.at<float>(yi, std::clamp(xi + o, 0, W_ - 1)));
        }
        ++total;
        hit += m > rm.t;
    }
    return {total ? (double)hit / total : 0.0, total};
}

double estimate_halfwidth(const cv::Mat& d_struct, const cv::Mat& dL, const cv::Mat& valid, double ppm, const Fold& f,
                          double struct_thr, double max_mm, const cv::Mat& ref_L, double shift_px) {
    int ds = std::max(1, (int)pyround(ppm / 12));
    auto sub = [&](const cv::Mat& m) {
        cv::Mat o((m.rows + ds - 1) / ds, (m.cols + ds - 1) / ds, m.type());
        for (int y = 0; y < o.rows; ++y)
            for (int x = 0; x < o.cols; ++x) {
                if (m.type() == CV_8U) o.at<uchar>(y, x) = m.at<uchar>(y * ds, x * ds);
                else o.at<float>(y, x) = m.at<float>(y * ds, x * ds);
            }
        return o;
    };
    cv::Mat D = sub(d_struct), V = sub(valid), Ab;
    cv::GaussianBlur(dL, Ab, cv::Size(), 2);
    cv::Mat A = cv::abs(sub(Ab));
    int Hd = D.rows, Wd = D.cols;
    double dx = f.p1.x - f.p0.x, dy = f.p1.y - f.p0.y, n = std::hypot(dx, dy);
    if (n == 0) n = 1;
    double nx = -dy / n, ny = dx / n;
    cv::Mat S(Hd, Wd, CV_64F), T(Hd, Wd, CV_64F);
    for (int y = 0; y < Hd; ++y)
        for (int x = 0; x < Wd; ++x) {
            S.at<double>(y, x) = ((x * ds - f.p0.x) * nx + (y * ds - f.p0.y) * ny) / ppm;
            T.at<double>(y, x) = ((x * ds - f.p0.x) * (dx / n) + (y * ds - f.p0.y) * (dy / n)) / ppm;
        }
    const double step = 0.5;
    std::vector<double> farA;
    for (int y = 0; y < Hd; ++y)
        for (int x = 0; x < Wd; ++x)
            if (V.at<uchar>(y, x) && std::abs(S.at<double>(y, x)) > max_mm + 2) farA.push_back(A.at<float>(y, x));
    double noise_A = farA.size() > 200 ? percentile(farA, 75) : 2.0;

    std::map<std::pair<int, int>, double> shifts;
    double shift_thr = -1;
    if (!ref_L.empty()) {
        cv::Mat rLb, dLb;
        cv::GaussianBlur(ref_L, rLb, cv::Size(), 0.6 * ds);
        cv::GaussianBlur(dL, dLb, cv::Size(), 0.6 * ds);
        cv::Mat rL = sub(rLb), dLs = sub(dLb), gx, gy;
        cv::Sobel(rL, gx, CV_32F, 1, 0, 3);
        cv::Sobel(rL, gy, CV_32F, 0, 1, 3);
        gx /= 8;
        gy /= 8;
        double smax = 0;
        for (int y = 0; y < Hd; ++y)
            for (int x = 0; x < Wd; ++x)
                if (V.at<uchar>(y, x)) smax = std::max(smax, std::abs(S.at<double>(y, x)));
        int n_steps = (int)(smax / step) + 1;
        // grupper per (tecken, remsa, bit om 10 mm längs linjen)
        struct Acc { double xx = 0, xy = 0, yy = 0, xr = 0, yr = 0, rr = 0; int n = 0; std::vector<int> idx; };
        std::map<std::tuple<int, int, long>, Acc> groups;
        for (int y = 0; y < Hd; ++y)
            for (int x = 0; x < Wd; ++x) {
                if (!V.at<uchar>(y, x)) continue;
                double s = S.at<double>(y, x);
                int k = (int)std::floor(std::abs(s) / step);
                if (k >= n_steps) continue;
                int sign = s < 0 ? -1 : 1;
                long seg = (long)std::floor(T.at<double>(y, x) / 10.0);
                auto& g = groups[{sign, k, seg}];
                double Gx = gx.at<float>(y, x), Gy = gy.at<float>(y, x), r = dLs.at<float>(y, x);
                g.xx += Gx * Gx;
                g.xy += Gx * Gy;
                g.yy += Gy * Gy;
                g.xr += Gx * r;
                g.yr += Gy * r;
                g.rr += r * r;
                g.n++;
                g.idx.push_back(y * Wd + x);
            }
        std::map<std::pair<int, int>, std::vector<double>> every;
        for (auto& [key, g] : groups) {
            if (g.n < 30) continue;
            double tr = g.xx + g.yy, det = g.xx * g.yy - g.xy * g.xy;
            double lmin = tr / 2 - std::sqrt(std::max(0.0, tr * tr / 4 - det));
            if (lmin < 400.0) continue;
            double ux = -(g.yy * g.xr - g.xy * g.yr) / det, uy = -(g.xx * g.yr - g.xy * g.xr) / det;
            double mag = std::hypot(ux, uy);
            auto k2 = std::make_pair(std::get<0>(key), std::get<1>(key));
            every[k2].push_back(mag);
            // förskjutningen måste förklara resten (annars kan en strimma vidga zonen)
            double e1 = 0;
            for (int id : g.idx) {
                int y = id / Wd, x = id % Wd;
                double res = dLs.at<float>(y, x) + gx.at<float>(y, x) * ux + gy.at<float>(y, x) * uy;
                e1 += res * res;
            }
            if (g.rr <= 0 || 1 - e1 / g.rr < 0.4) continue;
            shifts[k2] = std::max(shifts.count(k2) ? shifts[k2] : 0.0, mag * ds);
        }
        std::vector<double> far_v;
        for (auto& [k2, vs] : every)
            if (k2.second * step > max_mm)
                for (double v : vs) far_v.push_back(v * ds);
        double floor = far_v.size() >= 5 ? percentile(far_v, 30) : 0.4;
        shift_thr = std::max(shift_px, 2.5 * floor);
    }
    double best = 0;
    for (int sign : {-1, 1}) {
        double reach = 0;
        int gap = 0;
        for (int k = 0; k < (int)(max_mm / step); ++k) {
            std::vector<double> dv, av;
            for (int y = 0; y < Hd; ++y)
                for (int x = 0; x < Wd; ++x) {
                    double s = sign * S.at<double>(y, x);
                    if (V.at<uchar>(y, x) && s >= k * step && s < (k + 1) * step) {
                        dv.push_back(D.at<float>(y, x));
                        av.push_back(A.at<float>(y, x));
                    }
                }
            if (dv.size() < 30) {
                if (++gap > 4) break;
                continue;
            }
            bool hot = percentile(dv, 97) > struct_thr || median(av) > std::max(3.0, 2 * noise_A);
            auto it = shifts.find({sign, k});
            if (shift_thr >= 0 && it != shifts.end() && it->second > shift_thr) hot = true;
            if (hot) {
                reach = (k + 1) * step;
                gap = 0;
            } else if (++gap > (reach > 0 ? 2 : 8)) {
                break;
            }
        }
        best = std::max(best, reach);
    }
    return best * ppm;
}

}  // namespace pc::scan
