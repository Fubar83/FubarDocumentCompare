// Dekorfärger som inte trycks (stanslinje, lack, punktskrift) och en sanerad PDF utan dem.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "printcmp/pdf/document.hpp"

namespace pc::pdf {

struct SpotPath {
    std::string role, spot_name, paint;
    std::vector<std::vector<Pt>> polygons;   // PDF-punkter
};

struct SpotAnalysis {
    std::vector<SpotPath> paths;
    std::vector<std::string> spot_names;
    Bytes sanitized_pdf;
    std::vector<std::vector<Pt>> polygons(const std::string& role) const;
};

// Ordnade regler (roll -> reguljärt uttryck); första träff gäller.
using SpotRules = std::vector<std::pair<std::string, std::string>>;
SpotRules default_spot_rules();
SpotAnalysis analyze_spots(const Bytes& pdf, int page, const SpotRules& rules);
double polygon_area(const std::vector<Pt>& p);

}  // namespace pc::pdf
