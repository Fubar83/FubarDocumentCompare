// Veck på arknivå (efter att varje etikett analyserats för sig).
#pragma once

#include <opencv2/core.hpp>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/scan/aggregate.hpp"
#include "printcmp/scan/folds.hpp"

namespace pc::scan {

// En etikett på arket: index, avbildning etikett-px -> ark-px och dess veckkandidater.
struct SheetLabel {
    int index = 0;
    Mat3 H;
    std::vector<Fold> folds;
};

// Tar bort veckkandidater som ligger på samma ställe i etiketterna (≥ 40 % av dem) men inte på
// en linje över arket: det är ändringar i originalet, inte veck.
void drop_systematic_fold_candidates(std::vector<SheetLabel>& labels, cv::Size ref_size, double ppm, double sppm);

// Smala fynd i ≥ min_inst etiketter på samma linje i arket (på olika ställen i etiketterna och i
// minst hälften av etiketterna linjen korsar) blir en osäker veckzon. inst_out får vecken per etikett.
std::vector<json> collinear_folds(std::vector<json> diffs, const std::vector<SheetLabel>& labels, const cv::Mat& valid,
                                  double ppm, double sppm, json& inst_out, double tol_mm = 0.6, int min_inst = 3,
                                  double band_mm = 2.0);

}  // namespace pc::scan
