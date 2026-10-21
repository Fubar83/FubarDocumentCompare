// Sammanställning över etiketterna: systematiska och lokala fel, osäkra zoner.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::scan {

// Slår ihop fynd som ligger på samma ställe i flera etiketter (systematiska fel).
std::vector<json> aggregate(std::vector<json> diffs, int n_instances, double ratio, double cluster_mm = 1.0);

inline json bbox_json_px(const cv::Rect& r) { return json::array({r.x, r.y, r.width, r.height}); }
json uncertain_zone(int instance, const cv::Rect& r, double ppm, const std::string& cause);

}  // namespace pc::scan
