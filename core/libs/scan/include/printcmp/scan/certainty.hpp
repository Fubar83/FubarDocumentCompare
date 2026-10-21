// Säkerhetsgrad per fynd: nivå, poäng och skäl.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::scan {

extern const std::vector<std::string> LEVELS;   // confirmed, high, probable, possible
json assess(const json& d, int n_inst, const json& tol, const std::string& sample_kind,
            const std::map<int, json>& inst_folds, bool color_calibrated);

// json-hjälp

}  // namespace pc::scan
