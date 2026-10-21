// Testfall: felkatalog (mutationer med facit), tryck- och scansimulering, och generatorerna
// för scannade ark (make_case) och PDF-mot-PDF (doccases).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/testgen/label.hpp"
#include "printcmp/testgen/rng.hpp"

namespace pc::testgen {

// ---------------------------------------------------------------- felkatalog

extern const std::vector<std::string> STRUCT_ANY;
std::vector<std::string> mutation_names();             // katalogen (utan riktade)
// n mutationer på olika element utan överlappande facitrutor. Returnerar (ny spec, facitfel).
std::pair<json, json> mutate_label(json spec, Rng& rng, int n, const std::vector<std::string>& allowed = {});

// ---------------------------------------------------------------- scannade ark

struct CaseOptions {
    std::string template_name;
    uint64_t seed = 100;
    std::string difficulty = "normal";   // easy | normal | hard
    int cols = 2, rows = 4;
    std::vector<std::string> mutations;  // tom = hela katalogen
    std::optional<int> n_artwork, n_print;
    int dpi = 600;
    std::string scanner = "legacy";      // legacy | cis | ccd
    std::optional<double> fold_lift, fold_p;
    std::optional<double> rotation_deg;   // arkets rotation; annars slumpad inom svårighetens gräns
    std::optional<double> scale;          // arkets skala (1 = 100 %); annars nära 1
};
json make_case(const CaseOptions& o, const std::filesystem::path& out_dir);

// ---------------------------------------------------------------- PDF mot PDF

std::filesystem::path make_book_case(uint64_t seed, const std::filesystem::path& out, const std::string& profile,
                                     int n_mut = 9);
std::filesystem::path make_doc_label_case(uint64_t seed, const std::filesystem::path& out, const std::string& tpl,
                                          int n_mut = 3);

}  // namespace pc::testgen
