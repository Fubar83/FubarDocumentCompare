// Tillhandahållna typsnitt: fontfiler, uppsättningar, namnbyte, rendering med uppsättningens
// fontfiler (även två uppsättningar med samma typsnittsnamn) och breddkontroll.
#include <cstdlib>
#include <filesystem>
#include <opencv2/core.hpp>

#include "printcmp/core/io.hpp"
#include "printcmp/docview/docview.hpp"
#include "printcmp/pdf/fonts.hpp"
#include "printcmp/pdf/info.hpp"
#include "test.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

// DejaVu finns i testmaskinens fontkataloger (samma sökning som testgeneratorn).
pdf::Bytes font_bytes(const std::string& file) {
    std::vector<fs::path> dirs;
    if (auto* d = std::getenv("PRINTCMP_FONT_DIR")) dirs.push_back(fs::u8path(d));
    if (auto* d = std::getenv("LOCALAPPDATA")) dirs.push_back(fs::u8path(d) / "Microsoft" / "Windows" / "Fonts");
    dirs.push_back(fs::u8path(std::getenv("WINDIR") ? std::getenv("WINDIR") : "C:\\Windows") / "Fonts");
    dirs.push_back("/usr/share/fonts/truetype/dejavu");
    for (auto& d : dirs)
        if (fs::exists(d / file)) {
            auto s = read_file_bytes((d / file).string());
            return pdf::Bytes(s.begin(), s.end());
        }
    throw Error(ErrorCode::FileNotFound, file + " saknas; sätt PRINTCMP_FONT_DIR");
}

// En sida med texten "Hello World" i det inte inbäddade typsnittet `name`, med /Widths ur `metrics`.
pdf::Bytes pdf_with_font(const std::string& name, const pdf::FontFile& metrics) {
    std::string widths;
    for (int c = 32; c <= 126; ++c)
        widths += std::to_string((int)std::lround(metrics.advance(metrics.glyph(c)) * 1000.0 / metrics.units_per_em)) + " ";
    std::string content = "BT /F1 28 Tf 12 30 Td (Hello World) Tj ET";
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 80] /Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>",
        "<< /Type /Font /Subtype /TrueType /BaseFont /" + name + " /FirstChar 32 /LastChar 126 /Widths [" + widths +
            "] /Encoding /WinAnsiEncoding /FontDescriptor 5 0 R >>",
        "<< /Type /FontDescriptor /FontName /" + name +
            " /Flags 32 /FontBBox [-1021 -463 1793 1232] /ItalicAngle 0 /Ascent 928 /Descent -236 /CapHeight 729 /StemV 80 >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
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

cv::Mat render(const pdf::Bytes& b) {
    auto doc = pdf::Document::open(b);
    pdf::RenderSpec s;
    s.dpi = 144;
    s.box = "media";
    return pdf::render_page(*doc, 0, s).pixels;
}

int diff(const cv::Mat& a, const cv::Mat& b) {
    if (a.size() != b.size()) return 1 << 20;
    cv::Mat d;
    cv::absdiff(a, b, d);
    return cv::countNonZero(d.reshape(1));
}

}  // namespace

TEST(fonts_parse_names_and_metrics) {
    auto f = pdf::parse_font(font_bytes("DejaVuSans.ttf"));
    CHECK_EQ(f->postscript_name, std::string("DejaVuSans"));
    CHECK_EQ(f->family, std::string("DejaVu Sans"));
    CHECK(f->glyph(U'A') > 0);
    CHECK(f->glyph(U'å') > 0);
    CHECK_EQ(f->units_per_em, 2048);
    CHECK(f->advance(f->glyph(U'W')) > f->advance(f->glyph(U'i')));
    CHECK_EQ(f->sha256.size(), size_t(64));
    bool threw = false;
    try {
        pdf::parse_font(pdf::Bytes{'h', 'e', 'j', 0, 0, 0, 0, 0, 0, 0, 0, 0});
    } catch (const Error& e) {
        threw = e.code() == ErrorCode::FileFormat;
    }
    CHECK(threw);
}

TEST(fonts_set_matches_names_and_aliases) {
    auto f = pdf::parse_font(font_bytes("DejaVuSans.ttf"), {"AcmeSans-Regular"});
    pdf::FontSet set({f});
    CHECK(set.match("AcmeSans-Regular") == f);
    CHECK(set.match("ABCDEF+AcmeSans-Regular") == f);
    CHECK(set.match("AcmeSans,Regular") == f);   // utan skiljetecken
    CHECK(set.match("dejavusans") == f);
    CHECK(set.match("Other") == nullptr);
    pdf::FontSet same({pdf::parse_font(font_bytes("DejaVuSans.ttf"), {"AcmeSans-Regular"})});
    CHECK_EQ(set.hash(), same.hash());
    pdf::FontSet other({f}, pdf::FontFallback::System);
    CHECK(set.hash() != other.hash());
}

TEST(fonts_supplied_font_is_used_for_rendering) {
    auto regular = pdf::parse_font(font_bytes("DejaVuSans.ttf"), {"AcmeSans-Regular"});
    auto bytes = pdf_with_font("AcmeSans-Regular", *regular);
    auto plain = pdf::font_decisions(bytes, nullptr);
    CHECK_EQ(plain.size(), size_t(1));
    CHECK(plain[0].kind == pdf::FontDecision::Kind::System);

    pdf::FontSet set({regular});
    std::vector<pdf::FontDecision> d;
    auto applied = pdf::apply_fonts(bytes, set, &d);
    CHECK(d.size() == 1 && d[0].kind == pdf::FontDecision::Kind::Supplied && d[0].sha256 == regular->sha256);
    auto loads = pdf::supplied_font_loads();
    auto with = render(applied);
    CHECK(pdf::supplied_font_loads() > loads);
    CHECK(diff(with, render(bytes)) > 50);   // inte samma ersättning som utan uppsättning
    // sidfakta visar PDF:ens namn, inte tokenet
    auto facts = pdf::page_facts(*pdf::Document::open(applied), 0);
    CHECK(facts.fonts.size() == 1 && facts.fonts[0].name == "AcmeSans-Regular");
    CHECK_EQ(pdf::display_font_name(facts.fonts[0].base_name), std::string("AcmeSans-Regular"));
}

TEST(fonts_same_name_different_files_do_not_mix) {
    auto regular = pdf::parse_font(font_bytes("DejaVuSans.ttf"), {"AcmeSans-Regular"});
    auto bold = pdf::parse_font(font_bytes("DejaVuSans-Bold.ttf"), {"AcmeSans-Regular"});
    auto bytes = pdf_with_font("AcmeSans-Regular", *regular);
    pdf::FontSet a({regular}), b({bold});
    auto plain = render(bytes);   // systemets ersättning först (samma ordning som i .NET-testet)
    // Båda dokumenten hålls öppna (som i docview-cachen): PDFium behåller då de laddade typsnitten i
    // sin cache, nycklad på namnet fontuppslaget rapporterar – det måste vara unikt per fil.
    auto doc_a = pdf::Document::open(pdf::apply_fonts(bytes, a));
    auto doc_b = pdf::Document::open(pdf::apply_fonts(bytes, b));
    pdf::RenderSpec spec;
    spec.dpi = 144;
    spec.box = "media";
    auto ra = pdf::render_page(*doc_a, 0, spec).pixels;
    auto rb = pdf::render_page(*doc_b, 0, spec).pixels;
    CHECK(diff(ra, plain) > 50);
    CHECK(diff(ra, rb) > 50);   // samma familj ("DejaVu Sans") och vikt i PDF:en, men olika filer
    CHECK_EQ(diff(render(pdf::apply_fonts(bytes, a)), ra), 0);   // tenant A får fortfarande A:s fil
    CHECK_EQ(diff(render(pdf::apply_fonts(bytes, b)), rb), 0);
}

TEST(fonts_builtin_fallback_is_reported) {
    auto regular = pdf::parse_font(font_bytes("DejaVuSans.ttf"));
    auto bytes = pdf_with_font("AcmeSans-Regular", *regular);
    pdf::FontSet empty({}, pdf::FontFallback::Builtin);
    std::vector<pdf::FontDecision> d;
    auto applied = pdf::apply_fonts(bytes, empty, &d);
    CHECK(d.size() == 1 && d[0].kind == pdf::FontDecision::Kind::Fallback);
    CHECK(!render(applied).empty());
    pdf::FontSet system({}, pdf::FontFallback::System);
    CHECK(pdf::apply_fonts(bytes, system, &d) == bytes);
    CHECK(d[0].kind == pdf::FontDecision::Kind::System);
}

TEST(fonts_validate_widths_against_pdf) {
    auto regular = pdf::parse_font(font_bytes("DejaVuSans.ttf"));
    auto bold = pdf::parse_font(font_bytes("DejaVuSans-Bold.ttf"));
    auto doc = pdf::Document::open(pdf_with_font("AcmeSans-Regular", *regular));
    auto ok = pdf::validate_font(*doc, "AcmeSans-Regular", *regular);
    CHECK(ok.ok);
    CHECK_EQ(ok.characters, 7);   // H e l o W r d – olika tecken utom mellanslag
    CHECK(ok.max_width_error < 0.01);
    auto wrong = pdf::validate_font(*doc, "AcmeSans-Regular", *bold);
    CHECK(!wrong.ok);
    CHECK(wrong.width_mismatches > 0);
    CHECK(!wrong.example.empty());
    auto none = pdf::validate_font(*doc, "Other", *regular);
    CHECK(!none.ok && none.characters == 0);
}

TEST(fonts_docview_reports_status_and_renders_with_set) {
    auto regular = pdf::parse_font(font_bytes("DejaVuSans.ttf"), {"AcmeSans-Regular"});
    auto doc = docview::Pdf::from_bytes(pdf_with_font("AcmeSans-Regular", *regular));
    docview::Cache cache;
    auto plain = docview::describe(cache, doc);
    CHECK_EQ(plain.pages[0].fonts.at(0).status, std::string("system"));
    CHECK_EQ(plain.warnings.size(), size_t(1));
    CHECK_EQ(plain.warnings[0].code, std::string("font.not_embedded"));
    auto set = std::make_shared<const pdf::FontSet>(std::vector<std::shared_ptr<const pdf::FontFile>>{regular});
    auto with = docview::describe(cache, doc, set);
    CHECK_EQ(with.pages[0].fonts.at(0).status, std::string("supplied"));
    CHECK_EQ(with.pages[0].fonts.at(0).supplied_sha256, regular->sha256);
    CHECK(with.warnings.empty());
    CHECK_EQ(docview::to_json(with)["pages"][0]["fonts"][0]["status"].get<std::string>(), std::string("supplied"));

    pdf::RenderSpec spec;
    spec.box = "media";
    docview::Variant v;
    v.fonts = set;
    auto a = docview::render(cache, doc, 1, spec).pixels;
    auto b = docview::render(cache, doc, 1, spec, v).pixels;
    CHECK(diff(a, b) > 50);
    auto words = docview::overlays(cache, doc, 1, docview::Words, "media", v).words;
    CHECK(words.size() == 2 && words[0].font == "AcmeSans-Regular");
    CHECK(docview::validate_font(cache, doc, "AcmeSans-Regular", *regular).ok);
    CHECK_EQ(docview::to_json(*set)["fonts"][0]["postScriptName"].get<std::string>(), std::string("DejaVuSans"));
}

// testdata/fonts/acme-sans.pdf (för .NET-testerna) skrivs om med FDC_WRITE_FONT_FIXTURE=1.
TEST(fonts_fixture_for_bindings) {
    auto regular = pdf::parse_font(font_bytes("DejaVuSans.ttf"));
    auto bytes = pdf_with_font("AcmeSans-Regular", *regular);
    std::string path = std::string(PC_TESTDATA) + "/fonts/acme-sans.pdf";
    if (std::getenv("FDC_WRITE_FONT_FIXTURE")) {
        fs::create_directories(fs::u8path(std::string(PC_TESTDATA) + "/fonts"));
        write_file(path, std::string(bytes.begin(), bytes.end()));
    }
    auto s = read_file_bytes(path);
    CHECK(pdf::Bytes(s.begin(), s.end()) == bytes);   // fixturen är aktuell
}
