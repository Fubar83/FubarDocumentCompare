#include "printcmp/core/text.hpp"

#include <utf8proc.h>

#include <cstdlib>

#include "printcmp/core/core.hpp"
#include "printcmp/core/difflib.hpp"

namespace pc::text {

static std::string map(const std::string& s, int options) {
    utf8proc_uint8_t* out = nullptr;
    utf8proc_ssize_t n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(s.data()),
                                      static_cast<utf8proc_ssize_t>(s.size()), &out,
                                      static_cast<utf8proc_option_t>(options));
    if (n < 0) return s;   // ogiltig UTF-8: lämna orörd
    std::string r(reinterpret_cast<char*>(out), static_cast<size_t>(n));
    std::free(out);
    return r;
}

std::string nfc(const std::string& s) { return map(s, UTF8PROC_STABLE | UTF8PROC_COMPOSE); }
std::string nfkc(const std::string& s) { return map(s, UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT); }
std::string casefold(const std::string& s) {
    return map(s, UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
}

std::vector<char32_t> decode(const std::string& s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const utf8proc_uint8_t*>(s.data());
    utf8proc_ssize_t left = static_cast<utf8proc_ssize_t>(s.size());
    while (left > 0) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate(p, left, &cp);
        if (n <= 0) {
            out.push_back(0xFFFD);
            n = 1;
        } else {
            out.push_back(static_cast<char32_t>(cp));
        }
        p += n;
        left -= n;
    }
    return out;
}

std::string encode(char32_t cp) {
    utf8proc_uint8_t buf[4];
    utf8proc_ssize_t n = utf8proc_encode_char(static_cast<utf8proc_int32_t>(cp), buf);
    return std::string(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
}

std::string encode(const std::vector<char32_t>& cps) {
    std::string s;
    for (char32_t c : cps) s += encode(c);
    return s;
}

std::string lower(const std::string& s) {
    std::string o;
    for (char32_t c : decode(s)) o += encode(static_cast<char32_t>(utf8proc_tolower(static_cast<utf8proc_int32_t>(c))));
    return o;
}

std::string upper(const std::string& s) {
    std::string o;
    for (char32_t c : decode(s)) o += encode(static_cast<char32_t>(utf8proc_toupper(static_cast<utf8proc_int32_t>(c))));
    return o;
}

size_t length(const std::string& s) { return decode(s).size(); }

static utf8proc_category_t cat(char32_t c) { return utf8proc_category(static_cast<utf8proc_int32_t>(c)); }

bool is_space(char32_t c) {
    if (c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x1F || c == 0x85)
        return true;
    auto k = cat(c);
    return k == UTF8PROC_CATEGORY_ZS || k == UTF8PROC_CATEGORY_ZL || k == UTF8PROC_CATEGORY_ZP;
}

bool is_alpha(char32_t c) {
    auto k = cat(c);
    return k == UTF8PROC_CATEGORY_LU || k == UTF8PROC_CATEGORY_LL || k == UTF8PROC_CATEGORY_LT ||
           k == UTF8PROC_CATEGORY_LM || k == UTF8PROC_CATEGORY_LO;
}

bool is_digit(char32_t c) { return cat(c) == UTF8PROC_CATEGORY_ND || (c >= 0x00B2 && c <= 0x00B3) || c == 0x00B9; }
bool is_alnum(char32_t c) { return is_alpha(c) || is_digit(c) || cat(c) == UTF8PROC_CATEGORY_NL || cat(c) == UTF8PROC_CATEGORY_NO; }
bool is_upper(char32_t c) { return cat(c) == UTF8PROC_CATEGORY_LU || cat(c) == UTF8PROC_CATEGORY_LT; }
bool is_lower(char32_t c) { return cat(c) == UTF8PROC_CATEGORY_LL; }

bool is_alpha_str(const std::string& s) {
    auto cps = decode(s);
    if (cps.empty()) return false;
    for (char32_t c : cps)
        if (!is_alpha(c)) return false;
    return true;
}

bool is_digit_str(const std::string& s) {
    auto cps = decode(s);
    if (cps.empty()) return false;
    for (char32_t c : cps)
        if (!is_digit(c)) return false;
    return true;
}

std::string strip(const std::string& s) {
    auto cps = decode(s);
    size_t a = 0, b = cps.size();
    while (a < b && is_space(cps[a])) ++a;
    while (b > a && is_space(cps[b - 1])) --b;
    return encode(std::vector<char32_t>(cps.begin() + a, cps.begin() + b));
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char32_t c : decode(s)) {
        if (is_space(c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += encode(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += sep;
        o += v[i];
    }
    return o;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::u32string to_u32(const std::string& s) {
    auto v = decode(s);
    return std::u32string(v.begin(), v.end());
}

std::string from_u32(const std::u32string& s) { return encode(std::vector<char32_t>(s.begin(), s.end())); }

}  // namespace pc::text

namespace pc::difflib {

std::vector<char32_t> codepoints(const std::string& utf8) { return pc::text::decode(utf8); }

std::string from_codepoints(const std::vector<char32_t>& cps, size_t i, size_t j) {
    return pc::text::encode(std::vector<char32_t>(cps.begin() + i, cps.begin() + j));
}

std::string char_diff(const std::string& a, const std::string& b) {
    auto ca = codepoints(a), cb = codepoints(b);
    SequenceMatcher<char32_t> sm(ca, cb);
    std::string out;
    for (const auto& op : sm.get_opcodes()) {
        if (op.tag == Opcode::Equal) out += from_codepoints(ca, op.i1, op.i2);
        else out += "[" + from_codepoints(ca, op.i1, op.i2) + "→" + from_codepoints(cb, op.j1, op.j2) + "]";
    }
    return out;
}

double char_ratio(const std::string& a, const std::string& b) {
    auto ca = codepoints(a), cb = codepoints(b);
    SequenceMatcher<char32_t> sm(ca, cb);
    return sm.ratio();
}

}  // namespace pc::difflib
