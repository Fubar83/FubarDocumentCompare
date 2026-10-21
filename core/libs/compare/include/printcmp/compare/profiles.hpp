// Toleransprofiler (label_standard, label_strict, leaflet, pdf_vs_pdf) och kärnans version.
#pragma once

#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::compare {

extern const char* VERSION;

json resolve_profile(const std::string& name, const json& overrides);
std::vector<std::string> profile_names();

}  // namespace pc::compare
