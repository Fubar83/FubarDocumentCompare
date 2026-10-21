// Öppna PDF:er, sidmått, rendering och sidobjekt (PDFium bakom ett lås).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

typedef struct fpdf_document_t__* FPDF_DOCUMENT;

namespace pc::pdf {

using Bytes = std::vector<unsigned char>;

// PDFium är inte trådsäkert: allt anrop sker under detta lås.
std::recursive_mutex& lock();
void init();   // idempotent

// Sidans geometri: använd box (left, bottom, right, top) i PDF-punkter och dpi.
struct Geometry {
    double box[4] = {0, 0, 0, 0};
    double dpi = 0;
    double width_mm() const { return (box[2] - box[0]) * PT_MM; }
    double height_mm() const { return (box[3] - box[1]) * PT_MM; }
    double ppm() const { return dpi / MM_PER_INCH; }
    Pt to_px(double x, double y) const {
        double s = dpi / PT_PER_INCH;
        return {(x - box[0]) * s, (box[3] - y) * s};
    }
    Box box_px(double l, double b, double r, double t) const {
        Pt p0 = to_px(l, t), p1 = to_px(r, b);
        return {p0.x, p0.y, p1.x, p1.y};
    }
};

// Öppet dokument. Bytes hålls vid liv så länge dokumentet är öppet.
class Document {
public:
    explicit Document(std::shared_ptr<const Bytes> data);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    int page_count() const;
    FPDF_DOCUMENT handle() const { return doc_; }   // bara för pc_pdf:s egen kod
    static std::shared_ptr<Document> open(const Bytes& data);
    static std::shared_ptr<Document> open_file(const std::string& path);

private:
    std::shared_ptr<const Bytes> data_;
    FPDF_DOCUMENT doc_ = nullptr;
};

struct PageSize {
    double width_pt = 0, height_pt = 0;
};
PageSize page_size(const Document& doc, int index);   // 0-baserad sida, beskärningsruta i punkter

struct Rendered {
    cv::Mat rgb;     // CV_8UC3
    Geometry geom;
};
Rendered render(const Bytes& pdf, int page, double dpi, const std::string& box = "trim");
cv::Mat render_page(const Document& doc, int page, double dpi);   // hela sidan (beskärningsrutan)

// Rendering med alla val. Rastret för (dpi, box) är detsamma som render() ger; ett utsnitt (x, y,
// width, height i rastrets px) renderas direkt, utan att resten av sidan ritas – för rutor vid zoom.
struct RenderSpec {
    double dpi = 150;
    std::string box = "trim";             // trim|media|crop|art|bleed
    int x = 0, y = 0, width = 0, height = 0;   // utsnitt; width/height 0 = till rastrets kant
    enum class Pixels { Rgb, Rgba, Gray } pixels = Pixels::Rgb;   // Rgba: genomskinlig bakgrund
    bool antialias = true;
    bool annotations = true;
    bool printing = false;                // PDFiums utskriftsläge
};
struct Raster {
    cv::Mat pixels;                       // CV_8UC3 (RGB), CV_8UC4 (RGBA) eller CV_8UC1
    Geometry geom;                        // hela rastrets geometri (rutan och upplösningen)
    int x = 0, y = 0;                     // utsnittets läge i hela rastret
    int full_width = 0, full_height = 0;  // hela rastrets storlek
};
struct RasterSize {
    int width = 0, height = 0;
};
RasterSize raster_size(const Document& doc, int page, double dpi, const std::string& box = "trim");
Raster render_page(const Document& doc, int page, const RenderSpec& spec);

// Sidobjekt (för objektkartan), som pypdfium2 get_objects(max_depth=4).
// Sidobjektens typer (samma värden som PDFiums FPDF_PAGEOBJ_*).
enum ObjType : int { OBJ_UNKNOWN = 0, OBJ_TEXT = 1, OBJ_PATH = 2, OBJ_IMAGE = 3, OBJ_SHADING = 4, OBJ_FORM = 5 };

struct PageObject {
    int type;          // ObjType
    double l, b, r, t; // gränser i PDF-punkter
};
std::vector<PageObject> page_objects(const Document& doc, int page, int max_depth = 4);

}  // namespace pc::pdf
