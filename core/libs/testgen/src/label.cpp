// Etikettbeskrivning -> PDF och exakt geometri per element.
#include "printcmp/testgen/label.hpp"

#include <algorithm>
#include <set>

#include "printcmp/barcode/encode.hpp"
#include "printcmp/braille/braille.hpp"
#include "util.hpp"

namespace pc::testgen {

Rect rect_union(const Rect& a, const Rect& b) {
    double x0 = std::min(a[0], b[0]), y0 = std::min(a[1], b[1]);
    double x1 = std::max(a[0] + a[2], b[0] + b[2]), y1 = std::max(a[1] + a[3], b[1] + b[3]);
    return {x0, y0, x1 - x0, y1 - y0};
}

std::pair<double, double> text_metrics(const std::string& font_name, double size_pt) {
    auto f = font(font_name);
    double s = size_pt * 25.4 / 72 / f->units_per_em();
    return {f->ascent() * s, -f->descent() * s};
}

double text_width_mm(const std::string& font_name, double size_pt, const std::u32string& s) {
    return font(font_name)->width(s, size_pt) * 25.4 / 72;
}

double text_width_mm(const std::string& font_name, double size_pt, const std::string& s) {
    return text_width_mm(font_name, size_pt, u32(s));
}

Rect char_bbox_mm(const json& el, size_t i0, size_t i1) {
    auto t = u32(el["text"].get<std::string>());
    const auto& font_name = el["font"].get<std::string>();
    double size = el["size_pt"].get<double>();
    double px = el["pos_mm"][0].get<double>(), by = el["pos_mm"][1].get<double>();
    double x0 = px + text_width_mm(font_name, size, t.substr(0, std::min(i0, t.size())));
    double x1 = px + text_width_mm(font_name, size, t.substr(0, std::min(i1, t.size())));
    auto [asc, desc] = text_metrics(font_name, size);
    return {x0, by - asc, std::max(x1 - x0, 0.05), asc + desc};
}

// ------------------------------------------------------------------ tabell

TableLayout table_layout(const json& el) {
    TableLayout L;
    double x = el["pos_mm"][0], y = el["pos_mm"][1];
    std::vector<double> cw = el["col_widths_mm"].get<std::vector<double>>();
    double rh = el["row_h_mm"], pad = el.value("pad_mm", 0.8);
    L.xs = {x};
    for (double w : cw) L.xs.push_back(L.xs.back() + w);
    json dxs = el.value("row_dx_mm", json::object());
    const auto& rows = el["rows"];
    int n = int(rows.size());
    double size = el["size_pt"];
    for (int r = 0; r < n; ++r) {
        double top = y + r * rh, base = top + 0.70 * rh;
        std::string font_name = (r == 0 && el.value("header", true)) ? el.value("header_font", el["font"].get<std::string>())
                                                                      : el["font"].get<std::string>();
        for (int c = 0; c < int(rows[r].size()); ++c) {
            std::string txt = rows[r][c];
            double tw = text_width_mm(font_name, size, txt);
            double off = dxs.contains(std::to_string(r)) ? dxs[std::to_string(r)].get<double>() : 0.0;
            double tx = (el["align"][c] == "right" ? L.xs[c + 1] - pad - tw : L.xs[c] + pad) + off;
            L.cells.push_back({r, c, txt, font_name, tx, base, size, tw, {L.xs[c], top, cw[c], rh}});
        }
    }
    std::string mode = el.value("rules", "grid");
    if (mode == "grid" || mode == "horizontal")
        for (int i = 0; i <= n; ++i) L.rules.push_back({i, 'h', L.xs.front(), y + i * rh, L.xs.back(), y + i * rh});
    if (mode == "grid")
        for (int j = 0; j <= int(cw.size()); ++j) L.rules.push_back({100 + j, 'v', L.xs[j], y, L.xs[j], y + n * rh});
    std::set<int> skip;
    for (auto& s : el.value("skip_rules", json::array())) skip.insert(s.get<int>());
    std::erase_if(L.rules, [&](const TableRuleGeom& r) { return skip.count(r.index) > 0; });
    L.bbox = {L.xs.front(), y, L.xs.back() - L.xs.front(), n * rh};
    return L;
}

Rect cell_text_bbox_mm(const TableCell& c) {
    auto [asc, desc] = text_metrics(c.font, c.size_pt);
    return {c.x, c.baseline - asc, std::max(c.width, 0.05), asc + desc};
}

Rect rule_bbox_mm(const TableRuleGeom& r, double lw) {
    return {std::min(r.x0, r.x1) - lw / 2, std::min(r.y0, r.y1) - lw / 2, std::fabs(r.x1 - r.x0) + lw, std::fabs(r.y1 - r.y0) + lw};
}

// ------------------------------------------------------------------ punktskrift

int braille_cell_count(const std::string& text) { return int(braille::text_to_cells(text).size()); }

std::vector<BrailleDot> braille_dots_mm(const json& el) {
    auto cells = braille::text_to_cells(el["text"].get<std::string>());
    double p = el.value("dot_pitch_mm", 2.5), c = el.value("cell_pitch_mm", 6.0);
    double x0 = el["origin_mm"][0], y0 = el["origin_mm"][1];
    std::vector<BrailleDot> pts;
    for (int i = 0; i < int(cells.size()); ++i)
        for (int n : cells[i]) {   // std::set: stigande ordning, som sorted()
            int col = n <= 3 ? 0 : 1, row = (n - 1) % 3;
            pts.push_back({x0 + i * c + col * p, y0 + row * p, i, n});
        }
    return pts;
}

Rect braille_cell_bbox_mm(const json& el, int i) {
    double p = el.value("dot_pitch_mm", 2.5), c = el.value("cell_pitch_mm", 6.0), r = el.value("dot_d_mm", 1.5) / 2;
    double x0 = el["origin_mm"][0], y0 = el["origin_mm"][1];
    return {x0 + i * c - r, y0 - r, p + 2 * r, 2 * p + 2 * r};
}

Rect element_bbox_mm(const json& el) {
    std::string t = el["type"];
    if (t == "text" || t == "variable" || t == "outlined") return char_bbox_mm(el, 0, u32(el["text"].get<std::string>()).size());
    if (t == "table") return table_layout(el).bbox;
    if (t == "braille") {
        int n = braille_cell_count(el["text"]);
        auto a = braille_cell_bbox_mm(el, 0), b = braille_cell_bbox_mm(el, n - 1);
        return {a[0], a[1], b[0] + b[2] - a[0], a[3]};
    }
    if (t == "rect" || t == "ean13" || t == "qr" || t == "varnish")
        return {el["rect_mm"][0], el["rect_mm"][1], el["rect_mm"][2], el["rect_mm"][3]};
    if (t == "circle") {
        double cx = el["center_mm"][0], cy = el["center_mm"][1], r = el["r_mm"];
        return {cx - r, cy - r, 2 * r, 2 * r};
    }
    if (t == "poly") {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (auto& p : el["points_mm"]) {
            x0 = std::min(x0, p[0].get<double>()), x1 = std::max(x1, p[0].get<double>());
            y0 = std::min(y0, p[1].get<double>()), y1 = std::max(y1, p[1].get<double>());
        }
        return {x0, y0, x1 - x0, y1 - y0};
    }
    throw Error(ErrorCode::InvalidArgument, "okänd elementtyp " + t);
}

// ------------------------------------------------------------------ rendering

namespace {

Color spec_color(const json& el, const char* key = "color") {
    json c = el.contains(key) && !el[key].is_null() ? el[key] : json{{"cmyk", {0, 0, 0, 1}}};
    auto v = c.value("cmyk", std::vector<double>{0, 0, 0, 1});
    if (c.contains("spot") && !c["spot"].get<std::string>().empty()) return Color::separation(c["spot"], v[0], v[1], v[2], v[3]);
    return Color::cmyk(v[0], v[1], v[2], v[3]);
}

// EAN-13 som reportlabs Ean13BarcodeWidget(fontSize=8, barHeight=36): 113 moduler (9 tysta till
// vänster), kantstaplarna går ned i textraden; siffrorna i tre grupper under staplarna.
void draw_ean13(Canvas& c, const std::string& data, double x, double y, double w, double h) {
    const double mw = 0.9354330708661419, W = 113 * mw, H = 36;
    auto bits = barcode::encode_ean13(data);
    std::string full = data.substr(0, 12);
    int sum = 0;
    for (int k = 0; k < 12; ++k) sum += (full[k] - '0') * (k % 2 ? 3 : 1);
    full += char('0' + (10 - sum % 10) % 10);
    c.save();
    c.concat(w / W, 0, 0, h / H, x, y);
    c.fill_color(Color::cmyk(0, 0, 0, 1));
    auto guard = [](int i) { return i < 3 || (i >= 45 && i < 50) || i >= 92; };
    for (int i = 0; i < 95;) {
        if (!bits.get(i, 0)) {
            ++i;
            continue;
        }
        int j = i;
        while (j < 95 && bits.get(j, 0) && guard(j) == guard(i)) ++j;
        double y0 = guard(i) ? 0 : 9.6;
        c.rect((9 + i) * mw, y0, (j - i) * mw, H - y0, true, false);
        i = j;
    }
    c.text("DejaVuSans", 8, mw, 1.92, full.substr(0, 1));
    c.text_centred("DejaVuSans", 8, 33 * mw, 1.92, full.substr(1, 6));
    c.text_centred("DejaVuSans", 8, 80 * mw, 1.92, full.substr(7, 6));
    c.restore();
}

// QR som reportlabs QrCodeWidget(barLevel="M"): 4 moduler marginal, kvadratisk, nedre vänstra hörnet i rutan.
void draw_qr(Canvas& c, const std::string& data, double x, double y, double w, double h) {
    auto bits = barcode::encode_qr(data, 'M');
    int n = bits.width, tot = n + 8;
    double side = std::min(w, h), m = side / tot;
    c.fill_color(Color::cmyk(0, 0, 0, 1));
    for (int r = 0; r < n; ++r)
        for (int i = 0; i < n;) {
            if (!bits.get(i, r)) {
                ++i;
                continue;
            }
            int j = i;
            while (j < n && bits.get(j, r)) ++j;
            c.rect(x + (4 + i) * m, y + (tot - 5 - r) * m, (j - i) * m, m, true, false);
            i = j;
        }
}

void draw_outlined(Canvas& c, const json& el, double H) {
    auto f = font(el["font"]);
    double s = el["size_pt"].get<double>() / f->units_per_em();
    double x = el["pos_mm"][0].get<double>() * MM, yb = (H - el["pos_mm"][1].get<double>()) * MM;
    c.fill_color(spec_color(el));
    for (char32_t ch : u32(el["text"].get<std::string>())) {
        uint16_t g = f->glyph(ch);
        if (!g) continue;
        if (ch != U' ') {
            auto ops = f->outline(g);
            if (!ops.empty()) {
                for (auto& o : ops) {
                    if (o.op == 'm') c.move_to(x + o.p[0] * s, yb + o.p[1] * s);
                    else if (o.op == 'l') c.line_to(x + o.p[0] * s, yb + o.p[1] * s);
                    else if (o.op == 'c')
                        c.curve_to(x + o.p[0] * s, yb + o.p[1] * s, x + o.p[2] * s, yb + o.p[3] * s, x + o.p[4] * s, yb + o.p[5] * s);
                    else c.close();
                }
                c.draw_path(true, false);
            }
        }
        x += f->advance(g) * s;
    }
}

void draw_element(Canvas& c, const json& el, double H) {
    std::string t = el["type"];
    auto X = [](double v) { return v * MM; };
    auto Y = [H](double v) { return (H - v) * MM; };
    if (t == "text" || t == "variable") {
        c.fill_color(spec_color(el));
        c.text(el["font"], el["size_pt"], X(el["pos_mm"][0]), Y(el["pos_mm"][1]), el["text"]);
    } else if (t == "outlined") {
        draw_outlined(c, el, H);
    } else if (t == "table") {
        auto L = table_layout(el);
        c.fill_color(spec_color(el));
        for (auto& cell : L.cells) c.text(cell.font, cell.size_pt, X(cell.x), Y(cell.baseline), cell.text);
        c.stroke_color(spec_color(el, "rule_color"));
        c.line_width(el.value("rule_w_mm", 0.15) * MM);
        c.line_cap(2);
        for (auto& r : L.rules) c.line(X(r.x0), Y(r.y0), X(r.x1), Y(r.y1));
    } else if (t == "braille") {
        // prägling: egen dekorfärg som inte trycks
        c.fill_color(Color::separation(el.value("spot", "Braille"), 0, 0.6, 0, 0));
        double rr = el.value("dot_d_mm", 1.5) / 2;
        for (auto& d : braille_dots_mm(el)) c.circle(X(d.x), Y(d.y), rr * MM, true, false);
    } else if (t == "rect") {
        double x = el["rect_mm"][0], y = el["rect_mm"][1], w = el["rect_mm"][2], h = el["rect_mm"][3];
        c.fill_color(spec_color(el));
        c.rect(X(x), Y(y + h), w * MM, h * MM, true, false);
    } else if (t == "circle") {
        c.fill_color(spec_color(el));
        c.circle(X(el["center_mm"][0]), Y(el["center_mm"][1]), el["r_mm"].get<double>() * MM, true, false);
    } else if (t == "poly") {
        c.fill_color(spec_color(el));
        const auto& pts = el["points_mm"];
        c.move_to(X(pts[0][0]), Y(pts[0][1]));
        for (size_t i = 1; i < pts.size(); ++i) c.line_to(X(pts[i][0]), Y(pts[i][1]));
        c.close();
        c.draw_path(true, false);
    } else if (t == "varnish") {
        double x = el["rect_mm"][0], y = el["rect_mm"][1], w = el["rect_mm"][2], h = el["rect_mm"][3];
        c.fill_color(Color::separation("Varnish", 0, 0, 0, 0.15));
        c.rect(X(x), Y(y + h), w * MM, h * MM, true, false);
    } else if (t == "ean13" || t == "qr") {
        double x = el["rect_mm"][0], y = el["rect_mm"][1], w = el["rect_mm"][2], h = el["rect_mm"][3];
        if (t == "ean13") draw_ean13(c, el["data"], X(x), Y(y + h), w * MM, h * MM);
        else draw_qr(c, el["data"], X(x), Y(y + h), w * MM, h * MM);
    } else {
        throw Error(ErrorCode::InvalidArgument, "okänd elementtyp " + t);
    }
}

}  // namespace

Bytes render_label_pdf(const json& spec) {
    double W = spec["size_mm"][0], H = spec["size_mm"][1];
    PdfDocument doc;
    auto& c = doc.new_page(W * MM, H * MM);
    c.set_trim_box(0, 0, W * MM, H * MM);
    for (const auto& el : spec["elements"]) {
        std::string lay = el.value("layer", "");
        if (!lay.empty()) {
            c.save();
            c.begin_layer(lay);
        }
        draw_element(c, el, H);
        if (!lay.empty()) {
            c.end_layer();
            c.restore();
        }
    }
    if (spec.contains("dieline") && !spec["dieline"].is_null()) {
        c.stroke_color(Color::separation("CutContour", 0, 1, 0, 0));
        c.line_width(0.25);
        c.round_rect(0, 0, W * MM, H * MM, spec["dieline"].value("radius_mm", 0.0) * MM, false, true);
    }
    return doc.save();
}

}  // namespace pc::testgen
