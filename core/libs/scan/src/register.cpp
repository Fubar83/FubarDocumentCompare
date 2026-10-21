// Registrering: faskorrelation i tiles + RANSAC (affin), lokalt DIS-flöde
// regulariserat till ett nät och verifierat per område, och en enda omsampling.
#include <algorithm>
#include <cmath>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <opencv2/video/tracking.hpp>

#include "printcmp/image/image.hpp"
#include "printcmp/scan/register.hpp"

namespace pc::scan {

namespace {

cv::Mat to_cv(const Mat3& H) {
    cv::Mat m(3, 3, CV_64F);
    for (int i = 0; i < 9; ++i) m.at<double>(i / 3, i % 3) = H.m[i];
    return m;
}

cv::Mat warp_to_ref(const cv::Mat& sample, const Mat3& H, cv::Size shape, double border) {
    cv::Mat out;
    cv::warpPerspective(sample, out, to_cv(H), shape, cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT,
                        cv::Scalar::all(border));
    return out;
}


cv::Mat local_norm(const cv::Mat& img, cv::Size sz) {
    cv::Mat x, mu, var, sd;
    cv::resize(img, x, sz, 0, 0, cv::INTER_AREA);
    x.convertTo(x, CV_32F);
    cv::GaussianBlur(x, x, cv::Size(), 1.2);
    cv::GaussianBlur(x, mu, cv::Size(), 6);
    cv::Mat dev = x - mu;
    cv::GaussianBlur(dev.mul(dev), var, cv::Size(), 6);
    cv::max(var, 1e-6, var);
    cv::sqrt(var, sd);
    cv::Mat z = dev / (sd + 0.08);
    cv::Mat out;
    z.convertTo(out, CV_8U, 60, 128);   // klipps till 0..255
    return out;
}

std::pair<cv::Mat, cv::Mat> local_flow(const cv::Mat& ref_gray, const cv::Mat& warped_gray, const cv::Mat& valid, double ppm,
                                       double cell_mm, double max_shift_mm, double scale = 0.5) {
    int h = ref_gray.rows, w = ref_gray.cols;
    int sw = std::max(8, (int)(w * scale)), sh = std::max(8, (int)(h * scale));
    cv::Mat r8 = local_norm(ref_gray, {sw, sh}), w8 = local_norm(warped_gray, {sw, sh});
    auto dis = cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_MEDIUM);
    cv::Mat flow;
    dis->calc(r8, w8, flow);
    cv::Mat r8f, gx, gy, energy;
    r8.convertTo(r8f, CV_32F);
    cv::Sobel(r8f, gx, CV_32F, 1, 0, 3);
    cv::Sobel(r8f, gy, CV_32F, 0, 1, 3);
    cv::GaussianBlur(gx.mul(gx) + gy.mul(gy), energy, cv::Size(), 2.0);
    cv::Mat vs;
    cv::resize(valid, vs, {sw, sh}, 0, 0, cv::INTER_NEAREST);
    int cell = std::max(4, (int)pyround(cell_mm * ppm * scale));
    int ny = (int)std::ceil(sh / (double)cell), nx = (int)std::ceil(sw / (double)cell);
    cv::Mat grid = cv::Mat::zeros(ny, nx, CV_32FC2), conf = cv::Mat::zeros(ny, nx, CV_32F);
    auto ev = image::masked_values(energy, vs);
    double e_ref = ev.empty() ? 1.0 : percentile(std::vector<double>(ev.begin(), ev.end()), 75);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            cv::Rect r(i * cell, j * cell, std::min(sw, (i + 1) * cell) - i * cell, std::min(sh, (j + 1) * cell) - j * cell);
            std::vector<double> fx, fy;
            int n = 0;
            for (int y = r.y; y < r.y + r.height; ++y)
                for (int x = r.x; x < r.x + r.width; ++x) {
                    if (vs.at<uchar>(y, x) && energy.at<float>(y, x) > 0.15 * e_ref) {
                        auto f = flow.at<cv::Vec2f>(y, x);
                        fx.push_back(f[0]);
                        fy.push_back(f[1]);
                        ++n;
                    }
                }
            if (n < 0.08 * r.area()) continue;
            grid.at<cv::Vec2f>(j, i) = cv::Vec2f((float)median(fx), (float)median(fy));
            conf.at<float>(j, i) = (float)std::min(1.0, 2.0 * n / r.area());
        }
    cv::Mat holes = conf <= 0;
    std::vector<cv::Mat> ch;
    cv::split(grid, ch);
    if (cv::countNonZero(holes) == holes.total()) {
        grid.setTo(0);
        cv::split(grid, ch);
    } else if (cv::countNonZero(holes)) {
        for (auto& c : ch) {
            cv::Mat o;
            cv::inpaint(c, holes, o, 2, cv::INPAINT_TELEA);
            c = o;
        }
    }
    if (ny >= 3 && nx >= 3)
        for (auto& c : ch) cv::medianBlur(c, c, 3);
    cv::merge(ch, grid);
    cv::Mat smooth;
    cv::GaussianBlur(grid, smooth, cv::Size(), 0.8);
    double lim = max_shift_mm * ppm * scale;
    smooth.forEach<cv::Vec2f>([&](cv::Vec2f& v, const int*) {
        double m = std::hypot(v[0], v[1]);
        if (m > lim) v *= (float)(lim / std::max(m, 1e-6));
    });
    cv::Mat full;
    cv::resize(smooth, full, {w, h}, 0, 0, cv::INTER_CUBIC);
    full /= scale;
    return {full, cv::Mat::ones(h, w, CV_32F)};
}

std::pair<cv::Mat, cv::Mat> build_maps(const Mat3& H, const cv::Mat& flow, cv::Size shape) {
    cv::Mat mx(shape, CV_32F), my(shape, CV_32F);
    cv::parallel_for_(cv::Range(0, shape.height), [&](const cv::Range& rr) {
        for (int y = rr.start; y < rr.end; ++y)
            for (int x = 0; x < shape.width; ++x) {
                double xs = x, ys = y;
                if (!flow.empty()) {
                    auto f = flow.at<cv::Vec2f>(y, x);
                    xs += f[0];
                    ys += f[1];
                }
                double den = H.m[6] * xs + H.m[7] * ys + H.m[8];
                mx.at<float>(y, x) = (float)((H.m[0] * xs + H.m[1] * ys + H.m[2]) / den);
                my.at<float>(y, x) = (float)((H.m[3] * xs + H.m[4] * ys + H.m[5]) / den);
            }
    });
    return {mx, my};
}

cv::Mat grad_err(const cv::Mat& ref_gs, const cv::Mat& smp_gs, const Mat3& H, const cv::Mat& flow, double sigma) {
    auto [mx, my] = build_maps(H, flow, ref_gs.size());
    cv::Mat w;
    cv::remap(smp_gs, w, mx, my, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    cv::Mat gxr, gyr, gxw, gyw;
    cv::Sobel(ref_gs, gxr, CV_32F, 1, 0, 3);
    cv::Sobel(ref_gs, gyr, CV_32F, 0, 1, 3);
    cv::Sobel(w, gxw, CV_32F, 1, 0, 3);
    cv::Sobel(w, gyw, CV_32F, 0, 1, 3);
    cv::Mat e = cv::abs(gxr - gxw) + cv::abs(gyr - gyw), out;
    cv::GaussianBlur(e, out, cv::Size(), sigma);
    return out;
}

cv::Mat verify_flow(const cv::Mat& ref_gs, const cv::Mat& smp_gs, const Mat3& H, const cv::Mat& flow, double cell_px,
                    double min_gain = 0.05) {
    if (flow.empty()) return {};
    double sig = std::max(2.0, cell_px / 2);
    cv::Mat e0 = grad_err(ref_gs, smp_gs, H, cv::Mat(), sig);
    cv::Mat e1 = grad_err(ref_gs, smp_gs, H, flow, sig);
    cv::Mat keep;
    cv::Mat(e1 < (1 - min_gain) * e0).convertTo(keep, CV_32F, 1.0 / 255);
    cv::GaussianBlur(keep, keep, cv::Size(), sig);
    double mx;
    cv::minMaxLoc(keep, nullptr, &mx);
    if (mx < 0.2) return {};
    cv::Mat k2;
    cv::merge(std::vector<cv::Mat>{keep, keep}, k2);
    return flow.mul(k2);
}

}  // namespace

std::tuple<Mat3, double, int> refine_global(const cv::Mat& ref_gray, const cv::Mat& sample_gray, const Mat3& H0,
                                            const cv::Mat& valid, int tile, int iterations, int seed) {
    int h = ref_gray.rows, w = ref_gray.cols;
    Mat3 H = H0;
    cv::Mat win;
    cv::createHanningWindow(win, cv::Size(tile, tile), CV_32F);
    double rms = NAN;
    int used = 0;
    cv::setRNGSeed(seed);
    for (int it = 0; it < iterations; ++it) {
        cv::Mat warped = warp_to_ref(sample_gray, H, ref_gray.size(), 1.0);
        std::vector<cv::Point2d> src, dst;
        int step = it > 0 ? tile / 2 : tile;
        for (int y0 = 0; y0 + tile <= h; y0 += step)
            for (int x0 = 0; x0 + tile <= w; x0 += step) {
                cv::Rect r(x0, y0, tile, tile);
                if (cv::mean(valid(r))[0] < 0.6) continue;
                cv::Scalar mu, sd;
                cv::meanStdDev(ref_gray(r), mu, sd);
                if (sd[0] < 0.04) continue;
                // OBS: phaseCorrelate skriver fönstret in i indata på plats – skicka kopior
                cv::Mat a = ref_gray(r).clone(), b = warped(r).clone();
                double resp = 0;
                cv::Point2d d = cv::phaseCorrelate(a, b, win, &resp);
                if (resp < 0.08 || std::abs(d.x) > tile / 4.0 || std::abs(d.y) > tile / 4.0) continue;
                cv::Point2d c(x0 + tile / 2.0, y0 + tile / 2.0);
                src.push_back(c);
                dst.push_back(c + d);
            }
        if (src.size() < 4) break;
        std::vector<cv::Point2d> dst_s;
        for (auto& p : dst) {
            Pt q = H.apply({p.x, p.y});
            dst_s.emplace_back(q.x, q.y);
        }
        cv::Mat inl, A;
        // planskanner: affin modell, inte homografi (ett veckat ark ger falska perspektivtermer)
        if (src.size() >= 6) A = cv::estimateAffine2D(src, dst_s, inl, cv::RANSAC, 1.5, 2000, 0.999);
        else A = cv::estimateAffinePartial2D(src, dst_s, inl, cv::RANSAC, 1.5);
        if (A.empty()) break;
        Mat3 Hn;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 3; ++j) Hn(i, j) = A.at<double>(i, j);
        H = Hn;
        double se = 0;
        used = 0;
        for (size_t k = 0; k < src.size(); ++k) {
            if (!inl.empty() && !inl.at<uchar>((int)k)) continue;
            Pt q = H.apply({src[k].x, src[k].y});
            se += (q.x - dst_s[k].x) * (q.x - dst_s[k].x) + (q.y - dst_s[k].y) * (q.y - dst_s[k].y);
            ++used;
        }
        rms = used ? std::sqrt(se / used) : NAN;
    }
    return {H, rms, used};
}

cv::Mat prepare_gray(const cv::Mat& rgb, double ppm) {
    cv::Mat g = image::gray_f32(rgb), out;
    cv::GaussianBlur(g, out, cv::Size(), std::max(0.8, 0.08 * ppm));
    return out;
}

Registration register_images(const cv::Mat& ref_gs, const cv::Mat& smp_gs, const Mat3& H0, const cv::Mat& valid,
                             double ppm, const json& tol, int seed, bool local) {
    int tile = (int)std::min(256.0, std::max(96.0, std::pow(2.0, (double)pyround(std::log2(8 * ppm)))));
    auto [H, rms, n] = refine_global(ref_gs, smp_gs, H0, valid, tile, 3, seed);
    Registration reg;
    reg.H = H;
    reg.rms_px = rms;
    reg.n_tiles = n;
    reg.confidence = cv::Mat::ones(ref_gs.size(), CV_32F);
    if (local) {
        cv::Mat warped = warp_to_ref(smp_gs, H, ref_gs.size(), 1.0);
        auto [flow, conf] = local_flow(ref_gs, warped, valid, ppm, tol["local_cell_mm"], tol["max_local_shift_mm"]);
        reg.confidence = conf;
        reg.flow = verify_flow(ref_gs, smp_gs, H, flow, tol["local_cell_mm"].get<double>() * ppm);
    }
    return reg;
}

cv::Mat resample(const cv::Mat& sample, const Mat3& H, const cv::Mat& flow, cv::Size shape, int interp) {
    auto [mx, my] = build_maps(H, flow, shape);
    cv::Mat out;
    cv::remap(sample, out, mx, my, interp, cv::BORDER_CONSTANT, cv::Scalar(255, 255, 255));
    return out;
}

}  // namespace pc::scan
