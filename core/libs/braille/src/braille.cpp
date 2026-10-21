// Svensk punktskrift, grundnivå: tabellen och översättningen text <-> celler.
#include "printcmp/braille/braille.hpp"

#include <utility>

#include "printcmp/core/core.hpp"
#include "printcmp/core/text.hpp"

namespace pc::braille {

namespace {

const std::vector<std::pair<char32_t, Cell>>& letters() {
    static const std::vector<std::pair<char32_t, Cell>> L = {
        {U'a', {1}}, {U'b', {1, 2}}, {U'c', {1, 4}}, {U'd', {1, 4, 5}}, {U'e', {1, 5}}, {U'f', {1, 2, 4}},
        {U'g', {1, 2, 4, 5}}, {U'h', {1, 2, 5}}, {U'i', {2, 4}}, {U'j', {2, 4, 5}}, {U'k', {1, 3}}, {U'l', {1, 2, 3}},
        {U'm', {1, 3, 4}}, {U'n', {1, 3, 4, 5}}, {U'o', {1, 3, 5}}, {U'p', {1, 2, 3, 4}}, {U'q', {1, 2, 3, 4, 5}},
        {U'r', {1, 2, 3, 5}}, {U's', {2, 3, 4}}, {U't', {2, 3, 4, 5}}, {U'u', {1, 3, 6}}, {U'v', {1, 2, 3, 6}},
        {U'w', {2, 4, 5, 6}}, {U'x', {1, 3, 4, 6}}, {U'y', {1, 3, 4, 5, 6}}, {U'z', {1, 3, 5, 6}},
        {U'å', {1, 6}}, {U'ä', {3, 4, 5}}, {U'ö', {2, 4, 6}}};
    return L;
}
const std::vector<std::pair<char32_t, Cell>>& punct() {
    static const std::vector<std::pair<char32_t, Cell>> P = {{U',', {2}}, {U'.', {3}}, {U'-', {3, 6}}};
    return P;
}
const Cell NUMBER_SIGN{3, 4, 5, 6};
const Cell CAPITAL_SIGN{6};
const std::u32string DIGITS = U"1234567890";
const std::u32string AJ = U"abcdefghij";

const Cell* letter_cell(char32_t c) {
    for (auto& [k, v] : letters())
        if (k == c) return &v;
    return nullptr;
}

}  // namespace

std::vector<Cell> text_to_cells(const std::string& s) {
    std::vector<Cell> cells;
    bool in_number = false;
    for (char32_t ch : text::decode(s)) {
        char32_t lc = text::decode(text::lower(text::encode(ch)))[0];
        if (ch == U' ') {
            cells.push_back({});
            in_number = false;
        } else if (text::is_digit(ch) && DIGITS.find(ch) != std::u32string::npos) {
            if (!in_number) {
                cells.push_back(NUMBER_SIGN);
                in_number = true;
            }
            cells.push_back(*letter_cell(AJ[DIGITS.find(ch)]));
        } else if (letter_cell(lc)) {
            if (in_number && AJ.find(lc) != std::u32string::npos) cells.push_back({});
            in_number = false;
            if (text::is_upper(ch)) cells.push_back(CAPITAL_SIGN);
            cells.push_back(*letter_cell(lc));
        } else {
            const Cell* p = nullptr;
            for (auto& [k, v] : punct())
                if (k == ch) p = &v;
            if (!p) throw Error(ErrorCode::InvalidArgument, "tecknet '" + text::encode(ch) + "' saknas i punktskriftstabellen");
            in_number = false;
            cells.push_back(*p);
        }
    }
    return cells;
}

std::string cells_to_text(const std::vector<Cell>& cells) {
    std::string out;
    bool in_number = false, cap = false;
    for (const auto& c : cells) {
        if (c.empty()) {
            out += " ";
            in_number = false;
            continue;
        }
        if (c == NUMBER_SIGN) {
            in_number = true;
            continue;
        }
        if (c == CAPITAL_SIGN) {
            cap = true;
            continue;
        }
        bool done = false;
        for (auto& [k, v] : letters())
            if (v == c) {
                size_t aj = AJ.find(k);
                if (in_number && aj != std::u32string::npos) out += text::encode(DIGITS[aj]);
                else {
                    out += cap ? text::upper(text::encode(k)) : text::encode(k);
                    cap = false;
                }
                done = true;
                break;
            }
        if (done) continue;
        for (auto& [k, v] : punct())
            if (v == c) {
                out += text::encode(k);
                done = true;
                break;
            }
        if (!done) out += "?";
    }
    return out;
}

std::string unicode_of(const Cell& dots) {
    char32_t v = 0x2800;
    for (int n : dots) v += 1u << (n - 1);
    return text::encode(v);
}

}  // namespace pc::braille
