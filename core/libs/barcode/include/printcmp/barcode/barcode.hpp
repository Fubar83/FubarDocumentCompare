// pc_barcode: streckkoder via zxing-cpp (EAN/UPC, Code 128/39, ITF, QR, DataMatrix, PDF417 …).
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::barcode {

struct Code {
    std::string symbology;
    std::string data;
    Box bbox;   // bildens px
};

std::vector<Code> detect(const cv::Mat& rgb, double dpi, double work_dpi = 300.0);
// Avkoda kring box med växande marginal (tyst zon), till sist hela bilden.
std::vector<Code> decode_region(const cv::Mat& rgb, const Box& box, int pad, double dpi);

}  // namespace pc::barcode
