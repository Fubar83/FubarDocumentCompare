// Privat för pc_pdf: en öppnad PDFium-sida (anropas under pdf::lock()).
#pragma once

#include "printcmp/pdf/document.hpp"

typedef struct fpdf_page_t__* FPDF_PAGE;

namespace pc::pdf {

class Page {
public:
    Page(const Document& doc, int index);
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;
    FPDF_PAGE handle() const { return page_; }
    double width_pt() const;
    double height_pt() const;
    void box(const std::string& which, double out[4]) const;   // trim|media|crop|art|bleed

private:
    FPDF_PAGE page_ = nullptr;
};

}  // namespace pc::pdf
