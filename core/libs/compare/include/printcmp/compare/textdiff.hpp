// Textlagren i två PDF:er på samma sida: ändrade, tillagda, borttagna och flyttade ord.
#pragma once

#include <string>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/pdf/document.hpp"

namespace pc::compare {

struct TextWord {
    std::string text;
    Box bbox;      // px i den egna sidans rendering
    double size = 0;
    std::string font;
};
std::vector<TextWord> extract_words(const pdf::Bytes& pdf, int page, const pdf::Geometry& geom);
std::vector<json> text_compare(const std::vector<TextWord>& ref, const std::vector<TextWord>& smp, const Mat3& H, double ppm,
                               double size_tol = 0.05);

}  // namespace pc::compare
