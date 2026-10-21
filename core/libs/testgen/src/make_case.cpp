// Testfall för scannade ark: referens-PDF, simulerat scannat ark, JobSpec och facit.
#include <cmath>
#include <map>
#include <opencv2/imgproc.hpp>
#include <set>

#include "printcmp/image/image.hpp"
#include "printcmp/pdf/document.hpp"
#include "printcmp/pdf/spots.hpp"
#include "printcmp/testgen/cases.hpp"
#include "printcmp/testgen/sim.hpp"
#include "util.hpp"

namespace pc::testgen {

namespace fs = std::filesystem;

namespace {

const char* GENERATOR_VERSION = "testgen-cpp 0.4.0";

struct Difficulty {
    bool halftone;
    double rot, fold_p;
    std::array<double, 2> noise, mtf;
    double misreg;
    std::array<double, 2> gain;
    double inst_rot180_p;
    std::array<double, 2> emboss;
    double emboss_missing_p;
    std::array<double, 2> lift;
};

const Difficulty& difficulty(const std::string& name) {
    static const std::map<std::string, Difficulty> D = {
        {"easy", {false, 1.0, 0.0, {0.8, 1.5}, {0.6, 0.8}, 0.0, {0.04, 0.08}, 0.0, {0.18, 0.28}, 0.5, {0.1, 0.3}}},
        {"normal", {true, 2.0, 0.5, {1.0, 2.5}, {0.7, 1.0}, 0.06, {0.06, 0.16}, 0.0, {0.12, 0.22}, 0.5, {0.1, 0.6}}},
        {"hard", {true, 3.5, 0.9, {2.0, 3.5}, {0.9, 1.2}, 0.12, {0.10, 0.22}, 0.15, {0.08, 0.16}, 0.6, {0.2, 1.2}}},
    };
    auto it = D.find(name);
    if (it == D.end()) throw Error(ErrorCode::InvalidArgument, "okänd svårighet: " + name);
    return it->second;
}

// PDF -> RGB utan dekorfärger som inte trycks (stanslinje, lack, punktskrift)
cv::Mat render_rgb(const Bytes& pdf_bytes, double dpi) {
    auto clean = pdf::analyze_spots(pdf_bytes, 0, pdf::default_spot_rules()).sanitized_pdf;
    auto doc = pdf::Document::open(clean);
    return pdf::render_page(*doc, 0, dpi);
}

void write_bytes(const fs::path& p, const Bytes& b) { write_file(p.string(), std::string(b.begin(), b.end())); }

json r3(double v) { return round_to(v, 3); }
json r3(const Rect& r) { return json::array({round_to(r[0], 3), round_to(r[1], 3), round_to(r[2], 3), round_to(r[3], 3)}); }

bool line_hits_rect(cv::Point2d p0, cv::Point2d p1, double x0, double y0, double x1, double y1) {
    // sampla linjen tätt (veck är långa raka linjer)
    for (int i = 0; i < 2000; ++i) {
        double t = -2 + 5.0 * i / 1999;
        double x = p0.x + t * (p1.x - p0.x), y = p0.y + t * (p1.y - p0.y);
        if (x0 <= x && x <= x1 && y0 <= y && y <= y1) return true;
    }
    return false;
}

cv::Matx33d to3(const cv::Matx23d& m) { return {m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2), 0, 0, 1}; }

}  // namespace

json make_case(const CaseOptions& o, const fs::path& out_dir) {
    Rng rng(o.seed);
    const auto& D = difficulty(o.difficulty);
    fs::create_directories(out_dir);
    json base = label_template(o.template_name);
    double W = base["size_mm"][0], H = base["size_mm"][1], dpi = o.dpi, ppm = dpi / 25.4;
    int rows = o.rows, cols = o.cols;

    write_bytes(out_dir / "reference.pdf", render_label_pdf(base));

    int na = o.n_artwork ? *o.n_artwork : int(rng.integers(0, 4));
    auto [mutated, art_defects] = mutate_label(base, rng, na, o.mutations);
    Bytes mut_pdf = render_label_pdf(mutated);

    // ---- tryck ----
    sim::PrintParams p;
    p.halftone = D.halftone;
    p.lpcm = rng.uniform(60, 80);
    for (auto& g : p.dot_gain) g = rng.uniform(D.gain[0], D.gain[1]);
    for (auto& d : p.density) d = rng.uniform(0.95, 1.05);
    for (auto& m : p.misreg_mm) m = {rng.uniform(-D.misreg, D.misreg), rng.uniform(-D.misreg, D.misreg)};
    p.misreg_mm[3] = {0.0, 0.0};
    double dpi_hi = D.halftone ? 1200 : dpi;
    cv::Mat label = sim::print_label(render_rgb(mut_pdf, dpi_hi), dpi_hi, dpi, p, rng);
    int lh = label.rows, lw = label.cols;
    cv::Mat die = sim::rounded_rect_mask(lw, lh, mutated["dieline"]["radius_mm"].get<double>() * ppm);

    const json* var_el = nullptr;
    for (auto& e : base["elements"])
        if (e["type"] == "variable") {
            var_el = &e;
            break;
        }
    std::optional<Rect> var_box;
    if (var_el) var_box = Rect{(*var_el)["rect_mm"][0], (*var_el)["rect_mm"][1], (*var_el)["rect_mm"][2], (*var_el)["rect_mm"][3]};

    // ---- ark ----
    double gap = 4.0, margin = 12.0;
    double sw_mm = cols * W + (cols - 1) * gap + 2 * margin, sh_mm = rows * H + (rows - 1) * gap + 2 * margin;
    int SW = int(std::lround(sw_mm * ppm)), SH = int(std::lround(sh_mm * ppm));
    cv::Mat sheet(SH, SW, CV_32FC3, cv::Scalar(0.93, 0.925, 0.86));
    json print_defects = json::array();
    struct Inst {
        int index, row, col, rotation;
        cv::Matx33d A;
    };
    std::vector<Inst> instances;
    int np_total = o.n_print ? *o.n_print : int(rng.integers(0, 5));
    std::set<int> defect_inst;
    if (np_total)
        for (int k : rng.sample(rows * cols, std::min(np_total, rows * cols))) defect_inst.insert(k);
    std::vector<Rect> avoid;
    for (auto& d : art_defects) avoid.push_back({d["bbox_ref_mm"][0], d["bbox_ref_mm"][1], d["bbox_ref_mm"][2], d["bbox_ref_mm"][3]});
    if (var_box) avoid.push_back(*var_box);
    // punktskrift: präglade punkter (från den muterade layouten) per instans
    struct Dot {
        double x, y;
        int cell, dot;
        double r;
    };
    std::vector<Dot> b_dots;
    for (auto& e : mutated["elements"])
        if (e["type"] == "braille") {
            for (auto& d : braille_dots_mm(e)) b_dots.push_back({d.x, d.y, d.cell, d.dot, e.value("dot_d_mm", 1.5) / 2});
            avoid.push_back(element_bbox_mm(e));
        }
    double emb_strength = rng.uniform(D.emboss[0], D.emboss[1]);
    cv::Point2d emb_light = rng.random() < 0.5 ? cv::Point2d(0, -1) : cv::Point2d(0, 1);
    std::optional<sim::ScannerModel> smodel;
    if (o.scanner != "legacy") {
        // scannermodellen bestämmer ljusriktning och präglingens skuggstyrka
        Rng mrng(o.seed * 7919 + 13);
        smodel = sim::make_model(o.scanner, mrng);
        emb_strength *= smodel->emboss_gain;
        emb_light = {0, smodel->light_sign};
    }
    std::map<int, int> emb_missing;
    if (!b_dots.empty() && rng.random() < D.emboss_missing_p) {
        int k = int(rng.integers(rows * cols)) + 1;
        emb_missing[k] = rng.index(b_dots.size());
    }
    cv::Mat edge_kernel = cv::Mat::ones(3, 3, CV_8U);
    int idx = 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            ++idx;
            cv::Mat img = label.clone();
            double dens = rng.uniform(0.97, 1.03);
            for (int y = 0; y < img.rows; ++y) {
                auto* row = img.ptr<cv::Vec3f>(y);
                for (int x = 0; x < img.cols; ++x)
                    for (int ch = 0; ch < 3; ++ch)
                        row[x][ch] = std::clamp(float(sim::PAPER[ch] - (sim::PAPER[ch] - row[x][ch]) * dens), 0.f, 1.f);
            }
            if (defect_inst.count(idx - 1)) {
                static const char* kinds[] = {"spot", "streak", "hickey"};
                std::string kind = kinds[rng.integers(0, 3)];
                auto res = sim::add_print_defect(img, ppm, kind, rng, {2.5, 2.5, W - 2.5, H - 2.5}, avoid);
                if (res) {
                    std::string k = kind == "spot" ? "Spot" : kind == "streak" ? "Streak" : "Hickey";
                    print_defects.push_back({{"id", "p" + std::to_string(print_defects.size() + 1)}, {"kind", k},
                                             {"accept", STRUCT_ANY}, {"systematic", false}, {"instances", {idx}},
                                             {"bbox_ref_mm", r3(res->box)}, {"polarity", res->polarity},
                                             {"strength", r3(res->strength)}, {"mutation", kind}});
                }
            }
            if (var_el) {
                double x = (*var_el)["rect_mm"][0], y = (*var_el)["rect_mm"][1], w = (*var_el)["rect_mm"][2], h = (*var_el)["rect_mm"][3];
                int x0 = int(std::lround(x * ppm)), y0 = int(std::lround(y * ppm)), x1 = int(std::lround((x + w) * ppm)),
                    y1 = int(std::lround((y + h) * ppm));
                // sudda platshållartexten med etikettens bakgrund (median av en ring runt rutan)
                std::array<std::vector<float>, 3> ring;
                auto take = [&](int ra, int rb) {
                    for (int ry = std::max(0, ra); ry < std::min(img.rows, rb); ++ry)
                        for (int xx = x0; xx < x1; ++xx)
                            for (int ch = 0; ch < 3; ++ch) ring[ch].push_back(img.at<cv::Vec3f>(ry, xx)[ch]);
                };
                take(y0 - 3, y0);
                take(y1, y1 + 3);
                cv::Vec3f fill = sim::PAPER;
                if (!ring[0].empty())
                    for (int ch = 0; ch < 3; ++ch) {
                        auto& v = ring[ch];
                        size_t n = v.size();
                        std::sort(v.begin(), v.end());
                        fill[ch] = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
                    }
                img(cv::Rect(x0, y0, x1 - x0, y1 - y0)).setTo(fill);
                char txt[64];
                std::snprintf(txt, sizeof txt, "LOT %d-%04d", int(rng.integers(1000, 9999)), idx);
                cv::Mat tm = cv::Mat::zeros(y1 - y0, x1 - x0, CV_8U);
                cv::putText(tm, txt, cv::Point(4, tm.rows - int(0.7 * ppm)), cv::FONT_HERSHEY_SIMPLEX, 0.085 * ppm / 2.0, 255,
                            std::max(1, int(0.12 * ppm)), cv::LINE_AA);
                for (int yy = 0; yy < tm.rows; ++yy)
                    for (int xx = 0; xx < tm.cols; ++xx) {
                        float a = tm.at<uchar>(yy, xx) / 255.f;
                        auto& v = img.at<cv::Vec3f>(y0 + yy, x0 + xx);
                        v = v * (1 - a) + cv::Vec3f(0.1f, 0.1f, 0.1f) * a;
                    }
            }
            if (!b_dots.empty()) {
                std::vector<cv::Point2d> keep;
                for (int j = 0; j < int(b_dots.size()); ++j)
                    if (!(emb_missing.count(idx) && emb_missing[idx] == j)) keep.push_back({b_dots[j].x * ppm, b_dots[j].y * ppm});
                sim::emboss(img, keep, b_dots[0].r * ppm, emb_strength, emb_light);
                if (emb_missing.count(idx)) {
                    const auto& d = b_dots[emb_missing[idx]];
                    print_defects.push_back({{"id", "p" + std::to_string(print_defects.size() + 1)}, {"kind", "Braille"},
                                             {"category", "Braille"}, {"accept", {"Braille"}}, {"systematic", false},
                                             {"instances", {idx}},
                                             {"bbox_ref_mm", {r3(d.x - d.r - 0.3), r3(d.y - d.r - 0.3), r3(2 * d.r + 0.6), r3(2 * d.r + 0.6)}},
                                             {"mutation", "emboss_missing"}, {"detail", {{"cell", d.cell}, {"dot", d.dot}}}});
                }
            }
            bool rot180 = rng.random() < D.inst_rot180_p;
            cv::Mat dmask = die;
            if (rot180) {
                cv::flip(img, img, -1);
                cv::flip(die, dmask, -1);
            }
            double ox = margin + c * (W + gap) + rng.uniform(-0.3, 0.3);
            double oy = margin + r * (H + gap) + rng.uniform(-0.3, 0.3);
            int ox_px = int(std::lround(ox * ppm)), oy_px = int(std::lround(oy * ppm));
            cv::Mat region = sheet(cv::Rect(ox_px, oy_px, lw, lh));
            img.copyTo(region, dmask);
            // stanskant: tunn skugga
            cv::Mat edge;
            cv::morphologyEx(dmask, edge, cv::MORPH_GRADIENT, edge_kernel);
            for (int y = 0; y < lh; ++y)
                for (int x = 0; x < lw; ++x)
                    if (edge.at<uchar>(y, x)) region.at<cv::Vec3f>(y, x) *= 0.82f;
            cv::Matx33d A = rot180 ? cv::Matx33d(-1, 0, ox_px + lw - 1, 0, -1, oy_px + lh - 1, 0, 0, 1)
                                   : cv::Matx33d(1, 0, ox_px, 0, 1, oy_px, 0, 0, 1);
            instances.push_back({idx, r, c, rot180 ? 180 : 0, A});
        }

    // ---- arkgeometri: rotation + skala + förskjutning ----
    double theta = rng.uniform(-D.rot, D.rot), scale = 1 + rng.uniform(-0.002, 0.002);
    double shx = rng.uniform(-2, 2) * ppm, shy = rng.uniform(-2, 2) * ppm;
    cv::Mat Gm = cv::getRotationMatrix2D(cv::Point2f(float(SW / 2.0), float(SH / 2.0)), theta, scale);
    cv::Matx23d G(Gm.at<double>(0, 0), Gm.at<double>(0, 1), Gm.at<double>(0, 2) + shx, Gm.at<double>(1, 0), Gm.at<double>(1, 1),
                  Gm.at<double>(1, 2) + shy);
    cv::Vec3f lid(0.97f, 0.97f, 0.97f);
    json folds = json::array();
    struct V1Fold {
        cv::Point2d p0, p1;
        double width_mm, amp_mm;
    };
    std::vector<V1Fold> v1folds;
    double fp = o.fold_p ? *o.fold_p : D.fold_p;
    std::array<double, 2> mtf = {rng.uniform(D.mtf[0], D.mtf[1]), rng.uniform(D.mtf[0], D.mtf[1])};
    double noise = rng.uniform(D.noise[0], D.noise[1]);
    cv::Mat scan_u8;
    std::unique_ptr<sim::Geometry> geo;
    std::vector<std::array<int, 4>> dust;
    if (!smodel) {
        // ---- simulator v1: affin placering, geometriskt veck, enkel scanner ----
        cv::Mat scan;
        cv::warpAffine(sheet, scan, G, sheet.size(), cv::INTER_CUBIC, cv::BORDER_CONSTANT, cv::Scalar(lid[0], lid[1], lid[2]));
        sheet.release();
        if (rng.random() < fp) {
            bool vertical = rng.random() < 0.5;
            double a = rng.uniform(-3, 3) * M_PI / 180;
            cv::Point2d p0, p1;
            if (vertical) {
                double x = rng.uniform(0.3, 0.7) * SW;
                p0 = {x - std::tan(a) * SH / 2, 0.0};
                p1 = {x + std::tan(a) * SH / 2, double(SH)};
            } else {
                double y = rng.uniform(0.3, 0.7) * SH;
                p0 = {0.0, y - std::tan(a) * SW / 2};
                p1 = {double(SW), y + std::tan(a) * SW / 2};
            }
            double wpx = rng.uniform(0.6, 1.6) * ppm, st = rng.uniform(0.06, 0.2), amp = rng.uniform(0.05, 0.25) * ppm;
            scan = sim::fold_v1(scan, p0, p1, wpx, st, amp);
            v1folds.push_back({p0, p1, wpx / ppm, amp / ppm});
            folds.push_back({{"p0_px", {p0.x, p0.y}}, {"p1_px", {p1.x, p1.y}}, {"width_mm", r3(wpx / ppm)}, {"strength", r3(st)},
                             {"amp_mm", r3(amp / ppm)}, {"band_mm", r3(wpx / ppm * 2.5)}});
        }
        scan_u8 = sim::scanner_v1(scan, rng, mtf, noise);
    } else {
        // ---- simulator v2: flatbäddsscanner (CIS/CCD) med fysiskt veck ----
        std::vector<sim::Fold> geo_folds;
        if (rng.random() < fp) {
            std::array<double, 2> lr = o.fold_lift ? std::array<double, 2>{*o.fold_lift, *o.fold_lift} : D.lift;
            auto f = sim::random_fold(rng, SW, SH, ppm, lr);
            geo_folds.push_back(f);
            if (f.crack) sim::crack_ink(sheet, G, f, ppm, rng);
            folds.push_back({{"p0_px", {f.p0.x, f.p0.y}}, {"p1_px", {f.p1.x, f.p1.y}}, {"kind", f.kind}, {"lift_mm", r3(f.lift_mm)},
                             {"width_mm", r3(f.width_mm)}, {"crack", f.crack}, {"band_mm", r3(2 * f.band_mm())}});
        }
        auto res = sim::scan(sheet, G, geo_folds, *smodel, rng, ppm, mtf, noise, lid);
        sheet.release();
        scan_u8 = std::move(res.rgb);
        geo = std::move(res.geo);
        dust = std::move(res.dust);
    }
    image::write_tiff((out_dir / "sample.tif").string(), scan_u8, dpi);
    scan_u8.release();

    // ---- facit ----
    json inst_truth = json::array(), expect_uncertain = json::array(), scan_artifacts = json::array();
    cv::Matx33d G3 = to3(G);
    for (auto& inst : instances) {
        cv::Matx33d Hm = G3 * inst.A;   // etikett-px -> scan-px
        if (geo) {
            // v2: bästa affina approximation av den verkliga avbildningen över etikettens yta
            cv::Mat P(81, 3, CV_64F), Q(81, 2, CV_64F);
            int k = 0;
            for (int iy = 0; iy < 9; ++iy)
                for (int ix = 0; ix < 9; ++ix, ++k) {
                    double gx = lw * ix / 8.0, gy = lh * iy / 8.0;
                    P.at<double>(k, 0) = gx, P.at<double>(k, 1) = gy, P.at<double>(k, 2) = 1;
                    cv::Vec3d s = inst.A * cv::Vec3d(gx, gy, 1);
                    auto q = geo->to_scan(s[0], s[1], G);
                    Q.at<double>(k, 0) = q.x, Q.at<double>(k, 1) = q.y;
                }
            cv::Mat sol;
            cv::solve(P, Q, sol, cv::DECOMP_SVD);
            Hm = cv::Matx33d(sol.at<double>(0, 0), sol.at<double>(1, 0), sol.at<double>(2, 0), sol.at<double>(0, 1),
                             sol.at<double>(1, 1), sol.at<double>(2, 1), 0, 0, 1);
        }
        // v1: vecket förskjuter hela arkhalvan: lägg in förskjutningen vid etikettens mitt i facit
        for (auto& f : v1folds) {
            cv::Vec3d cc = Hm * cv::Vec3d(lw / 2.0, lh / 2.0, 1);
            cv::Point2d c(cc[0] / cc[2], cc[1] / cc[2]);
            cv::Point2d dv = f.p1 - f.p0;
            double nn = std::hypot(dv.x, dv.y);
            cv::Point2d nv(-dv.y / nn, dv.x / nn);
            double s = (c - f.p0).dot(nv), wpx = f.width_mm * ppm, dd = f.amp_mm * ppm * std::tanh(s / (3 * wpx));
            Hm = cv::Matx33d(1, 0, dd * nv.x, 0, 1, dd * nv.y, 0, 0, 1) * Hm;
        }
        json rec = {{"index", inst.index}, {"row", inst.row}, {"col", inst.col}, {"rotation", inst.rotation}};
        json tr = json::array();
        for (int i = 0; i < 9; ++i) tr.push_back(Hm.val[i]);
        rec["transform_ref_to_sample"] = tr;
        cv::Matx33d Hinv = Hm.inv();
        json fl = json::array();
        for (auto& f : folds) {
            auto proj = [&](const json& pt) {
                cv::Vec3d v = Hinv * cv::Vec3d(pt[0].get<double>(), pt[1].get<double>(), 1);
                return cv::Point2d(v[0] / v[2] / ppm, v[1] / v[2] / ppm);
            };
            auto a = proj(f["p0_px"]), b = proj(f["p1_px"]);
            double m_ = f["band_mm"].get<double>() / 2;
            if (line_hits_rect(a, b, -m_, -m_, W + m_, H + m_))
                fl.push_back({{"p0_mm", {r3(a.x), r3(a.y)}}, {"p1_mm", {r3(b.x), r3(b.y)}}, {"band_mm", f["band_mm"]}});
        }
        if (!fl.empty()) expect_uncertain.push_back({{"instance", inst.index}, {"cause", "fold"}, {"lines", fl}});
        rec["folds"] = fl;
        inst_truth.push_back(rec);
        for (auto& d : dust) {
            cv::Vec3d cc = Hinv * cv::Vec3d(d[0] + d[2] / 2.0, d[1] + d[3] / 2.0, 1);
            double cx = cc[0] / cc[2] / ppm, cy = cc[1] / cc[2] / ppm;
            if (-1 <= cx && cx <= W + 1 && -1 <= cy && cy <= H + 1) {
                double rr = std::max(d[2], d[3]) / 2.0 / ppm + 0.1;
                scan_artifacts.push_back({{"instance", inst.index}, {"kind", "dust"},
                                          {"bbox_ref_mm", {r3(cx - rr), r3(cy - rr), r3(2 * rr), r3(2 * rr)}}});
            }
        }
    }

    json defects = art_defects;
    for (auto& d : print_defects) defects.push_back(d);
    json sp = p.to_json();
    sp["mtf_sigma_px"] = mtf;
    sp["noise_sigma"] = noise;
    sp["emboss_strength"] = emb_strength;
    sp["scanner"] = o.scanner;
    if (smodel) sp["scanner_model"] = smodel->to_json();
    json truth = {{"schema", "printcmp.truth/1"},
                  {"seed", o.seed},
                  {"generator", GENERATOR_VERSION},
                  {"template", o.template_name},
                  {"difficulty", o.difficulty},
                  {"reference", "reference.pdf"},
                  {"sample", "sample.tif"},
                  {"dpi", o.dpi},
                  {"label_size_mm", {W, H}},
                  {"sheet",
                   {{"rows", rows}, {"cols", cols}, {"size_px", {SW, SH}}, {"rotation_deg", round_to(theta, 4)},
                    {"scale", round_to(scale, 5)}, {"folds", folds}}},
                  {"instances", inst_truth},
                  {"defects", defects},
                  {"expect_uncertain", expect_uncertain},
                  {"scan_artifacts", scan_artifacts},
                  {"variable_mask_mm", var_box ? json(*var_box) : json(nullptr)},
                  {"sim_params", sp}};
    write_file((out_dir / "truth.json").string(), truth.dump(2));

    json job = {{"schema", "printcmp.jobspec/1"},
                {"seed", 1},
                {"reference", {{"path", "reference.pdf"}}},
                {"sample", {{"path", "sample.tif"}, {"kind", "scan"}}},
                {"work_dpi", o.dpi},
                {"instances",
                 {{"mode", "multi"},
                  {"expected_layout", {{"rows", rows}, {"cols", cols}}},
                  {"rotations", D.inst_rot180_p > 0 ? json{0, 180} : json{0}}}},
                {"masks", var_box ? json::array({{{"reason", "variable"}, {"rect_mm", *var_box}}}) : json::array()},
                {"tolerance_profile", "label_standard"},
                {"outputs", {{"dir", "out"}}}};
    write_file((out_dir / "jobspec.json").string(), job.dump(2));
    return truth;
}

}  // namespace pc::testgen
