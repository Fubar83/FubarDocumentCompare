// Motor och referens: skapa, öppna originalet och läsa det som ska beslutas före ett jobb.
#include "internal.hpp"

extern "C" {

FDC_API fdc_status fdc_engine_create(const fdc_engine_options* opt, fdc_engine** out) {
    return guard([&] {
        if (!out) return set_error(FDC_ERR_INVALID_ARGUMENT, "out saknas");
        auto e = new fdc_engine();
        int n = opt && opt->max_parallel_jobs > 0 ? opt->max_parallel_jobs : (int)std::max(1u, std::thread::hardware_concurrency());
        e->max_jobs = n;
        uint64_t cache = opt && opt->size >= offsetof(fdc_engine_options, cache_bytes) + sizeof(uint64_t) ? opt->cache_bytes : 0;
        e->cache = std::make_unique<docview::Cache>(cache ? (size_t)cache : size_t(1) << 30);
        pdf::init();
        *out = e;
        return FDC_OK;
    });
}

FDC_API void fdc_engine_free(fdc_engine* e) { delete e; }

FDC_API void fdc_reference_options_init(fdc_reference_options* o) {
    if (!o) return;
    std::memset(o, 0, sizeof *o);
    o->size = sizeof *o;
    o->page = 1;
    o->dpi = 600;
    o->profile = FDC_PROFILE_LABEL;
}

}  // extern "C"

namespace {

// Kastar vid fel; anroparna kör den inuti guard().
fdc_status open_reference(fdc_engine* e, const pdf::Bytes& bytes, const std::string& path, const fdc_reference_options* opt,
                         fdc_reference** out) {
    if (!e || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor eller out saknas");
    fdc_reference_options d;
    fdc_reference_options_init(&d);
    const fdc_reference_options& o = opt ? *opt : d;
    compare::ReferenceOptions ro;
    ro.page = o.page > 0 ? o.page : 1;
    ro.box = o.box ? o.box : "trim";
    ro.dpi = o.dpi > 0 ? o.dpi : 600;
    ro.profile = profile_name(o.profile);
    ro.include_layers = split_list(o.include_layers);
    ro.exclude_layers = split_list(o.exclude_layers);
    ro.exclude_spots = split_list(o.exclude_spots);
    if (o.tolerance_overrides_json && *o.tolerance_overrides_json) ro.tolerance_overrides = json::parse(o.tolerance_overrides_json);
    if (o.size >= offsetof(fdc_reference_options, fonts) + sizeof(o.fonts) && o.fonts) ro.fonts = o.fonts->set;
    auto r = new fdc_reference();
    try {
        r->model = compare::analyse_reference(bytes, path, ro);
    } catch (...) {
        delete r;
        throw;
    }
    for (auto& g : r->model->layers.ocgs) r->layer_names.push_back(g.name);
    r->spot_names = r->model->spots.spot_names;
    for (auto& c : r->model->codes) {
        r->code_data.push_back(c.data);
        r->code_sym.push_back(c.symbology);
    }
    for (auto& b : r->model->braille) r->braille_text.push_back(b.text());
    *out = r;
    return FDC_OK;
}

}  // namespace

extern "C" {

FDC_API fdc_status fdc_reference_open(fdc_engine* e, const char* path, const fdc_reference_options* opt, fdc_reference** out) {
    return guard([&] {
        if (!path) return set_error(FDC_ERR_INVALID_ARGUMENT, "sökväg saknas");
        return open_reference(e, read_file_bytes(path), path, opt, out);
    });
}

FDC_API fdc_status fdc_reference_open_memory(fdc_engine* e, const void* data, size_t len, const fdc_reference_options* opt, fdc_reference** out) {
    return guard([&] {
        if (!data || !len) return set_error(FDC_ERR_INVALID_ARGUMENT, "PDF-data saknas");
        const auto* p = static_cast<const unsigned char*>(data);
        return open_reference(e, pdf::Bytes(p, p + len), "", opt, out);
    });
}

FDC_API void fdc_reference_free(fdc_reference* r) { delete r; }
FDC_API double fdc_reference_width_mm(const fdc_reference* r) { return r ? r->model->ref.geom.width_mm() : 0; }
FDC_API double fdc_reference_height_mm(const fdc_reference* r) { return r ? r->model->ref.geom.height_mm() : 0; }
FDC_API int fdc_reference_layer_count(const fdc_reference* r) { return r ? (int)r->layer_names.size() : 0; }
FDC_API const char* fdc_reference_layer_name(const fdc_reference* r, int i) {
    return r && i >= 0 && i < (int)r->layer_names.size() ? r->layer_names[i].c_str() : nullptr;
}
FDC_API int fdc_reference_layer_default_on(const fdc_reference* r, int i) {
    return r && i >= 0 && i < (int)r->model->layers.ocgs.size() ? r->model->layers.ocgs[i].default_on : 0;
}
FDC_API int fdc_reference_spot_count(const fdc_reference* r) { return r ? (int)r->spot_names.size() : 0; }
FDC_API const char* fdc_reference_spot_name(const fdc_reference* r, int i) {
    return r && i >= 0 && i < (int)r->spot_names.size() ? r->spot_names[i].c_str() : nullptr;
}
FDC_API int fdc_reference_barcode_count(const fdc_reference* r) { return r ? (int)r->code_data.size() : 0; }
FDC_API const char* fdc_reference_barcode_data(const fdc_reference* r, int i) {
    return r && i >= 0 && i < (int)r->code_data.size() ? r->code_data[i].c_str() : nullptr;
}
FDC_API const char* fdc_reference_barcode_symbology(const fdc_reference* r, int i) {
    return r && i >= 0 && i < (int)r->code_sym.size() ? r->code_sym[i].c_str() : nullptr;
}
FDC_API int fdc_reference_table_count(const fdc_reference* r) { return r ? (int)r->model->tables.size() : 0; }
FDC_API int fdc_reference_braille_line_count(const fdc_reference* r) { return r ? (int)r->braille_text.size() : 0; }
FDC_API const char* fdc_reference_braille_text(const fdc_reference* r, int i) {
    return r && i >= 0 && i < (int)r->braille_text.size() ? r->braille_text[i].c_str() : nullptr;
}

FDC_API void fdc_source_init(fdc_source* s) {
    if (!s) return;
    std::memset(s, 0, sizeof *s);
    s->size = sizeof *s;
    s->page = 1;
}

FDC_API void fdc_compare_options_init(fdc_compare_options* o) {
    if (!o) return;
    std::memset(o, 0, sizeof *o);
    o->size = sizeof *o;
    o->max_skew_deg = 4.0;
    o->systematic_ratio = 0.8;
    o->consensus = 1;
    o->text_compare = 1;
    o->seed = 1;
}

FDC_API void fdc_document_options_init(fdc_document_options* o) {
    if (!o) return;
    std::memset(o, 0, sizeof *o);
    o->size = sizeof *o;
    o->min_move_words = 3;
    o->move_tol_mm = 0.3;
}


}  // extern "C"
