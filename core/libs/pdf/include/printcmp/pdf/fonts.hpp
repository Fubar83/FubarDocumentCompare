// Typsnitt som anroparen tillhandahåller för PDF:er där de inte är inbäddade.
//
// En FontSet är oföränderlig och identifieras av sitt innehåll (hash). apply_fonts byter namn på
// de inte inbäddade typsnitten i en kopia av PDF:en till en token som pekar på en fontfil i
// uppsättningen; PDFium hämtar sedan fontfilen genom bibliotekets gemensamma fontuppslag. Samma
// typsnittsnamn med olika filer i olika uppsättningar kan därför användas parallellt utan att
// blandas ihop (även PDFiums egen fontcache är nycklad på innehållet).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "printcmp/pdf/document.hpp"

namespace pc::pdf {

// En fontfil (TrueType eller OpenType/CFF) med namn och mått ur filens tabeller.
struct FontFile {
    std::string sha256;
    std::shared_ptr<const Bytes> data;
    std::string postscript_name, family, full_name, subfamily;
    std::vector<std::string> aliases;       // extra namn som PDF:er kan använda
    int units_per_em = 1000;
    int weight = 400;
    bool italic = false;
    uint16_t fs_type = 0;                   // OS/2 fsType: inbäddningsrättigheter (0 = fri)

    uint16_t glyph(char32_t cp) const;      // 0 = saknas
    int advance(uint16_t gid) const;        // fontenheter

    std::unordered_map<char32_t, uint16_t> cmap;
    std::vector<uint16_t> advances;
};

// Läser en TTF/OTF. Kastar FileFormat om filen inte är ett typsnitt (TTC stöds inte).
std::shared_ptr<const FontFile> parse_font(Bytes data, std::vector<std::string> aliases = {});

// Vad som händer med typsnitt som varken är inbäddade eller finns i uppsättningen.
enum class FontFallback {
    System,    // operativsystemets typsnitt (PDFiums vanliga uppslag)
    Builtin,   // PDFiums inbyggda ersättningar: samma resultat på alla maskiner
};

class FontSet {
public:
    FontSet(std::vector<std::shared_ptr<const FontFile>> fonts, FontFallback fallback = FontFallback::Builtin);
    const std::vector<std::shared_ptr<const FontFile>>& fonts() const { return fonts_; }
    FontFallback fallback() const { return fallback_; }
    const std::string& hash() const { return hash_; }   // innehåll, alias och reserv
    // Fontfilen för ett typsnittsnamn i en PDF (delmängdsprefix tas bort): PostScript-namn, alias,
    // fullständigt namn och till sist namnen utan skiljetecken och versaler.
    std::shared_ptr<const FontFile> match(const std::string& pdf_font_name) const;

private:
    std::vector<std::shared_ptr<const FontFile>> fonts_;
    FontFallback fallback_;
    std::string hash_;
    std::unordered_map<std::string, std::shared_ptr<const FontFile>> exact_, loose_;
};

// Hur ett typsnitt i PDF:en hanteras.
struct FontDecision {
    std::string name;            // utan delmängdsprefix
    // Embedded: i PDF:en. Supplied: ur uppsättningen. Standard: ett av PDF:s 14 standardtypsnitt (eller
    // vanliga alias som Arial), PDFiums inbyggda. Fallback: PDFiums inbyggda ersättning. System: OS:ets uppslag.
    enum class Kind { Embedded, Supplied, Standard, Fallback, System } kind = Kind::Embedded;
    std::string sha256;          // Supplied: fontfilen
};

// Kopia av PDF:en där inte inbäddade typsnitt pekar på uppsättningens fontfiler (eller på PDFiums
// inbyggda ersättningar). Fontfilerna registreras så länge uppsättningen lever.
Bytes apply_fonts(const Bytes& pdf, const FontSet& set, std::vector<FontDecision>* decisions = nullptr);

// Hur uppsättningen skulle hantera typsnitten, utan att ändra PDF:en.
std::vector<FontDecision> font_decisions(const Bytes& pdf, const FontSet* set);

// Stämmer en fontfil med ett inte inbäddat typsnitt i PDF:en? Jämför stegbredderna PDF:en räknar
// med (/Widths) mot fontfilens för de tecken som används, och letar efter tecken som saknas i filen.
struct FontCheck {
    std::string font;
    int characters = 0;          // olika tecken som används med typsnittet
    int width_mismatches = 0;    // tecken vars bredd skiljer mer än 2 % av em
    int missing_glyphs = 0;      // tecken som fontfilen saknar
    double max_width_error = 0;  // största skillnad, andel av em
    std::string example;         // några av de avvikande tecknen
    bool ok = false;
};
FontCheck validate_font(const Document& doc, const std::string& pdf_font_name, const FontFile& candidate, int page_index = -1);

// Typsnittsnamnet som det ska visas: utan delmängdsprefix och utan apply_fonts token.
std::string display_font_name(const std::string& pdf_font_name);

// Antal gånger PDFium har hämtat en fontfil ur en uppsättning (för tester och diagnostik).
uint64_t supplied_font_loads();

}  // namespace pc::pdf
