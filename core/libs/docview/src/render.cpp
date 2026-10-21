// Rendering av hela sidor, utsnitt och rutor.
#include <cmath>

#include "internal.hpp"

namespace pc::docview {

pdf::Raster render(Cache& cache, const Pdf& pdf, int page, const pdf::RenderSpec& spec, const Variant& variant) {
    auto base = detail::document(cache, pdf);
    int idx = detail::page_index(*base, page);
    auto v = detail::variant(cache, pdf, idx, variant);
    return pdf::render_page(*v.doc, idx, spec);
}

double tile_dpi(int level) {
    if (level < 0 || level > 8) throw Error(ErrorCode::InvalidArgument, "rutnivån måste vara 0–8");
    return 18.0 * std::ldexp(1.0, level);
}

TileGrid tile_grid(Cache& cache, const Pdf& pdf, int page, int level, int size, const std::string& box) {
    if (size < 64 || size > 4096) throw Error(ErrorCode::InvalidArgument, "rutstorleken måste vara 64–4096 px");
    auto doc = detail::document(cache, pdf);
    auto r = pdf::raster_size(*doc, detail::page_index(*doc, page), tile_dpi(level), box);
    return {(r.width + size - 1) / size, (r.height + size - 1) / size, r.width, r.height};
}

pdf::Raster render_tile(Cache& cache, const Pdf& pdf, int page, const Tile& t, pdf::RenderSpec spec, const Variant& variant) {
    auto grid = tile_grid(cache, pdf, page, t.level, t.size, spec.box);
    if (t.x < 0 || t.y < 0 || t.x >= grid.columns || t.y >= grid.rows)
        throw Error(ErrorCode::InvalidArgument, "rutan finns inte på nivå " + std::to_string(t.level));
    spec.dpi = tile_dpi(t.level);
    spec.x = t.x * t.size;
    spec.y = t.y * t.size;
    spec.width = spec.height = t.size;
    return render(cache, pdf, page, spec, variant);
}

}  // namespace pc::docview
