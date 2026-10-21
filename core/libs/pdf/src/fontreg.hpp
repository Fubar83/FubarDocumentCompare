// Privat för pc_pdf: registret för tillhandahållna fontfiler och PDFiums fontuppslag.
#pragma once

#include <memory>
#include <string>

#include "printcmp/pdf/fonts.hpp"

namespace pc::pdf::detail {

// Tokenprefix i typsnittsnamn som apply_fonts skriver in: "FDCF<20 hex>_<namn>" pekar på en
// registrerad fontfil, "FDCN_<namn>" betyder "inget systemtypsnitt, använd PDFiums inbyggda".
constexpr const char* SUPPLIED_PREFIX = "FDCF";
constexpr const char* BUILTIN_PREFIX = "FDCN_";

// Registrerar fontfilen (svag referens: den lever så länge någon FontSet håller den) och ger tokenet.
std::string register_font(const std::shared_ptr<const FontFile>& font);
// Typsnittsnamnet utan apply_fonts token (delmängdsprefix behålls).
std::string untoken(const std::string& pdf_font_name);
// Installerar bibliotekets fontuppslag i PDFium (anropas av init()).
void install_font_info();

}  // namespace pc::pdf::detail
