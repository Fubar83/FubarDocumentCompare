// Områden i en bild: rektangel, polygon, cirkel eller fri mask. Ett område är en omslutande
// rektangel plus (valfritt) en mask i den rektangelns koordinater.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace pc::imgproc {

struct Region {
    cv::Rect rect;          // omslutande rektangel, bildens px
    cv::Mat mask;           // CV_8U, rect.size(); tom = hela rektangeln
    std::string label;      // fritt namn (t.ex. "streckkod" eller mallens namn)
    double score = 0;       // t.ex. träffens poäng när området kommer från en sökning

    static Region box(const cv::Rect& r, std::string label = {});
    static Region polygon(const std::vector<cv::Point>& pts, std::string label = {});
    static Region circle(cv::Point center, int radius, std::string label = {});
    static Region rotated(const cv::RotatedRect& r, std::string label = {});
    // Från en mask över hela bilden (alla pixlar != 0 blir ett område).
    static Region from_mask(const cv::Mat& full_mask, std::string label = {});
    // Från en ruta i mm (origo övre vänster) och upplösning.
    static Region mm(double x, double y, double w, double h, double dpi, std::string label = {});

    bool has_mask() const { return !mask.empty(); }
    int area() const;                                   // antal pixlar i området
    bool contains(cv::Point p) const;
    cv::Mat full_mask(cv::Size image_size) const;       // CV_8U över hela bilden
    Region clipped(cv::Size image_size) const;          // beskuren till bilden (kan bli tom)
    Region translated(cv::Point offset) const;
    bool empty() const { return rect.empty(); }
};

using RegionSet = std::vector<Region>;

// Sammanhängande områden i en mask (8-grannar), minst min_area px. Etiketterna blir "1", "2" …
RegionSet regions_from_mask(const cv::Mat& mask, int min_area = 1);
// Union av områdena som mask över hela bilden.
cv::Mat mask_of(const RegionSet& regions, cv::Size image_size);
// rows x cols lika stora rutor i area, med gap px mellan.
RegionSet grid(const cv::Rect& area, int rows, int cols, int gap = 0);
RegionSet clip(const RegionSet& regions, cv::Size image_size);
// Rektanglarnas skärning genom union (IoU).
double iou(const cv::Rect& a, const cv::Rect& b);

}  // namespace pc::imgproc
