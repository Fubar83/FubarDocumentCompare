// Överlägg (bildkarta): ord, sidobjekt, dekorfärgernas former och streckkoder i mm.
#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "printcmp/barcode/barcode.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/pdf/info.hpp"
#include "printcmp/pdf/text.hpp"

namespace pc::docview {

namespace {

const pdf::Rect4& pick_box(const pdf::PageBoxes& b, const std::string& name) {
    if (name == "trim") return b.trim;
    if (name == "crop") return b.crop;
    if (name == "media") return b.media;
    if (name == "bleed") return b.bleed;
    if (name == "art") return b.art;
    throw Error(ErrorCode::InvalidArgument, "okänd ruta: " + name + " (trim, crop, media, bleed, art)");
}

struct Frame {   // PDF-punkter -> mm i rutan, y nedåt
    pdf::Rect4 box;
    Pt pt(double x, double y) const { return {(x - box[0]) * PT_MM, (box[3] - y) * PT_MM}; }
    RectMm rect(double l, double b, double r, double t) const {
        return {(l - box[0]) * PT_MM, (box[3] - t) * PT_MM, (r - l) * PT_MM, (t - b) * PT_MM};
    }
};

std::string strip_subset(const std::string& n) {
    if (n.size() > 7 && n[6] == '+') return n.substr(7);
    return n;
}

double effective_size(const pdf::TextChar& c) {
    if (!c.has_matrix) return c.font_size;
    const double* m = c.matrix;
    return c.font_size * std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
}

// Tecken -> ord: bryt vid blanktecken, byte av typsnitt eller storlek, lucka eller ny rad.
std::vector<WordBox> words_of(const pdf::Document& doc, int idx, const Frame& fr) {
    std::vector<WordBox> out;
    std::vector<char32_t> cps;
    double l = 0, b = 0, r = 0, t = 0;
    const pdf::TextChar* first = nullptr;
    const pdf::TextChar* prev = nullptr;   // senaste tecknet med en ruta
    auto flush = [&] {
        if (first && !cps.empty() && r > l) {
            WordBox w;
            w.text = text::encode(cps);
            w.box = fr.rect(l, b, r, t);
            w.font = strip_subset(first->font_name);
            w.size_pt = std::round(effective_size(*first) * 100) / 100;
            w.color = {first->fill_r, first->fill_g, first->fill_b, first->fill_a};
            out.push_back(std::move(w));
        }
        cps.clear();
        first = prev = nullptr;
    };
    auto chars = pdf::text_chars(doc, idx);
    for (const auto& c : chars) {
        if (c.generated || c.code == 0 || c.code == 0xFFFE || text::is_space(c.code)) {
            flush();
            continue;
        }
        bool has_box = c.r > c.l || c.t > c.b;
        if (prev && has_box) {
            double size = std::max(1.0, prev->font_size);
            bool new_font = c.font_name != prev->font_name || std::abs(c.font_size - prev->font_size) > 0.01;
            bool gap = c.l - prev->r > 0.3 * size || c.r < prev->l - 0.1 * size;
            bool new_line = std::abs((c.b + c.t) / 2 - (prev->b + prev->t) / 2) > 0.6 * size;
            if (new_font || gap || new_line) flush();
        }
        if (!first) {
            first = &c;
            l = c.l, b = c.b, r = c.r, t = c.t;
        } else if (has_box) {
            l = std::min(l, c.l), b = std::min(b, c.b), r = std::max(r, c.r), t = std::max(t, c.t);
        }
        cps.push_back(c.code);
        if (has_box) prev = &c;
    }
    flush();
    return out;
}

const char* type_name(int t) {
    switch (t) {
        case pdf::OBJ_TEXT: return "text";
        case pdf::OBJ_PATH: return "path";
        case pdf::OBJ_IMAGE: return "image";
        case pdf::OBJ_SHADING: return "shading";
        case pdf::OBJ_FORM: return "form";
        default: return "unknown";
    }
}

}  // namespace

Overlays overlays(Cache& cache, const Pdf& pdf, int page, unsigned kinds, const std::string& box, const Variant& variant) {
    auto base = detail::document(cache, pdf);
    int idx = detail::page_index(*base, page);
    auto v = detail::variant(cache, pdf, idx, variant);
    auto facts = pdf::page_facts(*v.doc, idx);
    Frame fr{pick_box(facts.boxes, box)};
    Overlays o;
    o.page = page;
    o.box = box;
    o.width_mm = (fr.box[2] - fr.box[0]) * PT_MM;
    o.height_mm = (fr.box[3] - fr.box[1]) * PT_MM;

    if (kinds & Words) o.words = words_of(*v.doc, idx, fr);
    if (kinds & Objects)
        for (auto& p : pdf::page_objects(*v.doc, idx, 2)) o.objects.push_back({type_name(p.type), fr.rect(p.l, p.b, p.r, p.t)});
    if (kinds & Spots) {
        // alla dekorfärger utom registreringsfärgerna; rollen enligt standardreglerna
        auto sa = pdf::analyze_spots(*v.bytes, idx, {{"spot", "^(?!(?:All|None)$)"}});
        for (auto& sp : sa.paths) {
            auto it = std::find_if(o.spots.begin(), o.spots.end(), [&](const SpotShape& x) { return x.name == sp.spot_name; });
            if (it == o.spots.end()) {
                o.spots.push_back({sp.spot_name, detail::spot_role(sp.spot_name), {}});
                it = o.spots.end() - 1;
            }
            for (auto& poly : sp.polygons) {
                std::vector<Pt> mm;
                mm.reserve(poly.size());
                for (auto& q : poly) mm.push_back(fr.pt(q.x, q.y));
                it->polygons.push_back(std::move(mm));
            }
        }
    }
    if (kinds & Barcodes) {
        constexpr double dpi = 300;
        pdf::RenderSpec spec;
        spec.dpi = dpi;
        spec.box = box;
        auto r = pdf::render_page(*v.doc, idx, spec);
        double mm = MM_PER_INCH / dpi;
        for (auto& c : barcode::detect(r.pixels, dpi))
            o.barcodes.push_back({c.symbology, c.data, {c.bbox.x0 * mm, c.bbox.y0 * mm, c.bbox.w() * mm, c.bbox.h() * mm}});
    }
    return o;
}

}  // namespace pc::docview
