// Etiketterna på ett ark jämförda med varandra, utan original: genererade ark med kända tryckfel.
#include <cstdio>
#include <filesystem>
#include <opencv2/imgproc.hpp>

#include "printcmp/compare/peers.hpp"
#include "printcmp/image/image.hpp"
#include "printcmp/testgen/cases.hpp"
#include "test.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

struct Case {
    json truth;
    image::Scan scan;
};

Case generate(const std::string& name, testgen::CaseOptions o) {
    auto d = fs::temp_directory_path() / ("printcmp-test-peers-" + name);
    fs::remove_all(d);
    fs::create_directories(d);
    Case c;
    c.truth = testgen::make_case(o, d);
    c.scan = image::read_scan((d / "sample.tif").string());
    return c;
}

// Vid fel: arket i fjärdedels storlek med de hittade etiketterna i temp-katalogen
void dump(const cv::Mat& sheet, const std::vector<compare::DetectedLabel>& found, const std::string& name) {
    cv::Mat small;
    cv::resize(sheet, small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
    for (auto& d : found)
        for (int k = 0; k < 4; ++k)
            cv::line(small, cv::Point((int)(d.corners[k].x / 4), (int)(d.corners[k].y / 4)),
                     cv::Point((int)(d.corners[(k + 1) % 4].x / 4), (int)(d.corners[(k + 1) % 4].y / 4)), cv::Scalar(255, 0, 0), 2);
    auto p = fs::temp_directory_path() / ("printcmp-test-peers-" + name);
    image::write_png(p.string(), small);
    std::printf("    bild: %s\n", p.string().c_str());
}

bool overlaps(const compare::PeerFinding& f, const json& bb, double pad) {
    double x = bb[0], y = bb[1], w = bb[2], h = bb[3];
    return !(f.x_mm > x + w + pad || x > f.x_mm + f.w_mm + pad || f.y_mm > y + h + pad || y > f.y_mm + f.h_mm + pad);
}

const compare::PeerLabel* at(const compare::PeerResult& r, int row, int col) {
    for (auto& l : r.labels)
        if (l.row == row && l.col == col) return &l;
    return nullptr;
}

// Ligger punkten (mm) i ett av facitets veckband för instansen? Där är fel osäkra, som i utvärderingen.
bool in_fold(const json& inst, double x, double y) {
    for (auto& f : inst.value("folds", json::array())) {
        double x0 = f["p0_mm"][0], y0 = f["p0_mm"][1], x1 = f["p1_mm"][0], y1 = f["p1_mm"][1];
        double n = std::hypot(x1 - x0, y1 - y0), d = std::abs((x1 - x0) * (y0 - y) - (x0 - x) * (y1 - y0)) / std::max(n, 1e-9);
        if (d < f["band_mm"].get<double>() / 2 + 1.0) return true;
    }
    return false;
}

// Enskilda tryckfel ska hittas på rätt etikett (utom i veckband); utanför fel och veck ska fynden vara få
void check_case(const Case& c, int rows, int cols) {
    compare::PeerOptions opt;
    auto r = compare::compare_peers(c.scan.rgb, c.scan.dpi, opt);
    std::printf("    %zu etiketter, ark %.2f°, %dx%d px\n", r.labels.size(), r.sheet_angle_deg, r.label_size.width, r.label_size.height);
    CHECK_EQ((int)r.labels.size(), rows * cols);
    if ((int)r.labels.size() != rows * cols) {
        std::vector<compare::DetectedLabel> ds;
        for (auto& l : r.labels) ds.push_back({l.corners, l.angle_deg});
        dump(c.scan.rgb, ds, "found-" + std::to_string(rows) + "x" + std::to_string(cols) + ".png");
        return;
    }
    CHECK_EQ(r.consensus_rgb.size(), r.label_size);
    for (auto& t : c.truth["instances"]) {
        auto* l = at(r, t["row"], t["col"]);
        CHECK(l != nullptr);
        if (!l) continue;
        double want = t.value("rotation", 0.0), got = l->angle_deg;
        double diff = std::abs(std::fmod(got - want - c.truth["sheet"].value("rotation_deg", 0.0) + 540.0, 360.0) - 180.0);
        std::printf("    etikett %d (%d,%d): %.2f° (facit %.0f°), likhet %.3f, rms %.2f, %zu fynd\n", l->index, l->row, l->col, got,
                    want, l->similarity, l->registration_rms_px, l->findings.size());
        CHECK(diff < 1.5);
    }
    int found = 0, total = 0;
    std::vector<std::pair<int, json>> defects;   // instans (1..), facitruta
    for (auto& d : c.truth["defects"]) {
        if (d.value("systematic", true) || !d.contains("bbox_ref_mm")) continue;
        for (int inst : d["instances"]) {
            defects.push_back({inst, d["bbox_ref_mm"]});
            auto& t = c.truth["instances"][inst - 1];
            auto* l = at(r, t["row"], t["col"]);
            if (!l) continue;
            bool hit = false;
            for (auto& f : l->findings) hit |= f.kind != "Uncertain" && overlaps(f, d["bbox_ref_mm"], 1.5);
            auto& bb = d["bbox_ref_mm"];
            bool fold = in_fold(t, bb[0].get<double>() + bb[2].get<double>() / 2, bb[1].get<double>() + bb[3].get<double>() / 2);
            std::printf("    fel %s (%s) på etikett %d: %s\n", d.value("id", "").c_str(), d.value("kind", "").c_str(), l->index,
                        hit ? "hittat" : fold ? "missat i veckband (godtas)" : "MISSAT");
            if (fold && !hit) continue;
            ++total;
            found += hit;
        }
    }
    CHECK_EQ(found, total);
    for (auto& t : c.truth["instances"]) {
        auto* l = at(r, t["row"], t["col"]);
        int n = 0;
        for (auto& f : l->findings) {
            if (f.kind == "Uncertain" || in_fold(t, f.x_mm + f.w_mm / 2, f.y_mm + f.h_mm / 2)) continue;
            bool real = false;
            for (auto& [inst, bb] : defects) real |= inst == t["index"].get<int>() && overlaps(f, bb, 1.5);
            if (real) continue;
            ++n;
            std::printf("      falskt på %d: %s %.1f,%.1f %.1fx%.1f mm ΔL %.1f %s\n", l->index, f.kind.c_str(), f.x_mm, f.y_mm, f.w_mm,
                        f.h_mm, f.delta_l, f.detail.c_str());
        }
        CHECK(n <= 1);
    }
}

}  // namespace

TEST(peers_find_single_print_defects) {
    testgen::CaseOptions o;
    o.template_name = "oat_drink";
    o.seed = 21;
    o.rows = 2;
    o.cols = 3;
    o.dpi = 300;
    o.n_artwork = 0;
    o.n_print = 3;
    o.fold_p = 0.0;
    o.rotation_deg = 2.0;
    o.difficulty = "easy";
    check_case(generate("easy", o), 2, 3);
}

TEST(peers_with_skew_scale_and_folds) {
    testgen::CaseOptions o;
    o.template_name = "spice";
    o.seed = 1;
    o.rows = 3;
    o.cols = 2;
    o.dpi = 300;
    o.n_artwork = 0;
    o.n_print = 2;
    o.fold_p = 1.0;
    o.rotation_deg = -5.5;
    o.scale = 1.01;
    o.difficulty = "hard";
    o.scanner = "cis";
    check_case(generate("hard", o), 3, 2);
}

TEST(peers_detect_without_expected_count) {
    testgen::CaseOptions o;
    o.template_name = "nutrition_grid";
    o.seed = 3;
    o.rows = 2;
    o.cols = 2;
    o.dpi = 300;
    o.n_artwork = 0;
    o.n_print = 0;
    o.rotation_deg = 12.0;
    auto c = generate("detect", o);
    auto found = compare::detect_labels(c.scan.rgb, c.scan.dpi);
    if (found.size() != 4) dump(c.scan.rgb, found, "found-detect.png");
    CHECK_EQ((int)found.size(), 4);
    compare::PeerOptions opt;
    opt.expected_count = 4;
    CHECK_EQ((int)compare::detect_labels(c.scan.rgb, c.scan.dpi, opt).size(), 4);
}

TEST(peers_label_upside_down) {
    testgen::CaseOptions o;
    o.template_name = "oat_drink";
    o.seed = 21;
    o.rows = 2;
    o.cols = 3;
    o.dpi = 300;
    o.n_artwork = 0;
    o.n_print = 3;
    o.fold_p = 0.0;
    o.rotation_deg = 0.0;
    o.scale = 1.0;
    o.difficulty = "easy";
    auto c = generate("flip", o);
    // vänd etiketten med det första enskilda felet ett halvt varv på plats (rutan kring den, med lite marginal)
    int inst = -1;
    for (auto& d : c.truth["defects"])
        if (!d.value("systematic", true) && d.contains("bbox_ref_mm")) {
            inst = d["instances"][0];
            break;
        }
    CHECK(inst > 0);
    if (inst <= 0) return;
    auto& t = c.truth["instances"][inst - 1];
    auto& T = t["transform_ref_to_sample"];
    double ppm_ref = c.truth["dpi"].get<double>() / 25.4, w = c.truth["label_size_mm"][0].get<double>() * ppm_ref,
           h = c.truth["label_size_mm"][1].get<double>() * ppm_ref;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (auto [px, py] : {std::pair{0.0, 0.0}, {w, 0.0}, {w, h}, {0.0, h}}) {
        double d = T[6].get<double>() * px + T[7].get<double>() * py + T[8].get<double>();
        double x = (T[0].get<double>() * px + T[1].get<double>() * py + T[2].get<double>()) / d;
        double y = (T[3].get<double>() * px + T[4].get<double>() * py + T[5].get<double>()) / d;
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    }
    int m = (int)(c.scan.dpi / 25.4);
    cv::Rect r((int)x0 - m, (int)y0 - m, (int)(x1 - x0) + 2 * m, (int)(y1 - y0) + 2 * m);
    r &= cv::Rect(0, 0, c.scan.rgb.cols, c.scan.rgb.rows);
    cv::Mat roi = c.scan.rgb(r), flipped;
    cv::flip(roi, flipped, -1);
    flipped.copyTo(roi);
    t["rotation"] = 180;
    check_case(c, 2, 3);
}
