// Datakontroll: förväntade uppgifter (.txt/.csv/.json) mot textlager och streckkoder.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "printcmp/doc/doctext.hpp"

namespace pc::compare {

struct DataCheckOptions {
    std::string path;
    std::optional<int> row;
    std::string row_match;     // "kolumn=värde"
    bool ignore_case = false;
    std::string source = "sample";
};

struct DataItem {
    std::string field, value;
    bool regex = false, barcode = false;
};
std::vector<std::vector<DataItem>> load_expected(const std::string& path);
std::vector<json> codes_from_pdf(const std::string& path, int max_pages = 20, double dpi = 400);
json data_check(const std::vector<std::vector<DataItem>>& variants, const doc::Document* doc, const std::vector<json>& codes,
                const DataCheckOptions& opt);
json run_data_check(const DataCheckOptions& opt, const doc::Document* doc, std::vector<json> codes);

}  // namespace pc::compare
