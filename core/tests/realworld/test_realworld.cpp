// Verkliga tryckfiler (se fetch.cmake): GWG:s Processing Steps-svit (ISO 19593-lager för förpackningar),
// GWG:s Output Suite (PDF/X-4) och Avery-mallar för etiketter. Körs bara med FDC_REALWORLD_TESTS=ON.
#include <cstdlib>
#include <filesystem>

#include "printcmp/core/io.hpp"
#include "printcmp/docview/docview.hpp"
#include "printcmp/pdf/layers.hpp"
#include "printcmp/pdf/spots.hpp"
#include "test.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path root() {
    auto* d = std::getenv("FDC_REALWORLD_DIR");
    return d ? fs::u8path(d) : fs::path();
}

std::vector<fs::path> pdfs(const std::string& sub = "") {
    std::vector<fs::path> out;
    auto dir = root() / sub;
    if (root().empty() || !fs::exists(dir)) return out;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        auto ext = u8(e.path().extension());
        if (e.is_regular_file() && (ext == ".pdf" || ext == ".PDF")) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

pdf::Bytes bytes_of(const fs::path& p) {
    auto s = read_file_bytes(u8(p));
    return pdf::Bytes(s.begin(), s.end());
}

std::string name_of(const fs::path& p) { return u8(p.filename()); }

}  // namespace

TEST(realworld_corpus_is_present) {
    CHECK(!root().empty());
    CHECK(pdfs("gwg-steps").size() >= 40);
    CHECK(pdfs("gwg-output").size() >= 3);
    CHECK(pdfs("avery").size() >= 4);
}

// Varje fil: metadata, rendering av första sidan och överlägg utan fel.
TEST(realworld_every_pdf_describes_renders_and_maps) {
    docview::Cache cache(256 << 20);
    for (auto& p : pdfs()) {
        try {
            auto pdf = docview::Pdf::from_bytes(bytes_of(p));
            auto m = docview::describe(cache, pdf);
            CHECK(m.page_count >= 1);
            pdf::RenderSpec spec;
            spec.dpi = 36;
            auto r = docview::render(cache, pdf, 1, spec);
            CHECK(!r.pixels.empty());
            docview::overlays(cache, pdf, 1, docview::Words | docview::Spots | docview::Objects);
        } catch (const std::exception& e) {
            pctest::fail(__FILE__, __LINE__, name_of(p) + ": " + e.what());
        }
    }
}

// ISO 19593: varje patch har bearbetningssteg i lagren; används ett Structural-steg på sidan blir
// innehållet stanslinjer, och den sanerade sidan går att rendera.
TEST(realworld_gwg_processing_steps) {
    int with_structural = 0, with_dielines = 0;
    for (auto& p : pdfs("gwg-steps")) {
        if (name_of(p).rfind("Patch", 0) != 0) continue;
        auto b = bytes_of(p);
        auto layers = pdf::list_layers(b, 0);
        bool any_step = false, structural = false;
        for (auto& o : layers.ocgs) {
            any_step = any_step || !o.step_group.empty();
            structural = structural || (o.step_group == "Structural" && o.used_on_page);
        }
        if (!any_step) pctest::fail(__FILE__, __LINE__, name_of(p) + ": inga bearbetningssteg i lagren");
        auto sa = pdf::analyze_spots(b, 0, pdf::default_spot_rules());
        if (structural) {
            ++with_structural;
            if (!sa.polygons("dieline").empty()) ++with_dielines;
            else pctest::fail(__FILE__, __LINE__, name_of(p) + ": Structural-lager men inga stanslinjer");
        }
        for (auto& path : sa.paths)
            if (path.spot_name == "Black" && path.role == "varnish")
                pctest::fail(__FILE__, __LINE__, name_of(p) + ": dekorfärgen Black klassad som lack");
        auto r = pdf::render(sa.sanitized_pdf, 0, 36, "trim");
        CHECK(!r.rgb.empty());
    }
    std::fprintf(stderr, "  %d patchar med Structural-lager, %d med stanslinjer\n", with_structural, with_dielines);
    CHECK(with_structural > 10);
}

// Avery: dekorfärgen DIELINE är stanslinjen, BLEEDLINE och SAFELINE är hjälplinjer (ingen roll).
TEST(realworld_avery_dielines) {
    docview::Cache cache;
    for (auto& p : pdfs("avery")) {
        auto b = bytes_of(p);
        auto sa = pdf::analyze_spots(b, 0, pdf::default_spot_rules());
        if (sa.polygons("dieline").empty()) pctest::fail(__FILE__, __LINE__, name_of(p) + ": ingen stanslinje");
        auto m = docview::describe(cache, docview::Pdf::from_bytes(b));
        for (auto& s : m.pages[0].spots) {
            if (s.name == "DIELINE") CHECK_EQ(s.role, std::string("dieline"));
            if (s.name == "BLEEDLINE" || s.name == "SAFELINE") CHECK_EQ(s.role, std::string(""));
        }
    }
}
