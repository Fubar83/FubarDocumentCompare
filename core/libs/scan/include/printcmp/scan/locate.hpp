// Hitta etiketterna på arket (rotation och skevhet) och ordna dem i rader och kolumner.
#pragma once

#include <optional>
#include <opencv2/core.hpp>
#include <utility>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::scan {

struct Instance {
    int index = 0;
    Pt center;
    double angle_deg = 0, score = 0;
    int row = -1, col = -1;
    Mat3 H0;   // ref px -> prov px
};

Mat3 similarity_H(cv::Size ref, double ref_dpi, double sample_dpi, Pt center, double angle_deg);
std::pair<std::vector<Instance>, json> locate(const cv::Mat& ref_rgb, const cv::Mat& inside, double ref_dpi,
                                              const cv::Mat& sheet_rgb, double sheet_dpi, const std::vector<int>& rotations,
                                              double max_skew_deg, std::optional<int> expected_count,
                                              double coarse_dpi = 50.0);
std::vector<Instance> assign_grid(std::vector<Instance> insts, double angle_deg, double row_tol_px);

}  // namespace pc::scan
