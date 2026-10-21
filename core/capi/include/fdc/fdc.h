/*
 * FubarDocumentCompare – C-API (stabil ABI), FubarDocumentCompare.Native.dll / libFubarDocumentCompare.Native.so.
 *
 * Modell:  motor (fdc_engine) → referens (fdc_reference, analyseras en gång, återanvänds, trådsäker)
 *          → jobb (fdc_job, asynkront, händelser, avbrott) → resultat (fdc_result, oföränderligt)
 *
 * Regler:
 *   - Alla handtag är ogenomskinliga. Den som skapar något frigör det (pc_*_free).
 *   - Strängar är UTF-8. Strängar i ett resultat lever lika länge som resultatet; strängar i en
 *     händelse lever under återanropet (eller till nästa fdc_job_poll).
 *   - Varje struct börjar med `size` (sätts av anroparen till sizeof). Nya fält läggs bara till sist.
 *   - PDF-tjänsterna (fdc_pdf_*) är tillståndslösa: varje anrop är en funktion av PDF:ens innehåll
 *     och valen. Motorns cache (nyckel: SHA-256) gör bara upprepade anrop snabbare.
 *   - Inspektera → konfigurera → kör: allt som kräver ett beslut (lager, dekorfärger) läses från
 *     referensen innan jobbet startar. Ett jobb ställer aldrig frågor medan det körs.
 *   - fdc_engine och fdc_reference får delas mellan trådar. Ett fdc_job används från ett ställe.
 *   - Återanrop sker på en arbetstråd. Alternativ: fdc_job_poll från värdens egen tråd.
 */
#ifndef FDC_H
#define FDC_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef FDC_BUILD
#    define FDC_API __declspec(dllexport)
#  else
#    define FDC_API __declspec(dllimport)
#  endif
#else
#  define FDC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FDC_API_VERSION 2

typedef struct fdc_engine fdc_engine;
typedef struct fdc_reference fdc_reference;
typedef struct fdc_job fdc_job;
typedef struct fdc_result fdc_result;
typedef struct fdc_fontset fdc_fontset;
typedef struct fdc_buffer fdc_buffer;   /* bytes som biblioteket lämnar ut (JSON, bilder) */

typedef enum fdc_status {
    FDC_OK = 0,
    FDC_ERR_INVALID_ARGUMENT = 1,
    FDC_ERR_FILE_NOT_FOUND = 2,
    FDC_ERR_FILE_FORMAT = 3,
    FDC_ERR_PDF = 4,
    FDC_ERR_CANCELLED = 5,
    FDC_ERR_NOT_FOUND = 6,
    FDC_ERR_TIMEOUT = 7,
    FDC_ERR_BUFFER_TOO_SMALL = 8,
    FDC_ERR_INTERNAL = 99
} fdc_status;

typedef enum fdc_profile {
    FDC_PROFILE_LABEL = 0,       /* label_standard */
    FDC_PROFILE_LABEL_STRICT = 1,
    FDC_PROFILE_LEAFLET = 2,
    FDC_PROFILE_PDF = 3          /* pdf_vs_pdf (väljs automatiskt för PDF-prov) */
} fdc_profile;

typedef enum fdc_kind {
    FDC_KIND_MISSING = 0, FDC_KIND_ADDED, FDC_KIND_CHANGED, FDC_KIND_MOVED, FDC_KIND_COLOR, FDC_KIND_SHAPE,
    FDC_KIND_BARCODE, FDC_KIND_BRAILLE, FDC_KIND_UNCERTAIN, FDC_KIND_STYLE, FDC_KIND_STYLE_SYSTEMATIC,
    FDC_KIND_PAGE_NUMBER, FDC_KIND_OTHER = 99
} fdc_kind;

typedef enum fdc_severity { FDC_SEV_INFO = 0, FDC_SEV_MINOR, FDC_SEV_MAJOR, FDC_SEV_CRITICAL } fdc_severity;
typedef enum fdc_certainty { FDC_CERT_CONFIRMED = 0, FDC_CERT_HIGH, FDC_CERT_PROBABLE, FDC_CERT_POSSIBLE } fdc_certainty;
typedef enum fdc_stage {
    FDC_STAGE_RENDER = 0, FDC_STAGE_LOCATE, FDC_STAGE_REGISTER, FDC_STAGE_NORMALIZE, FDC_STAGE_DIFF, FDC_STAGE_FOLDS,
    FDC_STAGE_CLASSIFY, FDC_STAGE_AGGREGATE, FDC_STAGE_TEXT, FDC_STAGE_DATA_CHECK, FDC_STAGE_REPORT, FDC_STAGE_DONE
} fdc_stage;
typedef enum fdc_label_status { FDC_LABEL_PASS = 0, FDC_LABEL_WARN = 1, FDC_LABEL_FAIL = 2 } fdc_label_status;
typedef enum fdc_data_status { FDC_DATA_FOUND = 0, FDC_DATA_NEAR, FDC_DATA_MISSING, FDC_DATA_MISMATCH } fdc_data_status;
typedef enum fdc_result_kind { FDC_RESULT_LABEL = 0, FDC_RESULT_DOCUMENT = 1, FDC_RESULT_DATA_CHECK = 2 } fdc_result_kind;
typedef enum fdc_side { FDC_SIDE_REFERENCE = 0, FDC_SIDE_SAMPLE = 1 } fdc_side;

typedef struct fdc_point { double x, y; } fdc_point;

/* ------------------------------------------------------------------ version och fel */

FDC_API uint32_t fdc_api_version(void);
FDC_API const char* fdc_version_string(void);
/* Senaste felets text för den anropande tråden. */
FDC_API const char* fdc_last_error(void);

/* ------------------------------------------------------------------ motor */

typedef struct fdc_engine_options {
    uint32_t size;
    int max_parallel_jobs;      /* 0 = antal kärnor */
    uint64_t cache_bytes;       /* minnesbudget för PDF-cachen (dokument, varianter, metadata); 0 = 1 GiB */
} fdc_engine_options;

FDC_API fdc_status fdc_engine_create(const fdc_engine_options* opt, fdc_engine** out);
FDC_API void fdc_engine_free(fdc_engine* e);

/* ------------------------------------------------------------------ referens (originalet) */

typedef struct fdc_reference_options {
    uint32_t size;
    int page;                       /* 1-baserad */
    const char* box;                /* "trim" (standard), "media", "crop", "art" */
    double dpi;                     /* arbetsupplösning, standard 600 */
    fdc_profile profile;
    const char* include_layers;     /* namn eller mönster, separerade med ';' (NULL = alla) */
    const char* exclude_layers;
    const char* exclude_spots;      /* dekorfärger att utesluta, t.ex. "White;Varnish*" */
    const char* tolerance_overrides_json;   /* avancerat: {"delta_e": 5} */
    const fdc_fontset* fonts;       /* typsnitt som inte är inbäddade (original och PDF-prov); NULL = systemets */
} fdc_reference_options;

FDC_API void fdc_reference_options_init(fdc_reference_options* opt);
FDC_API fdc_status fdc_reference_open(fdc_engine* e, const char* pdf_path, const fdc_reference_options* opt, fdc_reference** out);
FDC_API fdc_status fdc_reference_open_memory(fdc_engine* e, const void* pdf, size_t len, const fdc_reference_options* opt,
                                          fdc_reference** out);
FDC_API void fdc_reference_free(fdc_reference* r);

FDC_API double fdc_reference_width_mm(const fdc_reference* r);
FDC_API double fdc_reference_height_mm(const fdc_reference* r);
FDC_API int fdc_reference_layer_count(const fdc_reference* r);
FDC_API const char* fdc_reference_layer_name(const fdc_reference* r, int i);
FDC_API int fdc_reference_layer_default_on(const fdc_reference* r, int i);
FDC_API int fdc_reference_spot_count(const fdc_reference* r);
FDC_API const char* fdc_reference_spot_name(const fdc_reference* r, int i);
FDC_API int fdc_reference_barcode_count(const fdc_reference* r);
FDC_API const char* fdc_reference_barcode_data(const fdc_reference* r, int i);
FDC_API const char* fdc_reference_barcode_symbology(const fdc_reference* r, int i);
FDC_API int fdc_reference_table_count(const fdc_reference* r);
FDC_API int fdc_reference_braille_line_count(const fdc_reference* r);
FDC_API const char* fdc_reference_braille_text(const fdc_reference* r, int i);

/* ------------------------------------------------------------------ prov */

typedef enum fdc_source_kind {
    FDC_SOURCE_SCAN_FILE = 0,     /* TIFF/PNG med dpi (eller dpi nedan) */
    FDC_SOURCE_PDF_FILE = 1,
    FDC_SOURCE_IMAGE_BUFFER = 2,  /* t.ex. direkt från scannerns SDK – ingen temporär fil */
    FDC_SOURCE_PDF_MEMORY = 3
} fdc_source_kind;

typedef struct fdc_source {
    uint32_t size;
    fdc_source_kind kind;
    const char* path;
    int page;                    /* PDF: 1-baserad */
    double dpi;                  /* 0 = läs ur filen */
    const void* data;            /* IMAGE_BUFFER: pixlar (RGB/RGBA/grå, 8 bit); PDF_MEMORY: PDF-bytes */
    size_t data_len;
    int width, height, stride, channels;
} fdc_source;

FDC_API void fdc_source_init(fdc_source* s);

/* ------------------------------------------------------------------ jämförelse */

typedef struct fdc_compare_options {
    uint32_t size;
    int expected_labels;         /* 0 = okänt */
    int rows, cols;              /* förväntad utskjutning (alternativ till expected_labels) */
    int multi;                   /* 1 = ark med flera etiketter; 0 = en etikett */
    double max_skew_deg;
    double systematic_ratio;     /* andel etiketter för "systematisk", standard 0.8 */
    int consensus;               /* jämför etiketter mot varandra (standard 1) */
    int text_compare;            /* PDF-prov: textlagret (standard 1) */
    int color_calibrated;        /* scannern är IT8-kalibrerad */
    int seed;
    const char* data_check_path; /* förväntade uppgifter (.txt/.csv/.json), NULL = ingen */
    const char* data_check_row;  /* "3" eller "kolumn=värde", NULL = bäst stämmande */
} fdc_compare_options;

FDC_API void fdc_compare_options_init(fdc_compare_options* o);

typedef struct fdc_document_options {
    uint32_t size;
    int fixed_layout;            /* 0 = omflödat (manus mot bok), 1 = fast layout */
    int compare_furniture;       /* 1 = jämför även sidhuvud/sidfot */
    int strict_typography;       /* 1 = citattecken/streck/ligaturer räknas */
    int ignore_case;
    int min_move_words;
    double move_tol_mm;
    const char* data_check_path;
} fdc_document_options;

FDC_API void fdc_document_options_init(fdc_document_options* o);

/* ------------------------------------------------------------------ händelser */

typedef enum fdc_event_type {
    FDC_EVENT_PROGRESS = 0,
    FDC_EVENT_SHEET_LOADED,       /* width_px, height_px, dpi */
    FDC_EVENT_LABEL_LOCATED,      /* index, rotation_deg, corners (provets px) */
    FDC_EVENT_FOLD_DETECTED,      /* corners[0..1] = linjens ändar (arkets px) */
    FDC_EVENT_LABEL_ANALYSED,     /* index, preliminary, label_status */
    FDC_EVENT_WARNING,            /* code, message */
    FDC_EVENT_COMPLETED,          /* jobbet klart: hämta resultatet med fdc_job_wait */
    FDC_EVENT_FAILED,             /* error, message */
    FDC_EVENT_CANCELLED
} fdc_event_type;

typedef struct fdc_event {
    uint32_t size;
    fdc_event_type type;
    fdc_stage stage;
    double fraction;
    int index;
    double rotation_deg;
    fdc_point corners[4];
    double width_px, height_px, dpi;
    int preliminary;
    fdc_label_status label_status;
    fdc_status error;
    const char* code;
    const char* message;
} fdc_event;

typedef void (*fdc_event_fn)(fdc_job* job, const fdc_event* ev, void* user);

/* ------------------------------------------------------------------ jobb */

FDC_API fdc_status fdc_job_start_compare(fdc_engine* e, fdc_reference* ref, const fdc_source* sample, const fdc_compare_options* opt,
                                      fdc_event_fn fn, void* user, fdc_job** out);
FDC_API fdc_status fdc_job_start_documents(fdc_engine* e, const char* reference_pdf, const char* sample_pdf,
                                        const fdc_document_options* opt, fdc_event_fn fn, void* user, fdc_job** out);
FDC_API fdc_status fdc_job_start_data_check(fdc_engine* e, const char* pdf, const char* expected_path, const char* row,
                                         fdc_event_fn fn, void* user, fdc_job** out);
/* Hämta nästa händelse (om inget återanrop används). FDC_ERR_TIMEOUT om ingen kom. */
FDC_API fdc_status fdc_job_poll(fdc_job* job, fdc_event* out, int timeout_ms);
FDC_API fdc_status fdc_job_cancel(fdc_job* job);
/* Vänta på resultatet (timeout_ms < 0 = obegränsat). Resultatet ägs av anroparen. */
FDC_API fdc_status fdc_job_wait(fdc_job* job, int timeout_ms, fdc_result** out);
FDC_API void fdc_job_free(fdc_job* job);

/* Synkron bekvämlighet: start + vänta. */
FDC_API fdc_status fdc_compare(fdc_engine* e, fdc_reference* ref, const fdc_source* sample, const fdc_compare_options* opt,
                            fdc_result** out);

/* ------------------------------------------------------------------ resultat */

typedef struct fdc_summary {
    uint32_t size;
    int differences;
    int labels_found, labels_expected;
    int labels_pass, labels_warn, labels_fail;
    int data_found, data_near, data_missing, data_mismatch;
    double match_ratio;          /* dokumentläge: andel lika ord */
} fdc_summary;

typedef struct fdc_finding {
    uint32_t size;
    int id;
    fdc_kind kind;
    fdc_severity severity;
    fdc_certainty certainty;
    int score;                   /* 0–100 */
    int systematic;
    int label;                   /* första etiketten (0 = hela arket/dokumentet) */
    int label_count;
    int page;                    /* dokumentläge: sida i referensen (annars provet), 1-baserad */
    double x_mm, y_mm, w_mm, h_mm;
    const char* category;        /* Text, TableCell, Barcode, Braille, Style, Layout … */
    const char* text_from;       /* NULL om ingen text */
    const char* text_to;
    const char* detail;          /* en rad för människor */
    const char* reasons;         /* säkerhetsgradens skäl, radbrytningsseparerade */
    const char* fingerprint;     /* stabilt id för "godkänn/ignorera" mellan versioner */
} fdc_finding;

typedef struct fdc_label_info {
    uint32_t size;
    int index, row, col;
    fdc_label_status status;
    double rotation_deg;
    fdc_point corners[4];         /* i provets px */
    int folds;
    double registration_rms_px;
} fdc_label_info;

typedef struct fdc_data_item {
    uint32_t size;
    const char* field;
    const char* expected;
    fdc_data_status status;
    const char* found_text;      /* near: kandidaten; mismatch: avkodad streckkod */
    const char* diff;
    int count;
    int page;
} fdc_data_item;

FDC_API fdc_result_kind fdc_result_get_kind(const fdc_result* r);
FDC_API fdc_status fdc_result_summary(const fdc_result* r, fdc_summary* out);
FDC_API int fdc_result_finding_count(const fdc_result* r);
FDC_API fdc_status fdc_result_finding(const fdc_result* r, int i, fdc_finding* out);
FDC_API int fdc_result_label_count(const fdc_result* r);
FDC_API fdc_status fdc_result_label(const fdc_result* r, int i, fdc_label_info* out);
FDC_API int fdc_result_data_item_count(const fdc_result* r);
FDC_API fdc_status fdc_result_data_item(const fdc_result* r, int i, fdc_data_item* out);
/* Utsnitt kring ett fynd som RGB (8 bit). Anropa med buffer = NULL för att få storleken. */
FDC_API fdc_status fdc_result_render_crop(const fdc_result* r, int finding_index, fdc_side side, int max_width,
                                       unsigned char* buffer, size_t buffer_len, int* width, int* height);
/* Export i minnet (för tjänster som sparar i ett objektlager): result.json, HTML-rapporten och
 * utsnitt som PNG. Bufferten ägs av anroparen (fdc_buffer_free). */
FDC_API fdc_status fdc_result_json(const fdc_result* r, fdc_buffer** out);
FDC_API fdc_status fdc_result_report_html(const fdc_result* r, fdc_buffer** out);
FDC_API fdc_status fdc_result_crop_png(const fdc_result* r, int finding_index, fdc_side side, int max_width, fdc_buffer** out);
/* Export till filer. */
FDC_API fdc_status fdc_result_write_report(const fdc_result* r, const char* html_path);
FDC_API fdc_status fdc_result_write_json(const fdc_result* r, const char* json_path);
FDC_API fdc_status fdc_result_write_layers(const fdc_result* r, const char* dir, double dpi);
FDC_API void fdc_result_free(fdc_result* r);

/* ------------------------------------------------------------------ buffertar */

/* Bytes som biblioteket lämnar ut (JSON, kodade bilder). Ägs av anroparen. */
FDC_API const void* fdc_buffer_data(const fdc_buffer* b);
FDC_API size_t fdc_buffer_size(const fdc_buffer* b);
FDC_API void fdc_buffer_free(fdc_buffer* b);

/* ------------------------------------------------------------------ PDF-tjänster (tillståndslösa) */

/* En PDF identifierad av sitt innehåll (SHA-256). Oföränderlig, får delas mellan trådar. */
typedef struct fdc_pdf fdc_pdf;
/* Bilder: oföränderliga pixlar (RGB, RGBA eller grå, 8 bit, rad för rad). */
typedef struct fdc_image fdc_image;

/* Läs in PDF-bytes (kopieras) och lägg dem i motorns cache. */
FDC_API fdc_status fdc_pdf_load(fdc_engine* e, const void* data, size_t len, fdc_pdf** out);
/* Samma PDF ur cachen utan att skicka bytes igen; FDC_ERR_NOT_FOUND om den inte finns där. */
FDC_API fdc_status fdc_pdf_find(fdc_engine* e, const char* sha256, fdc_pdf** out);
FDC_API const char* fdc_pdf_sha256(const fdc_pdf* p);
FDC_API void fdc_pdf_free(fdc_pdf* p);

/* Metadata som JSON (camelCase): sidor, rutor (mm), lager, dekorfärger, typsnitt, varningar. */
/* fonts: typsnittens status och varningar mot uppsättningen (NULL = systemets typsnitt). */
FDC_API fdc_status fdc_pdf_describe(fdc_engine* e, const fdc_pdf* p, const fdc_fontset* fonts, fdc_buffer** json_out);

typedef enum fdc_pixels { FDC_PIXELS_RGB = 0, FDC_PIXELS_RGBA = 1, FDC_PIXELS_GRAY = 2 } fdc_pixels;

typedef struct fdc_render_options {
    uint32_t size;
    int page;                    /* 1-baserad */
    double dpi;                  /* standard 150 */
    const char* box;             /* "trim" (standard), "crop", "media", "bleed", "art" */
    int x, y, width, height;     /* utsnitt i rastrets px; width/height 0 = till kanten */
    fdc_pixels pixels;           /* RGBA = genomskinlig bakgrund */
    int antialias;               /* standard 1 */
    int annotations;             /* standard 1 */
    int printing;                /* PDFiums utskriftsläge */
    const char* include_layers;  /* namn eller mönster, ';'-separerade (NULL = alla) */
    const char* exclude_layers;
    const char* exclude_spots;   /* dekorfärger att ta bort, t.ex. "Varnish*" */
    int hide_nonprinting;        /* ta bort stanslinje, lack och punktskrift */
    const fdc_fontset* fonts;    /* typsnitt som inte är inbäddade; NULL = systemets */
} fdc_render_options;

FDC_API void fdc_render_options_init(fdc_render_options* o);
FDC_API fdc_status fdc_pdf_render(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* o, fdc_image** out);

/* Rutpyramid: nivå L = 18 · 2^L dpi (0–8). Rutan (x, y) täcker px [x·size, (x+1)·size). */
typedef struct fdc_tile {
    uint32_t size;
    int level, x, y;
    int tile_size;               /* px, standard 256 */
} fdc_tile;
typedef struct fdc_tile_grid {
    uint32_t size;
    int columns, rows, width_px, height_px;
    double dpi;
} fdc_tile_grid;
FDC_API fdc_status fdc_pdf_tile_grid(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* o, const fdc_tile* t,
                                     fdc_tile_grid* out);
/* o: sida, ruta, pixlar och variant; dpi och utsnitt kommer från rutan. */
FDC_API fdc_status fdc_pdf_render_tile(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* o, const fdc_tile* t,
                                       fdc_image** out);

typedef enum fdc_overlay_kind {
    FDC_OVERLAY_WORDS = 1, FDC_OVERLAY_OBJECTS = 2, FDC_OVERLAY_SPOTS = 4, FDC_OVERLAY_BARCODES = 8, FDC_OVERLAY_ALL = 15
} fdc_overlay_kind;
/* Bildkarta som JSON: ord, sidobjekt, dekorfärgernas former och streckkoder i mm från rutans övre
 * vänstra hörn (o: sida, ruta och variant). */
FDC_API fdc_status fdc_pdf_overlays(fdc_engine* e, const fdc_pdf* p, const fdc_render_options* o, unsigned kinds,
                                    fdc_buffer** json_out);

typedef struct fdc_image_info {
    uint32_t size;
    int width, height, stride, channels;
    double dpi;
    int x, y;                    /* utsnittets läge i hela rastret */
    int full_width, full_height;
    const unsigned char* data;   /* lever lika länge som bilden */
} fdc_image_info;
FDC_API fdc_status fdc_image_get_info(const fdc_image* img, fdc_image_info* out);

/* ------------------------------------------------------------------ typsnitt */

/* En oföränderlig uppsättning fontfiler (TTF/OTF) för PDF:er där typsnitten inte är inbäddade.
 * Varje ändring ger en ny uppsättning; den gamla påverkas inte och får användas parallellt.
 * Uppsättningar med samma typsnittsnamn men olika filer blandas aldrig ihop. */
typedef enum fdc_font_fallback {
    FDC_FONT_FALLBACK_BUILTIN = 0,   /* typsnitt som saknas: PDFiums inbyggda (samma på alla maskiner) */
    FDC_FONT_FALLBACK_SYSTEM = 1     /* typsnitt som saknas: operativsystemets */
} fdc_font_fallback;

FDC_API fdc_status fdc_fontset_create(fdc_font_fallback fallback, fdc_fontset** out);
/* Ny uppsättning = base + fontfilen (kopieras). aliases: extra namn, ';'-separerade, eller NULL.
 * FDC_ERR_FILE_FORMAT om filen inte är ett TrueType/OpenType-typsnitt. */
FDC_API fdc_status fdc_fontset_add(const fdc_fontset* base, const void* data, size_t len, const char* aliases, fdc_fontset** out);
/* Ny uppsättning med a:s typsnitt före b:s (a:s reservval). */
FDC_API fdc_status fdc_fontset_combine(const fdc_fontset* a, const fdc_fontset* b, fdc_fontset** out);
/* Innehållets hash (ändras när filer, alias eller reservval ändras). */
FDC_API const char* fdc_fontset_hash(const fdc_fontset* s);
/* JSON: hash, fallback och fonts (postScriptName, family, aliases, sha256, fsType …). */
FDC_API fdc_status fdc_fontset_describe(const fdc_fontset* s, fdc_buffer** json_out);
FDC_API void fdc_fontset_free(fdc_fontset* s);

/* Stämmer fontfilen med typsnittet font_name i PDF:en? Jämför stegbredderna PDF:en räknar med mot
 * filens och letar efter tecken som saknas. page 0 = alla sidor. JSON: ok, characters,
 * widthMismatches, missingGlyphs, maxWidthError, example. */
FDC_API fdc_status fdc_font_validate(fdc_engine* e, const fdc_pdf* p, int page, const char* font_name, const void* data,
                                     size_t len, fdc_buffer** json_out);

typedef enum fdc_image_format { FDC_FORMAT_PNG = 0, FDC_FORMAT_JPEG = 1 } fdc_image_format;
/* quality: JPEG 1–100, PNG 0–9 (komprimering); -1 = standard. */
FDC_API fdc_status fdc_image_encode(const fdc_image* img, fdc_image_format format, int quality, fdc_buffer** out);
FDC_API void fdc_image_free(fdc_image* img);

#ifdef __cplusplus
}
#endif

#endif /* FDC_H */
