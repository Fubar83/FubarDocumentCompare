// Etikett/ark: ett prov (scan, PDF eller pixlar) mot det analyserade originalet.
#pragma once

#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <vector>

#include "printcmp/compare/datacheck.hpp"
#include "printcmp/compare/reference.hpp"
#include "printcmp/scan/locate.hpp"

namespace pc::compare {

struct SampleSource {
    enum class Kind { Scan, Pdf, Image } kind = Kind::Scan;
    std::string path;
    int page = 1;
    double dpi = 0;
    cv::Mat rgb;           // bildbuffert (Kind::Image) – annars läses path
    pdf::Bytes pdf_bytes;  // PDF i minnet (Kind::Pdf utan path)
};

struct CompareOptions {
    std::string instance_mode = "single";   // single | multi
    int expected_count = 0, expected_rows = 0, expected_cols = 0;
    std::vector<int> rotations = {0, 90, 180, 270};
    double max_skew_deg = 4.0;
    double systematic_ratio = 0.8;
    bool consensus = true;
    bool text_compare = true;
    bool color_calibrated = false;
    int seed = 1;
    std::optional<DataCheckOptions> data_check;
};

struct LabelResult {
    json result;                     // innehållet i result.json
    cv::Mat ref_rgb, sample_rgb;
    double dpi = 0, sample_dpi = 0;
    std::vector<scan::Instance> instances;
    std::vector<cv::Mat> aligned;    // per instans, i referensens ram (för bildlager och utsnitt)
};

LabelResult compare_label(const ReferenceModel& ref, const SampleSource& sample, const CompareOptions& opt,
                          const JobContext& ctx = {});

}  // namespace pc::compare
