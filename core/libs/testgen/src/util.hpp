// Privat hjälp för testgeneratorn: UTF-32-text (Pythons strängindex är kodpunkter) och
// typerna från PDF-skrivaren.
#pragma once

#include <string>

#include "printcmp/core/core.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/pdfwrite/writer.hpp"

namespace pc::testgen {

using pdfwrite::Canvas;
using pdfwrite::Color;
using pdfwrite::PdfDocument;
using pdfwrite::font;

inline std::u32string u32(const std::string& s) { return text::to_u32(s); }
inline std::string u8(const std::u32string& s) { return text::from_u32(s); }
inline std::string u8(char32_t c) { return text::encode(c); }

constexpr double MM = pdfwrite::PT_PER_MM;   // punkter per mm (reportlab.lib.units.mm)

}  // namespace pc::testgen
