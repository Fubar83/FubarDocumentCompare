// pc_jobspec: JobSpec (printcmp.jobspec/1) – ett jobb beskrivet i JSON. Tolkningen är skild från
// körningen (runner.hpp).
#pragma once

#include <filesystem>
#include <string>

#include "printcmp/compare/reference.hpp"
#include "printcmp/compare/label.hpp"
#include "printcmp/compare/document.hpp"

namespace pc::jobspec {

inline constexpr const char* SCHEMA = "printcmp.jobspec/1";

struct Outputs {
    std::filesystem::path dir;                 // absolut
    std::string json = "result.json";
    std::string report_html = "report.html";   // tom = ingen rapport
    bool layers = true;
    double layer_dpi = 200;
};

struct JobSpec {
    enum class Mode { Label, Document } mode = Mode::Label;
    std::string reference_path, sample_path;   // absoluta, UTF-8
    compare::ReferenceOptions reference;       // etikettläge
    compare::SampleSource sample;              // etikettläge
    compare::CompareOptions label;             // etikettläge
    compare::DocumentOptions document;         // dokumentläge
    Outputs outputs;
};

// Relativa sökvägar tolkas mot base_dir (JobSpec-filens mapp). Kastar pc::Error vid fel schema.
JobSpec parse(const json& spec, const std::filesystem::path& base_dir);
JobSpec load(const std::string& path);

}  // namespace pc::jobspec
