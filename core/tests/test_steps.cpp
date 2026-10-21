// Dekorfärgernas roller och ISO 19593-bearbetningssteg (lager med GTS_Metadata): stanslinjer
// känns igen på lagret oavsett färg, och stegen tas bort ur den sanerade PDF:en.
#include <opencv2/core.hpp>

#include "printcmp/core/regex.hpp"
#include "printcmp/docview/docview.hpp"
#include "printcmp/pdf/layers.hpp"
#include "printcmp/pdf/spots.hpp"
#include "test.hpp"

using namespace pc;

namespace {

// Rollen enligt standardreglerna (första träffen), tom om ingen.
std::string role(const std::string& spot) {
    for (auto& [r, rx] : pdf::default_spot_rules())
        if (Regex(rx).search(spot)) return r;
    return "";
}

// 100 × 100 pt. Lagret "Artwork": en ruta i dekorfärgen "Black". Lagret "Cutting" (ISO 19593
// Structural/Cutting): en stanslinje i processcyan 5 pt innanför kanten.
pdf::Bytes pdf_with_steps() {
    std::string content =
        "/OC /Pr2 BDC /CS0 cs 1 scn 30 30 40 40 re f EMC\n"
        "/OC /Pr1 BDC 1 0 0 0 K 2 w 5 5 90 90 re S EMC\n";
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [5 0 R 6 0 R] /D << /Order [5 0 R 6 0 R] >> >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R /Resources << /Properties << /Pr1 5 0 R /Pr2 6 0 R >> "
        "/ColorSpace << /CS0 [/Separation /Black /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0 0 1] /N 1 >>] >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /OCG /Name (Cutting) /GTS_Metadata << /GTS_ProcStepsGroup /Structural /GTS_ProcStepsType /Cutting >> >>",
        "<< /Type /OCG /Name (Artwork) >>",
    };
    std::string out = "%PDF-1.7\n";
    std::vector<size_t> offs;
    for (size_t i = 0; i < objs.size(); ++i) {
        offs.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (auto o : offs) {
        char b[24];
        std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
        out += b;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf::Bytes(out.begin(), out.end());
}

cv::Vec3b pixel(const cv::Mat& rgb, double x_pt, double y_pt, double dpi) {
    double s = dpi / 72.0;
    return rgb.at<cv::Vec3b>((int)((100 - y_pt) * s), (int)(x_pt * s));
}

}  // namespace

TEST(spot_roles_by_name) {
    CHECK_EQ(role("Black"), std::string(""));            // en svart dekorfärg, inte lack
    CHECK_EQ(role("PANTONE Black C"), std::string(""));
    CHECK_EQ(role("Varnish"), std::string("varnish"));
    CHECK_EQ(role("UV-lack"), std::string("varnish"));
    CHECK_EQ(role("Spotlack"), std::string("varnish"));
    CHECK_EQ(role("Blanklack"), std::string("varnish"));
    CHECK_EQ(role("Lack"), std::string("varnish"));
    CHECK_EQ(role("CutContour"), std::string("dieline"));
    CHECK_EQ(role("Structural"), std::string("dieline"));
    CHECK_EQ(role("Structural - Punching"), std::string("dieline"));
    CHECK_EQ(role("Stans"), std::string("dieline"));
    CHECK_EQ(role("Braille"), std::string("braille"));
}

TEST(processing_steps_are_read_from_layers) {
    auto info = pdf::list_layers(pdf_with_steps(), 0);
    CHECK_EQ(info.ocgs.size(), size_t(2));
    const pdf::Ocg* cut = nullptr;
    const pdf::Ocg* art = nullptr;
    for (auto& o : info.ocgs) (o.name == "Cutting" ? cut : art) = &o;
    CHECK(cut && art);
    if (!cut || !art) return;
    CHECK_EQ(cut->step_group, std::string("Structural"));
    CHECK_EQ(cut->step_type, std::string("Cutting"));
    CHECK(art->step_group.empty());
    CHECK(cut->used_on_page);
}

TEST(processing_steps_become_dielines_and_are_not_printed) {
    auto bytes = pdf_with_steps();
    auto sa = pdf::analyze_spots(bytes, 0, pdf::default_spot_rules());
    auto cuts = sa.polygons("dieline");
    CHECK_EQ(cuts.size(), size_t(1));   // stanslinjen i processcyan, känd på lagret
    double minx = 1e9;
    for (auto& p : cuts)
        for (auto& q : p) minx = std::min(minx, q.x);
    CHECK_NEAR(minx, 5.0, 0.01);
    CHECK(sa.polygons("varnish").empty());   // "Black" är ingen lack
    bool named = false;
    for (auto& p : sa.paths) named = named || (p.role == "dieline" && p.spot_name == "Cutting");
    CHECK(named);

    auto before = pdf::render(bytes, 0, 72, "media").rgb;
    auto after = pdf::render(sa.sanitized_pdf, 0, 72, "media").rgb;
    CHECK(pixel(before, 5, 50, 72)[0] < 128);    // stanslinjen syns i originalet (cyan: lite rött)
    CHECK(pixel(after, 5, 50, 72)[0] > 240);     // men inte i den sanerade
    CHECK(pixel(after, 50, 50, 72)[0] < 64);     // rutan i "Black" trycks fortfarande
}

TEST(processing_steps_in_metadata_and_overlays) {
    docview::Cache cache;
    auto pdf = docview::Pdf::from_bytes(pdf_with_steps());
    auto m = docview::describe(cache, pdf);
    const auto& page = m.pages.at(0);
    bool found = false;
    for (auto& l : page.layers)
        if (l.name == "Cutting") found = l.step_group == "Structural" && l.step_type == "Cutting";
    CHECK(found);
    CHECK_EQ(page.spots.size(), size_t(1));
    CHECK_EQ(page.spots[0].role, std::string(""));   // "Black"
    auto j = docview::to_json(m);
    bool json_ok = false;
    for (auto& l : j["pages"][0]["layers"])
        if (l["name"] == "Cutting") json_ok = l["processingStep"]["type"] == "Cutting";
    CHECK(json_ok);

    auto o = docview::overlays(cache, pdf, 1, docview::Spots, "media");
    bool cut = false;
    for (auto& s : o.spots) cut = cut || (s.name == "Cutting" && s.role == "dieline");
    CHECK(cut);
}
