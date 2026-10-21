// Tolkning av JobSpec, med standardvärden för allt som inte anges.
#include "printcmp/jobspec/jobspec.hpp"

#include <algorithm>

#include "printcmp/core/io.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/pdf/fonts.hpp"

namespace pc::jobspec {

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path resolve(const fs::path& base, const std::string& p) {
    fs::path q = fs::u8path(p);
    return q.is_absolute() ? q : fs::absolute(base / q).lexically_normal();
}

compare::DataCheckOptions data_check(const json& dc, const fs::path& base) {
    compare::DataCheckOptions d;
    d.path = u8(resolve(base, dc["path"]));
    if (dc.contains("row")) {
        if (dc["row"].is_number_integer()) d.row = dc["row"].get<int>();
        else d.row_match = dc["row"].get<std::string>();
    }
    d.ignore_case = dc.value("ignore_case", false);
    d.source = dc.value("source", "sample");
    return d;
}

// "fonts": {"dir": "fonts", "files": ["a.otf", {"path": "b.ttf", "aliases": ["AcmeSans"]}], "fallback": "builtin"}
std::shared_ptr<const pdf::FontSet> font_set(const json& f, const fs::path& base) {
    std::vector<std::shared_ptr<const pdf::FontFile>> files;
    auto add = [&](const fs::path& p, std::vector<std::string> aliases) {
        auto s = read_file_bytes(u8(p));
        files.push_back(pdf::parse_font(pdf::Bytes(s.begin(), s.end()), std::move(aliases)));
    };
    if (f.contains("dir")) {
        std::vector<fs::path> found;
        for (auto& e : fs::directory_iterator(resolve(base, f["dir"]))) {
            auto ext = text::lower(u8(e.path().extension()));
            if (e.is_regular_file() && (ext == ".ttf" || ext == ".otf")) found.push_back(e.path());
        }
        std::sort(found.begin(), found.end());   // samma uppsättning (och hash) oavsett katalogordning
        for (auto& p : found) add(p, {});
    }
    for (auto& x : f.value("files", json::array())) {
        if (x.is_string()) add(resolve(base, x.get<std::string>()), {});
        else add(resolve(base, x["path"].get<std::string>()), x.value("aliases", std::vector<std::string>{}));
    }
    auto fb = f.value("fallback", "builtin") == "system" ? pdf::FontFallback::System : pdf::FontFallback::Builtin;
    return std::make_shared<const pdf::FontSet>(std::move(files), fb);
}

compare::DocumentOptions document_options(const json& spec, const fs::path& base) {
    json cmp = spec.value("compare", json::object());
    compare::DocumentOptions o;
    o.layout = cmp.value("layout", "reflow");
    o.furniture_ignore = cmp.value("furniture", "ignore") == "ignore";
    o.typography_normalize = cmp.value("typography", "normalize") == "normalize";
    o.ignore_case = cmp.value("ignore_case", false);
    o.min_move_words = cmp.value("min_move_words", 3);
    o.move_tol_mm = cmp.value("move_tol_mm", 0.3);
    if (cmp.contains("pages_reference")) {
        o.ref_first = cmp["pages_reference"][0];
        o.ref_last = cmp["pages_reference"][1];
    }
    if (cmp.contains("pages_sample")) {
        o.smp_first = cmp["pages_sample"][0];
        o.smp_last = cmp["pages_sample"][1];
    }
    if (spec.contains("data_check")) o.data_check = data_check(spec["data_check"], base);
    return o;
}

}  // namespace

JobSpec parse(const json& spec, const fs::path& base) {
    if (spec.value("schema", "") != SCHEMA) throw Error(ErrorCode::InvalidArgument, "fel schema i JobSpec");
    JobSpec j;
    json outputs = {{"dir", "out"}, {"json", "result.json"}, {"report_html", "report.html"}, {"layers", true}, {"layer_dpi", 200}};
    if (spec.contains("outputs")) outputs.update(spec["outputs"]);
    j.outputs.dir = resolve(base, outputs["dir"]);
    j.outputs.json = outputs["json"];
    j.outputs.report_html = outputs["report_html"];
    j.outputs.layers = outputs["layers"];
    j.outputs.layer_dpi = outputs["layer_dpi"];
    j.reference_path = u8(resolve(base, spec["reference"]["path"]));
    j.sample_path = u8(resolve(base, spec["sample"]["path"]));
    if (spec.value("compare", json::object()).value("mode", "label") == "document") {
        j.mode = JobSpec::Mode::Document;
        j.document = document_options(spec, base);
        return j;
    }
    // etikettläge: ett PDF-prov ger profilen pdf_vs_pdf om ingen anges
    std::string kind = spec["sample"].value("kind", text::ends_with(text::lower(j.sample_path), ".pdf") ? "pdf" : "scan");
    auto& ro = j.reference;
    ro.page = spec["reference"].value("page", 1);
    ro.box = spec["reference"].value("box", "trim");
    ro.dpi = spec.value("work_dpi", 600.0);
    ro.profile = spec.value("tolerance_profile", kind == "pdf" ? "pdf_vs_pdf" : "label_standard");
    ro.tolerance_overrides = spec.value("tolerance_overrides", json::object());
    json layers = spec.value("layers", json::object());
    for (auto& x : layers.value("include", json::array())) ro.include_layers.push_back(x);
    for (auto& x : layers.value("exclude", json::array())) ro.exclude_layers.push_back(x);
    for (auto& x : layers.value("exclude_spots", json::array())) ro.exclude_spots.push_back(x);
    json spot_rules = spec.value("spot_rules", json::object());   // items() får inte peka in i ett temporärt värde
    for (auto& [k, v] : spot_rules.items()) ro.extra_spot_rules.push_back({k, v.get<std::string>()});
    for (auto& m : spec.value("masks", json::array()))
        ro.masks.push_back({m.value("reason", "user"), m["rect_mm"][0], m["rect_mm"][1], m["rect_mm"][2], m["rect_mm"][3]});
    if (spec.contains("fonts")) ro.fonts = font_set(spec["fonts"], base);

    auto& src = j.sample;
    src.kind = kind == "pdf" ? compare::SampleSource::Kind::Pdf : compare::SampleSource::Kind::Scan;
    src.path = j.sample_path;
    src.page = spec["sample"].value("page", 1);
    src.dpi = spec["sample"].value("dpi", 0.0);

    auto& co = j.label;
    json ispec = spec.value("instances", json::object());
    co.instance_mode = ispec.value("mode", "single");
    co.expected_count = ispec.value("expected_count", 0);
    if (ispec.contains("expected_layout")) {
        co.expected_rows = ispec["expected_layout"].value("rows", 0);
        co.expected_cols = ispec["expected_layout"].value("cols", 0);
    }
    if (ispec.contains("rotations")) co.rotations = ispec["rotations"].get<std::vector<int>>();
    co.max_skew_deg = ispec.value("max_skew_deg", 4.0);
    co.systematic_ratio = ispec.value("systematic_ratio", 0.8);
    co.consensus = ispec.value("consensus", true);
    co.text_compare = spec.value("text_compare", true);
    co.color_calibrated = spec.value("color_calibrated", false);
    co.seed = spec.value("seed", 1);
    if (spec.contains("data_check")) co.data_check = data_check(spec["data_check"], base);
    return j;
}

JobSpec load(const std::string& path) { return parse(json::parse(read_file(path)), fs::u8path(path).parent_path()); }

}  // namespace pc::jobspec
