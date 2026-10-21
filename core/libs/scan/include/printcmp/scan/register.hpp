// Passning: global (faskorrelation + RANSAC) och lokal (verifierat optiskt flöde).
#pragma once

#include <cmath>
#include <opencv2/core.hpp>
#include <tuple>

#include "printcmp/core/core.hpp"

namespace pc::scan {

struct Registration {
    Mat3 H;
    cv::Mat flow;          // CV_32FC2 i ref-px, tom = inget lokalt flöde
    cv::Mat confidence;    // CV_32F
    double rms_px = NAN;
    int n_tiles = 0;
};

cv::Mat prepare_gray(const cv::Mat& rgb, double ppm);
Registration register_images(const cv::Mat& ref_gs, const cv::Mat& smp_gs, const Mat3& H0, const cv::Mat& valid,
                             double ppm, const json& tol, int seed, bool local);
cv::Mat resample(const cv::Mat& sample, const Mat3& H, const cv::Mat& flow, cv::Size shape, int interp);
// Global passning: faskorrelation i rutor (tile px) och RANSAC, iterativt. Returnerar (H, rms px, antal rutor).
std::tuple<Mat3, double, int> refine_global(const cv::Mat& ref_gray, const cv::Mat& sample_gray, const Mat3& H0,
                                            const cv::Mat& valid, int tile, int iterations = 3, int seed = 1);

}  // namespace pc::scan
