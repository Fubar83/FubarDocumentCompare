// Tillhandahållna typsnitt: namnbyte i PDF:en (qpdf) och kontroll av en fontfil mot PDF:ens mått.
#include <qpdf/QPDFObjectHandle.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "fontreg.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/pdf/fonts.hpp"
#include "printcmp/pdf/text.hpp"
#include "qpdfio.hpp"

namespace pc::pdf {

namespace {

std::string strip_subset(const std::string& n) {
    if (n.size() > 7 && n[6] == '+') {
        bool tag = true;
        for (int i = 0; i < 6; ++i) tag = tag && n[i] >= 'A' && n[i] <= 'Z';
        if (tag) return n.substr(7);
    }
    return n;
}

std::string loose(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

// De 14 standardtypsnitten och de alias PDFium ersätter med samma inbyggda typsnitt och mått.
bool is_standard(const std::string& name) {
    std::string l = loose(name);
    for (const char* p : {"courier", "helvetica", "times", "symbol", "zapfdingbats", "arial"})
        if (l.rfind(p, 0) == 0) return true;
    return false;
}

QPDFObjectHandle descriptor_of(QPDFObjectHandle font) {
    if (font.hasKey("/FontDescriptor")) return font.getKey("/FontDescriptor");
    if (font.hasKey("/DescendantFonts")) {
        auto d = font.getKey("/DescendantFonts");
        if (d.isArray() && d.getArrayNItems() > 0 && d.getArrayItem(0).isDictionary())
            return descriptor_of(d.getArrayItem(0));
    }
    return QPDFObjectHandle::newNull();
}

bool is_embedded(QPDFObjectHandle font) {
    auto d = descriptor_of(font);
    if (!d.isDictionary()) return false;
    return d.hasKey("/FontFile") || d.hasKey("/FontFile2") || d.hasKey("/FontFile3");
}

std::string subtype(QPDFObjectHandle font) {
    auto s = font.getKey("/Subtype");
    return s.isName() ? s.getName() : "";
}

// Alla typsnittsordlistor i filen (indirekta objekt med /Type /Font, även underordnade CID-typsnitt).
std::vector<QPDFObjectHandle> fonts_of(QPDF& q) {
    std::vector<QPDFObjectHandle> out;
    for (auto& o : q.getAllObjects()) {
        if (!o.isDictionary()) continue;
        auto t = o.getKey("/Type");
        if (t.isName() && t.getName() == "/Font" && o.getKey("/BaseFont").isName()) out.push_back(o);
    }
    return out;
}

std::vector<FontDecision> decide(QPDF& q, const FontSet* set, bool rewrite) {
    std::vector<FontDecision> out;
    std::set<std::string> seen;
    for (auto& f : fonts_of(q)) {
        std::string st = subtype(f);
        if (st == "/Type3") continue;   // glyferna är innehåll i PDF:en
        std::string base = f.getKey("/BaseFont").getName().substr(1);
        std::string name = strip_subset(base);
        FontDecision d;
        d.name = name;
        std::string new_name;
        if (is_embedded(f)) {
            d.kind = FontDecision::Kind::Embedded;
        } else if (auto file = set ? set->match(name) : nullptr) {
            d.kind = FontDecision::Kind::Supplied;
            d.sha256 = file->sha256;
            if (rewrite) new_name = detail::register_font(file) + "_" + name;
        } else if (is_standard(name)) {
            d.kind = FontDecision::Kind::Standard;
        } else if (set && set->fallback() == FontFallback::Builtin) {
            d.kind = FontDecision::Kind::Fallback;
            if (rewrite) new_name = std::string(detail::BUILTIN_PREFIX) + name;
        } else {
            d.kind = FontDecision::Kind::System;
        }
        if (!new_name.empty()) {
            f.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + new_name));
            auto desc = f.getKey("/FontDescriptor");
            if (desc.isDictionary() && desc.hasKey("/FontName")) desc.replaceKey("/FontName", QPDFObjectHandle::newName("/" + new_name));
        }
        // underordnade CID-typsnitt har samma namn som sitt Type0: en rad per namn räcker
        if (seen.insert(name).second) out.push_back(d);
    }
    return out;
}

}  // namespace

namespace detail {

std::string untoken(const std::string& n) {
    std::string up = n;
    for (auto& c : up) c = (char)std::toupper((unsigned char)c);
    if (up.rfind(SUPPLIED_PREFIX, 0) == 0) {
        auto us = n.find('_');
        if (us != std::string::npos) return n.substr(us + 1);
    }
    if (up.rfind(BUILTIN_PREFIX, 0) == 0) return n.substr(std::string(BUILTIN_PREFIX).size());
    return n;
}

}  // namespace detail

std::string display_font_name(const std::string& pdf_font_name) { return strip_subset(detail::untoken(pdf_font_name)); }

Bytes apply_fonts(const Bytes& pdf, const FontSet& set, std::vector<FontDecision>* decisions) {
    auto q = detail::qpdf_load(pdf);
    auto d = decide(*q, &set, true);
    bool changed = std::any_of(d.begin(), d.end(), [](const FontDecision& x) {
        return x.kind == FontDecision::Kind::Supplied || x.kind == FontDecision::Kind::Fallback;
    });
    if (decisions) *decisions = d;
    return changed ? detail::qpdf_save(*q) : pdf;
}

std::vector<FontDecision> font_decisions(const Bytes& pdf, const FontSet* set) {
    auto q = detail::qpdf_load(pdf);
    return decide(*q, set, false);
}

FontCheck validate_font(const Document& doc, const std::string& pdf_font_name, const FontFile& candidate, int page_index) {
    FontCheck c;
    c.font = display_font_name(pdf_font_name);
    std::string want = loose(c.font);
    int first = page_index < 0 ? 0 : page_index, last = page_index < 0 ? doc.page_count() - 1 : page_index;
    // Stegbredden i PDF:en = avståndet till nästa teckens origo på samma baslinje (PDFium placerar
    // tecknen efter /Widths). Teckenavstånd (Tc) räknas in; det är ovanligt i tryckoriginal.
    std::map<char32_t, double> pdf_width;   // tecken -> stegbredd, andel av em
    std::set<char32_t> used;
    for (int p = first; p <= last; ++p) {
        auto chars = text_chars(doc, p);
        for (size_t i = 0; i < chars.size(); ++i) {
            const auto& ch = chars[i];
            if (ch.generated || ch.code == 0 || text::is_space(ch.code) || ch.font_size <= 0) continue;
            if (loose(display_font_name(ch.font_name)) != want) continue;
            used.insert(ch.code);
            if (pdf_width.count(ch.code) || i + 1 >= chars.size()) continue;
            const auto& next = chars[i + 1];
            double scale = ch.has_matrix ? std::sqrt(std::abs(ch.matrix[0] * ch.matrix[3] - ch.matrix[1] * ch.matrix[2])) : 1.0;
            double em = ch.font_size * scale;
            bool horizontal = !ch.has_matrix || std::abs(ch.matrix[1]) < 1e-6 * std::abs(ch.matrix[0]);
            bool same_run = !next.generated && ch.has_origin && next.has_origin && next.font_name == ch.font_name &&
                            std::abs(next.origin_y - ch.origin_y) < 0.01 * em && next.origin_x > ch.origin_x;
            if (em <= 0 || !horizontal || !same_run) continue;
            pdf_width.emplace(ch.code, (next.origin_x - ch.origin_x) / em);
        }
    }
    for (char32_t cp : used) pdf_width.emplace(cp, -1);   // använt men inte mätbart: bara glyfkontroll
    c.characters = (int)pdf_width.size();
    std::string bad;
    for (auto& [cp, w] : pdf_width) {
        uint16_t g = candidate.glyph(cp);
        if (!g) {
            ++c.missing_glyphs;
            if (bad.size() < 40) bad += text::encode(cp);
            continue;
        }
        if (w < 0) continue;   // inte mätbar i PDF:en
        double fw = double(candidate.advance(g)) / candidate.units_per_em;
        double err = std::abs(fw - w);
        c.max_width_error = std::max(c.max_width_error, err);
        if (err > 0.02) {
            ++c.width_mismatches;
            if (bad.size() < 40) bad += text::encode(cp);
        }
    }
    c.example = bad;
    c.ok = c.characters > 0 && c.missing_glyphs == 0 && c.width_mismatches == 0;
    return c;
}

}  // namespace pc::pdf
