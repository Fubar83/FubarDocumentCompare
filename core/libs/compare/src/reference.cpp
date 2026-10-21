// Originalet analyseras en gång: lager, dekorfärger, rendering, streckkoder, objektkarta, tabeller,
// punktskrift och masker. Resultatet är oföränderligt och delas mellan jobben.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <set>

#include "printcmp/compare/profiles.hpp"
#include "printcmp/compare/reference.hpp"
#include "printcmp/scan/aggregate.hpp"
#include "printcmp/scan/certainty.hpp"
#include "printcmp/scan/classify.hpp"
#include "printcmp/scan/diff.hpp"
#include "printcmp/scan/folds.hpp"
#include "printcmp/scan/normalize.hpp"
#include "printcmp/scan/register.hpp"
#include "printcmp/scan/sheet_folds.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/image/image.hpp"

#include "label_steps.hpp"

namespace pc::compare {

json ReferenceModel::info() const {
    json codes = json::array(), outlined = json::array(), tbl = json::array(), br = json::array();
    std::map<std::string, int> by_kind;
    for (auto& c : this->codes)
        codes.push_back({{"symbology", c.symbology}, {"data", c.data},
                         {"bbox_mm", {round_to(c.bbox.x0 / ppm, 2), round_to(c.bbox.y0 / ppm, 2), round_to(c.bbox.x1 / ppm, 2), round_to(c.bbox.y1 / ppm, 2)}}});
    for (auto& o : objs) {
        by_kind[o.kind]++;
        if (o.kind == "OutlinedText")
            outlined.push_back({round_to(o.bbox.x0 / ppm, 2), round_to(o.bbox.y0 / ppm, 2), round_to(o.bbox.x1 / ppm, 2), round_to(o.bbox.y1 / ppm, 2)});
    }
    for (auto& t : tables) tbl.push_back(t.to_json(ppm));
    for (auto& ln : braille) {
        std::string uni;
        for (auto& c : ln.cell_list()) uni += braille::unicode_of(c);
        Box b = ln.bbox();
        br.push_back({{"line", ln.index}, {"text", ln.text()}, {"source", ln.source}, {"unicode", uni}, {"cells", ln.kmax - ln.kmin + 1},
                      {"bbox_mm", {round_to(b.x0 / ppm, 2), round_to(b.y0 / ppm, 2), round_to(b.x1 / ppm, 2), round_to(b.y1 / ppm, 2)}},
                      {"dot_pitch_mm", round_to(ln.pitch / ppm, 2)}, {"cell_pitch_mm", round_to(ln.cell_pitch / ppm, 2)}});
    }
    json obk = json::object();
    for (auto& [k, v] : by_kind) obk[k] = v;
    return {{"path", path}, {"sha256", "sha256:" + sha256_hex(original.data(), original.size())},
            {"size_mm", {round_to(ref.geom.width_mm(), 3), round_to(ref.geom.height_mm(), 3)}}, {"dpi", dpi},
            {"dieline", masks.has_dieline}, {"spot_colors", spots.spot_names}, {"barcodes", codes}, {"objects", objs.size()},
            {"objects_by_kind", obk}, {"outlined_text_mm", outlined}, {"tables", tbl}, {"layers", detail::layer_json(layers, layer_decision)},
            {"braille", br}};
}

std::shared_ptr<const ReferenceModel> analyse_reference(const pdf::Bytes& pdf_bytes, const std::string& path,
                                                        const ReferenceOptions& opt, const JobContext& ctx) {
    auto R = std::make_shared<ReferenceModel>();
    R->path = path;
    R->original = pdf_bytes;
    R->opt = opt;
    R->tol = resolve_profile(opt.profile, opt.tolerance_overrides);
    R->dpi = opt.dpi;
    R->ppm = opt.dpi / MM_PER_INCH;
    // regler: standard, sedan jobbets egna (ersätter roller med samma namn), sedan uteslutna dekorfärger
    R->rules = pdf::default_spot_rules();
    auto merge_rules = [&](const pdf::SpotRules& extra) {
        for (auto& [role, rx] : extra) {
            auto it = std::find_if(R->rules.begin(), R->rules.end(), [&](auto& p) { return p.first == role; });
            if (it != R->rules.end()) it->second = rx;
            else R->rules.push_back({role, rx});
        }
    };
    merge_rules(opt.extra_spot_rules);
    merge_rules(pdf::excluded_spot_rule(opt.exclude_spots));
    int page = opt.page - 1;
    ctx.progress(Stage::Render, 0.0);
    R->layers = pdf::list_layers(pdf_bytes, page);
    R->filtered = pdf::filter_layers(pdf_bytes, page, opt.include_layers, opt.exclude_layers, &R->layer_decision);
    if (opt.fonts) R->filtered = pdf::apply_fonts(R->filtered, *opt.fonts);
    R->spots = pdf::analyze_spots(R->filtered, page, R->rules);
    ctx.check();
    R->ref = pdf::render(R->spots.sanitized_pdf, page, opt.dpi, opt.box);
    R->codes = barcode::detect(R->ref.rgb, opt.dpi);
    std::vector<Box> code_boxes;
    for (auto& c : R->codes) code_boxes.push_back(c.bbox);
    R->objs = artwork::build_objects(R->spots.sanitized_pdf, page, R->ref.geom, code_boxes);
    R->tables = artwork::detect_tables(R->objs, R->ppm);
    R->braille = artwork::braille_from_pdf(R->spots, R->filtered, page, R->ref.geom);
    cv::Mat bmask = R->braille.empty() ? cv::Mat() : artwork::braille_mask(R->ref.rgb.size(), R->braille, R->ppm);
    R->masks = artwork::build_masks(R->ref.rgb.size(), R->ref.geom, R->spots, opt.masks, R->tol["dieline_margin_mm"], bmask);
    R->text_mask = cv::Mat::zeros(R->ref.rgb.size(), CV_8U);
    int pad = std::max(1, (int)pyround(0.2 * R->ppm));
    for (auto& o : R->objs)
        if (o.kind == "Text" && o.bbox.h() <= R->tol["text_small_mm"].get<double>() * R->ppm) {
            cv::Rect r(cv::Point((int)pyround(o.bbox.x0) - pad, (int)pyround(o.bbox.y0) - pad),
                       cv::Point((int)pyround(o.bbox.x1) + pad, (int)pyround(o.bbox.y1) + pad));
            r &= cv::Rect(0, 0, R->text_mask.cols, R->text_mask.rows);
            if (!r.empty()) R->text_mask(r).setTo(1);
        }
    ctx.progress(Stage::Render, 1.0);
    return R;
}

}  // namespace pc::compare
