// Skillnadskartor (struktur, SSIM, CIEDE2000) och sammanhängande avvikelser.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::scan {

struct DiffMaps {
    cv::Mat ref_lab, smp_lab, d_struct, ssim, delta_e, struct_mask, color_mask, text_mask;
    cv::Mat ref_L() const;
    cv::Mat smp_L() const;
};

struct Blob {
    cv::Rect bbox;
    int area_px = 0;
    cv::Mat mask;      // CV_8U, bbox-utsnitt
    std::string kind_hint;
    double max_dl = 0, mean_de = 0, mean_signed_dl = 0;
};

cv::Mat range_dist(const cv::Mat& a, const cv::Mat& b, int radius);
DiffMaps compute_diff(const cv::Mat& ref_f, const cv::Mat& smp_f, const cv::Mat& valid, const cv::Mat& varnish,
                      double ppm, const json& tol, const cv::Mat& text_mask);
std::vector<Blob> find_blobs(const DiffMaps& maps, double ppm, const json& tol);

}  // namespace pc::scan
