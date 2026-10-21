// pc_docview: tillståndslösa PDF-tjänster för visning. Varje funktion är en ren funktion av sina
// indata (PDF:ens innehåll, sida, val); Cache gör bara upprepade anrop snabbare.
//
// Koordinater i överlägg och metadata: millimeter, origo uppe till vänster i den valda rutan
// (standard trim), y nedåt – samma ruta och riktning som renderingen. px = mm · dpi / 25,4.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/docview/cache.hpp"
#include "printcmp/pdf/document.hpp"
#include "printcmp/pdf/fonts.hpp"

namespace pc::docview {

using Bytes = pdf::Bytes;

// En PDF identifierad av sitt innehåll.
struct Pdf {
    std::string sha256;                    // gemener, hex
    std::shared_ptr<const Bytes> bytes;
    static Pdf from_bytes(Bytes data);
};
// Lägg PDF:en i cachen (nyckel = SHA-256) och få tillbaka den.
Pdf remember(Cache& cache, Bytes data);
// PDF:en med denna SHA-256 om den finns i cachen.
std::optional<Pdf> recall(Cache& cache, const std::string& sha256);

// Vad som ska synas: lager (namn eller mönster) och dekorfärger att ta bort.
struct Variant {
    std::vector<std::string> include_layers, exclude_layers, exclude_spots;
    bool hide_nonprinting = false;         // ta bort stanslinje, lack och punktskrift (standardreglerna)
    std::shared_ptr<const pdf::FontSet> fonts;   // typsnitt för de som inte är inbäddade (null = systemets)
    bool empty() const;
    std::string key() const;
};

// ------------------------------------------------------------------ metadata

struct RectMm {
    double x = 0, y = 0, w = 0, h = 0;
};
struct LayerFacts {
    std::string name;
    bool default_on = true;
    bool used_on_page = false;
};
struct SpotFacts {
    std::string name;
    std::string role;                      // dieline, varnish, braille eller tom (tryckande dekorfärg)
};
struct FontFacts {
    std::string name, base_name, family;
    bool embedded = false, subset = false;
    int flags = 0, weight = 0, text_objects = 0;
    std::string status;                    // embedded, supplied, standard, fallback eller system
    std::string supplied_sha256;           // supplied: fontfilen
};
struct Warning {
    std::string code, message;
    int page = 0;                          // 1-baserad, 0 = dokumentet
};
struct PageMetadata {
    int number = 1;                        // 1-baserad
    double width_mm = 0, height_mm = 0;    // trimrutan
    int rotation_deg = 0;
    // rutorna relativt mediarutans övre vänstra hörn; trim/bleed/art saknas om de inte är satta
    RectMm media, crop;
    std::optional<RectMm> trim, bleed, art;
    std::vector<LayerFacts> layers;
    std::vector<SpotFacts> spots;
    std::vector<FontFacts> fonts;
};
struct Metadata {
    std::string sha256;
    size_t size_bytes = 0;
    int page_count = 0;
    int pdf_version = 0;                   // 17 = PDF 1.7
    std::vector<std::pair<std::string, std::string>> info;
    std::vector<PageMetadata> pages;
    std::vector<Warning> warnings;         // t.ex. font.not_embedded
};
// Typsnittens status (och varningarna) beror på uppsättningen; null = systemets typsnitt.
Metadata describe(Cache& cache, const Pdf& pdf, const std::shared_ptr<const pdf::FontSet>& fonts = nullptr);

// Stämmer fontfilen med typsnittet i PDF:en? page 0 = alla sidor.
pdf::FontCheck validate_font(Cache& cache, const Pdf& pdf, const std::string& font_name, const pdf::FontFile& candidate, int page = 0);

// ------------------------------------------------------------------ rendering

// Hela sidan eller ett utsnitt (spec.x/y/width/height). page är 1-baserad.
pdf::Raster render(Cache& cache, const Pdf& pdf, int page, const pdf::RenderSpec& spec, const Variant& variant = {});

// Rutpyramid: nivå L har upplösningen tile_dpi(L) = 18 · 2^L dpi (A4 är en ruta på nivå 0 med
// 256 px; nivå 6 = 1152 dpi). Rutan (x, y) täcker px [x·size, (x+1)·size) × [y·size, (y+1)·size).
struct Tile {
    int level = 0, x = 0, y = 0, size = 256;
};
double tile_dpi(int level);
struct TileGrid {
    int columns = 0, rows = 0, width_px = 0, height_px = 0;
};
TileGrid tile_grid(Cache& cache, const Pdf& pdf, int page, int level, int size = 256, const std::string& box = "trim");
// spec.dpi/x/y/width/height ersätts av rutans; kanten mot sidans slut blir mindre än size.
pdf::Raster render_tile(Cache& cache, const Pdf& pdf, int page, const Tile& tile, pdf::RenderSpec spec, const Variant& variant = {});

// ------------------------------------------------------------------ överlägg (bildkarta)

enum OverlayKind : unsigned { Words = 1, Objects = 2, Spots = 4, Barcodes = 8, AllOverlays = 15 };

struct WordBox {
    std::string text;
    RectMm box;
    std::string font;
    double size_pt = 0;
    std::array<unsigned, 4> color{0, 0, 0, 255};   // RGBA
};
struct ObjectBox {
    std::string type;                      // text, path, image, shading, form
    RectMm box;
};
struct SpotShape {
    std::string name, role;
    std::vector<std::vector<Pt>> polygons; // mm
};
struct BarcodeBox {
    std::string symbology, data;
    RectMm box;
};
struct Overlays {
    int page = 1;
    std::string box = "trim";
    double width_mm = 0, height_mm = 0;
    std::vector<WordBox> words;
    std::vector<ObjectBox> objects;
    std::vector<SpotShape> spots;
    std::vector<BarcodeBox> barcodes;
};
Overlays overlays(Cache& cache, const Pdf& pdf, int page, unsigned kinds = AllOverlays, const std::string& box = "trim",
                  const Variant& variant = {});

// ------------------------------------------------------------------ JSON (camelCase, för REST och bindningar)

json to_json(const Metadata& m);
json to_json(const Overlays& o);
json to_json(const pdf::FontCheck& c);
json to_json(const pdf::FontSet& s);

}  // namespace pc::docview
