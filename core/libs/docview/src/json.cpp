// JSON i camelCase, samma form som REST-gränssnittet och .NET-typerna.
#include <cmath>

#include "printcmp/docview/docview.hpp"

namespace pc::docview {

namespace {

double r3(double v) { return std::round(v * 1000) / 1000; }

json rect(const RectMm& r) { return {{"x", r3(r.x)}, {"y", r3(r.y)}, {"width", r3(r.w)}, {"height", r3(r.h)}}; }

json role(const std::string& r) { return r.empty() ? json(nullptr) : json(r); }

}  // namespace

json to_json(const Metadata& m) {
    json pages = json::array();
    for (auto& p : m.pages) {
        json boxes = {{"media", rect(p.media)}, {"crop", rect(p.crop)}};
        if (p.trim) boxes["trim"] = rect(*p.trim);
        if (p.bleed) boxes["bleed"] = rect(*p.bleed);
        if (p.art) boxes["art"] = rect(*p.art);
        json layers = json::array(), spots = json::array(), fonts = json::array();
        for (auto& l : p.layers)
            layers.push_back({{"name", l.name}, {"defaultOn", l.default_on}, {"usedOnPage", l.used_on_page},
                              {"processingStep", l.step_group.empty() ? json(nullptr) : json{{"group", l.step_group}, {"type", l.step_type}}}});
        for (auto& s : p.spots) spots.push_back({{"name", s.name}, {"role", role(s.role)}});
        for (auto& f : p.fonts)
            fonts.push_back({{"name", f.name}, {"baseName", f.base_name}, {"family", f.family}, {"embedded", f.embedded},
                             {"subset", f.subset}, {"flags", f.flags}, {"weight", f.weight}, {"textObjects", f.text_objects},
                             {"status", f.status}, {"suppliedSha256", f.supplied_sha256.empty() ? json(nullptr) : json(f.supplied_sha256)}});
        pages.push_back({{"number", p.number}, {"widthMm", r3(p.width_mm)}, {"heightMm", r3(p.height_mm)},
                         {"rotation", p.rotation_deg}, {"boxes", boxes}, {"layers", layers}, {"spots", spots}, {"fonts", fonts}});
    }
    json info = json::object();
    for (auto& [k, v] : m.info) info[k] = v;
    json warnings = json::array();
    for (auto& w : m.warnings) warnings.push_back({{"code", w.code}, {"message", w.message}, {"page", w.page}});
    return {{"sha256", m.sha256}, {"sizeBytes", m.size_bytes}, {"pageCount", m.page_count}, {"pdfVersion", m.pdf_version},
            {"info", info}, {"pages", pages}, {"warnings", warnings}};
}

json to_json(const Overlays& o) {
    json words = json::array(), objects = json::array(), spots = json::array(), codes = json::array();
    for (auto& w : o.words)
        words.push_back({{"text", w.text}, {"box", rect(w.box)}, {"font", w.font}, {"sizePt", w.size_pt},
                         {"color", {w.color[0], w.color[1], w.color[2], w.color[3]}}});
    for (auto& b : o.objects) objects.push_back({{"type", b.type}, {"box", rect(b.box)}});
    for (auto& s : o.spots) {
        json polys = json::array();
        for (auto& p : s.polygons) {
            json pts = json::array();
            for (auto& q : p) pts.push_back({r3(q.x), r3(q.y)});
            polys.push_back(pts);
        }
        spots.push_back({{"name", s.name}, {"role", role(s.role)}, {"polygons", polys}});
    }
    for (auto& c : o.barcodes) codes.push_back({{"symbology", c.symbology}, {"data", c.data}, {"box", rect(c.box)}});
    return {{"page", o.page}, {"box", o.box}, {"widthMm", r3(o.width_mm)}, {"heightMm", r3(o.height_mm)},
            {"words", words}, {"objects", objects}, {"spots", spots}, {"barcodes", codes}};
}


json to_json(const pdf::FontCheck& c) {
    return {{"font", c.font}, {"ok", c.ok}, {"characters", c.characters}, {"widthMismatches", c.width_mismatches},
            {"missingGlyphs", c.missing_glyphs}, {"maxWidthError", r3(c.max_width_error)}, {"example", c.example}};
}

json to_json(const pdf::FontSet& s) {
    json fonts = json::array();
    for (auto& f : s.fonts())
        fonts.push_back({{"postScriptName", f->postscript_name}, {"family", f->family}, {"fullName", f->full_name},
                         {"subfamily", f->subfamily}, {"aliases", f->aliases}, {"sha256", f->sha256}, {"weight", f->weight},
                         {"italic", f->italic}, {"fsType", f->fs_type}, {"sizeBytes", f->data->size()}});
    return {{"hash", s.hash()}, {"fallback", s.fallback() == pdf::FontFallback::System ? "system" : "builtin"}, {"fonts", fonts}};
}

}  // namespace pc::docview
