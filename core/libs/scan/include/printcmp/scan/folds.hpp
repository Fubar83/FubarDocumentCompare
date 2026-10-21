// Veck i en etikett: långa raka residualband, förfining och zonens bredd.
#pragma once

#include <opencv2/core.hpp>
#include <utility>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::scan {

struct Fold {
    Pt p0, p1;
    double band_px = 0;
};

std::vector<Fold> detect_folds(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm, double band_mm);
Fold refine_fold(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm, const Fold& prior,
                 double search_mm = 4.0);
struct ResidualMap {
    cv::Mat mag, flat;
    double t = 0;
    int ds = 1;
};
ResidualMap residual_map(const cv::Mat& ref_L, const cv::Mat& smp_L, const cv::Mat& valid, double ppm);
std::pair<double, int> fold_support(const ResidualMap& rm, const Fold& f);
double estimate_halfwidth(const cv::Mat& d_struct, const cv::Mat& dL, const cv::Mat& valid, double ppm, const Fold& f,
                          double struct_thr, double max_mm, const cv::Mat& ref_L, double shift_px = 1.0);
void draw_extended_band(cv::Mat& img, const Fold& f);
double point_line_dist(Pt p, Pt a, Pt b);
double angle_diff(Pt a0, Pt a1, Pt b0, Pt b1);

}  // namespace pc::scan
