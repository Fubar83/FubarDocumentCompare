// Mätning i områden och ritning av områden/träffar på en bild.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/imgproc/match.hpp"
#include "printcmp/imgproc/region.hpp"

namespace pc::imgproc {

struct RegionStats {
    int area = 0;                  // pixlar
    cv::Scalar mean, stddev;       // per kanal
    double min = 0, max = 0;       // första kanalen
    cv::Point2d centroid;          // bildens px
};
RegionStats measure(const cv::Mat& image, const Region& region);
std::vector<RegionStats> measure(const cv::Mat& image, const RegionSet& regions);

// Områden ur en binär bild (t.ex. efter threshold/otsu), som regions_from_mask, filtrerade på yta.
RegionSet blobs(const cv::Mat& binary, int min_area = 1, int max_area = 0);

// ---------------------------------------------------------------- ritning (på plats, RGB)
void draw_regions(cv::Mat& image, const RegionSet& regions, const cv::Scalar& color = {255, 0, 0}, int thickness = 2,
                  bool labels = true);
void draw_matches(cv::Mat& image, const std::vector<Match>& matches, const cv::Scalar& color = {0, 160, 0}, int thickness = 2);
// Lägg en färgad mask (≠ 0) över bilden med alpha 0..1.
void overlay(cv::Mat& image, const cv::Mat& mask, const cv::Scalar& color = {255, 0, 0}, double alpha = 0.4);
// Gör en bild visningsbar som RGB 8-bit (t.ex. en CV_32F-respons eller ett spektrum): min–max-sträckt.
cv::Mat visualize(const cv::Mat& image);

}  // namespace pc::imgproc
