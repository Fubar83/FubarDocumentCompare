#include "printcmp/barcode/barcode.hpp"

#include <ZXing/ReadBarcode.h>

#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace pc::barcode {

static void read_into(const cv::Mat& gray_in, double s, std::vector<Code>& out) {
    cv::Mat gray = gray_in.isContinuous() ? gray_in : gray_in.clone();
    ZXing::ImageView view(gray.data, gray.cols, gray.rows, ZXing::ImageFormat::Lum, (int)gray.step);
    ZXing::ReaderOptions opts;
    opts.setFormats(ZXing::BarcodeFormat::Any);
    opts.setTryHarder(true);
    opts.setTryRotate(true);
    for (const auto& r : ZXing::ReadBarcodes(view, opts)) {
        if (!r.isValid() || r.text().empty()) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Code& c) { return c.data == r.text(); })) continue;
        auto p = r.position();
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& q : {p.topLeft(), p.topRight(), p.bottomRight(), p.bottomLeft()}) {
            x0 = std::min<double>(x0, q.x);
            y0 = std::min<double>(y0, q.y);
            x1 = std::max<double>(x1, q.x);
            y1 = std::max<double>(y1, q.y);
        }
        out.push_back({ZXing::ToString(r.format()), r.text(), {x0 / s, y0 / s, x1 / s, y1 / s}});
    }
}

std::vector<Code> detect(const cv::Mat& rgb, double dpi, double work_dpi) {
    // Arbetsupplösning först (snabbt). Omsampling ger gråa stapelkanter som ibland stjälper
    // avkodningen, så pröva sedan full upplösning och en tröskad kopia innan vi ger upp.
    double s = std::min(1.0, work_dpi / dpi);
    cv::Mat gray, small;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
    std::vector<Code> out;
    if (s < 1.0) {
        cv::resize(gray, small, cv::Size(), s, s, cv::INTER_AREA);
        read_into(small, s, out);
    }
    if (out.empty()) read_into(gray, 1.0, out);
    if (out.empty()) {
        cv::Mat bin;
        cv::threshold(s < 1.0 ? small : gray, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        read_into(bin, s < 1.0 ? s : 1.0, out);
    }
    return out;
}

std::vector<Code> decode_region(const cv::Mat& rgb, const Box& box, int pad, double dpi) {
    int h = rgb.rows, w = rgb.cols;
    double ppm = dpi / MM_PER_INCH;
    int bx0 = (int)std::lround(box.x0), by0 = (int)std::lround(box.y0), bx1 = (int)std::lround(box.x1),
        by1 = (int)std::lround(box.y1);
    for (int p : {pad, (int)(5 * ppm), (int)(10 * ppm)}) {
        int x0 = std::max(0, bx0 - p), y0 = std::max(0, by0 - p), x1 = std::min(w, bx1 + p), y1 = std::min(h, by1 + p);
        if (x1 <= x0 || y1 <= y0) continue;
        auto out = detect(rgb(cv::Rect(x0, y0, x1 - x0, y1 - y0)), dpi);
        for (auto& c : out) {
            c.bbox.x0 += x0;
            c.bbox.x1 += x0;
            c.bbox.y0 += y0;
            c.bbox.y1 += y0;
        }
        if (!out.empty()) return out;
    }
    std::vector<Code> keep;
    for (auto& c : detect(rgb, dpi))
        if (!(c.bbox.x1 < bx0 || bx1 < c.bbox.x0 || c.bbox.y1 < by0 || by1 < c.bbox.y0)) keep.push_back(c);
    return keep;
}

}  // namespace pc::barcode
