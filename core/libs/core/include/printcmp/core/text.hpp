// UTF-8 och Unicode (utf8proc): normalisering och teckenklasser som i Python.
#pragma once

#include <string>
#include <vector>

namespace pc::text {

std::string nfc(const std::string& s);
std::string nfkc(const std::string& s);
std::string casefold(const std::string& s);
std::string lower(const std::string& s);
std::string upper(const std::string& s);

std::vector<char32_t> decode(const std::string& s);
std::u32string to_u32(const std::string& s);        // UTF-8 -> kodpunkter (Pythons strängindex)
std::string from_u32(const std::u32string& s);
std::string encode(char32_t cp);
std::string encode(const std::vector<char32_t>& cps);
size_t length(const std::string& s);                 // antal kodpunkter

bool is_space(char32_t c);
bool is_alpha(char32_t c);
bool is_digit(char32_t c);        // Unicode Nd (Pythons isdigit för vanliga siffror)
bool is_alnum(char32_t c);
bool is_upper(char32_t c);
bool is_lower(char32_t c);
bool is_alpha_str(const std::string& s);           // icke-tom och bara bokstäver
bool is_digit_str(const std::string& s);

std::string strip(const std::string& s);
std::vector<std::string> split_ws(const std::string& s);
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);

}  // namespace pc::text
