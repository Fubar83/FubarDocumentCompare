// Dokument- och sidfakta via PDFium.
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <map>

#include "fontreg.hpp"
#include "page.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/pdf/fonts.hpp"
#include "printcmp/pdf/info.hpp"

namespace pc::pdf {

namespace {

// UTF-16LE (PDFiums strängar, med avslutande nolla) -> UTF-8
std::string utf16le(const std::vector<unsigned short>& w) {
    std::vector<char32_t> cps;
    for (size_t i = 0; i < w.size() && w[i]; ++i) {
        char32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        cps.push_back(c);
    }
    return text::encode(cps);
}

std::string meta(FPDF_DOCUMENT doc, const char* key) {
    unsigned long n = FPDF_GetMetaText(doc, key, nullptr, 0);
    if (n <= 2) return "";
    std::vector<unsigned short> buf(n / 2 + 1, 0);
    FPDF_GetMetaText(doc, key, buf.data(), (unsigned long)(buf.size() * 2));
    return utf16le(buf);
}

std::string font_string(FPDF_FONT font, size_t (*get)(FPDF_FONT, char*, size_t)) {
    size_t n = get(font, nullptr, 0);
    if (n <= 1) return "";
    std::string s(n, '\0');
    get(font, s.data(), n);
    s.resize(n - 1);
    return s;
}

bool is_subset_name(const std::string& n) {
    if (n.size() < 8 || n[6] != '+') return false;
    for (int i = 0; i < 6; ++i)
        if (n[i] < 'A' || n[i] > 'Z') return false;
    return true;
}

void collect_fonts(FPDF_PAGEOBJECT o, int depth, std::map<std::string, FontUse>& fonts, std::vector<std::string>& order) {
    int type = FPDFPageObj_GetType(o);
    if (type == FPDF_PAGEOBJ_TEXT) {
        FPDF_FONT f = FPDFTextObj_GetFont(o);
        if (!f) return;
        std::string base = font_string(f, FPDFFont_GetBaseFontName);
        auto it = fonts.find(base);
        if (it == fonts.end()) {
            FontUse u;
            u.base_name = detail::untoken(base);
            u.subset = is_subset_name(base);
            u.name = display_font_name(base);
            u.family = font_string(f, FPDFFont_GetFamilyName);
            u.embedded = FPDFFont_GetIsEmbedded(f) == 1;
            u.flags = FPDFFont_GetFlags(f);
            u.weight = std::max(0, FPDFFont_GetWeight(f));
            it = fonts.emplace(base, u).first;
            order.push_back(base);
        }
        ++it->second.text_objects;
    } else if (type == FPDF_PAGEOBJ_FORM && depth < 16) {
        int n = FPDFFormObj_CountObjects(o);
        for (int i = 0; i < n; ++i) collect_fonts(FPDFFormObj_GetObject(o, (unsigned long)i), depth + 1, fonts, order);
    }
}

}  // namespace

DocumentFacts document_facts(const Document& doc) {
    std::lock_guard<std::recursive_mutex> g(lock());
    DocumentFacts f;
    f.page_count = FPDF_GetPageCount(doc.handle());
    FPDF_GetFileVersion(doc.handle(), &f.file_version);
    for (const char* k : {"Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate", "ModDate"}) {
        auto v = meta(doc.handle(), k);
        if (!v.empty()) f.info.emplace_back(k, v);
    }
    return f;
}

PageFacts page_facts(const Document& doc, int page_index) {
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(doc, page_index);
    FPDF_PAGE p = page.handle();
    PageFacts f;
    f.index = page_index;
    f.width_pt = FPDF_GetPageWidthF(p);
    f.height_pt = FPDF_GetPageHeightF(p);
    f.rotation_deg = (FPDFPage_GetRotation(p) % 4) * 90;
    auto get = [&](FPDF_BOOL (*fn)(FPDF_PAGE, float*, float*, float*, float*), Rect4& out) {
        float l = 0, b = 0, r = 0, t = 0;
        if (!fn(p, &l, &b, &r, &t)) return false;
        out = {l, b, r, t};
        return true;
    };
    if (!get(FPDFPage_GetMediaBox, f.boxes.media)) f.boxes.media = {0, 0, f.width_pt, f.height_pt};
    f.boxes.has_crop = get(FPDFPage_GetCropBox, f.boxes.crop);
    if (!f.boxes.has_crop) f.boxes.crop = f.boxes.media;
    f.boxes.has_trim = get(FPDFPage_GetTrimBox, f.boxes.trim);
    if (!f.boxes.has_trim) f.boxes.trim = f.boxes.crop;
    f.boxes.has_bleed = get(FPDFPage_GetBleedBox, f.boxes.bleed);
    if (!f.boxes.has_bleed) f.boxes.bleed = f.boxes.crop;
    f.boxes.has_art = get(FPDFPage_GetArtBox, f.boxes.art);
    if (!f.boxes.has_art) f.boxes.art = f.boxes.crop;

    std::map<std::string, FontUse> fonts;
    std::vector<std::string> order;
    int n = FPDFPage_CountObjects(p);
    for (int i = 0; i < n; ++i) collect_fonts(FPDFPage_GetObject(p, i), 1, fonts, order);
    for (auto& k : order) f.fonts.push_back(fonts[k]);
    return f;
}

}  // namespace pc::pdf
