// Liten PDF-skrivare för testmaterialet: sidor, banor, CMYK/RGB/dekorfärger (Separation),
// inbäddade TrueType-delmängder med ToUnicode, PDF-lager (OCG) och Flate-komprimering.
// Koordinater i punkter med origo nere till vänster, som i PDF.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "printcmp/pdfwrite/truetype.hpp"

namespace pc::pdfwrite {

using Bytes = std::vector<unsigned char>;

constexpr double PT_PER_MM = 72.0 / 25.4;

struct Color {
    enum Kind { Gray, Rgb, Cmyk, Spot } kind = Gray;
    double v[4] = {0, 0, 0, 0};   // Gray: v[0]; Rgb: v[0..2]; Cmyk/Spot: CMYK (Spot = alternativfärg)
    std::string spot;             // dekorfärgens namn
    double tint = 1;

    static Color gray(double g) { return {Gray, {g, 0, 0, 0}}; }
    static Color rgb(double r, double g, double b) { return {Rgb, {r, g, b, 0}}; }
    static Color cmyk(double c, double m, double y, double k) { return {Cmyk, {c, m, y, k}}; }
    static Color separation(const std::string& name, double c, double m, double y, double k, double tint = 1) {
        Color x{Spot, {c, m, y, k}, name, tint};
        return x;
    }
};

class PdfDocument;

class Canvas {
public:
    void fill_color(const Color& c);
    void stroke_color(const Color& c);
    void line_width(double pt);
    void line_cap(int cap);
    void save();
    void restore();
    void concat(double a, double b, double c, double d, double e, double f);   // cm

    void move_to(double x, double y);
    void line_to(double x, double y);
    void curve_to(double x1, double y1, double x2, double y2, double x3, double y3);
    void close();
    void draw_path(bool fill, bool stroke);   // avslutar den aktuella banan

    void rect(double x, double y, double w, double h, bool fill, bool stroke);
    void circle(double cx, double cy, double r, bool fill, bool stroke);
    void ellipse_path(double cx, double cy, double rx, double ry);
    void round_rect(double x, double y, double w, double h, double r, bool fill, bool stroke);
    void line(double x0, double y0, double x1, double y1);

    // Text (UTF-8) med baslinjen vid y. Returnerar bredden i punkter.
    double text(const std::string& font, double size, double x, double y, const std::string& s);
    double text_right(const std::string& font, double size, double x_right, double y, const std::string& s);
    double text_centred(const std::string& font, double size, double x_mid, double y, const std::string& s);

    // Allt mellan begin_layer och end_layer hamnar i PDF-lagret (Optional Content Group) med namnet.
    void begin_layer(const std::string& name);
    void end_layer();

    void set_trim_box(double x0, double y0, double x1, double y1);
    double width() const { return w_; }
    double height() const { return h_; }

private:
    friend class PdfDocument;
    Canvas(PdfDocument* doc, double w, double h) : doc_(doc), w_(w), h_(h) {}
    void color_op(const Color& c, bool stroke);
    void put(const std::string& s);
    PdfDocument* doc_;
    double w_, h_;
    double trim_[4] = {0, 0, 0, 0};
    bool has_trim_ = false;
    std::string content_;
};

class PdfDocument {
public:
    PdfDocument();
    ~PdfDocument();
    Canvas& new_page(double w_pt, double h_pt);
    int page_count() const { return int(pages_.size()); }
    Bytes save() const;

private:
    friend class Canvas;
    struct FontUse {
        std::shared_ptr<const TrueType> ttf;
        std::string res;                       // /F0 …
        std::map<uint16_t, std::u32string> used;   // glyf -> Unicode (för ToUnicode)
    };
    FontUse& font_use(const std::string& name);
    std::string colorspace(const Color& c);
    std::string layer(const std::string& name);

    std::vector<std::unique_ptr<Canvas>> pages_;
    std::map<std::string, FontUse> fonts_;
    std::vector<std::string> font_order_;
    std::vector<std::pair<std::string, Color>> spaces_;   // /CS0 … i ordning
    std::vector<std::string> layers_;                     // /L0 …
};

std::string fmt_num(double v);

}  // namespace pc::pdfwrite
