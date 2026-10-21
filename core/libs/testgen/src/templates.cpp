// Etikettmallar för testgeneratorn. Alla mått i mm, origo övre vänster, text = baslinje.
#include "printcmp/testgen/label.hpp"

namespace pc::testgen {

namespace {

const char* OAT_DRINK = R"({
  "name": "oat_drink", "size_mm": [80, 50], "dieline": {"shape": "rounded_rect", "radius_mm": 3},
  "elements": [
    {"id": "band", "type": "rect", "rect_mm": [0, 40, 80, 10], "color": {"spot": "PANTONE 347 C", "cmyk": [0.9, 0.0, 0.8, 0.1]}},
    {"id": "logo_circle", "type": "circle", "center_mm": [12, 10], "r_mm": 6, "color": {"cmyk": [0.0, 0.75, 0.9, 0.0]}},
    {"id": "logo_leaf", "type": "poly", "points_mm": [[9, 12], [12, 5.5], [15, 12]], "color": {"cmyk": [0.0, 0.1, 0.9, 0.0]}},
    {"id": "title", "type": "text", "text": "Ekologisk Havredryck", "font": "DejaVuSans-Bold", "size_pt": 11, "pos_mm": [21, 11], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "subtitle", "type": "text", "text": "Naturell · 1 liter", "font": "DejaVuSans", "size_pt": 7, "pos_mm": [21, 16], "color": {"cmyk": [0.6, 0.2, 0.0, 0.3]}},
    {"id": "ingr1", "type": "text", "text": "Ingredienser: vatten, havre 10 %,", "font": "DejaVuSans", "size_pt": 6, "pos_mm": [4, 23], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "ingr2", "type": "text", "text": "rapsolja, kalciumkarbonat, salt.", "font": "DejaVuSans", "size_pt": 6, "pos_mm": [4, 26.2], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "store", "type": "text", "text": "Förvaras kylt efter öppning.", "font": "DejaVuSans", "size_pt": 6, "pos_mm": [4, 29.4], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "intl", "type": "text", "text": "Овсяный напиток · Ρόφημα βρώμης", "font": "DejaVuSans", "size_pt": 5.5, "pos_mm": [4, 32.6], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "lot", "type": "variable", "text": "LOT 2410-0001", "font": "DejaVuSans", "size_pt": 5.5, "pos_mm": [4, 37.2], "color": {"cmyk": [0, 0, 0, 1]}, "rect_mm": [3.5, 34.6, 22, 3.6]},
    {"id": "ean", "type": "ean13", "data": "731086500123", "rect_mm": [53, 19, 24, 17]},
    {"id": "best", "type": "text", "text": "Bäst före: se toppen", "font": "DejaVuSans-Bold", "size_pt": 6.5, "pos_mm": [4, 46.4], "color": {"cmyk": [0, 0, 0, 0]}},
    {"id": "eco", "type": "circle", "center_mm": [72, 45], "r_mm": 3.2, "color": {"cmyk": [0, 0, 0, 0]}},
    {"id": "varnish", "type": "varnish", "rect_mm": [5, 3, 14, 14]}
  ]})";

const char* SPICE = R"({
  "name": "spice", "size_mm": [70, 45], "dieline": {"shape": "rounded_rect", "radius_mm": 4},
  "elements": [
    {"id": "bg", "type": "rect", "rect_mm": [0, 0, 70, 45], "color": {"cmyk": [0.02, 0.06, 0.22, 0.0]}},
    {"id": "header", "type": "rect", "rect_mm": [0, 0, 70, 11], "color": {"cmyk": [0.1, 0.85, 0.95, 0.25]}},
    {"id": "title", "type": "text", "text": "Rökt Paprika", "font": "DejaVuSerif", "size_pt": 13, "pos_mm": [4, 8.2], "color": {"cmyk": [0, 0, 0, 0]}},
    {"id": "weight", "type": "text", "text": "45 g", "font": "DejaVuSans-Bold", "size_pt": 8, "pos_mm": [57, 7.6], "color": {"cmyk": [0, 0, 0, 0]}},
    {"id": "desc1", "type": "text", "text": "Mild och rökig. Passar till", "font": "DejaVuSansCondensed", "size_pt": 6.5, "pos_mm": [4, 16.5], "color": {"cmyk": [0.3, 0.6, 0.7, 0.6]}},
    {"id": "desc2", "type": "text", "text": "grytor, korv och grönsaker.", "font": "DejaVuSansCondensed", "size_pt": 6.5, "pos_mm": [4, 19.8], "color": {"cmyk": [0.3, 0.6, 0.7, 0.6]}},
    {"id": "origin", "type": "text", "text": "Ursprung: Spanien", "font": "DejaVuSans", "size_pt": 5.5, "pos_mm": [4, 24.0], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "pepper1", "type": "circle", "center_mm": [12, 33], "r_mm": 4.2, "color": {"cmyk": [0.05, 0.95, 0.9, 0.05]}},
    {"id": "pepper2", "type": "circle", "center_mm": [19, 35], "r_mm": 3.0, "color": {"cmyk": [0.0, 0.6, 0.95, 0.0]}},
    {"id": "stem", "type": "poly", "points_mm": [[11, 28.5], [13.5, 25.8], [14.5, 27.0], [12.2, 29.2]], "color": {"cmyk": [0.7, 0.1, 0.9, 0.2]}},
    {"id": "qr", "type": "qr", "data": "https://example.com/p/4410", "rect_mm": [50, 15, 16, 16]},
    {"id": "lot", "type": "variable", "text": "BF 2027-03 L0412", "font": "DejaVuSans", "size_pt": 5, "pos_mm": [41, 41.0], "color": {"cmyk": [0, 0, 0, 1]}, "rect_mm": [40.5, 38.6, 26, 3.4]},
    {"id": "footer", "type": "rect", "rect_mm": [0, 43, 70, 2], "color": {"cmyk": [0.1, 0.85, 0.95, 0.25]}}
  ]})";

const char* NUTRITION_ROWS = R"([
    ["Näringsvärde", "per 100 ml"], ["Energi", "189 kJ / 45 kcal"], ["Fett", "1,5 g"], ["varav mättat fett", "0,2 g"],
    ["Kolhydrater", "6,6 g"], ["varav sockerarter", "4,1 g"], ["Protein", "1,0 g"], ["Salt", "0,10 g"]])";

// Tabell med rutnät, konturerad produkttitel och präglad punktskrift.
const char* NUTRITION_GRID = R"({
  "name": "nutrition_grid", "size_mm": [75, 58], "dieline": {"shape": "rounded_rect", "radius_mm": 3},
  "elements": [
    {"id": "brand", "type": "outlined", "text": "Havrebryggan", "font": "DejaVuSans-Bold", "size_pt": 12, "pos_mm": [4, 8.5], "color": {"cmyk": [1.0, 0.7, 0.0, 0.3]}},
    {"id": "sub", "type": "text", "text": "Havredryck · 1 liter", "font": "DejaVuSans", "size_pt": 6.5, "pos_mm": [4, 13.2], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "nut", "type": "table", "pos_mm": [4, 16.0], "col_widths_mm": [23, 21], "row_h_mm": 3.3, "rows": null,
     "font": "DejaVuSans", "header_font": "DejaVuSans-Bold", "size_pt": 5.5, "align": ["left", "right"], "rules": "grid", "rule_w_mm": 0.15, "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "ean", "type": "ean13", "data": "731086500456", "rect_mm": [51, 17, 21, 15]},
    {"id": "store", "type": "text", "text": "Skakas före användning.", "font": "DejaVuSans", "size_pt": 5.5, "pos_mm": [50, 37.5], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "braille", "type": "braille", "text": "havredryck", "origin_mm": [6.0, 48.0], "dot_d_mm": 1.5, "dot_pitch_mm": 2.5, "cell_pitch_mm": 6.0}
  ]})";

// Tabell utan linjer (bara justering), konturerad titel och punktskrift.
const char* NUTRITION_OPEN = R"({
  "name": "nutrition_open", "size_mm": [75, 58], "dieline": {"shape": "rounded_rect", "radius_mm": 3},
  "elements": [
    {"id": "band", "type": "rect", "rect_mm": [0, 0, 75, 3.5], "color": {"cmyk": [0.0, 0.35, 0.9, 0.0]}},
    {"id": "brand", "type": "outlined", "text": "Mandeldryck", "font": "DejaVuSans-Bold", "size_pt": 11, "pos_mm": [4, 10.0], "color": {"cmyk": [0.1, 0.6, 1.0, 0.3]}},
    {"id": "sub", "type": "text", "text": "Osötad · 1 liter", "font": "DejaVuSans", "size_pt": 6.5, "pos_mm": [4, 14.4], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "nut", "type": "table", "pos_mm": [4, 17.0], "col_widths_mm": [27, 19], "row_h_mm": 3.2, "rows": null,
     "font": "DejaVuSans", "header_font": "DejaVuSans-Bold", "size_pt": 5.5, "align": ["left", "right"], "rules": "none", "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "ingr", "type": "text", "text": "Ingredienser: vatten, mandel 2,5 %.", "font": "DejaVuSans", "size_pt": 5.5, "pos_mm": [4, 46.0], "color": {"cmyk": [0, 0, 0, 1]}},
    {"id": "qr", "type": "qr", "data": "https://example.com/p/7710", "rect_mm": [55, 18, 15, 15]},
    {"id": "braille", "type": "braille", "text": "mandel", "origin_mm": [36.0, 50.0], "dot_d_mm": 1.5, "dot_pitch_mm": 2.5, "cell_pitch_mm": 6.0}
  ]})";

}  // namespace

std::vector<std::string> template_names() { return {"oat_drink", "spice", "nutrition_grid", "nutrition_open"}; }

json label_template(const std::string& name) {
    const char* src = name == "oat_drink" ? OAT_DRINK : name == "spice" ? SPICE : name == "nutrition_grid" ? NUTRITION_GRID
                    : name == "nutrition_open" ? NUTRITION_OPEN : nullptr;
    if (!src) throw Error(ErrorCode::InvalidArgument, "okänd mall: " + name);
    json t = json::parse(src);
    for (auto& e : t["elements"])
        if (e["type"] == "table") e["rows"] = json::parse(NUTRITION_ROWS);
    return t;
}

}  // namespace pc::testgen
