// Röktester för hela kedjan: testmaterial ur generatorn,
// analys i kärnan och utvärdering mot facit.
#include <filesystem>
#include <opencv2/imgproc.hpp>

#include "printcmp/compare/reference.hpp"
#include "printcmp/compare/datacheck.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/eval/eval.hpp"
#include "printcmp/image/image.hpp"
#include "printcmp/jobspec/runner.hpp"
#include "printcmp/scan/register.hpp"
#include "printcmp/scan/folds.hpp"
#include "printcmp/scan/sheet_folds.hpp"
#include "printcmp/scan/sheet_folds.hpp"
#include "test.hpp"
#include "printcmp/testgen/cases.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

fs::path tmpdir(const std::string& name) {
    auto d = fs::temp_directory_path() / ("printcmp-test-" + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_pdf(const fs::path& p, const json& spec) {
    auto b = testgen::render_label_pdf(spec);
    write_file(p.string(), std::string(b.begin(), b.end()));
}

json& element(json& spec, const std::string& id) {
    for (auto& e : spec["elements"])
        if (e["id"] == id) return e;
    throw std::runtime_error("element saknas: " + id);
}

std::shared_ptr<const compare::ReferenceModel> analyse(const json& spec, double dpi = 300) {
    compare::ReferenceOptions ro;
    ro.dpi = dpi;
    return compare::analyse_reference(testgen::render_label_pdf(spec), "test.pdf", ro);
}

// PDF mot PDF med en JobSpec; returnerar result.json
json run_pdf_pair(const std::string& name, const json& a, const json& b, json extra = json::object()) {
    auto d = tmpdir(name);
    write_pdf(d / "ref.pdf", a);
    write_pdf(d / "smp.pdf", b);
    json js = {{"schema", "printcmp.jobspec/1"}, {"reference", {{"path", "ref.pdf"}}}, {"sample", {{"path", "smp.pdf"}}},
               {"work_dpi", 300}, {"outputs", {{"dir", "out"}, {"layers", false}, {"report_html", ""}}}};
    js.update(extra);
    write_file((d / "jobspec.json").string(), js.dump());
    return jobspec::run_file((d / "jobspec.json").string()).result;
}

std::vector<json> real(const json& res) {
    std::vector<json> out;
    for (auto& d : res["differences"])
        if (d["kind"] != "Uncertain") out.push_back(d);
    return out;
}

json layered() {
    json s = testgen::label_template("oat_drink");
    for (auto& e : s["elements"]) {
        if (e["id"] == "ingr1" || e["id"] == "ingr2") e["layer"] = "Ingredienser SV";
        else if (e["type"] == "text") e["layer"] = "Text";
    }
    return s;
}

json run_and_eval_doc(const fs::path& c) {
    jobspec::run_file((c / "jobspec.json").string());
    return eval::evaluate_doc_case(json::parse(read_file((c / "out" / "result.json").string())),
                                   json::parse(read_file((c / "truth.json").string())));
}

}  // namespace

// ------------------------------------------------------------------ original

TEST(spots_dieline_and_varnish_found_and_removed) {
    auto pdf = testgen::render_label_pdf(testgen::label_template("oat_drink"));
    auto a = pdf::analyze_spots(pdf, 0, pdf::default_spot_rules());
    CHECK(!a.polygons("dieline").empty());
    CHECK(!a.polygons("varnish").empty());
    CHECK(std::count(a.spot_names.begin(), a.spot_names.end(), "CutContour") == 1);
    CHECK(std::count(a.spot_names.begin(), a.spot_names.end(), "PANTONE 347 C") == 1);
    CHECK(!a.sanitized_pdf.empty());
}

TEST(braille_roundtrip_swedish) {
    for (std::string t : {"havredryck", "Fett 1,5 g", "åäö - salt."}) CHECK_EQ(braille::cells_to_text(braille::text_to_cells(t)), t);
}

TEST(braille_decode_from_pdf) {
    auto m = analyse(testgen::label_template("nutrition_grid"));
    CHECK_EQ(m->braille.size(), size_t(1));
    if (m->braille.empty()) return;
    CHECK_EQ(m->braille[0].text(), std::string("havredryck"));
    CHECK_NEAR(m->braille[0].pitch / m->ppm, 2.5, 0.05);
    CHECK_NEAR(m->braille[0].cell_pitch / m->ppm, 6.0, 0.05);
}

TEST(tables_ruled_and_unruled) {
    for (auto [name, kind] : {std::pair<std::string, std::string>{"nutrition_grid", "ruled_grid"}, {"nutrition_open", "unruled"}}) {
        auto m = analyse(testgen::label_template(name));
        CHECK_EQ(m->tables.size(), size_t(1));
        if (m->tables.empty()) continue;
        const auto& t = m->tables[0];
        CHECK_EQ(t.kind, kind);
        CHECK_EQ(t.n_rows(), 8);
        CHECK_EQ(t.n_cols(), 2);
        CHECK_EQ(t.cells.at({2, 1}), std::string("1,5 g"));
        CHECK_EQ(t.cells.at({3, 0}), std::string("varav mättat fett"));
    }
}

TEST(tables_none_in_paragraphs) { CHECK(analyse(testgen::label_template("oat_drink"))->tables.empty()); }

TEST(outlined_text) {
    auto m = analyse(testgen::label_template("nutrition_grid"));
    std::vector<const artwork::Obj*> ot;
    for (auto& o : m->objs)
        if (o.kind == "OutlinedText") ot.push_back(&o);
    CHECK_EQ(ot.size(), size_t(1));
    if (!ot.empty()) CHECK_EQ(ot[0]->glyphs.size(), size_t(12));   // "Havrebryggan"
}

TEST(ciede2000_sharma_pair1) {
    CHECK_NEAR(image::delta_e_2000(cv::Vec3f(50.0f, 2.6772f, -79.7751f), cv::Vec3f(50.0f, 0.0f, -82.7485f)), 2.0425, 1e-3);
}

TEST(registration_global_shift_recovered) {
    cv::Mat ref(600, 800, CV_32F);
    cv::RNG(0).fill(ref, cv::RNG::UNIFORM, 0.0, 1.0);
    cv::GaussianBlur(ref, ref, cv::Size(0, 0), 3);
    cv::normalize(ref, ref, 0, 1, cv::NORM_MINMAX);
    cv::Mat smp;
    cv::warpAffine(ref, smp, cv::Matx23f(1, 0, 7.3f, 0, 1, -4.6f), cv::Size(900, 700), cv::INTER_LINEAR, cv::BORDER_REFLECT);
    cv::Mat valid(ref.size(), CV_8U, cv::Scalar(1));
    auto [H, rms, n] = scan::refine_global(ref, smp, Mat3{}, valid, 128);
    CHECK(n > 10);
    CHECK_NEAR(H(0, 2), 7.3, 0.15);
    CHECK_NEAR(H(1, 2), -4.6, 0.15);
}

// ------------------------------------------------------------------ PDF mot PDF

TEST(pdf_identical_has_no_differences) {
    auto s = testgen::label_template("oat_drink");
    CHECK(real(run_pdf_pair("identical", s, s)).empty());
}

TEST(pdf_changed_character_is_found) {
    auto s = testgen::label_template("oat_drink"), m = s;
    element(m, "ingr1")["text"] = text::replace_all(element(m, "ingr1")["text"], "10 %", "18 %");
    auto hits = real(run_pdf_pair("changed", s, m));
    CHECK(!hits.empty());
    std::string texts;
    for (auto& d : hits)
        for (auto& o : d.value("objects", json::array())) texts += o.value("text", "") + " ";
    CHECK(texts.find("10") != std::string::npos);
}

TEST(layers_list_and_filter) {
    auto pdf = testgen::render_label_pdf(layered());
    auto info = pdf::list_layers(pdf, 0);
    std::set<std::string> names;
    for (auto& g : info.ocgs) names.insert(g.name);
    CHECK((names == std::set<std::string>{"Text", "Ingredienser SV"}));
    auto full = pdf::render(pdf, 0, 150).rgb;
    std::map<std::string, bool> dec;
    auto f2 = pdf::filter_layers(pdf, 0, {}, {"Text"}, &dec);
    CHECK((dec == std::map<std::string, bool>{{"Text", false}, {"Ingredienser SV", true}}));
    auto part = pdf::render(f2, 0, 150);
    cv::Mat diff;
    cv::absdiff(part.rgb, full, diff);
    CHECK(cv::sum(diff)[0] > 0);   // det dolda lagret syns inte längre
    std::set<std::string> words;
    for (auto& o : artwork::build_objects(f2, 0, part.geom, {}))
        if (o.kind == "Text") words.insert(o.text);
    CHECK(words.count("Ingredienser:") == 1);
    CHECK(words.count("Ekologisk") == 0);
}

TEST(layers_excluded_change_is_ignored) {
    auto a = layered(), b = a;
    element(b, "ingr1")["text"] = text::replace_all(element(b, "ingr1")["text"], "10 %", "18 %");
    CHECK(real(run_pdf_pair("layers_excl", a, b, {{"layers", {{"exclude", {"Ingredienser.*"}}}}})).empty());
    auto res = run_pdf_pair("layers_incl", a, b);
    bool tc = false;
    for (auto& d : res["differences"]) tc |= d.contains("text_change") && !d["text_change"].empty();
    CHECK(tc);
}

TEST(text_change_and_text_only) {
    auto a = testgen::label_template("oat_drink"), b = a;
    element(b, "ingr1")["text"] = text::replace_all(element(b, "ingr1")["text"], "10 %", "18 %");
    // vit text på vit botten: syns inte i tryck, men finns i textlagret
    json hidden = {{"id", "hidden"}, {"type", "text"}, {"text", "Batch A"}, {"font", "DejaVuSans"}, {"size_pt", 5},
                   {"pos_mm", {45, 33}}, {"color", {{"cmyk", {0, 0, 0, 0}}}}};
    a["elements"].push_back(hidden);
    hidden["text"] = "Batch B";
    b["elements"].push_back(hidden);
    auto res = run_pdf_pair("textonly", a, b);
    bool ten = false, all_confirmed = true;
    std::vector<std::pair<std::string, std::string>> only;
    for (auto& d : res["differences"]) {
        for (auto& tc : d.value("text_change", json::array()))
            ten |= tc["from"] == "10" && tc["to"] == "18";
        if (d.value("text_only", false))
            for (auto& tc : d["text_change"]) only.push_back({tc["from"], tc["to"]});
        all_confirmed &= d["certainty"]["level"] == "confirmed";
    }
    CHECK(ten);
    CHECK((only == std::vector<std::pair<std::string, std::string>>{{"A", "B"}}));
    CHECK(all_confirmed);
}

TEST(categories_table_outline_braille) {
    auto a = testgen::label_template("nutrition_grid"), b = a;
    element(b, "nut")["rows"][3][1] = "0,7 g";
    element(b, "nut")["skip_rules"] = {5};
    element(b, "brand")["text"] = "Havrebrygqan";
    element(b, "braille")["text"] = "havredrvck";
    auto res = run_pdf_pair("categories", a, b);
    std::set<std::pair<std::string, std::string>> got;
    for (auto& d : res["differences"]) got.insert({d["kind"], d.value("category", "")});
    CHECK(got.count({"Changed", "TableCell"}));
    CHECK(got.count({"Missing", "TableRule"}));
    CHECK(got.count({"Changed", "OutlinedText"}));
    CHECK(got.count({"Braille", "Braille"}));
    for (auto& d : res["differences"]) {
        if (d.value("category", "") == "TableCell") {
            CHECK_EQ(d["table"]["row"].get<int>(), 3);
            CHECK_EQ(d["table"]["col"].get<int>(), 1);
        }
        if (d.value("category", "") == "TableRule") CHECK((d["table"]["between_rows"] == json{4, 5}));
        if (d["kind"] == "Braille") {
            CHECK_EQ(d["braille"]["expected"].get<std::string>(), std::string("y"));
            CHECK_EQ(d["braille"]["read"].get<std::string>(), std::string("v"));
        }
    }
    CHECK_EQ(res["differences"].size(), size_t(4));
}

// ------------------------------------------------------------------ veck (arknivå)

namespace {

const double PPM = 600 / 25.4;

// 2 kolumner x 4 rader, etiketter 80 x 50 mm med 5 mm mellanrum, ingen rotation
std::vector<scan::SheetLabel> fold_states() {
    std::vector<scan::SheetLabel> st;
    for (int k = 0; k < 8; ++k) {
        int col = k % 2, row = k / 2;
        Mat3 H;
        H(0, 2) = (5 + col * 85) * PPM;
        H(1, 2) = (5 + row * 55) * PPM;
        st.push_back({k + 1, H, {}});
    }
    return st;
}

json sliver(int inst, double x, double y) {
    return {{"instance", inst}, {"kind", "Missing"}, {"severity", "Minor"}, {"bbox_px", {x * PPM, y * PPM, 0.15 * PPM, 0.6 * PPM}},
            {"bbox_mm", {x, y, 0.15, 0.6}}, {"area_mm2", 0.09}, {"objects", json::array()}};
}

std::vector<json> run_collinear(const std::vector<json>& diffs) {
    cv::Mat valid(int(50 * PPM), int(80 * PPM), CV_8U, cv::Scalar(1));
    json io = json::array();
    for (int k = 1; k <= 8; ++k) io.push_back({{"index", k}, {"folds", json::array()}});
    return scan::collinear_folds(diffs, fold_states(), valid, PPM, PPM, io);
}

int count_kind(const std::vector<json>& v, const std::string& k, bool equal) {
    int n = 0;
    for (auto& d : v) n += (d["kind"] == k) == equal;
    return n;
}

}  // namespace

TEST(folds_collinear_slivers_become_fold_zone) {
    // ett lodrätt veck vid arkets x = 30 mm korsar etiketterna i vänster kolumn (x = 25 mm i etiketten)
    auto out = run_collinear({sliver(1, 25, 10), sliver(3, 25, 32), sliver(5, 25, 4), sliver(7, 25, 41)});
    CHECK_EQ(count_kind(out, "Uncertain", false), 0);
    std::vector<int> insts;
    for (auto& d : out) insts.push_back(d["instance"]);
    std::sort(insts.begin(), insts.end());
    CHECK((insts == std::vector<int>{1, 3, 5, 7}));
}

TEST(folds_same_position_in_column_is_artwork) {
    std::vector<json> d;
    for (int i : {1, 3, 5, 7}) d.push_back(sliver(i, 25, 20));
    auto out = run_collinear(d);
    CHECK_EQ(out.size(), size_t(4));
    CHECK_EQ(count_kind(out, "Uncertain", true), 0);
}

TEST(folds_repeating_edges_in_column_are_artwork) {
    // en flyttad streckkod: smala fynd längs samma kant på flera höjder, i varje etikett i kolumnen
    std::vector<json> d;
    for (int i : {1, 3, 5, 7})
        for (double y : {18, 22, 27, 31}) d.push_back(sliver(i, 25, y));
    auto out = run_collinear(d);
    CHECK_EQ(out.size(), size_t(16));
    CHECK_EQ(count_kind(out, "Uncertain", true), 0);
}

TEST(folds_two_labels_are_not_enough) {
    CHECK_EQ(count_kind(run_collinear({sliver(1, 25, 10), sliver(3, 25, 32)}), "Uncertain", true), 0);
}

TEST(folds_halfwidth_follows_sideways_shift) {
    // CCD-perspektiv: innehållet förskjuts i sidled ut till 6 mm från vecket, utan oskärpa eller skugga
    double ppm = 300 / 25.4;
    cv::Mat r(600, 600, CV_32F);
    cv::RNG(3).fill(r, cv::RNG::UNIFORM, 0.0, 1.0);
    cv::Mat L = (r > 0.97) / 255 * 80;
    L.convertTo(L, CV_32F);
    cv::GaussianBlur(L, L, cv::Size(0, 0), 1.2);
    cv::Mat mx(600, 600, CV_32F), my(600, 600, CV_32F);
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 600; ++x) {
            double s = std::fabs(y - 300) / ppm, u = s < 6 ? 3.0 * (1 - s / 6) : 0.0;   // > 1 px ut till 4 mm
            mx.at<float>(y, x) = float(x - u);
            my.at<float>(y, x) = float(y);
        }
    cv::Mat S;
    cv::remap(L, S, mx, my, cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    cv::Mat valid(L.size(), CV_8U, cv::Scalar(1)), D = cv::Mat::zeros(L.size(), CV_32F);
    scan::Fold f{{0, 300}, {600, 300}, 2 * ppm};
    cv::Mat dL = S - L;
    CHECK_EQ(scan::estimate_halfwidth(D, dL, valid, ppm, f, 1e9, 10, cv::Mat()), 0.0);
    double hw = scan::estimate_halfwidth(D, dL, valid, ppm, f, 1e9, 10, L) / ppm;
    CHECK(hw > 3.0 && hw < 8.0);
    CHECK_EQ(scan::estimate_halfwidth(D, cv::Mat::zeros(L.size(), CV_32F), valid, ppm, f, 1e9, 10, L), 0.0);
    // en strimma parallellt med vecket (tryckfel) får inte vidga zonen och därmed dölja sig själv
    cv::Mat streak = cv::Mat::zeros(L.size(), CV_32F);
    int y = int(300 + 3 * ppm);
    streak(cv::Rect(250, y, 95, 3)).setTo(-40);   // 8 mm lång, som i testgeneratorn
    CHECK_EQ(scan::estimate_halfwidth(D, streak, valid, ppm, f, 1e9, 10, L), 0.0);
}

// ------------------------------------------------------------------ dokument och datakontroll

TEST(documents_manuscript_vs_book_finds_all_changes) {
    for (auto [seed, prof] : {std::pair<int, std::string>{11, "book"}, {12, "magazine"}}) {
        auto c = testgen::make_book_case(seed, tmpdir("book" + std::to_string(seed)), prof);
        auto r = run_and_eval_doc(c);
        CHECK_EQ(r["found"].get<int>(), r["mutations"].get<int>());
        CHECK_EQ(r["false_alarms"].get<int>(), 0);
        if (r["false_alarms"].get<int>()) std::fprintf(stderr, "  %s\n", r["false_alarm_list"].dump().c_str());
    }
}

TEST(documents_unchanged_text_gives_no_findings) {
    auto c = testgen::make_book_case(13, tmpdir("clean"), "book", 0);
    auto res = jobspec::run_file((c / "jobspec.json").string()).result;
    CHECK(res["findings"].empty());
    CHECK(res["stats"]["hyphenations_joined"]["sample"].get<int>() > 0);   // boken ska ha avstavningar att laga
}

TEST(documents_label_versions_and_data_check) {
    for (auto [seed, tpl] : {std::pair<int, std::string>{21, "oat_drink"}, {22, "nutrition_grid"}}) {
        auto c = testgen::make_doc_label_case(seed, tmpdir("label" + std::to_string(seed)), tpl);
        auto r = run_and_eval_doc(c);
        CHECK_EQ(r["found"].get<int>(), r["mutations"].get<int>());
        CHECK_EQ(r["false_alarms"].get<int>(), 0);
        CHECK_EQ(r["data"]["correct"].get<int>(), r["data"]["items"].get<int>());
        CHECK(r["data"]["variant_ok"].get<bool>());
    }
}

TEST(data_check_statuses) {
    auto d = tmpdir("datacheck");
    write_pdf(d / "l.pdf", testgen::label_template("oat_drink"));
    write_file((d / "e.txt").string(),
               "# test\nNamn: Ekologisk Havredryck\nVolym: 1,5 liter\nAllergener: nötter\nLot: /LOT \\d{4}-\\d{4}/\nEAN: 731086500123\n");
    auto variants = compare::load_expected((d / "e.txt").string());
    auto doc = doc::load_file((d / "l.pdf").string());
    auto res = compare::data_check(variants, &doc, compare::codes_from_pdf((d / "l.pdf").string()), compare::DataCheckOptions{});
    std::map<std::string, std::string> got;
    for (auto& it : res["items"]) got[it["field"]] = it["status"];
    CHECK((got == std::map<std::string, std::string>{
                      {"Namn", "found"}, {"Volym", "near"}, {"Allergener", "missing"}, {"Lot", "found"}, {"EAN", "found"}}));
    for (auto& it : res["items"])
        if (it["field"] == "Volym") CHECK(it.value("found_text", "").find('1') != std::string::npos);
}

TEST(page_number_issues) {
    doc::Document d;
    d.page_labels = {std::nullopt, "2", "3", "40", "5", "6"};
    auto a = doc::page_number_issues(d);
    CHECK(a.size() == 1 && a[0].page == 4);   // ett fel tal
    d.page_labels = {std::nullopt, "2", "3", "5", "6", "7"};
    auto b = doc::page_number_issues(d);
    CHECK(b.size() == 1 && b[0].page == 4 && b[0].expected == "4");   // hopp
}
