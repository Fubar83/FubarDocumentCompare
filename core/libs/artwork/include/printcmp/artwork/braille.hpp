// Punktskrift i originalet: punkter ur PDF, cellnät, avläsning och kontroll mot provet.
#pragma once

#include <map>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <vector>

#include "printcmp/braille/braille.hpp"
#include "printcmp/core/core.hpp"
#include "printcmp/pdf/document.hpp"
#include "printcmp/pdf/spots.hpp"

namespace pc::artwork {

using Cell = braille::Cell;   // punkter 1..6

struct BrailleLine {
    int index = 0;
    double top = 0, origin = 0, pitch = 0, cell_pitch = 0, dot_r = 0;
    std::map<int, Cell> cells;
    int kmin = 0, kmax = 0;
    std::string source = "spot";
    Pt pos(int k, int n) const;
    Box cell_box(int k) const;
    Box bbox() const;
    std::vector<Cell> cell_list() const;
    std::string text() const { return braille::cells_to_text(cell_list()); }
};

std::pair<std::vector<Pt>, double> braille_dots_from_spots(const pdf::SpotAnalysis& spots, const pdf::Geometry& geom);
std::pair<std::vector<Pt>, double> braille_dots_from_text(const pdf::Bytes& pdf, int page, const pdf::Geometry& geom);
std::vector<BrailleLine> braille_decode(const std::vector<Pt>& dots, double dot_r, double ppm, const std::string& source);
std::vector<BrailleLine> braille_from_pdf(const pdf::SpotAnalysis& spots, const pdf::Bytes& pdf, int page,
                                          const pdf::Geometry& geom);
cv::Mat braille_mask(cv::Size size, const std::vector<BrailleLine>& lines, double ppm, double margin_mm = 0.45);

struct BrailleScanResult {
    std::vector<json> diffs;
    std::vector<std::optional<std::string>> reads;
    std::vector<json> stats;
};
BrailleScanResult braille_check_scan(const cv::Mat& resid_L, const std::vector<BrailleLine>& lines, double ppm,
                                     int instance, const cv::Mat& fold_band, double min_ratio = 1.8,
                                     double min_level = 0.8);
std::pair<std::vector<json>, std::vector<std::string>> braille_compare_dots(const std::vector<BrailleLine>& lines,
                                                                            const std::vector<Pt>& sample_dots_ref,
                                                                            double ppm, int instance);

}  // namespace pc::artwork
