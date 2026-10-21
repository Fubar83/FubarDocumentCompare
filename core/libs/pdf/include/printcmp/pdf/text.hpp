// Textlagret per tecken (PDFium). Allt som läser text ur en PDF går genom detta gränssnitt,
// så att PDFium bara används inuti pc_pdf.
#pragma once

#include <string>
#include <vector>

#include "printcmp/pdf/document.hpp"

namespace pc::pdf {

struct TextChar {
    char32_t code = 0;                 // Unicode (0 = okänt; 2 = mjukt bindestreck som PDFium genererat)
    bool generated = false;            // PDFium har lagt till tecknet (mellanslag, radbyte)
    bool hyphen = false;               // avstavningsbindestreck
    double l = 0, r = 0, b = 0, t = 0;   // teckenruta (bläck), PDF-punkter
    bool has_loose = false;
    double loose_l = 0, loose_t = 0, loose_r = 0, loose_b = 0;   // lös ruta (stegbredd, ascent/descent)
    bool has_origin = false;
    double origin_x = 0, origin_y = 0;   // tecknets origo (baslinjen), PDF-punkter
    double font_size = 0;              // teckenstorlek utan textmatris
    bool has_matrix = false;
    double matrix[6] = {1, 0, 0, 1, 0, 0};   // a b c d e f
    std::string font_name;             // som i PDF:en (med eventuellt delmängdsprefix), tom om okänt
    int font_flags = 0;                // FontDescriptor /Flags
    int font_weight = -1;              // -1 = okänd
    unsigned fill_r = 0, fill_g = 0, fill_b = 0, fill_a = 0;
};

// Alla tecken på sidan (0-baserad) i PDFiums ordning. Trådsäker (tar pdf::lock()).
std::vector<TextChar> text_chars(const Document& doc, int page_index);

}  // namespace pc::pdf
