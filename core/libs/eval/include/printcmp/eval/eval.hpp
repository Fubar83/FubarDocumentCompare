// Utvärdering mot facit.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::eval {

// Scannade ark: result.json mot truth.json (printcmp.truth/1).
json evaluate_scan_case(const json& result, const json& truth);
json aggregate_scan(const std::vector<json>& cases);

// PDF mot PDF: result.json mot truth.json (printcmp.doctruth/1).
json evaluate_doc_case(const json& result, const json& truth);
json aggregate_doc(const std::vector<json>& cases);

// Minsta kostnad för en rektangulär matris (rader <-> kolumner), som scipy linear_sum_assignment.
std::vector<std::pair<int, int>> linear_sum_assignment(const std::vector<std::vector<double>>& cost);

}  // namespace pc::eval
