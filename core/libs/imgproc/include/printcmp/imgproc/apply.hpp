// Filter på områden. Varje område filtreras med filtrets marginal av omgivande pixlar, så att
// resultatet inuti området blir detsamma som när hela bilden filtreras (för flyttalsbilder exakt;
// i 8 bitar kan OpenCV välja en annan heltalsväg för små utsnitt, högst 1 nivås skillnad). Filter
// som räknar på områdets egen statistik (otsu, normalize_range, clahe) gäller per område.
// Bara områdets pixlar (rektangeln, eller masken om den finns) skrivs.
#pragma once

#include <opencv2/core.hpp>
#include <vector>

#include "printcmp/imgproc/filter.hpp"
#include "printcmp/imgproc/region.hpp"

namespace pc::imgproc {

// Var resultatet hamnar i målbilden: målruta = källruta · scale + offset.
struct Placement {
    cv::Point offset{0, 0};
    double scale = 1.0;
};

// Filtrerat innehåll i ett område (rektangelns storlek); tomt om området ligger utanför bilden.
cv::Mat filter_region(const Filter& f, const cv::Mat& src, const Region& region);
std::vector<cv::Mat> filter_regions(const Filter& f, const cv::Mat& src, const RegionSet& regions);

// En kopia av src där områdena är filtrerade. Har filtret en annan typ än bilden (t.ex. gray()
// eller sobel()) får hela kopian filtrets typ.
cv::Mat apply_regions(const Filter& f, const cv::Mat& src, const RegionSet& regions);
// Skriver resultaten i dst (som kan vara src). dst skapas som en kopia av src om den är tom.
// Typen konverteras till dst:s (mättat; enkanalig <-> RGB).
void apply_regions(const Filter& f, const cv::Mat& src, const RegionSet& regions, cv::Mat& dst, const Placement& place = {});
// Resultatet av from[i] skalas in i to[i] (samma antal) i dst; to[i]:s mask (annars from[i]:s) avgör pixlarna.
void apply_to_targets(const Filter& f, const cv::Mat& src, const RegionSet& from, cv::Mat& dst, const RegionSet& to);

// Klistra in en bild i ett område (skalas till områdets rektangel, bara maskens pixlar).
void paste(const cv::Mat& patch, cv::Mat& dst, const Region& to);
// Typkonvertering: djup (mättat) och kanaler (grå <-> RGB, RGBA -> RGB).
cv::Mat fit_type(const cv::Mat& m, int type);

}  // namespace pc::imgproc
