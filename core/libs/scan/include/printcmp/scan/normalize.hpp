// Normalisering av provet: rasterdämpning, skärpematchning, belysning och färgavbildning.
#pragma once

#include <optional>
#include <opencv2/core.hpp>
#include <vector>

namespace pc::scan {

std::optional<double> estimate_screen_period_px(const cv::Mat& sample_gray, const cv::Mat& ref_gray, const cv::Mat& valid,
                                                int n = 128);
double estimate_extra_blur(const cv::Mat& ref_f, const cv::Mat& smp_f, const cv::Mat& valid);
cv::Mat descreen(const cv::Mat& img_f, double sigma_px);
cv::Mat flatten_illumination(const cv::Mat& sample_f, const cv::Mat& ref_f, const cv::Mat& valid, double ppm,
                             double sigma_mm = 8.0);
struct ColorModel {
    cv::Mat B;   // 10x3 CV_64F
    std::vector<std::vector<double>> M() const;   // 3x4 (affin del), för rapporten
};
ColorModel fit_color(const cv::Mat& sample_f, const cv::Mat& ref_f, const cv::Mat& valid, int iters = 6);
cv::Mat apply_color(const cv::Mat& sample_f, const ColorModel& m);

}  // namespace pc::scan
