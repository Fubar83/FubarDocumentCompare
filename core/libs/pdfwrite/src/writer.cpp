#include "printcmp/pdfwrite/writer.hpp"

#include <zlib.h>

#include <cmath>
#include <set>

#include "printcmp/core/core.hpp"
#include "printcmp/core/text.hpp"

namespace pc::pdfwrite {

std::string fmt_num(double v) {
    if (std::fabs(v) < 5e-5) return "0";
    std::string s = format("%.4f", v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

namespace {

constexpr double KAPPA = 0.5522847498;

std::string pdf_name(const std::string& n) {
    std::string out = "/";
    for (unsigned char c : n) {
        if (c < 33 || c > 126 || std::string("#()<>[]{}/%").find(char(c)) != std::string::npos) out += format("#%02X", c);
        else out += char(c);
    }
    return out;
}

std::string pdf_string(const std::string& s) {
    std::string out = "(";
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') out += '\\';
        out += c;
    }
    return out + ")";
}

Bytes deflate(const std::string& in) {
    uLongf n = compressBound(uLong(in.size()));
    Bytes out(n);
    if (compress2(out.data(), &n, reinterpret_cast<const Bytef*>(in.data()), uLong(in.size()), 6) != Z_OK)
        throw Error(ErrorCode::Internal, "zlib: komprimeringen misslyckades");
    out.resize(n);
    return out;
}

std::string utf16be_hex(const std::u32string& s) {
    std::string h;
    for (char32_t c : s) {
        if (c >= 0x10000) {
            c -= 0x10000;
            h += format("%04X%04X", 0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF));
        } else h += format("%04X", unsigned(c));
    }
    return h;
}

// Sexbokstavstagg för delmängdsfonter, ur namn och glyfmängd (deterministisk).
std::string subset_tag(const std::string& name, const std::set<uint16_t>& gids) {
    std::string key = name;
    for (auto g : gids) key += format(",%u", g);
    auto h = sha256_hex(key);
    std::string tag;
    for (int i = 0; i < 6; ++i) tag += char('A' + std::stoi(h.substr(2 * i, 2), nullptr, 16) % 26);
    return tag;
}

}  // namespace

// ------------------------------------------------------------------ Canvas

void Canvas::put(const std::string& s) {
    content_ += s;
    content_ += '\n';
}

void Canvas::color_op(const Color& c, bool stroke) {
    switch (c.kind) {
        case Color::Gray: put(fmt_num(c.v[0]) + (stroke ? " G" : " g")); break;
        case Color::Rgb: put(fmt_num(c.v[0]) + " " + fmt_num(c.v[1]) + " " + fmt_num(c.v[2]) + (stroke ? " RG" : " rg")); break;
        case Color::Cmyk:
            put(fmt_num(c.v[0]) + " " + fmt_num(c.v[1]) + " " + fmt_num(c.v[2]) + " " + fmt_num(c.v[3]) + (stroke ? " K" : " k"));
            break;
        case Color::Spot: {
            auto cs = doc_->colorspace(c);
            put(cs + (stroke ? " CS " : " cs ") + fmt_num(c.tint) + (stroke ? " SCN" : " scn"));
            break;
        }
    }
}

void Canvas::fill_color(const Color& c) { color_op(c, false); }
void Canvas::stroke_color(const Color& c) { color_op(c, true); }
void Canvas::line_width(double pt) { put(fmt_num(pt) + " w"); }
void Canvas::line_cap(int cap) { put(std::to_string(cap) + " J"); }
void Canvas::save() { put("q"); }
void Canvas::restore() { put("Q"); }
void Canvas::concat(double a, double b, double c, double d, double e, double f) {
    put(fmt_num(a) + " " + fmt_num(b) + " " + fmt_num(c) + " " + fmt_num(d) + " " + fmt_num(e) + " " + fmt_num(f) + " cm");
}
void Canvas::move_to(double x, double y) { put(fmt_num(x) + " " + fmt_num(y) + " m"); }
void Canvas::line_to(double x, double y) { put(fmt_num(x) + " " + fmt_num(y) + " l"); }
void Canvas::curve_to(double x1, double y1, double x2, double y2, double x3, double y3) {
    put(fmt_num(x1) + " " + fmt_num(y1) + " " + fmt_num(x2) + " " + fmt_num(y2) + " " + fmt_num(x3) + " " + fmt_num(y3) + " c");
}
void Canvas::close() { put("h"); }
void Canvas::draw_path(bool fill, bool stroke) { put(fill && stroke ? "B" : fill ? "f" : stroke ? "S" : "n"); }

void Canvas::rect(double x, double y, double w, double h, bool fill, bool stroke) {
    put(fmt_num(x) + " " + fmt_num(y) + " " + fmt_num(w) + " " + fmt_num(h) + " re");
    draw_path(fill, stroke);
}

void Canvas::ellipse_path(double cx, double cy, double rx, double ry) {
    double kx = KAPPA * rx, ky = KAPPA * ry;
    move_to(cx + rx, cy);
    curve_to(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    curve_to(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    curve_to(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    curve_to(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    close();
}

void Canvas::circle(double cx, double cy, double r, bool fill, bool stroke) {
    ellipse_path(cx, cy, r, r);
    draw_path(fill, stroke);
}

void Canvas::round_rect(double x, double y, double w, double h, double r, bool fill, bool stroke) {
    r = std::min({r, w / 2, h / 2});
    double k = KAPPA * r;
    move_to(x + r, y);
    line_to(x + w - r, y);
    curve_to(x + w - r + k, y, x + w, y + r - k, x + w, y + r);
    line_to(x + w, y + h - r);
    curve_to(x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h);
    line_to(x + r, y + h);
    curve_to(x + r - k, y + h, x, y + h - r + k, x, y + h - r);
    line_to(x, y + r);
    curve_to(x, y + r - k, x + r - k, y, x + r, y);
    close();
    draw_path(fill, stroke);
}

void Canvas::line(double x0, double y0, double x1, double y1) {
    move_to(x0, y0);
    line_to(x1, y1);
    draw_path(false, true);
}

double Canvas::text(const std::string& font, double size, double x, double y, const std::string& s) {
    auto& fu = doc_->font_use(font);
    auto cps = text::to_u32(s);
    std::string hex;
    long adv = 0;
    for (char32_t c : cps) {
        uint16_t g = fu.ttf->glyph(c);
        if (!g) continue;   // tecken som saknas i fonten ritas inte (som reportlab med .notdef)
        fu.used.emplace(g, std::u32string(1, c));
        hex += format("%04X", g);
        adv += fu.ttf->advance(g);
    }
    put("BT " + fu.res + " " + fmt_num(size) + " Tf " + fmt_num(x) + " " + fmt_num(y) + " Td <" + hex + "> Tj ET");
    return adv * size / fu.ttf->units_per_em();
}

double Canvas::text_right(const std::string& font, double size, double x_right, double y, const std::string& s) {
    double w = pdfwrite::font(font)->width(text::to_u32(s), size);
    return text(font, size, x_right - w, y, s);
}

double Canvas::text_centred(const std::string& font, double size, double x_mid, double y, const std::string& s) {
    double w = pdfwrite::font(font)->width(text::to_u32(s), size);
    return text(font, size, x_mid - w / 2, y, s);
}

void Canvas::begin_layer(const std::string& name) { put("/OC " + doc_->layer(name) + " BDC"); }
void Canvas::end_layer() { put("EMC"); }

void Canvas::set_trim_box(double x0, double y0, double x1, double y1) {
    trim_[0] = x0, trim_[1] = y0, trim_[2] = x1, trim_[3] = y1;
    has_trim_ = true;
}

// ------------------------------------------------------------------ PdfDocument

PdfDocument::PdfDocument() = default;
PdfDocument::~PdfDocument() = default;

Canvas& PdfDocument::new_page(double w_pt, double h_pt) {
    pages_.emplace_back(new Canvas(this, w_pt, h_pt));
    return *pages_.back();
}

PdfDocument::FontUse& PdfDocument::font_use(const std::string& name) {
    auto it = fonts_.find(name);
    if (it != fonts_.end()) return it->second;
    auto& fu = fonts_[name];
    fu.ttf = font(name);
    fu.res = "/F" + std::to_string(font_order_.size());
    font_order_.push_back(name);
    return fu;
}

std::string PdfDocument::colorspace(const Color& c) {
    for (size_t i = 0; i < spaces_.size(); ++i)
        if (spaces_[i].first == c.spot) return "/CS" + std::to_string(i);
    spaces_.push_back({c.spot, c});
    return "/CS" + std::to_string(spaces_.size() - 1);
}

std::string PdfDocument::layer(const std::string& name) {
    for (size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i] == name) return "/L" + std::to_string(i);
    layers_.push_back(name);
    return "/L" + std::to_string(layers_.size() - 1);
}

Bytes PdfDocument::save() const {
    // Objekt 1 = katalog, 2 = sidträd; resten i den ordning de skapas.
    std::vector<std::string> objs(2);
    auto add = [&](std::string body) {
        objs.push_back(std::move(body));
        return int(objs.size());
    };
    auto ref = [](int n) { return std::to_string(n) + " 0 R"; };
    auto stream = [&](const std::string& dict_extra, const std::string& data) {
        Bytes z = deflate(data);
        std::string body = "<< /Length " + std::to_string(z.size()) + " /Filter /FlateDecode" + dict_extra + " >>\nstream\n";
        body.append(reinterpret_cast<const char*>(z.data()), z.size());
        body += "\nendstream";
        return add(body);
    };

    // typsnitt
    std::string font_dict;
    for (const auto& name : font_order_) {
        const auto& fu = fonts_.at(name);
        const auto& t = *fu.ttf;
        std::set<uint16_t> gids;
        for (auto& [g, _] : fu.used) gids.insert(g);
        auto sub = t.subset(gids);
        std::string base = subset_tag(name, gids) + "+" + name;
        double s = 1000.0 / t.units_per_em();
        int file = stream(" /Length1 " + std::to_string(sub.size()), std::string(sub.begin(), sub.end()));
        auto bb = t.bbox();
        bool italic = t.italic_angle() != 0 || name.find("Italic") != std::string::npos || name.find("Oblique") != std::string::npos;
        int flags = 32 | (italic ? 64 : 0);
        int desc = add("<< /Type /FontDescriptor /FontName " + pdf_name(base) + " /Flags " + std::to_string(flags) +
                       " /FontBBox [" + fmt_num(bb[0] * s) + " " + fmt_num(bb[1] * s) + " " + fmt_num(bb[2] * s) + " " +
                       fmt_num(bb[3] * s) + "] /ItalicAngle " + fmt_num(t.italic_angle()) + " /Ascent " +
                       fmt_num(t.ascent() * s) + " /Descent " + fmt_num(t.descent() * s) + " /CapHeight " +
                       fmt_num(t.cap_height() * s) + " /StemV 80 /FontWeight " + std::to_string(t.weight()) +
                       " /FontFile2 " + ref(file) + " >>");
        std::string w = "[";
        for (auto g : gids) w += std::to_string(g) + " [" + fmt_num(t.advance(g) * s) + "] ";
        w += "]";
        int cid = add("<< /Type /Font /Subtype /CIDFontType2 /BaseFont " + pdf_name(base) +
                      " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
                      ref(desc) + " /DW 1000 /W " + w + " /CIDToGIDMap /Identity >>");
        std::string cmap =
            "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) "
            "/Ordering (UCS) /Supplement 0 >> def\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        std::vector<std::pair<uint16_t, std::u32string>> m(fu.used.begin(), fu.used.end());
        for (size_t i = 0; i < m.size(); i += 100) {
            size_t n = std::min<size_t>(100, m.size() - i);
            cmap += std::to_string(n) + " beginbfchar\n";
            for (size_t k = i; k < i + n; ++k) cmap += format("<%04X> <", m[k].first) + utf16be_hex(m[k].second) + ">\n";
            cmap += "endbfchar\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend";
        int tu = stream("", cmap);
        int f0 = add("<< /Type /Font /Subtype /Type0 /BaseFont " + pdf_name(base) +
                     " /Encoding /Identity-H /DescendantFonts [" + ref(cid) + "] /ToUnicode " + ref(tu) + " >>");
        font_dict += fu.res + " " + ref(f0) + " ";
    }
    // dekorfärger
    std::string cs_dict;
    for (size_t i = 0; i < spaces_.size(); ++i) {
        const auto& c = spaces_[i].second;
        int fn = add("<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [" + fmt_num(c.v[0]) + " " + fmt_num(c.v[1]) + " " +
                     fmt_num(c.v[2]) + " " + fmt_num(c.v[3]) + "] /N 1 >>");
        int cs = add("[/Separation " + pdf_name(c.spot) + " /DeviceCMYK " + ref(fn) + "]");
        cs_dict += "/CS" + std::to_string(i) + " " + ref(cs) + " ";
    }
    // lager
    std::string props, ocg_refs;
    for (size_t i = 0; i < layers_.size(); ++i) {
        int o = add("<< /Type /OCG /Name " + pdf_string(layers_[i]) + " >>");
        props += "/L" + std::to_string(i) + " " + ref(o) + " ";
        ocg_refs += ref(o) + " ";
    }
    std::string res = "<< /ProcSet [/PDF /Text]";
    if (!font_dict.empty()) res += " /Font << " + font_dict + ">>";
    if (!cs_dict.empty()) res += " /ColorSpace << " + cs_dict + ">>";
    if (!props.empty()) res += " /Properties << " + props + ">>";
    res += " >>";
    int resources = add(res);

    std::string kids;
    for (const auto& p : pages_) {
        int content = stream("", p->content_);
        std::string box = "[0 0 " + fmt_num(p->w_) + " " + fmt_num(p->h_) + "]";
        std::string page = "<< /Type /Page /Parent 2 0 R /MediaBox " + box;
        if (p->has_trim_)
            page += " /TrimBox [" + fmt_num(p->trim_[0]) + " " + fmt_num(p->trim_[1]) + " " + fmt_num(p->trim_[2]) + " " +
                    fmt_num(p->trim_[3]) + "]";
        page += " /Resources " + ref(resources) + " /Contents " + ref(content) + " >>";
        kids += ref(add(page)) + " ";
    }
    objs[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>";
    std::string cat = "<< /Type /Catalog /Pages 2 0 R";
    if (!layers_.empty())
        cat += " /OCProperties << /OCGs [" + ocg_refs + "] /D << /Order [" + ocg_refs + "] /ON [" + ocg_refs + "] /OFF [] >> >>";
    objs[0] = cat + " >>";

    std::string out = "%PDF-1.6\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offs;
    for (size_t i = 0; i < objs.size(); ++i) {
        offs.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offs) out += format("%010zu 00000 n \n", o);
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return Bytes(out.begin(), out.end());
}

}  // namespace pc::pdfwrite
