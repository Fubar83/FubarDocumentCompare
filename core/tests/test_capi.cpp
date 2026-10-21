// C-API:t (FubarDocumentCompare.Native.dll) mot testdata/ – samma flöde som .NET-bindningen använder.
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "fdc/fdc.h"
#include "test.hpp"

namespace {

std::string data(const char* rel) { return std::string(FDC_TESTDATA) + "/" + rel; }

struct Engine {
    fdc_engine* e = nullptr;
    Engine() {
        fdc_engine_options o{sizeof(fdc_engine_options), 2};
        fdc_engine_create(&o, &e);
    }
    ~Engine() { fdc_engine_free(e); }
};

fdc_reference* open_ref(fdc_engine* e, const std::string& path) {
    fdc_reference_options o;
    o.size = sizeof o;
    fdc_reference_options_init(&o);
    fdc_reference* r = nullptr;
    return fdc_reference_open(e, path.c_str(), &o, &r) == FDC_OK ? r : nullptr;
}

}  // namespace

TEST(capi_version) {
    CHECK_EQ(fdc_api_version(), uint32_t(FDC_API_VERSION));
    CHECK(std::strlen(fdc_version_string()) > 0);
}

TEST(capi_reference_inspection) {
    Engine en;
    auto* r = open_ref(en.e, data("label/ref.pdf"));
    CHECK(r != nullptr);
    if (!r) return;
    CHECK_NEAR(fdc_reference_width_mm(r), 80.0, 0.01);
    CHECK_NEAR(fdc_reference_height_mm(r), 50.0, 0.01);
    CHECK_EQ(fdc_reference_barcode_count(r), 1);
    CHECK_EQ(std::string(fdc_reference_barcode_data(r, 0)), std::string("7310865001238"));
    CHECK_EQ(fdc_reference_spot_count(r), 3);
    fdc_reference_free(r);
}

TEST(capi_errors_are_typed) {
    Engine en;
    fdc_reference_options o;
    o.size = sizeof o;
    fdc_reference_options_init(&o);
    fdc_reference* r = nullptr;
    CHECK_EQ(fdc_reference_open(en.e, data("finns-inte.pdf").c_str(), &o, &r), FDC_ERR_FILE_NOT_FOUND);
    CHECK(r == nullptr);
    CHECK(std::strlen(fdc_last_error()) > 0);
    CHECK_EQ(fdc_reference_open(nullptr, nullptr, nullptr, &r), FDC_ERR_INVALID_ARGUMENT);
}

TEST(capi_pdf_compare_with_events) {
    Engine en;
    auto* r = open_ref(en.e, data("label/ref.pdf"));
    CHECK(r != nullptr);
    if (!r) return;
    fdc_source s;
    s.size = sizeof s;
    fdc_source_init(&s);
    s.kind = FDC_SOURCE_PDF_FILE;
    std::string smp = data("label/smp.pdf");
    s.path = smp.c_str();
    struct Seen {
        std::atomic<int> progress{0}, located{0}, completed{0};
    } seen;
    fdc_job* job = nullptr;
    CHECK_EQ(fdc_job_start_compare(en.e, r, &s, nullptr,
                                  [](fdc_job*, const fdc_event* ev, void* u) {
                                      auto* k = static_cast<Seen*>(u);
                                      if (ev->type == FDC_EVENT_PROGRESS) ++k->progress;
                                      if (ev->type == FDC_EVENT_LABEL_LOCATED) ++k->located;
                                      if (ev->type == FDC_EVENT_COMPLETED) ++k->completed;
                                  },
                                  &seen, &job),
             FDC_OK);
    fdc_result* res = nullptr;
    CHECK_EQ(fdc_job_wait(job, -1, &res), FDC_OK);
    fdc_job_free(job);
    CHECK(seen.progress > 0);
    CHECK_EQ(seen.located.load(), 1);
    CHECK_EQ(seen.completed.load(), 1);
    if (!res) return;
    CHECK_EQ(fdc_result_get_kind(res), FDC_RESULT_LABEL);
    fdc_summary sum{};
    sum.size = sizeof sum;
    CHECK_EQ(fdc_result_summary(res, &sum), FDC_OK);
    CHECK_EQ(sum.differences, fdc_result_finding_count(res));
    // facit: minst en skillnad per ändring, och en bekräftad flytt om facit har en
    auto truth = nlohmann::json::parse(std::ifstream(data("label/truth.json")));
    bool truth_moved = false;
    for (auto& m : truth["mutations"]) truth_moved |= m["kind"] == "Moved";
    CHECK(sum.differences >= int(truth["mutations"].size()) - 1);
    bool moved = false;
    for (int i = 0; i < fdc_result_finding_count(res); ++i) {
        fdc_finding f{};
        f.size = sizeof f;
        fdc_result_finding(res, i, &f);
        moved |= f.kind == FDC_KIND_MOVED && f.certainty == FDC_CERT_CONFIRMED;
        CHECK_EQ(std::strlen(f.fingerprint), size_t(16));
    }
    CHECK(moved || !truth_moved);
    int w = 0, h = 0;
    CHECK_EQ(fdc_result_render_crop(res, 0, FDC_SIDE_SAMPLE, 200, nullptr, 0, &w, &h), FDC_OK);
    CHECK(w > 0 && w <= 200 && h > 0);
    std::vector<unsigned char> px(size_t(w) * h * 3);
    CHECK_EQ(fdc_result_render_crop(res, 0, FDC_SIDE_SAMPLE, 200, px.data(), 10, &w, &h), FDC_ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(fdc_result_render_crop(res, 0, FDC_SIDE_SAMPLE, 200, px.data(), px.size(), &w, &h), FDC_OK);
    fdc_result_free(res);
    fdc_reference_free(r);
}

TEST(capi_data_check_matches_answer_key) {
    Engine en;
    fdc_job* job = nullptr;
    CHECK_EQ(fdc_job_start_data_check(en.e, data("label/smp.pdf").c_str(), data("label/expected.txt").c_str(), nullptr,
                                     nullptr, nullptr, &job),
             FDC_OK);
    // utan återanrop: händelserna köas och kan hämtas med poll
    fdc_event ev{};
    ev.size = sizeof ev;
    int n = 0;
    bool done = false;
    while (!done && fdc_job_poll(job, &ev, 30000) == FDC_OK) {
        ++n;
        done = ev.type == FDC_EVENT_COMPLETED || ev.type == FDC_EVENT_FAILED;
    }
    CHECK(done && ev.type == FDC_EVENT_COMPLETED);
    fdc_result* res = nullptr;
    CHECK_EQ(fdc_job_wait(job, 0, &res), FDC_OK);
    fdc_job_free(job);
    if (!res) return;
    // facit ur truth.json (data_check.items: fält -> found|near|missing|mismatch)
    std::map<std::string, fdc_data_status> want;
    const std::map<std::string, fdc_data_status> by_name{
        {"found", FDC_DATA_FOUND}, {"near", FDC_DATA_NEAR}, {"missing", FDC_DATA_MISSING}, {"mismatch", FDC_DATA_MISMATCH}};
    auto truth = nlohmann::json::parse(std::ifstream(data("label/truth.json")));
    for (auto& [field, status] : truth["data_check"]["items"].items()) want[field] = by_name.at(status.get<std::string>());
    CHECK_EQ(fdc_result_data_item_count(res), int(want.size()));
    for (int i = 0; i < fdc_result_data_item_count(res); ++i) {
        fdc_data_item d{};
        d.size = sizeof d;
        fdc_result_data_item(res, i, &d);
        if (want.count(d.field)) CHECK_EQ(int(d.status), int(want[d.field]));
        else CHECK(false);
    }
    fdc_result_free(res);
}

TEST(capi_cancel) {
    Engine en;
    fdc_job* job = nullptr;
    CHECK_EQ(fdc_job_start_documents(en.e, data("book/manuscript.pdf").c_str(), data("book/book.pdf").c_str(), nullptr, nullptr,
                                    nullptr, &job),
             FDC_OK);
    fdc_job_cancel(job);
    fdc_result* res = nullptr;
    fdc_status s = fdc_job_wait(job, -1, &res);
    // jobbet kan hinna bli klart före avbrottet; annars ska det rapporteras som avbrutet
    CHECK(s == FDC_ERR_CANCELLED || s == FDC_OK);
    if (res) fdc_result_free(res);
    fdc_job_free(job);
}

TEST(capi_pdf_services_are_content_addressed) {
    Engine en;
    std::ifstream f(data("label/ref.pdf"), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    fdc_pdf* p = nullptr;
    CHECK_EQ(fdc_pdf_load(en.e, bytes.data(), bytes.size(), &p), FDC_OK);
    if (!p) return;
    std::string sha = fdc_pdf_sha256(p);
    CHECK_EQ(sha.size(), size_t(64));
    fdc_pdf* again = nullptr;
    CHECK_EQ(fdc_pdf_find(en.e, sha.c_str(), &again), FDC_OK);
    CHECK(again && sha == fdc_pdf_sha256(again));
    fdc_pdf_free(again);
    fdc_pdf* none = nullptr;
    CHECK_EQ(fdc_pdf_find(en.e, std::string(64, 'a').c_str(), &none), FDC_ERR_NOT_FOUND);
    CHECK_EQ(fdc_pdf_load(en.e, "hej", 3, &none), FDC_ERR_FILE_FORMAT);

    fdc_buffer* meta = nullptr;
    CHECK_EQ(fdc_pdf_describe(en.e, p, nullptr, &meta), FDC_OK);
    auto m = nlohmann::json::parse(std::string((const char*)fdc_buffer_data(meta), fdc_buffer_size(meta)));
    fdc_buffer_free(meta);
    CHECK_EQ(m["sha256"].get<std::string>(), sha);
    CHECK_EQ(m["pageCount"].get<int>(), 1);
    CHECK(m["pages"][0]["widthMm"].get<double>() > 10);

    fdc_render_options o;
    fdc_render_options_init(&o);
    o.dpi = 72;
    fdc_image* img = nullptr;
    CHECK_EQ(fdc_pdf_render(en.e, p, &o, &img), FDC_OK);
    fdc_image_info info{sizeof(fdc_image_info)};
    CHECK_EQ(fdc_image_get_info(img, &info), FDC_OK);
    CHECK_EQ(info.channels, 3);
    CHECK_EQ(info.width, info.full_width);
    CHECK_NEAR(info.width, m["pages"][0]["widthMm"].get<double>() / 25.4 * 72, 1.0);
    fdc_buffer* png = nullptr;
    CHECK_EQ(fdc_image_encode(img, FDC_FORMAT_PNG, -1, &png), FDC_OK);
    CHECK(fdc_buffer_size(png) > 8 && std::memcmp(fdc_buffer_data(png), "\x89PNG", 4) == 0);
    fdc_buffer_free(png);
    fdc_image_free(img);

    fdc_tile t{sizeof(fdc_tile), 2, 0, 0, 256};
    fdc_tile_grid g{sizeof(fdc_tile_grid)};
    CHECK_EQ(fdc_pdf_tile_grid(en.e, p, &o, &t, &g), FDC_OK);
    CHECK_NEAR(g.dpi, 72.0, 1e-9);
    CHECK(g.columns >= 1 && g.rows >= 1);
    CHECK_EQ(fdc_pdf_render_tile(en.e, p, &o, &t, &img), FDC_OK);
    CHECK_EQ(fdc_image_get_info(img, &info), FDC_OK);
    CHECK_EQ(info.width, std::min(256, g.width_px));
    fdc_image_free(img);
    t.x = g.columns;
    CHECK_EQ(fdc_pdf_render_tile(en.e, p, &o, &t, &img), FDC_ERR_INVALID_ARGUMENT);

    fdc_buffer* ov = nullptr;
    CHECK_EQ(fdc_pdf_overlays(en.e, p, &o, FDC_OVERLAY_WORDS | FDC_OVERLAY_BARCODES, &ov), FDC_OK);
    auto j = nlohmann::json::parse(std::string((const char*)fdc_buffer_data(ov), fdc_buffer_size(ov)));
    fdc_buffer_free(ov);
    CHECK(!j["words"].empty());
    CHECK(j["objects"].empty());   // inte begärda
    o.page = 2;
    CHECK_EQ(fdc_pdf_render(en.e, p, &o, &img), FDC_ERR_INVALID_ARGUMENT);
    fdc_pdf_free(p);
}

TEST(capi_fontsets_are_immutable_values) {
    Engine en;
    fdc_fontset* empty = nullptr;
    CHECK_EQ(fdc_fontset_create(FDC_FONT_FALLBACK_BUILTIN, &empty), FDC_OK);
    fdc_fontset* bad = nullptr;
    CHECK_EQ(fdc_fontset_add(empty, "inte en font", 12, nullptr, &bad), FDC_ERR_FILE_FORMAT);
    std::string h = fdc_fontset_hash(empty);
    CHECK_EQ(h.size(), size_t(64));
    fdc_fontset* sys = nullptr;
    fdc_fontset_create(FDC_FONT_FALLBACK_SYSTEM, &sys);
    CHECK(h != fdc_fontset_hash(sys));
    fdc_buffer* j = nullptr;
    CHECK_EQ(fdc_fontset_describe(empty, &j), FDC_OK);
    auto d = nlohmann::json::parse(std::string((const char*)fdc_buffer_data(j), fdc_buffer_size(j)));
    fdc_buffer_free(j);
    CHECK_EQ(d["fallback"].get<std::string>(), std::string("builtin"));
    CHECK(d["fonts"].empty());

    std::ifstream f(data("label/ref.pdf"), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    fdc_pdf* p = nullptr;
    fdc_pdf_load(en.e, bytes.data(), bytes.size(), &p);
    fdc_render_options o;
    fdc_render_options_init(&o);
    o.dpi = 50;
    o.fonts = empty;
    fdc_image* img = nullptr;
    CHECK_EQ(fdc_pdf_render(en.e, p, &o, &img), FDC_OK);
    fdc_image_free(img);
    fdc_buffer* meta = nullptr;
    CHECK_EQ(fdc_pdf_describe(en.e, p, empty, &meta), FDC_OK);
    auto m = nlohmann::json::parse(std::string((const char*)fdc_buffer_data(meta), fdc_buffer_size(meta)));
    fdc_buffer_free(meta);
    for (auto& font : m["pages"][0]["fonts"]) CHECK(font.contains("status"));
    fdc_pdf_free(p);
    fdc_fontset_free(sys);
    fdc_fontset_free(empty);
}
