// Privat för pc_compare: steg i etikettjobbet som inte hör till själva pipelinen.
#pragma once

#include <map>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/compare/label.hpp"
#include "printcmp/pdf/layers.hpp"
#include "printcmp/scan/locate.hpp"

namespace pc::compare::detail {

// Jämför etiketterna mot varandra: svaga fynd som syns i en etikett men inte i de andra.
std::vector<json> consensus(const std::vector<cv::Mat>& aligned_L, const std::vector<cv::Mat>& bands, const cv::Mat& valid,
                            const std::vector<json>& existing, const std::vector<scan::Instance>& insts, double ppm, const json& tol);
// För över textlagrets skillnader till de grafiska fynden (och lägger till "endast i textlagret").
std::vector<json> link_text(std::vector<json> diffs, const std::vector<json>& tdiffs, int instance);
// Sammanfattning (antal per typ och allvar, godkända/underkända etiketter).
json summary(const std::vector<json>& diffs, const json& insts);
// Lagerinformationen i result.json.
json layer_json(const pdf::LayerInfo& info, const std::map<std::string, bool>& decision);

}  // namespace pc::compare::detail
