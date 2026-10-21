// Körning av en tolkad JobSpec: jämförelse (pc_compare) och utdata (pc_report).
#include "printcmp/jobspec/runner.hpp"

#include <chrono>

#include "printcmp/report/report.hpp"

namespace pc::jobspec {

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace

RunResult run(const JobSpec& spec, const std::string& out_override, const JobContext& ctx) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    fs::path out_dir = out_override.empty() ? spec.outputs.dir : fs::absolute(fs::u8path(out_override));
    fs::create_directories(out_dir);
    auto out = [&](const std::string& name) { return u8(out_dir / fs::u8path(name)); };
    RunResult r;
    r.out_dir = u8(out_dir);

    if (spec.mode == JobSpec::Mode::Document) {
        r.result = compare::run_document_job(spec.reference_path, spec.sample_path, spec.document, ctx);
        report::write_json(out(spec.outputs.json), r.result);
        if (!spec.outputs.report_html.empty()) report::write_document_html(out(spec.outputs.report_html), r.result);
    } else {
        auto ref = compare::analyse_reference(read_file_bytes(spec.reference_path), spec.reference_path, spec.reference, ctx);
        auto res = compare::compare_label(*ref, spec.sample, spec.label, ctx);
        if (spec.outputs.layers) report::write_layers(u8(out_dir / "layers"), res, spec.outputs.layer_dpi);
        auto t_report = clock::now();
        if (!spec.outputs.report_html.empty()) report::write_label_html(out(spec.outputs.report_html), res);
        res.result["timing_ms"]["report"] = int(std::chrono::duration<double, std::milli>(clock::now() - t_report).count());
        int total = 0;
        for (auto& [k, v] : res.result["timing_ms"].items()) total += v.get<int>();
        res.result["timing_ms"]["total"] = total;
        report::write_json(out(spec.outputs.json), res.result);
        r.result = std::move(res.result);
    }
    r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return r;
}

}  // namespace pc::jobspec
