// Klassning av avvikelser: typ, kategori och allvar ur objektkartan.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/artwork/objmap.hpp"
#include "printcmp/artwork/structure.hpp"
#include "printcmp/barcode/barcode.hpp"
#include "printcmp/scan/diff.hpp"
#include "printcmp/scan/severity.hpp"

namespace pc::scan {


std::vector<json> classify(const std::vector<Blob>& blobs, const DiffMaps& maps, const std::vector<artwork::Obj>& objs,
                           const std::vector<barcode::Code>& ref_codes, const cv::Mat& smp_rgb_u8, const cv::Mat& fold_band,
                           const cv::Mat& confidence, double ppm, double dpi, const json& tol, int instance,
                           const std::vector<artwork::Table>& tables);

}  // namespace pc::scan
