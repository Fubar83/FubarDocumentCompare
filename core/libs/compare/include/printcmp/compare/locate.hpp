// Hitta etiketterna på ett ark utan att jämföra dem: läge, rotation (även 90/180/270°), rad och kolumn.
#pragma once

#include <array>
#include <opencv2/core.hpp>
#include <optional>
#include <vector>

#include "printcmp/compare/reference.hpp"

namespace pc::compare {

struct LocatedLabel {
    int index = 0, row = -1, col = -1;
    double angle_deg = 0;          // etikettens vridning på arket, moturs (0–360)
    double score = 0;              // träffens poäng 0..1
    Pt center;                     // arkets px
    std::array<Pt, 4> corners;     // originalets hörn på arket: övre vänster, övre höger, nedre höger, nedre vänster
};

struct LocatedSheet {
    double sheet_angle_deg = 0;    // arkets snedhet, −45..45° (median över etiketterna, utan 90°-steg)
    std::vector<LocatedLabel> labels;
};

struct LocateOptions {
    std::vector<int> rotations = {0, 90, 180, 270};
    double max_skew_deg = 4.0;
    std::optional<int> expected_count;
    bool refine = true;            // finjustera vinkeln per etikett (rätas upp och mäts igen)
};

// sheet_rgb: CV_8UC3 (RGB) i sheet_dpi.
LocatedSheet locate_labels(const ReferenceModel& reference, const cv::Mat& sheet_rgb, double sheet_dpi, const LocateOptions& opt = {});

}  // namespace pc::compare
