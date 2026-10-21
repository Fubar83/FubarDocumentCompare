// Etikettbeskrivningar (LabelSpec, JSON) -> PDF, plus exakt geometri per element.
// Mått i mm från etikettens övre vänstra hörn (y nedåt). Text: pos_mm = (x, baslinje).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/pdfwrite/writer.hpp"

namespace pc::testgen {

using pdfwrite::Bytes;

using Rect = std::array<double, 4>;   // x, y, w, h (mm)

Rect rect_union(const Rect& a, const Rect& b);

// Mallar: oat_drink, spice, nutrition_grid, nutrition_open
std::vector<std::string> template_names();
json label_template(const std::string& name);

// Mått
std::pair<double, double> text_metrics(const std::string& font, double size_pt);   // ascent, descent (mm, descent > 0)
double text_width_mm(const std::string& font, double size_pt, const std::u32string& s);
double text_width_mm(const std::string& font, double size_pt, const std::string& s);
Rect char_bbox_mm(const json& el, size_t i0, size_t i1);   // tecken i0..i1 (exklusivt), kodpunkter
inline Rect char_bbox_mm(const json& el, size_t i) { return char_bbox_mm(el, i, i + 1); }

struct TableCell {
    int row, col;
    std::string text, font;
    double x, baseline, size_pt, width;
    Rect cell;
};
struct TableRuleGeom {
    int index;
    char orient;
    double x0, y0, x1, y1;
};
struct TableLayout {
    std::vector<TableCell> cells;
    std::vector<TableRuleGeom> rules;
    Rect bbox;
    std::vector<double> xs;
};
TableLayout table_layout(const json& el);
Rect cell_text_bbox_mm(const TableCell& c);
Rect rule_bbox_mm(const TableRuleGeom& r, double lw_mm);

struct BrailleDot {
    double x, y;
    int cell, dot;
};
std::vector<BrailleDot> braille_dots_mm(const json& el);
Rect braille_cell_bbox_mm(const json& el, int i);
int braille_cell_count(const std::string& text);

Rect element_bbox_mm(const json& el);
Bytes render_label_pdf(const json& spec);

}  // namespace pc::testgen
