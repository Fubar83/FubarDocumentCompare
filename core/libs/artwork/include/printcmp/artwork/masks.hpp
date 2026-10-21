// Masker: utanför stanslinjen, variabel data, lack och punktskrift.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/pdf/document.hpp"
#include "printcmp/pdf/spots.hpp"

namespace pc::artwork {

struct UserMask {
    std::string reason;
    double x, y, w, h;   // mm
};

struct Masks {
    cv::Mat valid, inside, varnish, variable, braille;   // CV_8U 0/1
    bool has_dieline = false;
};

Masks build_masks(cv::Size size, const pdf::Geometry& geom, const pdf::SpotAnalysis& spots,
                  const std::vector<UserMask>& user, double erode_mm, const cv::Mat& braille_mask);

}  // namespace pc::artwork
