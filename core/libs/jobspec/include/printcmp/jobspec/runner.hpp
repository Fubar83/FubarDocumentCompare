// Kör en tolkad JobSpec: jämförelsen och därefter utdata (result.json, HTML-rapport, bildlager).
#pragma once

#include <string>

#include "printcmp/jobspec/jobspec.hpp"

namespace pc::jobspec {

struct RunResult {
    json result;            // samma innehåll som result.json
    std::string out_dir;    // UTF-8
    double seconds = 0;
};

// out_override (om satt) ersätter outputs.dir.
RunResult run(const JobSpec& spec, const std::string& out_override = "", const JobContext& ctx = {});

inline RunResult run_file(const std::string& path, const std::string& out_override = "", const JobContext& ctx = {}) {
    return run(load(path), out_override, ctx);
}

}  // namespace pc::jobspec
