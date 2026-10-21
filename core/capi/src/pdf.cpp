// PDF-tjänsterna: metadata, rendering, rutor och överlägg. Tillståndslösa – motorns cache
// (nyckel: SHA-256) gör bara upprepade anrop snabbare.
#include "internal.hpp"
#include "printcmp/image/image.hpp"

namespace {

pdf::RenderSpec spec_of(const fdc_render_options& o) {
    pdf::RenderSpec s;
    s.dpi = o.dpi > 0 ? o.dpi : 150;
    s.box = o.box && *o.box ? o.box : "trim";
    s.x = o.x, s.y = o.y, s.width = o.width, s.height = o.height;
    s.pixels = o.pixels == FDC_PIXELS_RGBA   ? pdf::RenderSpec::Pixels::Rgba
               : o.pixels == FDC_PIXELS_GRAY ? pdf::RenderSpec::Pixels::Gray
                                             : pdf::RenderSpec::Pixels::Rgb;
    s.antialias = o.antialias != 0;
    s.annotations = o.annotations != 0;
    s.printing = o.printing != 0;
    return s;
}

docview::Variant variant_of(const fdc_render_options& o) {
    docview::Variant v;
    v.include_layers = split_list(o.include_layers);
    v.exclude_layers = split_list(o.exclude_layers);
    v.exclude_spots = split_list(o.exclude_spots);
    v.hide_nonprinting = o.hide_nonprinting != 0;
    if (o.fonts) v.fonts = o.fonts->set;
    return v;
}

fdc_buffer* buffer_of(const std::string& s) {
    auto b = new fdc_buffer();
    b->data.assign(s.begin(), s.end());
    return b;
}

docview::Tile tile_of(const fdc_tile* t) {
    if (!t) throw Error(ErrorCode::InvalidArgument, "rutan saknas");
    return {t->level, t->x, t->y, t->tile_size > 0 ? t->tile_size : 256};
}

}  // namespace

extern "C" {

FDC_API const void* fdc_buffer_data(const fdc_buffer* b) { return b ? b->data.data() : nullptr; }
FDC_API size_t fdc_buffer_size(const fdc_buffer* b) { return b ? b->data.size() : 0; }
FDC_API void fdc_buffer_free(fdc_buffer* b) { delete b; }

FDC_API fdc_status fdc_pdf_load(fdc_engine* e, const void* data, size_t len, fdc_pdf** out) {
    return guard([&] {
        if (!e || !out || (!data && len)) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, data eller out saknas");
        auto p = static_cast<const unsigned char*>(data);
        if (len < 5 || std::memcmp(p, "%PDF-", 5) != 0) {
            // PDF-huvudet får ligga en bit in i filen (skräp före %PDF- förekommer)
            std::string head(reinterpret_cast<const char*>(p), std::min<size_t>(len, 1024));
            if (head.find("%PDF-") == std::string::npos) return set_error(FDC_ERR_FILE_FORMAT, "inte en PDF");
        }
        auto h = new fdc_pdf();
        h->pdf = docview::remember(*e->cache, docview::Bytes(p, p + len));
        *out = h;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_pdf_find(fdc_engine* e, const char* sha256, fdc_pdf** out) {
    return guard([&] {
        if (!e || !out || !sha256) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, sha256 eller out saknas");
        auto p = docview::recall(*e->cache, text::lower(sha256));
        if (!p) return set_error(FDC_ERR_NOT_FOUND, "PDF:en finns inte i cachen");
        *out = new fdc_pdf{*p};
        return FDC_OK;
    });
}

FDC_API const char* fdc_pdf_sha256(const fdc_pdf* p) { return p ? p->pdf.sha256.c_str() : nullptr; }
FDC_API void fdc_pdf_free(fdc_pdf* p) { delete p; }

FDC_API fdc_status fdc_pdf_describe(fdc_engine* e, const fdc_pdf* p, const fdc_fontset* fonts, fdc_buffer** out) {
    return guard([&] {
        if (!e || !p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, pdf eller out saknas");
        *out = buffer_of(docview::to_json(docview::describe(*e->cache, p->pdf, fonts ? fonts->set : nullptr)).dump());
        return FDC_OK;
    });
}

FDC_API void fdc_render_options_init(fdc_render_options* o) {
    if (!o) return;
    std::memset(o, 0, sizeof *o);
    o->size = sizeof *o;
    o->page = 1;
    o->dpi = 150;
    o->antialias = 1;
    o->annotations = 1;
}

FDC_API fdc_status fdc_pdf_render(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* opt, fdc_image** out) {
    return guard([&] {
        if (!e || !p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, pdf eller out saknas");
        auto o = read_options(opt, fdc_render_options_init);
        *out = new fdc_image{docview::render(*e->cache, p->pdf, o.page, spec_of(o), variant_of(o))};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_pdf_tile_grid(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* opt, const fdc_tile* t,
                                     fdc_tile_grid* out) {
    return guard([&] {
        if (!e || !p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, pdf eller out saknas");
        auto o = read_options(opt, fdc_render_options_init);
        auto tile = tile_of(t);
        auto g = docview::tile_grid(*e->cache, p->pdf, o.page, tile.level, tile.size, spec_of(o).box);
        out->size = sizeof *out;
        out->columns = g.columns, out->rows = g.rows, out->width_px = g.width_px, out->height_px = g.height_px;
        out->dpi = docview::tile_dpi(tile.level);
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_pdf_render_tile(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* opt, const fdc_tile* t,
                                       fdc_image** out) {
    return guard([&] {
        if (!e || !p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, pdf eller out saknas");
        auto o = read_options(opt, fdc_render_options_init);
        *out = new fdc_image{docview::render_tile(*e->cache, p->pdf, o.page, tile_of(t), spec_of(o), variant_of(o))};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_pdf_overlays(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* opt, unsigned kinds,
                                    fdc_buffer** out) {
    return guard([&] {
        if (!e || !p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, pdf eller out saknas");
        auto o = read_options(opt, fdc_render_options_init);
        auto ov = docview::overlays(*e->cache, p->pdf, o.page, kinds ? kinds : (unsigned)FDC_OVERLAY_ALL, spec_of(o).box, variant_of(o));
        *out = buffer_of(docview::to_json(ov).dump());
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_image_get_info(const fdc_image* img, fdc_image_info* out) {
    return guard([&] {
        if (!img || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "bild eller out saknas");
        const auto& r = img->raster;
        out->size = sizeof *out;
        out->width = r.pixels.cols;
        out->height = r.pixels.rows;
        out->stride = (int)r.pixels.step[0];
        out->channels = r.pixels.channels();
        out->dpi = r.geom.dpi;
        out->x = r.x, out->y = r.y;
        out->full_width = r.full_width, out->full_height = r.full_height;
        out->data = r.pixels.data;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_image_encode(const fdc_image* img, fdc_image_format format, int quality, fdc_buffer** out) {
    return guard([&] {
        if (!img || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "bild eller out saknas");
        auto b = new fdc_buffer();
        b->data = image::encode(img->raster.pixels, format == FDC_FORMAT_JPEG ? image::Format::Jpeg : image::Format::Png, quality);
        *out = b;
        return FDC_OK;
    });
}

FDC_API void fdc_image_free(fdc_image* img) { delete img; }

}  // extern "C"

// ------------------------------------------------------------------ typsnitt

extern "C" {

FDC_API fdc_status fdc_fontset_create(fdc_font_fallback fallback, fdc_fontset** out) {
    return guard([&] {
        if (!out) return set_error(FDC_ERR_INVALID_ARGUMENT, "out saknas");
        auto fb = fallback == FDC_FONT_FALLBACK_SYSTEM ? pdf::FontFallback::System : pdf::FontFallback::Builtin;
        *out = new fdc_fontset{std::make_shared<const pdf::FontSet>(std::vector<std::shared_ptr<const pdf::FontFile>>{}, fb)};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_fontset_add(const fdc_fontset* base, const void* data, size_t len, const char* aliases, fdc_fontset** out) {
    return guard([&] {
        if (!base || !out || !data) return set_error(FDC_ERR_INVALID_ARGUMENT, "uppsättning, data eller out saknas");
        auto p = static_cast<const unsigned char*>(data);
        auto file = pdf::parse_font(pdf::Bytes(p, p + len), split_list(aliases));
        auto fonts = base->set->fonts();
        fonts.push_back(file);
        *out = new fdc_fontset{std::make_shared<const pdf::FontSet>(std::move(fonts), base->set->fallback())};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_fontset_combine(const fdc_fontset* a, const fdc_fontset* b, fdc_fontset** out) {
    return guard([&] {
        if (!a || !b || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "uppsättningar eller out saknas");
        auto fonts = a->set->fonts();
        fonts.insert(fonts.end(), b->set->fonts().begin(), b->set->fonts().end());
        *out = new fdc_fontset{std::make_shared<const pdf::FontSet>(std::move(fonts), a->set->fallback())};
        return FDC_OK;
    });
}

FDC_API const char* fdc_fontset_hash(const fdc_fontset* s) { return s ? s->set->hash().c_str() : nullptr; }

FDC_API fdc_status fdc_fontset_describe(const fdc_fontset* s, fdc_buffer** out) {
    return guard([&] {
        if (!s || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "uppsättning eller out saknas");
        *out = buffer_of(docview::to_json(*s->set).dump());
        return FDC_OK;
    });
}

FDC_API void fdc_fontset_free(fdc_fontset* s) { delete s; }

FDC_API fdc_status fdc_font_validate(fdc_engine* e, const fdc_pdf* p, int page, const char* font_name, const void* data,
                                     size_t len, fdc_buffer** out) {
    return guard([&] {
        if (!e || !p || !font_name || !data || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "argument saknas");
        auto d = static_cast<const unsigned char*>(data);
        auto file = pdf::parse_font(pdf::Bytes(d, d + len));
        *out = buffer_of(docview::to_json(docview::validate_font(*e->cache, p->pdf, font_name, *file, page)).dump());
        return FDC_OK;
    });
}

}  // extern "C"
