// Etikett/ark mot original: referensen analyseras en gång, varje prov
// jämförs mot den. Händelser (etikett hittad, veck, etikett klar) och avbrott mellan stegen.
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
#include "printcmp/compare/label.hpp"
#include "printcmp/compare/textdiff.hpp"
#include "printcmp/compare/datacheck.hpp"
#include "label_steps.hpp"
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

namespace pc::compare {

namespace {

using Clock = std::chrono::steady_clock;

struct Timer {
    json t = json::object();
    void add(const char* k, Clock::time_point t0) {
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        t[k] = t.value(k, 0.0) + ms;
    }
};

std::string file_sha(const std::string& path, const pdf::Bytes* bytes = nullptr) {
    if (bytes) return "sha256:" + sha256_hex(bytes->data(), bytes->size());
    if (path.empty() || !std::filesystem::exists(std::filesystem::u8path(path))) return "";
    auto b = read_file_bytes(path);
    return "sha256:" + sha256_hex(b.data(), b.size());
}


struct State {
    scan::Instance inst;
    scan::Registration reg;
    cv::Mat aligned;
    scan::DiffMaps maps;
    std::vector<scan::Fold> fl;
    cv::Mat band;
    std::optional<scan::ColorModel> color;
};

std::vector<scan::SheetLabel> sheet_labels(const std::vector<State>& states) {
    std::vector<scan::SheetLabel> out;
    for (auto& s : states) out.push_back({s.inst.index, s.reg.H, s.fl});
    return out;
}

}  // namespace

LabelResult compare_label(const ReferenceModel& R, const SampleSource& sample, const CompareOptions& opt, const JobContext& ctx) {
    Timer T;
    const json& tol = R.tol;
    double dpi = R.dpi, ppm = R.ppm;
    auto t0 = Clock::now();
    const auto& masks = R.masks;
    const cv::Mat& ref_rgb = R.ref.rgb;

    // ---------- prov
    std::string kind = sample.kind == SampleSource::Kind::Pdf ? "pdf" : "scan";
    cv::Mat sample_rgb;
    double sample_dpi = 0;
    pdf::Bytes sbytes;
    pdf::SpotAnalysis sspots;
    pdf::Geometry sgeom;
    std::vector<Pt> sample_bdots;
    json sample_layers;
    if (sample.kind == SampleSource::Kind::Pdf) {
        int sp = sample.page - 1;
        sbytes = sample.pdf_bytes.empty() ? read_file_bytes(sample.path) : sample.pdf_bytes;
        auto linfo = pdf::list_layers(sbytes, sp);
        std::map<std::string, bool> dec;
        sbytes = pdf::filter_layers(sbytes, sp, R.opt.include_layers, R.opt.exclude_layers, &dec);
        if (R.opt.fonts) sbytes = pdf::apply_fonts(sbytes, *R.opt.fonts);
        sample_layers = detail::layer_json(linfo, dec);
        sspots = pdf::analyze_spots(sbytes, sp, R.rules);
        auto srend = pdf::render(sspots.sanitized_pdf, sp, dpi, R.opt.box);
        sample_rgb = srend.rgb;
        sgeom = srend.geom;
        sample_dpi = dpi;
        sample_bdots = artwork::braille_dots_from_spots(sspots, srend.geom).first;
        if (sample_bdots.empty()) sample_bdots = artwork::braille_dots_from_text(sbytes, sp, srend.geom).first;
    } else if (sample.kind == SampleSource::Kind::Image && !sample.rgb.empty()) {
        sample_rgb = sample.rgb;
        sample_dpi = sample.dpi;
        if (sample_dpi <= 0) throw Error(ErrorCode::InvalidArgument, "bildbufferten saknar upplösning");
    } else {
        auto s = image::read_scan(sample.path, sample.dpi);
        sample_rgb = s.rgb;
        sample_dpi = s.dpi;
    }
    bool is_scan = kind != "pdf";
    T.add("render", t0);
    {
        Event e{Event::Type::SheetLoaded};
        e.width_px = sample_rgb.cols;
        e.height_px = sample_rgb.rows;
        e.dpi = sample_dpi;
        ctx.emit(e);
    }
    if (is_scan && sample_dpi < 590) ctx.warn("scan_dpi_low", format("Scannen har %.0f dpi; minst 600 dpi rekommenderas", sample_dpi));
    ctx.check();

    // ---------- lokalisering
    t0 = Clock::now();
    ctx.progress(Stage::Locate, 0.0);
    std::optional<int> expected;
    if (opt.expected_count) expected = opt.expected_count;
    else if (opt.expected_rows * opt.expected_cols) expected = opt.expected_rows * opt.expected_cols;
    else if (opt.instance_mode == "single") expected = 1;
    std::vector<scan::Instance> insts;
    json loc_info;
    if (kind == "pdf" && opt.instance_mode == "single") {
        Pt c{sample_rgb.cols / 2.0, sample_rgb.rows / 2.0};
        scan::Instance in;
        in.index = 1;
        in.center = c;
        in.score = 1.0;
        in.row = in.col = 0;
        in.H0 = scan::similarity_H(ref_rgb.size(), dpi, sample_dpi, c, 0.0);
        insts.push_back(in);
        loc_info = {{"sheet_angle_deg", 0.0}, {"match_score", 1.0}};
    } else {
        std::tie(insts, loc_info) = scan::locate(ref_rgb, masks.inside, dpi, sample_rgb, sample_dpi, opt.rotations, opt.max_skew_deg, expected);
        double row_tol = 0.4 * std::min(ref_rgb.rows, ref_rgb.cols) * sample_dpi / dpi;
        insts = scan::assign_grid(insts, loc_info["sheet_angle_deg"], row_tol);
    }
    for (auto& in : insts) {
        Event e{Event::Type::LabelLocated};
        e.index = in.index;
        e.rotation_deg = in.angle_deg;
        Pt cs[4] = {{0, 0}, {(double)ref_rgb.cols, 0}, {(double)ref_rgb.cols, (double)ref_rgb.rows}, {0, (double)ref_rgb.rows}};
        for (int k = 0; k < 4; ++k) e.corners[k] = in.H0.apply(cs[k]);
        ctx.emit(e);
    }
    if (expected && (int)insts.size() < *expected)
        ctx.warn("labels_missing", format("Hittade %d av %d etiketter", (int)insts.size(), *expected));
    T.add("locate", t0);
    ctx.check();

    // ---------- per instans
    cv::Mat ref_f = image::to_float(ref_rgb);
    cv::Mat ref_gs = scan::prepare_gray(ref_rgb, ppm), smp_gs = scan::prepare_gray(sample_rgb, sample_dpi / MM_PER_INCH);
    double sigma = 0, extra_blur = 0;
    std::optional<double> period;
    cv::Mat ref_d = ref_f;
    std::vector<State> states;
    for (size_t k = 0; k < insts.size(); ++k) {
        ctx.progress(Stage::Register, (double)k / std::max<size_t>(1, insts.size()));
        t0 = Clock::now();
        State st;
        st.inst = insts[k];
        st.reg = scan::register_images(ref_gs, smp_gs, insts[k].H0, masks.valid, ppm, tol, opt.seed,
                                       tol["local_registration"].get<bool>() && is_scan);
        st.aligned = scan::resample(sample_rgb, st.reg.H, st.reg.flow, ref_rgb.size(), cv::INTER_LANCZOS4);
        T.add("register", t0);
        ctx.check();
        t0 = Clock::now();
        cv::Mat smp_f = image::to_float(st.aligned);
        if (k == 0 && tol["descreen"].get<bool>() && is_scan) {
            period = scan::estimate_screen_period_px(image::gray_f32(st.aligned), image::gray_f32(ref_rgb), masks.valid);
            // utan tonplattor går rastret inte att mäta: anta typiskt raster 70 linjer/cm
            sigma = std::clamp(0.55 * (period ? *period : dpi / (70 * 2.54)), 1.0, 3.0);
            ref_d = scan::descreen(ref_f, sigma);
        }
        cv::Mat smp_d = sigma > 0 ? scan::descreen(smp_f, sigma) : smp_f;
        if (k == 0 && is_scan && tol["normalize"].get<bool>()) {
            extra_blur = scan::estimate_extra_blur(ref_d, smp_d, masks.valid);
            if (extra_blur > 0) ref_d = scan::descreen(ref_d, extra_blur);
        }
        if (tol["normalize"].get<bool>() && is_scan) {
            st.color = scan::fit_color(smp_d, ref_d, masks.valid);
            smp_d = scan::apply_color(smp_d, *st.color);
            if (tol["illumination_flatten"].get<bool>()) {
                smp_d = scan::flatten_illumination(smp_d, ref_d, masks.valid, ppm);
                st.color = scan::fit_color(smp_d, ref_d, masks.valid);
                smp_d = scan::apply_color(smp_d, *st.color);
            }
        }
        T.add("normalize", t0);
        t0 = Clock::now();
        ctx.progress(Stage::Diff, (double)k / std::max<size_t>(1, insts.size()));
        st.maps = scan::compute_diff(ref_d, smp_d, masks.valid, masks.varnish, ppm, tol, R.text_mask);
        if (tol["fold_detection"].get<bool>() && is_scan)
            st.fl = scan::detect_folds(st.maps.ref_L(), st.maps.smp_L(), masks.valid, ppm, tol["fold_band_mm"]);
        T.add("diff", t0);
        states.push_back(std::move(st));
        ctx.check();
    }

    // ---------- veck på arknivå
    t0 = Clock::now();
    ctx.progress(Stage::Folds, 0.0);
    double sppm = sample_dpi / MM_PER_INCH;
    {
        auto sheet = sheet_labels(states);
        scan::drop_systematic_fold_candidates(sheet, ref_rgb.size(), ppm, sppm);
        for (size_t i = 0; i < states.size(); ++i) states[i].fl = std::move(sheet[i].folds);
    }
    std::vector<std::pair<std::vector<Pt>, double>> clusters;
    double tol_px = 3.0 * sppm;
    auto pca_line = [](const std::vector<Pt>& pts, Pt& mu, Pt& e) {
        std::vector<cv::Point2d> P;
        for (auto& p : pts) P.push_back({p.x, p.y});
        cv::PCA pca(cv::Mat(P).reshape(1), cv::Mat(), cv::PCA::DATA_AS_ROW);
        mu = {pca.mean.at<double>(0), pca.mean.at<double>(1)};
        e = {pca.eigenvectors.at<double>(0, 0), pca.eigenvectors.at<double>(0, 1)};
    };
    for (auto& st : states)
        for (auto& f : st.fl) {
            std::vector<Pt> pts;
            for (int i = 0; i < 25; ++i) {
                double t = i / 24.0;
                pts.push_back(st.reg.H.apply({f.p0.x * (1 - t) + f.p1.x * t, f.p0.y * (1 - t) + f.p1.y * t}));
            }
            double dx = pts.back().x - pts.front().x, dy = pts.back().y - pts.front().y, n = std::max(std::hypot(dx, dy), 1e-9);
            dx /= n;
            dy /= n;
            bool placed = false;
            for (auto& cl : clusters) {
                Pt mu, e;
                pca_line(cl.first, mu, e);
                double mx = 0;
                for (auto& p : pts) mx = std::max(mx, std::abs((p.x - mu.x) * -e.y + (p.y - mu.y) * e.x));
                if (std::abs(dx * e.x + dy * e.y) > 0.99 && mx < tol_px) {
                    cl.first.insert(cl.first.end(), pts.begin(), pts.end());
                    cl.second = std::max(cl.second, f.band_px);
                    placed = true;
                    break;
                }
            }
            if (!placed) clusters.push_back({pts, f.band_px});
        }
    struct Merged { Pt p0, p1; double bw; };
    std::vector<Merged> merged;
    for (auto& [pts, bw] : clusters) {
        Pt mu, e;
        pca_line(pts, mu, e);
        merged.push_back({{mu.x - 1e5 * e.x, mu.y - 1e5 * e.y}, {mu.x + 1e5 * e.x, mu.y + 1e5 * e.y}, bw});
    }
    if (!merged.empty()) {
        std::vector<scan::ResidualMap> rms;
        for (auto& st : states) rms.push_back(scan::residual_map(st.maps.ref_L(), st.maps.smp_L(), masks.valid, ppm));
        std::vector<Merged> keep;
        for (auto& m : merged) {
            int n_cross = 0, n_sup = 0;
            for (size_t i = 0; i < states.size(); ++i) {
                Mat3 Hinv = states[i].reg.H.inverse();
                auto [frac, n] = scan::fold_support(rms[i], {Hinv.apply(m.p0), Hinv.apply(m.p1), m.bw});
                if (n * rms[i].ds < 8 * ppm) continue;
                ++n_cross;
                n_sup += frac >= 0.4;
            }
            if (n_cross <= 1 || n_sup >= std::max(2, (int)std::ceil(0.5 * n_cross))) keep.push_back(m);
        }
        merged = keep;
    }
    for (auto& m : merged) {
        Event e{Event::Type::FoldDetected};
        e.corners[0] = m.p0;
        e.corners[1] = m.p1;
        ctx.emit(e);
    }
    int h_ref = ref_rgb.rows, w_ref = ref_rgb.cols;
    for (auto& st : states) {
        Mat3 Hinv = st.reg.H.inverse();
        std::vector<scan::Fold> fl_all;
        for (auto& m : merged) {
            Pt mid{(m.p0.x + m.p1.x) / 2, (m.p0.y + m.p1.y) / 2};
            double ex = m.p1.x - m.p0.x, ey = m.p1.y - m.p0.y, en = std::hypot(ex, ey);
            ex /= en;
            ey /= en;
            Pt c_lab = st.reg.H.apply({w_ref / 2.0, h_ref / 2.0});
            double t = (c_lab.x - mid.x) * ex + (c_lab.y - mid.y) * ey;
            Pt foot{mid.x + t * ex, mid.y + t * ey};
            double L = 2.0 * (w_ref + h_ref);
            scan::Fold f{Hinv.apply({foot.x - ex * L, foot.y - ey * L}), Hinv.apply({foot.x + ex * L, foot.y + ey * L}), m.bw};
            f = scan::refine_fold(st.maps.ref_L(), st.maps.smp_L(), masks.valid, ppm, f);
            bool dup = false;
            for (auto& g : fl_all)
                if (scan::point_line_dist({(f.p0.x + f.p1.x) / 2, (f.p0.y + f.p1.y) / 2}, g.p0, g.p1) < 2 * m.bw &&
                    scan::angle_diff(f.p0, f.p1, g.p0, g.p1) < 5)
                    dup = true;
            if (dup) continue;
            cv::Mat zone = cv::Mat::zeros(ref_rgb.size(), CV_8U);
            scan::draw_extended_band(zone, f);
            if (cv::countNonZero(zone & masks.valid)) {
                double hw = scan::estimate_halfwidth(st.maps.d_struct, st.maps.smp_L() - st.maps.ref_L(), masks.valid, ppm, f,
                                                     0.6 * tol["struct_dl"].get<double>(), 12.0, st.maps.ref_L());
                f.band_px = std::max(f.band_px, hw + 0.5 * ppm);
                fl_all.push_back(f);
            }
        }
        st.band = cv::Mat::zeros(ref_rgb.size(), CV_8U);
        for (auto& f : fl_all) scan::draw_extended_band(st.band, f);
        st.fl = fl_all;
    }
    T.add("folds", t0);
    ctx.check();

    // ---------- klassning per instans
    std::vector<json> all_diffs;
    json inst_out = json::array();
    std::vector<cv::Mat> aligned_L, bands;
    for (auto& st : states) {
        t0 = Clock::now();
        ctx.progress(Stage::Classify, (double)(&st - &states[0]) / std::max<size_t>(1, states.size()));
        auto& maps = st.maps;
        maps.struct_mask.setTo(0, st.band);
        maps.color_mask.setTo(0, st.band);
        auto blobs = scan::find_blobs(maps, ppm, tol);
        auto dl = scan::classify(blobs, maps, R.objs, R.codes, st.aligned, st.band, st.reg.confidence, ppm, dpi, tol, st.inst.index, R.tables);
        json b_reads = json::array(), b_stats = json::array();
        if (!R.braille.empty()) {
            if (is_scan) {
                auto bres = artwork::braille_check_scan(maps.smp_L() - maps.ref_L(), R.braille, ppm, st.inst.index, st.band);
                dl.insert(dl.end(), bres.diffs.begin(), bres.diffs.end());
                for (auto& r : bres.reads) b_reads.push_back(r ? json(*r) : json(nullptr));
                for (auto& s : bres.stats) b_stats.push_back(s);
            } else {
                Mat3 Hinv = st.reg.H.inverse();
                std::vector<Pt> pts;
                for (auto& p : sample_bdots) pts.push_back(Hinv.apply(p));
                auto [bd, reads] = artwork::braille_compare_dots(R.braille, pts, ppm, st.inst.index);
                dl.insert(dl.end(), bd.begin(), bd.end());
                for (auto& r : reads) b_reads.push_back(r);
            }
        }
        for (auto& f : st.fl) {
            cv::Mat zone = cv::Mat::zeros(st.band.size(), CV_8U);
            scan::draw_extended_band(zone, f);
            zone &= masks.valid;
            std::vector<cv::Point> pts;
            cv::findNonZero(zone, pts);
            if (pts.empty()) continue;
            json z = scan::uncertain_zone(st.inst.index, cv::boundingRect(pts), ppm, "fold");
            z["area_mm2"] = pts.size() / (ppm * ppm);
            z["line_mm"] = {{f.p0.x / ppm, f.p0.y / ppm}, {f.p1.x / ppm, f.p1.y / ppm}};
            dl.push_back(z);
        }
        // explicit streckkodskontroll även om diffen inte flaggade koden
        for (auto& c : R.codes) {
            bool covered = std::any_of(dl.begin(), dl.end(), [&](const json& d) {
                return (d["kind"] == "Barcode" || d["kind"] == "Shape") && d.contains("barcode") &&
                       d["barcode"].value("expected", "") == c.data && d["instance"] == st.inst.index;
            });
            if (covered) continue;
            auto got = barcode::decode_region(st.aligned, c.bbox, (int)(2 * ppm), dpi);
            if (std::none_of(got.begin(), got.end(), [&](auto& g) { return g.data == c.data; })) {
                json d = scan::uncertain_zone(st.inst.index,
                                              cv::Rect((int)c.bbox.x0, (int)c.bbox.y0, (int)c.bbox.w(), (int)c.bbox.h()), ppm, "");
                d.erase("cause");
                d["kind"] = "Barcode";
                d["severity"] = "Critical";
                d["bbox_mm"] = {c.bbox.x0 / ppm, c.bbox.y0 / ppm, c.bbox.w() / ppm, c.bbox.h() / ppm};
                d["area_mm2"] = c.bbox.w() * c.bbox.h() / (ppm * ppm);
                d["confidence"] = 0.8;
                d["barcode"] = {{"expected", c.data}, {"read", got.empty() ? json(nullptr) : json(got[0].data)}, {"symbology", c.symbology}};
                dl.push_back(d);
            }
        }
        {
            Event e{Event::Type::LabelAnalysed};
            e.index = st.inst.index;
            int real = 0;
            bool major = false;
            for (auto& d : dl)
                if (d["kind"] != "Uncertain") {
                    ++real;
                    major |= d["severity"] == "Major" || d["severity"] == "Critical";
                }
            e.preliminary = real;
            e.status = major ? 2 : (real ? 1 : 0);
            ctx.emit(e);
        }
        all_diffs.insert(all_diffs.end(), dl.begin(), dl.end());
        T.add("classify", t0);
        cv::Mat v = (masks.valid > 0) & (st.band == 0);
        json folds_j = json::array();
        for (auto& f : st.fl)
            folds_j.push_back({{"p0_mm", {f.p0.x / ppm, f.p0.y / ppm}}, {"p1_mm", {f.p1.x / ppm, f.p1.y / ppm}}, {"band_mm", round_to(f.band_px / ppm, 2)}});
        Mat3 Hn = st.reg.H;
        std::vector<double> hv;
        for (double x : Hn.m) hv.push_back(x / Hn.m[8]);
        json color_m = nullptr;
        if (st.color) {
            auto M = st.color->M();
            color_m = json::array();
            for (auto& row : M) {
                json r = json::array();
                for (double x : row) r.push_back(round_to(x, 4));
                color_m.push_back(r);
            }
        }
        inst_out.push_back({{"index", st.inst.index}, {"row", st.inst.row}, {"col", st.inst.col},
                            {"center_px", {round_to(st.inst.center.x, 1), round_to(st.inst.center.y, 1)}},
                            {"angle_deg", round_to(st.inst.angle_deg, 3)}, {"locate_score", round_to(st.inst.score, 3)},
                            {"transform", {{"h", hv}}},
                            {"registration_rms_px", std::isfinite(st.reg.rms_px) ? json(round_to(st.reg.rms_px, 3)) : json(nullptr)},
                            {"registration_tiles", st.reg.n_tiles},
                            {"mean_delta_e", cv::countNonZero(v) ? json(round_to(cv::mean(maps.delta_e, v)[0], 3)) : json(nullptr)},
                            {"folds", folds_j}, {"color_matrix", color_m},
                            {"braille", R.braille.empty() ? json(nullptr) : json{{"read", b_reads}, {"stats", b_stats}}}});
        aligned_L.push_back(maps.smp_L());
        bands.push_back(st.band);
        ctx.check();
    }

    // ---------- textlager (PDF mot PDF)
    if (kind == "pdf" && opt.text_compare && !states.empty()) {
        t0 = Clock::now();
        ctx.progress(Stage::Text, 0.0);
        auto rw = extract_words(R.spots.sanitized_pdf, R.opt.page - 1, R.ref.geom);
        auto sw = extract_words(sspots.sanitized_pdf, sample.page - 1, sgeom);
        auto tds = text_compare(rw, sw, states[0].reg.H, ppm);
        all_diffs = detail::link_text(all_diffs, tds, states[0].inst.index);
        T.add("text", t0);
    }

    // ---------- konsensus, svaga veck, aggregering
    t0 = Clock::now();
    ctx.progress(Stage::Aggregate, 0.0);
    if (opt.consensus && aligned_L.size() >= 3) {
        auto c = detail::consensus(aligned_L, bands, masks.valid, all_diffs, insts, ppm, tol);
        all_diffs.insert(all_diffs.end(), c.begin(), c.end());
    }
    if (tol["fold_detection"].get<bool>() && is_scan && states.size() >= 3)
        all_diffs = scan::collinear_folds(all_diffs, sheet_labels(states), masks.valid, ppm, sppm, inst_out);
    auto agg = scan::aggregate(all_diffs, (int)insts.size(), opt.systematic_ratio);
    if (expected && (int)insts.size() < *expected)
        agg.push_back({{"instance", 0}, {"instances", json::array()}, {"kind", "Missing"}, {"severity", "Critical"}, {"systematic", false},
                       {"reason", "instance_not_found"}, {"found", insts.size()}, {"expected", *expected}, {"bbox_mm", nullptr},
                       {"bbox_px", nullptr}, {"objects", json::array()}, {"metrics", json::object()}, {"confidence", 1.0}});
    std::map<int, json> inst_folds;
    for (auto& io : inst_out) inst_folds[io["index"].get<int>()] = io["folds"];
    for (auto& d : agg) d["certainty"] = scan::assess(d, (int)insts.size(), tol, kind, inst_folds, opt.color_calibrated);
    auto lvl = [](const json& d) {
        return (int)(std::find(scan::LEVELS.begin(), scan::LEVELS.end(), d["certainty"]["level"].get<std::string>()) - scan::LEVELS.begin());
    };
    std::stable_sort(agg.begin(), agg.end(), [&](const json& a, const json& b) {
        auto key = [&](const json& d) {
            int first = d["instances"].empty() ? 0 : d["instances"][0].get<int>();
            return std::make_tuple(lvl(d), -scan::sev_index(d["severity"]), -d["certainty"]["score"].get<int>(),
                                   !d.value("systematic", false), first);
        };
        return key(a) < key(b);
    });
    for (size_t i = 0; i < agg.size(); ++i) {
        auto& d = agg[i];
        d["id"] = i + 1;
        d.erase("instance");
        d.erase("shift_px");
        if (d.contains("bbox_mm") && !d["bbox_mm"].is_null())
            for (auto& v : d["bbox_mm"]) v = round_to(v.get<double>(), 3);
        if (d.contains("area_mm2")) d["area_mm2"] = round_to(d["area_mm2"].get<double>(), 3);
    }
    for (auto& io : inst_out) {
        bool any = false, major = false;
        for (auto& d : agg) {
            if (d["kind"] == "Uncertain") continue;
            bool in = std::any_of(d["instances"].begin(), d["instances"].end(), [&](const json& x) { return x == io["index"]; });
            if (!in) continue;
            any = true;
            major |= d["severity"] == "Major" || d["severity"] == "Critical";
        }
        io["status"] = major ? "fail" : (any ? "warn" : "pass");
    }
    T.add("aggregate", t0);

    // ---------- datakontroll
    json dcheck;
    if (opt.data_check) {
        t0 = Clock::now();
        ctx.progress(Stage::DataCheck, 0.0);
        const auto& dcs = *opt.data_check;
        bool use_sample = kind == "pdf" && dcs.source == "sample";
        doc::LoadOptions lo;
        lo.first_page = lo.last_page = use_sample ? sample.page : R.opt.page;
        lo.furniture = false;
        auto tdoc = doc::load(use_sample ? sspots.sanitized_pdf : R.spots.sanitized_pdf, lo);
        std::vector<json> codes;
        for (auto& c : barcode::detect(sample_rgb, sample_dpi)) codes.push_back({{"data", c.data}, {"symbology", c.symbology}, {"source", "sample"}});
        if (dcs.source == "reference" || codes.empty())
            for (auto& c : R.codes) codes.push_back({{"data", c.data}, {"symbology", c.symbology}, {"source", "reference"}});
        dcheck = run_data_check(dcs, &tdoc, codes);
        dcheck["checked"] = std::string(use_sample ? "provets textlager" : "originalets textlager") + " och streckkoder i " +
                            (!codes.empty() && codes[0]["source"] == "sample" ? "provet" : "originalet");
        T.add("data_check", t0);
    }

    // ---------- resultat
    json tolj = {{"profile", R.opt.profile}};
    for (auto& [k, v] : tol.items()) tolj[k] = v;
    json sheet = {{"rotation_deg", round_to(loc_info["sheet_angle_deg"].get<double>(), 3)},
                  {"match_score", round_to(loc_info["match_score"].get<double>(), 3)}, {"instances_found", insts.size()},
                  {"instances_expected", expected ? json(*expected) : json(nullptr)}};
    json sample_j = {{"path", sample.path}, {"sha256", file_sha(sample.path, sample.path.empty() && !sbytes.empty() ? &sbytes : nullptr)},
                     {"kind", kind}, {"dpi", sample_dpi}, {"size_px", {sample_rgb.cols, sample_rgb.rows}},
                     {"screen_period_px", period ? json(round_to(*period, 2)) : json(nullptr)},
                     {"descreen_sigma_px", round_to(sigma, 2)}, {"extra_blur_px", round_to(extra_blur, 2)}};
    if (!sample_layers.is_null()) sample_j["layers"] = sample_layers;
    json timing = json::object();
    for (auto& [k, v] : T.t.items()) timing[k] = (int)v.get<double>();
    std::string hash_src = tolj.dump() + R.info()["sha256"].get<std::string>() + sample_j["sha256"].get<std::string>() + VERSION;
    json result = {{"schema", "printcmp.result/1"},
                   {"job_hash", "sha256:" + sha256_hex(hash_src)},
                   {"engine", {{"printcmp", VERSION}, {"core", "native"}, {"renderer", "pdfium"}, {"opencv", CV_VERSION}}},
                   {"reference", R.info()},
                   {"sample", sample_j},
                   {"sheet", sheet},
                   {"tolerances", tolj},
                   {"instances", inst_out},
                   {"differences", agg},
                   {"summary", detail::summary(agg, inst_out)}};
    if (!dcheck.is_null()) result["data_check"] = dcheck;
    result["timing_ms"] = timing;
    ctx.progress(Stage::Done, 1.0);

    LabelResult out;
    out.result = std::move(result);
    out.ref_rgb = ref_rgb;
    out.sample_rgb = sample_rgb;
    out.dpi = dpi;
    out.sample_dpi = sample_dpi;
    out.instances = insts;
    for (auto& st : states) out.aligned.push_back(st.aligned);
    return out;
}

}  // namespace pc::compare
