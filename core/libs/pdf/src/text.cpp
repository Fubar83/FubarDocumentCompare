#include "printcmp/pdf/text.hpp"

#include "fontreg.hpp"
#include "page.hpp"

#include <fpdf_text.h>
#include <fpdfview.h>

namespace pc::pdf {

std::vector<TextChar> text_chars(const Document& doc, int page_index) {
    std::lock_guard<std::recursive_mutex> g(lock());
    Page page(doc, page_index);
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page.handle());
    std::vector<TextChar> out;
    if (!tp) return out;
    int n = FPDFText_CountChars(tp);
    out.reserve(n > 0 ? n : 0);
    for (int i = 0; i < n; ++i) {
        TextChar c;
        c.code = static_cast<char32_t>(FPDFText_GetUnicode(tp, i));
        c.generated = FPDFText_IsGenerated(tp, i) == 1;
        c.hyphen = FPDFText_IsHyphen(tp, i) == 1;
        FPDFText_GetCharBox(tp, i, &c.l, &c.r, &c.b, &c.t);
        FS_RECTF lb;
        if (FPDFText_GetLooseCharBox(tp, i, &lb)) {
            c.has_loose = true;
            c.loose_l = lb.left, c.loose_t = lb.top, c.loose_r = lb.right, c.loose_b = lb.bottom;
        }
        c.has_origin = FPDFText_GetCharOrigin(tp, i, &c.origin_x, &c.origin_y) != 0;
        c.font_size = FPDFText_GetFontSize(tp, i);
        FS_MATRIX m;
        if (FPDFText_GetMatrix(tp, i, &m)) {
            c.has_matrix = true;
            c.matrix[0] = m.a, c.matrix[1] = m.b, c.matrix[2] = m.c, c.matrix[3] = m.d, c.matrix[4] = m.e, c.matrix[5] = m.f;
        }
        char buf[256] = {0};
        unsigned long len = FPDFText_GetFontInfo(tp, i, buf, sizeof buf, &c.font_flags);
        if (len) c.font_name = detail::untoken(buf);
        c.font_weight = FPDFText_GetFontWeight(tp, i);
        FPDFText_GetFillColor(tp, i, &c.fill_r, &c.fill_g, &c.fill_b, &c.fill_a);
        out.push_back(std::move(c));
    }
    FPDFText_ClosePage(tp);
    return out;
}

}  // namespace pc::pdf
