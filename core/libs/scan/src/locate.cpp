// Hitta alla etiketter på arket och ordna dem i rader och kolumner.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <set>

#include "printcmp/scan/locate.hpp"

namespace pc::scan {

namespace {

std::pair<cv::Mat, cv::Mat> rotate_with_canvas(const cv::Mat& img, double angle, double border) {
    double cx = img.cols / 2.0, cy = img.rows / 2.0;
    cv::Mat M = cv::getRotationMatrix2D(cv::Point2f((float)cx, (float)cy), angle, 1.0);
    double c = std::abs(M.at<double>(0, 0)), s = std::abs(M.at<double>(0, 1));
    int nw = (int)std::ceil(img.rows * s + img.cols * c), nh = (int)std::ceil(img.rows * c + img.cols * s);
    M.at<double>(0, 2) += nw / 2.0 - cx;
    M.at<double>(1, 2) += nh / 2.0 - cy;
    cv::Mat out;
    cv::warpAffine(img, out, M, cv::Size(nw, nh), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(border));
    return {out, M};
}

struct Peak { int x, y; double v; };

std::vector<Peak> nms_peaks(cv::Mat r, int min_dist, double thr, int max_n) {
    std::vector<Peak> peaks;
    while ((int)peaks.size() < max_n) {
        double mv;
        cv::Point ml;
        cv::minMaxLoc(r, nullptr, &mv, nullptr, &ml);
        if (mv < thr) break;
        peaks.push_back({ml.x, ml.y, mv});
        cv::circle(r, ml, min_dist, -2.0, -1);
    }
    return peaks;
}

}  // namespace

Mat3 similarity_H(cv::Size ref, double ref_dpi, double sample_dpi, Pt center, double angle_deg) {
    double s = sample_dpi / ref_dpi;
    cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(0, 0), angle_deg, s);
    Mat3 T1, R, T2;
    T1(0, 2) = -ref.width / 2.0;
    T1(1, 2) = -ref.height / 2.0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j) R(i, j) = M.at<double>(i, j);
    T2(0, 2) = center.x;
    T2(1, 2) = center.y;
    return T2 * R * T1;
}

std::pair<std::vector<Instance>, json> locate(const cv::Mat& ref_rgb, const cv::Mat& inside, double ref_dpi,
                                              const cv::Mat& sheet_rgb, double sheet_dpi, const std::vector<int>& rotations,
                                              double max_skew_deg, std::optional<int> expected_count, double coarse_dpi) {
    double sr = coarse_dpi / ref_dpi, ss = coarse_dpi / sheet_dpi;
    cv::Mat rg, sg, rs, ms, shs, ins_f;
    cv::cvtColor(ref_rgb, rg, cv::COLOR_RGB2GRAY);
    rg.convertTo(rg, CV_32F, 1.0 / 255);
    cv::cvtColor(sheet_rgb, sg, cv::COLOR_RGB2GRAY);
    sg.convertTo(sg, CV_32F, 1.0 / 255);
    cv::resize(rg, rs, cv::Size(), sr, sr, cv::INTER_AREA);
    inside.convertTo(ins_f, CV_32F);
    cv::resize(ins_f, ms, rs.size(), 0, 0, cv::INTER_AREA);
    cv::threshold(ms, ms, 0.99, 1.0, cv::THRESH_BINARY);
    cv::resize(sg, shs, cv::Size(), ss, ss, cv::INTER_AREA);
    cv::GaussianBlur(rs, rs, cv::Size(), 0.7);
    cv::GaussianBlur(shs, shs, cv::Size(), 0.7);
    int n_exp = expected_count.value_or(1);

    struct Score { double sc; cv::Mat resp; cv::Size tshape; };
    auto score_at = [&](double angle) -> Score {
        auto [t, M1] = rotate_with_canvas(rs, angle, 1.0);
        auto [m, M2] = rotate_with_canvas(ms, angle, 0.0);
        cv::threshold(m, m, 0.5, 1.0, cv::THRESH_BINARY);
        if (t.rows >= shs.rows || t.cols >= shs.cols) return {-1.0, cv::Mat(), t.size()};
        cv::Mat resp;
        cv::matchTemplate(shs, t, resp, cv::TM_CCOEFF_NORMED, m);
        cv::patchNaNs(resp, -1.0);            // som np.nan_to_num(nan=-1, posinf=-1, neginf=-1)
        resp.setTo(-1.0, resp > 1e29);
        resp.setTo(-1.0, resp < -1e29);
        int md = (int)(0.6 * std::min(t.rows, t.cols));
        auto peaks = nms_peaks(resp.clone(), md, 0.3, n_exp);
        if (peaks.empty()) return {-1.0, resp, t.size()};
        double sc = 0;
        for (auto& p : peaks) sc += p.v;
        return {sc / peaks.size(), resp, t.size()};
    };

    double best_sc = -2.0, best_a = 0.0, step = 1.0;
    for (int base : rotations)
        for (double a = -max_skew_deg; a <= max_skew_deg + 1e-6; a += step) {
            auto s = score_at(base + a);
            if (s.sc > best_sc) {
                best_sc = s.sc;
                best_a = base + a;
            }
        }
    double lo = best_a - step, hi = best_a + step;
    for (double a = lo; a <= hi + 1e-6; a += 0.2) {
        auto s = score_at(a);
        if (s.sc > best_sc) {
            best_sc = s.sc;
            best_a = a;
        }
    }
    json info = {{"sheet_angle_deg", best_a}, {"match_score", best_sc}};
    auto wrap = [](double a) { return std::fmod(std::fmod(a + 180, 360) + 360, 360) - 180; };
    int base_best = rotations.empty() ? 0 : rotations[0];
    for (int r : rotations)
        if (std::abs(wrap(best_a - r)) < std::abs(wrap(best_a - base_best))) base_best = r;
    double skew = wrap(best_a - base_best);
    struct Cand { double x, y, a, sc, size; };
    std::vector<Cand> cands;
    double top = -1.0;
    for (int base : rotations) {
        double a = base + skew;
        auto s = score_at(a);
        if (s.resp.empty()) continue;
        double mx;
        cv::minMaxLoc(s.resp, nullptr, &mx);
        top = std::max(top, mx);
        int md = (int)(0.6 * std::min(s.tshape.width, s.tshape.height));
        for (auto& p : nms_peaks(s.resp.clone(), md, 0.3, expected_count.value_or(200)))
            cands.push_back({(p.x + s.tshape.width / 2.0) / ss, (p.y + s.tshape.height / 2.0) / ss, a, p.v,
                             std::min(s.tshape.width, s.tshape.height) / ss});
    }
    if (cands.empty()) return {{}, info};
    double thr = std::max(0.35, 0.6 * top);
    std::vector<Cand> good;
    for (auto& c : cands)
        if (c.sc >= thr) good.push_back(c);
    std::stable_sort(good.begin(), good.end(), [](const Cand& a, const Cand& b) { return a.sc > b.sc; });
    int max_n = expected_count.value_or(200);
    std::vector<Cand> chosen;
    for (auto& c : good) {
        bool ok = true;
        for (auto& o : chosen)
            if (std::hypot(c.x - o.x, c.y - o.y) <= 0.6 * c.size) ok = false;
        if (ok) chosen.push_back(c);
        if ((int)chosen.size() >= max_n) break;
    }
    std::vector<Instance> out;
    for (size_t i = 0; i < chosen.size(); ++i) {
        Instance in;
        in.index = (int)i + 1;
        in.center = {chosen[i].x, chosen[i].y};
        in.angle_deg = chosen[i].a;
        in.score = chosen[i].sc;
        in.H0 = similarity_H(ref_rgb.size(), ref_dpi, sheet_dpi, in.center, chosen[i].a);
        out.push_back(in);
    }
    return {out, info};
}

std::vector<Instance> assign_grid(std::vector<Instance> insts, double angle_deg, double row_tol_px) {
    if (insts.empty()) return insts;
    double skew = std::fmod(std::fmod(angle_deg + 45.0, 90.0) + 90.0, 90.0) - 45.0;
    double a = skew * CV_PI / 180;
    std::vector<double> qx(insts.size()), qy(insts.size());
    for (size_t i = 0; i < insts.size(); ++i) {
        qx[i] = std::cos(a) * insts[i].center.x - std::sin(a) * insts[i].center.y;
        qy[i] = std::sin(a) * insts[i].center.x + std::cos(a) * insts[i].center.y;
    }
    std::vector<size_t> order(insts.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t i, size_t j) { return qy[i] < qy[j]; });
    std::vector<int> row_id(insts.size(), 0);
    int r = 0;
    for (size_t k = 1; k < order.size(); ++k) {
        if (qy[order[k]] - qy[order[k - 1]] > row_tol_px) ++r;
        row_id[order[k]] = r;
    }
    for (int rr = 0; rr <= r; ++rr) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < insts.size(); ++i)
            if (row_id[i] == rr) idx.push_back(i);
        std::stable_sort(idx.begin(), idx.end(), [&](size_t i, size_t j) { return qx[i] < qx[j]; });
        for (size_t c = 0; c < idx.size(); ++c) {
            insts[idx[c]].row = rr;
            insts[idx[c]].col = (int)c;
        }
    }
    std::stable_sort(insts.begin(), insts.end(), [](const Instance& a, const Instance& b) {
        return std::tie(a.row, a.col) < std::tie(b.row, b.col);
    });
    for (size_t n = 0; n < insts.size(); ++n) insts[n].index = (int)n + 1;
    return insts;
}

}  // namespace pc::scan
