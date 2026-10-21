// pc_docview: metadata, rendering (hela sidor, utsnitt, rutor), varianter och överlägg på en
// genererad PDF med lager, dekorfärg och inbäddat typsnitt.
#include <opencv2/core.hpp>

#include "printcmp/docview/docview.hpp"
#include "printcmp/pdfwrite/writer.hpp"
#include "test.hpp"

using namespace pc;
namespace pw = pc::pdfwrite;

namespace {

constexpr double MM = pw::PT_PER_MM;

// 100 × 60 mm, trimruta 3 mm in. Lager "Bas" (grå ruta) och "SE" (text, även 90 pt så att tecken
// korsar rutgränserna), stanslinje i dekorfärgen "Stans".
docview::Bytes sample_pdf() {
    pw::PdfDocument doc;
    auto& c = doc.new_page(100 * MM, 60 * MM);
    c.set_trim_box(3 * MM, 3 * MM, 97 * MM, 57 * MM);
    c.begin_layer("Bas");
    c.fill_color(pw::Color::cmyk(0, 0, 0, 0.4));
    c.rect(10 * MM, 10 * MM, 30 * MM, 20 * MM, true, false);
    c.end_layer();
    c.begin_layer("SE");
    c.fill_color(pw::Color::gray(0));
    c.text("DejaVuSans", 14, 50 * MM, 30 * MM, "Hej världen");
    c.text("DejaVuSans", 90, 20 * MM, 40 * MM, "WM");
    c.end_layer();
    c.stroke_color(pw::Color::separation("Stans", 0, 1, 0, 0));
    c.line_width(1);
    c.rect(5 * MM, 5 * MM, 90 * MM, 50 * MM, false, true);
    return doc.save();
}

int max_abs_diff(const cv::Mat& a, const cv::Mat& b) {
    if (a.size() != b.size() || a.type() != b.type()) return 9999;
    cv::Mat d;
    cv::absdiff(a, b, d);
    double mx = 0;
    cv::minMaxLoc(d.reshape(1), nullptr, &mx);
    return (int)mx;
}

}  // namespace

TEST(docview_remember_and_recall_by_content) {
    docview::Cache cache(64 << 20);
    auto pdf = docview::remember(cache, sample_pdf());
    CHECK_EQ(pdf.sha256.size(), size_t(64));
    auto again = docview::recall(cache, pdf.sha256);
    CHECK(again.has_value());
    CHECK(again->bytes == pdf.bytes);
    CHECK(!docview::recall(cache, std::string(64, '0')).has_value());
}

TEST(docview_metadata_lists_boxes_layers_spots_fonts) {
    docview::Cache cache;
    auto pdf = docview::Pdf::from_bytes(sample_pdf());
    auto m = docview::describe(cache, pdf);
    CHECK_EQ(m.page_count, 1);
    CHECK_EQ(m.sha256, pdf.sha256);
    const auto& p = m.pages.at(0);
    CHECK_NEAR(p.width_mm, 94.0, 0.01);
    CHECK_NEAR(p.height_mm, 54.0, 0.01);
    CHECK(p.trim.has_value());
    CHECK_NEAR(p.trim->x, 3.0, 0.01);
    CHECK_NEAR(p.trim->y, 3.0, 0.01);
    CHECK_EQ(p.layers.size(), size_t(2));
    CHECK_EQ(p.layers[0].name, std::string("Bas"));
    CHECK_EQ(p.layers[1].name, std::string("SE"));
    CHECK_EQ(p.spots.size(), size_t(1));
    CHECK_EQ(p.spots[0].name, std::string("Stans"));
    CHECK_EQ(p.spots[0].role, std::string("dieline"));
    CHECK_EQ(p.fonts.size(), size_t(1));
    CHECK(p.fonts[0].embedded);
    CHECK(p.fonts[0].name.find("DejaVu") != std::string::npos);
    CHECK(m.warnings.empty());
    auto j = docview::to_json(m);
    CHECK_EQ(j["pages"][0]["spots"][0]["role"].get<std::string>(), std::string("dieline"));
    CHECK(j["pages"][0]["boxes"].contains("trim"));
}

TEST(docview_render_matches_reference_renderer) {
    docview::Cache cache;
    auto bytes = sample_pdf();
    auto pdf = docview::Pdf::from_bytes(bytes);
    pdf::RenderSpec spec;
    spec.dpi = 150;
    auto r = docview::render(cache, pdf, 1, spec);
    auto ref = pdf::render(bytes, 0, 150, "trim");
    CHECK_EQ(max_abs_diff(r.pixels, ref.rgb), 0);
    CHECK_EQ(r.full_width, r.pixels.cols);
    // utsnitt = samma pixlar som motsvarande del av hela rastret
    spec.x = 101, spec.y = 37, spec.width = 150, spec.height = 90;
    auto part = docview::render(cache, pdf, 1, spec);
    CHECK_EQ(max_abs_diff(part.pixels, ref.rgb(cv::Rect(101, 37, 150, 90))), 0);
    // gråskala och genomskinlig bakgrund
    spec = {};
    spec.pixels = pdf::RenderSpec::Pixels::Rgba;
    auto rgba = docview::render(cache, pdf, 1, spec);
    CHECK_EQ(rgba.pixels.channels(), 4);
    CHECK_EQ((int)rgba.pixels.at<cv::Vec4b>(2, 2)[3], 0);   // tomt papper är genomskinligt
    spec.pixels = pdf::RenderSpec::Pixels::Gray;
    CHECK_EQ(docview::render(cache, pdf, 1, spec).pixels.channels(), 1);
}

TEST(docview_tiles_stitch_to_full_page) {
    docview::Cache cache;
    auto pdf = docview::Pdf::from_bytes(sample_pdf());
    int level = 3;   // 144 dpi
    auto grid = docview::tile_grid(cache, pdf, 1, level, 128);
    CHECK(grid.columns >= 3 && grid.rows >= 2);
    pdf::RenderSpec full;
    full.dpi = docview::tile_dpi(level);
    auto whole = docview::render(cache, pdf, 1, full);
    CHECK_EQ(whole.pixels.cols, grid.width_px);
    cv::Mat stitched(grid.height_px, grid.width_px, CV_8UC3);
    for (int y = 0; y < grid.rows; ++y)
        for (int x = 0; x < grid.columns; ++x) {
            auto t = docview::render_tile(cache, pdf, 1, {level, x, y, 128}, {});
            t.pixels.copyTo(stitched(cv::Rect(t.x, t.y, t.pixels.cols, t.pixels.rows)));
        }
    CHECK_EQ(max_abs_diff(stitched, whole.pixels), 0);
    bool threw = false;
    try {
        docview::render_tile(cache, pdf, 1, {level, grid.columns, 0, 128}, {});
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(docview_variants_hide_layers_and_spots) {
    docview::Cache cache;
    auto pdf = docview::Pdf::from_bytes(sample_pdf());
    pdf::RenderSpec spec;
    spec.dpi = 100;
    auto all = docview::render(cache, pdf, 1, spec).pixels;
    docview::Variant no_text;
    no_text.exclude_layers = {"SE"};
    auto without = docview::render(cache, pdf, 1, spec, no_text).pixels;
    CHECK(max_abs_diff(all, without) > 100);
    docview::Variant printing;
    printing.hide_nonprinting = true;
    auto printed = docview::render(cache, pdf, 1, spec, printing).pixels;
    // stanslinjen (5 mm från mediakanten = 2 mm in i trimrutan) försvinner
    int px = (int)std::round(2.0 / 25.4 * 100);
    CHECK(max_abs_diff(all.row(all.rows / 2).colRange(px - 1, px + 2), printed.row(all.rows / 2).colRange(px - 1, px + 2)) > 50);
    // överläggen följer varianten
    auto words = docview::overlays(cache, pdf, 1, docview::Words).words;
    CHECK_EQ(words.size(), size_t(3));
    CHECK(docview::overlays(cache, pdf, 1, docview::Words, "trim", no_text).words.empty());
}

TEST(docview_overlays_in_mm_from_box_corner) {
    docview::Cache cache;
    auto pdf = docview::Pdf::from_bytes(sample_pdf());
    auto o = docview::overlays(cache, pdf, 1, docview::Words | docview::Spots | docview::Objects, "trim");
    CHECK_NEAR(o.width_mm, 94.0, 0.01);
    CHECK_EQ(o.words.size(), size_t(3));   // i läsordning: WM (högst upp), Hej, världen
    CHECK_EQ(o.words[0].text, std::string("WM"));
    CHECK_NEAR(o.words[0].size_pt, 90.0, 0.01);
    CHECK_EQ(o.words[1].text, std::string("Hej"));
    CHECK_EQ(o.words[2].text, std::string("världen"));
    CHECK_NEAR(o.words[1].box.x, 47.0, 0.5);              // 50 mm − 3 mm trim
    CHECK_NEAR(o.words[1].size_pt, 14.0, 0.01);
    CHECK(o.words[2].box.x > o.words[1].box.x + o.words[1].box.w);
    CHECK_EQ(o.spots.size(), size_t(1));
    CHECK_EQ(o.spots[0].role, std::string("dieline"));
    CHECK(!o.spots[0].polygons.empty());
    double minx = 1e9;
    for (auto& p : o.spots[0].polygons)
        for (auto& q : p) minx = std::min(minx, q.x);
    CHECK_NEAR(minx, 2.0, 0.1);                            // 5 mm − 3 mm
    CHECK(!o.objects.empty());
    auto j = docview::to_json(o);
    CHECK_EQ(j["words"][2]["text"].get<std::string>(), std::string("världen"));
}

TEST(docview_bad_page_is_an_error) {
    docview::Cache cache;
    auto pdf = docview::Pdf::from_bytes(sample_pdf());
    bool threw = false;
    try {
        docview::render(cache, pdf, 2, {});
    } catch (const Error& e) {
        threw = e.code() == ErrorCode::InvalidArgument;
    }
    CHECK(threw);
}
