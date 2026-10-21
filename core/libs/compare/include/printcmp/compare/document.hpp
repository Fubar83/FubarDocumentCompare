// Dokumentjämförelse: manus mot bok (omflödat) eller två versioner (fast layout).
#pragma once

#include <optional>
#include <string>

#include "printcmp/compare/datacheck.hpp"
#include "printcmp/doc/doctext.hpp"

namespace pc::compare {

struct DocumentOptions {
    std::string layout = "reflow";        // reflow | fixed
    bool furniture_ignore = true;
    bool typography_normalize = true;
    bool ignore_case = false;
    int ref_first = 0, ref_last = 0, smp_first = 0, smp_last = 0;
    int min_move_words = 3;
    double move_tol_mm = 0.3;
    std::optional<DataCheckOptions> data_check;
};

json compare_documents(const doc::Document& ref, const doc::Document& smp, const DocumentOptions& opt);
json run_document_job(const std::string& ref_path, const std::string& smp_path, const DocumentOptions& opt,
                      const JobContext& ctx = {});

}  // namespace pc::compare
