// PDFium: dokument, sidor, rendering och sidobjekt.
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "printcmp/pdf/document.hpp"
#include "fontreg.hpp"
#include "page.hpp"

namespace pc::pdf {

std::recursive_mutex& lock() {
    static std::recursive_mutex m;
    return m;
}

void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        FPDF_LIBRARY_CONFIG cfg{};
        cfg.version = 2;
        FPDF_InitLibraryWithConfig(&cfg);
        detail::install_font_info();
    });
}

Document::Document(std::shared_ptr<const Bytes> data) : data_(std::move(data)) {
    init();
    std::lock_guard<std::recursive_mutex> g(lock());
    doc_ = FPDF_LoadMemDocument64(data_->data(), data_->size(), nullptr);
    if (!doc_) throw Error(ErrorCode::PdfError, "PDF kan inte öppnas (fel " + std::to_string(FPDF_GetLastError()) + ")");
}

Document::~Document() {
    std::lock_guard<std::recursive_mutex> g(lock());
    if (doc_) FPDF_CloseDocument(doc_);
}

int Document::page_count() const {
    std::lock_guard<std::recursive_mutex> g(lock());
    return FPDF_GetPageCount(doc_);
}

std::shared_ptr<Document> Document::open(const Bytes& data) {
    return std::make_shared<Document>(std::make_shared<const Bytes>(data));
}

std::shared_ptr<Document> Document::open_file(const std::string& path) {
    return std::make_shared<Document>(std::make_shared<const Bytes>(read_file_bytes(path)));
}

Page::Page(const Document& doc, int index) {
    std::lock_guard<std::recursive_mutex> g(lock());
    if (index < 0 || index >= FPDF_GetPageCount(doc.handle()))
        throw Error(ErrorCode::InvalidArgument, "sidan " + std::to_string(index + 1) + " finns inte");
    page_ = FPDF_LoadPage(doc.handle(), index);
    if (!page_) throw Error(ErrorCode::PdfError, "sidan kan inte läsas");
}

Page::~Page() {
    std::lock_guard<std::recursive_mutex> g(lock());
    if (page_) FPDF_ClosePage(page_);
}

double Page::width_pt() const {
    std::lock_guard<std::recursive_mutex> g(lock());
    return FPDF_GetPageWidthF(page_);
}

double Page::height_pt() const {
    std::lock_guard<std::recursive_mutex> g(lock());
    return FPDF_GetPageHeightF(page_);
}

void Page::box(const std::string& which, double out[4]) const {
    std::lock_guard<std::recursive_mutex> g(lock());
    float l = 0, b = 0, r = 0, t = 0;
    FPDF_BOOL ok = 0;
    // pypdfium2: saknad trim/art -> beskärningsruta; saknad beskärningsruta -> mediaruta
    if (which == "trim") ok = FPDFPage_GetTrimBox(page_, &l, &b, &r, &t);
    else if (which == "art") ok = FPDFPage_GetArtBox(page_, &l, &b, &r, &t);
    else if (which == "bleed") ok = FPDFPage_GetBleedBox(page_, &l, &b, &r, &t);
    else if (which == "media") ok = FPDFPage_GetMediaBox(page_, &l, &b, &r, &t);
    if (!ok && which != "media") ok = FPDFPage_GetCropBox(page_, &l, &b, &r, &t);
    if (!ok) ok = FPDFPage_GetMediaBox(page_, &l, &b, &r, &t);
    if (!ok) {
        l = 0;
        b = 0;
        r = FPDF_GetPageWidthF(page_);
        t = FPDF_GetPageHeightF(page_);
    }
    out[0] = l;
    out[1] = b;
    out[2] = r;
    out[3] = t;
}

namespace {

// Hela rastrets placering, som pypdfium2 PdfPage.render(scale, crop): sidan ritas i
// src_w × src_h px och beskärs c[0] (vänster), c[1] (ned), c[2] (höger), c[3] (upp).
struct Placement {
    int src_w = 0, src_h = 0;
    int c[4] = {0, 0, 0, 0};
    int width() const { return src_w - c[0] - c[2]; }
    int height() const { return src_h - c[1] - c[3]; }
};

Placement placement(FPDF_PAGE page, double scale, const double crop[4]) {
    Placement p;
    p.src_w = (int)std::ceil(FPDF_GetPageWidthF(page) * scale);
    p.src_h = (int)std::ceil(FPDF_GetPageHeightF(page) * scale);
    for (int i = 0; i < 4; ++i) p.c[i] = (int)std::ceil(crop[i] * scale);
    if (p.width() <= 0 || p.height() <= 0) throw Error(ErrorCode::PdfError, "tom sida");
    return p;
}

// Rutan `which` som marginaler mot beskärningsrutan (PDFium ritar beskärningsrutan).
void box_insets(const Page& page, const std::string& which, double b[4], double crop[4]) {
    double cb[4];
    page.box(which, b);
    page.box("crop", cb);
    crop[0] = std::max(0.0, b[0] - cb[0]);
    crop[1] = std::max(0.0, b[1] - cb[1]);
    crop[2] = std::max(0.0, cb[2] - b[2]);
    crop[3] = std::max(0.0, cb[3] - b[3]);
}

cv::Mat render_bitmap(FPDF_PAGE page, const Placement& p, int x, int y, int w, int h, bool alpha, int flags) {
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, alpha ? FPDFBitmap_BGRA : FPDFBitmap_BGR, nullptr, 0);
    if (!bmp) throw Error(ErrorCode::PdfError, "bitmapp kan inte skapas (för stor?)");
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, alpha ? 0x00000000 : 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, page, -p.c[0] - x, -p.c[3] - y, p.src_w, p.src_h, 0, flags | FPDF_REVERSE_BYTE_ORDER);
    cv::Mat view(h, w, alpha ? CV_8UC4 : CV_8UC3, FPDFBitmap_GetBuffer(bmp), (size_t)FPDFBitmap_GetStride(bmp));
    cv::Mat out = view.clone();
    FPDFBitmap_Destroy(bmp);
    return out;
}

// Största teckenstorleken på sidan i punkter (teckengrad × text- och formmatrisernas skala).
double max_text_size_pt(FPDF_PAGEOBJECT o, double scale, int depth) {
    int type = FPDFPageObj_GetType(o);
    FS_MATRIX m{1, 0, 0, 1, 0, 0};
    FPDFPageObj_GetMatrix(o, &m);
    double s = scale * std::sqrt(std::abs((double)m.a * m.d - (double)m.b * m.c));
    if (type == FPDF_PAGEOBJ_TEXT) {
        float size = 0;
        return FPDFTextObj_GetFontSize(o, &size) ? size * s : 0.0;
    }
    double mx = 0;
    if (type == FPDF_PAGEOBJ_FORM && depth < 16) {
        int n = FPDFFormObj_CountObjects(o);
        for (int i = 0; i < n; ++i) mx = std::max(mx, max_text_size_pt(FPDFFormObj_GetObject(o, (unsigned long)i), s, depth + 1));
    }
    return mx;
}

double max_text_size_pt(FPDF_PAGE page) {
    double mx = 0;
    int n = FPDFPage_CountObjects(page);
    for (int i = 0; i < n; ++i) mx = std::max(mx, max_text_size_pt(FPDFPage_GetObject(page, i), 1.0, 1));
    return mx;
}

cv::Mat render_bitmap(FPDF_PAGE page, double scale, const double crop[4]) {
    Placement p = placement(page, scale, crop);
    return render_bitmap(page, p, 0, 0, p.width(), p.height(), false, FPDF_ANNOT);
}

}  // namespace

Rendered render(const Bytes& pdf, int page_index, double dpi, const std::string& which) {
    auto doc = Document::open(pdf);
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(*doc, page_index);
    double b[4], crop[4];
    box_insets(page, which, b, crop);
    Rendered r;
    r.rgb = render_bitmap(page.handle(), dpi / 72.0, crop);
    std::copy(b, b + 4, r.geom.box);
    r.geom.dpi = dpi;
    return r;
}

cv::Mat render_page(const Document& doc, int page_index, double dpi) {
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(doc, page_index);
    double crop[4] = {0, 0, 0, 0};
    return render_bitmap(page.handle(), dpi / 72.0, crop);
}

RasterSize raster_size(const Document& doc, int page_index, double dpi, const std::string& which) {
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(doc, page_index);
    double b[4], crop[4];
    box_insets(page, which, b, crop);
    Placement p = placement(page.handle(), dpi / 72.0, crop);
    return {p.width(), p.height()};
}

Raster render_page(const Document& doc, int page_index, const RenderSpec& s) {
    if (!(s.dpi > 0) || s.dpi > 4800) throw Error(ErrorCode::InvalidArgument, "dpi måste vara 0–4800");
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(doc, page_index);
    double b[4], crop[4];
    box_insets(page, s.box, b, crop);
    Placement p = placement(page.handle(), s.dpi / 72.0, crop);
    Raster r;
    r.full_width = p.width();
    r.full_height = p.height();
    r.x = std::clamp(s.x, 0, r.full_width);
    r.y = std::clamp(s.y, 0, r.full_height);
    int w = s.width > 0 ? std::min(s.width, r.full_width - r.x) : r.full_width - r.x;
    int h = s.height > 0 ? std::min(s.height, r.full_height - r.y) : r.full_height - r.y;
    if (w <= 0 || h <= 0) throw Error(ErrorCode::InvalidArgument, "utsnittet ligger utanför sidan");
    if ((double)w * h > 400e6) throw Error(ErrorCode::InvalidArgument, "bilden blir för stor (över 400 Mpx); rendera i rutor");
    int flags = 0;
    if (s.annotations) flags |= FPDF_ANNOT;
    if (s.printing) flags |= FPDF_PRINTING;
    if (!s.antialias) flags |= FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE | FPDF_RENDER_NO_SMOOTHPATH;
    bool alpha = s.pixels == RenderSpec::Pixels::Rgba;
    if (w == r.full_width && h == r.full_height) {
        r.pixels = render_bitmap(page.handle(), p, 0, 0, w, h, alpha, flags);
    } else {
        // PDFium ritar ett tecken vars origo ligger utanför bitmappen annorlunda (kantutjämningen vid
        // kanten skiljer). Rendera därför med en marginal runt utsnittet som rymmer sidans största
        // tecken och skär bort den: utsnitt och rutor blir då pixellika med hela sidan.
        double em = max_text_size_pt(page.handle()) * s.dpi / 72.0;
        auto pad = [&](double f) { return (int)std::min(1024.0, std::ceil(f * em) + 2); };
        int pl = std::min(r.x, pad(1.3)), pt = std::min(r.y, pad(0.5));
        int pr = std::min(r.full_width - r.x - w, pad(0.5)), pb = std::min(r.full_height - r.y - h, pad(1.3));
        cv::Mat big = render_bitmap(page.handle(), p, r.x - pl, r.y - pt, w + pl + pr, h + pt + pb, alpha, flags);
        r.pixels = big(cv::Rect(pl, pt, w, h)).clone();
    }
    if (s.pixels == RenderSpec::Pixels::Gray) {
        cv::Mat gray;
        cv::cvtColor(r.pixels, gray, cv::COLOR_RGB2GRAY);
        r.pixels = gray;
    }
    std::copy(b, b + 4, r.geom.box);
    r.geom.dpi = s.dpi;
    return r;
}

static_assert(OBJ_TEXT == FPDF_PAGEOBJ_TEXT && OBJ_PATH == FPDF_PAGEOBJ_PATH && OBJ_IMAGE == FPDF_PAGEOBJ_IMAGE &&
              OBJ_SHADING == FPDF_PAGEOBJ_SHADING && OBJ_FORM == FPDF_PAGEOBJ_FORM, "ObjType måste följa PDFium");

static void collect(FPDF_PAGEOBJECT o, int depth, int max_depth, std::vector<PageObject>& out) {
    int type = FPDFPageObj_GetType(o);
    float l = 0, b = 0, r = 0, t = 0;
    if (FPDFPageObj_GetBounds(o, &l, &b, &r, &t)) out.push_back({type, l, b, r, t});
    if (type == FPDF_PAGEOBJ_FORM && depth < max_depth) {
        int n = FPDFFormObj_CountObjects(o);
        for (int i = 0; i < n; ++i) collect(FPDFFormObj_GetObject(o, (unsigned long)i), depth + 1, max_depth, out);
    }
}

std::vector<PageObject> page_objects(const Document& doc, int page_index, int max_depth) {
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(doc, page_index);
    std::vector<PageObject> out;
    int n = FPDFPage_CountObjects(page.handle());
    for (int i = 0; i < n; ++i) collect(FPDFPage_GetObject(page.handle(), i), 1, max_depth, out);
    return out;
}

PageSize page_size(const Document& doc, int index) {
    std::lock_guard<std::recursive_mutex> g(lock());
    Page p(doc, index);
    return {p.width_pt(), p.height_pt()};
}

}  // namespace pc::pdf
