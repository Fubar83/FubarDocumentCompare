// Diff: struktur mot grannskapets min/max + SSIM, färg med CIEDE2000.
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "printcmp/image/image.hpp"
#include "printcmp/scan/diff.hpp"

namespace pc::scan {

cv::Mat DiffMaps::ref_L() const {
    cv::Mat c;
    cv::extractChannel(ref_lab, c, 0);
    return c;
}
cv::Mat DiffMaps::smp_L() const {
    cv::Mat c;
    cv::extractChannel(smp_lab, c, 0);
    return c;
}

cv::Mat range_dist(const cv::Mat& a, const cv::Mat& b, int radius) {
    if (radius <= 0) return cv::abs(a - b);
    cv::Mat k = image::ellipse_kernel(radius), bmax, bmin;
    cv::dilate(b, bmax, k);
    cv::erode(b, bmin, k);
    cv::Mat d1 = cv::max(a - bmax, 0.0), d2 = cv::max(bmin - a, 0.0);
    return d1 + d2;
}

DiffMaps compute_diff(const cv::Mat& ref_f, const cv::Mat& smp_f, const cv::Mat& valid, const cv::Mat& varnish, double ppm,
                      const json& tol, const cv::Mat& text_mask) {
    DiffMaps M;
    M.ref_lab = image::rgb2lab(ref_f);
    M.smp_lab = image::rgb2lab(smp_f);
    cv::Mat rL = M.ref_L(), sL = M.smp_L();
    int radius = (int)pyround(tol["edge_radius_mm"].get<double>() * ppm);
    cv::Mat d1 = range_dist(sL, rL, radius), d2 = range_dist(rL, sL, radius);
    cv::GaussianBlur(cv::max(d1, d2), M.d_struct, cv::Size(), 0.7);
    M.ssim = image::ssim_map(rL, sL, 100.0);
    cv::Mat absdl;
    cv::GaussianBlur(sL - rL, absdl, cv::Size(), 1.0);
    absdl = cv::abs(absdl);
    double sdl = tol["struct_dl"], strong = tol["struct_dl_strong"], ssim_t = tol["ssim"];
    cv::Mat st = ((M.d_struct > sdl) & (M.ssim < 0.95)) | (M.d_struct > strong) |
                 ((M.ssim < ssim_t) & (absdl > 0.75 * sdl) & (M.d_struct > 0.5 * sdl));
    if (!text_mask.empty()) {
        // inom liten text räcker en mindre avvikelse (ett kort streck: 6 -> 8)
        st |= (text_mask > 0) & (M.d_struct > tol["struct_dl_text"].get<double>()) & (M.ssim < 0.95);
        M.text_mask = text_mask;
    }
    M.struct_mask = st & valid;
    M.struct_mask.setTo(1, M.struct_mask);   // 0/1

    int k = std::max(3, (int)pyround(tol["color_window_mm"].get<double>() * ppm) | 1);
    cv::Mat rb, sb;
    cv::blur(M.ref_lab, rb, cv::Size(k, k));
    cv::blur(M.smp_lab, sb, cv::Size(k, k));
    M.delta_e = image::delta_e_2000(rb, sb);
    cv::Mat gr, gs;
    cv::morphologyEx(rL, gr, cv::MORPH_GRADIENT, cv::Mat::ones(3, 3, CV_8U));
    cv::morphologyEx(sL, gs, cv::MORPH_GRADIENT, cv::Mat::ones(3, 3, CV_8U));
    cv::Mat edgy;
    cv::dilate((gr > 8) | (gs > 8), edgy, cv::Mat::ones(k, k, CV_8U));
    cv::Mat rbL, sbL;
    cv::extractChannel(rb, rbL, 0);
    cv::extractChannel(sb, sbL, 0);
    cv::Mat dark = (rbL < 25) & (sbL < 25);   // svart: instabil kulörton i scannern
    cv::Mat varn = varnish.empty() ? cv::Mat::zeros(valid.size(), CV_8U) : (varnish > 0);
    M.color_mask = (M.delta_e > tol["delta_e"].get<double>()) & (valid > 0) & ~edgy & ~varn & ~dark;
    M.color_mask.setTo(1, M.color_mask);
    return M;
}

std::vector<Blob> find_blobs(const DiffMaps& maps, double ppm, const json& tol) {
    std::vector<Blob> out;
    int merge = std::max(1, (int)pyround(tol["merge_mm"].get<double>() * ppm));
    for (int kk = 0; kk < 2; ++kk) {
        std::string kind = kk == 0 ? "struct" : "color";
        const cv::Mat& m = kk == 0 ? maps.struct_mask : maps.color_mask;
        int open_k = kk == 0 ? 2 : 3;
        double min_area = (kk == 0 ? tol["min_blob_mm2"].get<double>() : tol["min_color_blob_mm2"].get<double>()) * ppm * ppm;
        cv::Mat mm;
        cv::morphologyEx(m, mm, cv::MORPH_OPEN, cv::Mat::ones(open_k, open_k, CV_8U));
        if (kind == "color") {
            cv::Mat sd;
            cv::dilate(maps.struct_mask, sd, cv::Mat::ones(5, 5, CV_8U));
            mm.setTo(0, sd);
        }
        cv::Mat grouped, labels, stats, cents;
        cv::dilate(mm, grouped, image::ellipse_kernel(merge));
        int n = cv::connectedComponentsWithStats(grouped, labels, stats, cents, 8, CV_32S);
        for (int i = 1; i < n; ++i) {
            int x = stats.at<int>(i, cv::CC_STAT_LEFT), y = stats.at<int>(i, cv::CC_STAT_TOP);
            int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            cv::Rect r(x, y, w, h);
            cv::Mat sub = (labels(r) == i) & (mm(r) > 0);
            int area = cv::countNonZero(sub);
            if (area == 0) continue;
            if (area < min_area) {
                // liten avvikelse i text godtas om den är tydlig och ligger i textytan
                if (!(kind == "struct" && !maps.text_mask.empty() && area >= tol["min_text_blob_mm2"].get<double>() * ppm * ppm))
                    continue;
                cv::Mat tm = maps.text_mask(r) > 0;
                double frac = (double)cv::countNonZero(tm & sub) / area;
                double mx;
                cv::minMaxLoc(maps.d_struct(r), nullptr, &mx, nullptr, nullptr, sub);
                if (frac < 0.5 || mx < tol["struct_dl"].get<double>()) continue;
            }
            cv::Rect tight = cv::boundingRect(sub);
            tight.x += x;
            tight.y += y;
            cv::Mat tsub = sub(cv::Rect(tight.x - x, tight.y - y, tight.width, tight.height)).clone();
            Blob b;
            b.bbox = tight;
            b.area_px = area;
            b.mask = tsub;
            b.kind_hint = kind;
            double mx;
            cv::minMaxLoc(maps.d_struct(tight), nullptr, &mx, nullptr, nullptr, tsub);
            b.max_dl = mx;
            b.mean_de = cv::mean(maps.delta_e(tight), tsub)[0];
            cv::Mat dl = maps.smp_L()(tight) - maps.ref_L()(tight);
            b.mean_signed_dl = cv::mean(dl, tsub)[0];
            out.push_back(std::move(b));
        }
    }
    return out;
}

}  // namespace pc::scan
