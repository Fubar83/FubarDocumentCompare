// Metadata: sidor, rutor, lager, dekorfärger, typsnitt och varningar.
#include <map>
#include <set>

#include "internal.hpp"
#include "printcmp/pdf/fonts.hpp"
#include "printcmp/pdf/info.hpp"
#include "printcmp/pdf/layers.hpp"

namespace pc::docview {

namespace {

// Ruta i mm relativt mediarutans övre vänstra hörn, y nedåt.
RectMm to_mm(const pdf::Rect4& r, const pdf::Rect4& media) {
    return {(r[0] - media[0]) * PT_MM, (media[3] - r[3]) * PT_MM, (r[2] - r[0]) * PT_MM, (r[3] - r[1]) * PT_MM};
}

const char* status_name(pdf::FontDecision::Kind k) {
    switch (k) {
        case pdf::FontDecision::Kind::Embedded: return "embedded";
        case pdf::FontDecision::Kind::Supplied: return "supplied";
        case pdf::FontDecision::Kind::Standard: return "standard";
        case pdf::FontDecision::Kind::Fallback: return "fallback";
        default: return "system";
    }
}

Metadata build(Cache& cache, const Pdf& pdf, const pdf::FontSet* fonts) {
    auto doc = detail::document(cache, pdf);
    auto facts = pdf::document_facts(*doc);
    Metadata m;
    m.sha256 = pdf.sha256;
    m.size_bytes = pdf.bytes->size();
    m.page_count = facts.page_count;
    m.pdf_version = facts.file_version;
    m.info = facts.info;
    std::vector<pdf::LayerInfo> layers;
    try {
        layers = pdf::list_layers(*pdf.bytes);
    } catch (const std::exception& e) {   // qpdf klarar inte filen: PDFium kan ändå rendera den
        m.warnings.push_back({"pdf.structure", std::string("lager och dekorfärger kan inte läsas: ") + e.what(), 0});
    }
    std::map<std::string, pdf::FontDecision> decided;   // typsnittsnamn -> hur det hanteras
    try {
        for (auto& d : pdf::font_decisions(*pdf.bytes, fonts)) decided.emplace(d.name, d);
    } catch (const std::exception&) {
    }
    std::set<std::string> warned_fonts;
    for (int i = 0; i < facts.page_count; ++i) {
        auto pf = pdf::page_facts(*doc, i);
        PageMetadata p;
        p.number = i + 1;
        p.rotation_deg = pf.rotation_deg;
        const auto& b = pf.boxes;
        p.media = to_mm(b.media, b.media);
        p.crop = to_mm(b.crop, b.media);
        if (b.has_trim) p.trim = to_mm(b.trim, b.media);
        if (b.has_bleed) p.bleed = to_mm(b.bleed, b.media);
        if (b.has_art) p.art = to_mm(b.art, b.media);
        p.width_mm = (b.trim[2] - b.trim[0]) * PT_MM;
        p.height_mm = (b.trim[3] - b.trim[1]) * PT_MM;
        if (i < (int)layers.size()) {
            for (auto& o : layers[i].ocgs) p.layers.push_back({o.name, o.default_on, o.used_on_page});
            for (auto& s : layers[i].spots) p.spots.push_back({s, detail::spot_role(s)});
        }
        for (auto& f : pf.fonts) {
            FontFacts ff{f.name, f.base_name, f.family, f.embedded, f.subset, f.flags, f.weight, f.text_objects, "embedded", ""};
            if (!f.embedded) {
                auto it = decided.find(f.name);
                auto kind = it != decided.end() ? it->second.kind : pdf::FontDecision::Kind::System;
                ff.status = status_name(kind);
                if (kind == pdf::FontDecision::Kind::Supplied) ff.supplied_sha256 = it->second.sha256;
                if ((kind == pdf::FontDecision::Kind::System || kind == pdf::FontDecision::Kind::Fallback) &&
                    warned_fonts.insert(f.name).second)
                    m.warnings.push_back({"font.not_embedded",
                                          "typsnittet " + f.name + " är inte inbäddat och ersätts av " +
                                              (kind == pdf::FontDecision::Kind::System ? "ett systemtypsnitt" : "PDFiums inbyggda") +
                                              " vid rendering",
                                          p.number});
            }
            p.fonts.push_back(std::move(ff));
        }
        if (pf.rotation_deg)
            m.warnings.push_back({"page.rotated", "sidan är roterad " + std::to_string(pf.rotation_deg) + "°", p.number});
        m.pages.push_back(std::move(p));
    }
    return m;
}

}  // namespace

Metadata describe(Cache& cache, const Pdf& pdf, const std::shared_ptr<const pdf::FontSet>& fonts) {
    return *cache.get<Metadata>("meta:" + pdf.sha256 + ":" + (fonts ? fonts->hash() : "-"), [&] {
        auto m = std::make_shared<const Metadata>(build(cache, pdf, fonts.get()));
        return std::pair{std::shared_ptr<const Metadata>(m), size_t(4096) * (m->pages.size() + 1)};
    });
}

pdf::FontCheck validate_font(Cache& cache, const Pdf& pdf, const std::string& font_name, const pdf::FontFile& candidate, int page) {
    auto doc = detail::document(cache, pdf);
    int idx = page > 0 ? detail::page_index(*doc, page) : -1;
    return pdf::validate_font(*doc, font_name, candidate, idx);
}

}  // namespace pc::docview
