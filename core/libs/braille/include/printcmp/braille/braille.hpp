// pc_braille: svensk punktskrift, grundnivå – text <-> celler (punkter 1..6).
// Används av analysen (pc_artwork) och testgeneratorn (pc_testgen).
#pragma once

#include <set>
#include <string>
#include <vector>

namespace pc::braille {

using Cell = std::set<int>;   // punkter 1..6; tom cell = mellanslag

// a–ö, siffror (med siffertecken), versaltecken, komma, punkt, bindestreck.
// Kastar pc::Error för tecken som saknas i tabellen.
std::vector<Cell> text_to_cells(const std::string& text);
std::string cells_to_text(const std::vector<Cell>& cells);   // okänd cell -> "?"
std::string unicode_of(const Cell& dots);                    // U+2800-blocket

}  // namespace pc::braille
