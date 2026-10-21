#include "printcmp/imgproc/measure.hpp"

#include <opencv2/imgproc.hpp>

namespace pc::imgproc {

RegionStats measure(const cv::Mat& image, const Region& region) {
    RegionStats s;
    Region c = region.clipped(image.size());
    if (c.empty()) return s;
    cv::Mat roi = image(c.rect);
    cv::Mat mask = c.has_mask() ? c.mask : cv::Mat(c.rect.size(), CV_8U, cv::Scalar(255));
    s.area = cv::countNonZero(mask);
    if (!s.area) return s;
    cv::meanStdDev(roi, s.mean, s.stddev, mask);
    cv::Mat first = roi.channels() == 1 ? roi : [&] {
        std::vector<cv::Mat> ch;
        cv::split(roi, ch);
        return ch[0];
    }();
    cv::minMaxLoc(first, &s.min, &s.max, nullptr, nullptr, mask);
    cv::Moments m = cv::moments(mask, true);
    s.centroid = {c.rect.x + m.m10 / m.m00, c.rect.y + m.m01 / m.m00};
    return s;
}

std::vector<RegionStats> measure(const cv::Mat& image, const RegionSet& regions) {
    std::vector<RegionStats> out;
    out.reserve(regions.size());
    for (auto& r : regions) out.push_back(measure(image, r));
    return out;
}

RegionSet blobs(const cv::Mat& binary, int min_area, int max_area) {
    cv::Mat b = binary;
    if (b.channels() > 1) cv::cvtColor(b, b, b.channels() == 4 ? cv::COLOR_RGBA2GRAY : cv::COLOR_RGB2GRAY);
    if (b.depth() != CV_8U) b.convertTo(b, CV_8U);
    RegionSet out;
    for (auto& r : regions_from_mask(b, min_area))
        if (max_area <= 0 || r.area() <= max_area) out.push_back(std::move(r));
    return out;
}

void draw_regions(cv::Mat& image, const RegionSet& regions, const cv::Scalar& color, int thickness, bool labels) {
    for (auto& r : regions) {
        if (r.empty()) continue;
        if (r.has_mask()) {
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(r.mask.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE, r.rect.tl());
            cv::drawContours(image, contours, -1, color, thickness, cv::LINE_AA);
        } else {
            cv::rectangle(image, r.rect, color, thickness, cv::LINE_AA);
        }
        if (labels && !r.label.empty())
            cv::putText(image, r.label, r.rect.tl() + cv::Point(0, -4), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv::LINE_AA);
    }
}

void draw_matches(cv::Mat& image, const std::vector<Match>& matches, const cv::Scalar& color, int thickness) {
    for (auto& m : matches) {
        std::vector<cv::Point> pts;
        for (auto& p : m.corners) pts.push_back(cv::Point(int(std::lround(p.x)), int(std::lround(p.y))));
        if (pts.empty()) cv::rectangle(image, m.box, color, thickness, cv::LINE_AA);
        else cv::polylines(image, pts, true, color, thickness, cv::LINE_AA);
        char txt[32];
        std::snprintf(txt, sizeof txt, "%.2f", m.score);
        cv::putText(image, txt, m.box.tl() + cv::Point(0, -4), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv::LINE_AA);
    }
}

void overlay(cv::Mat& image, const cv::Mat& mask, const cv::Scalar& color, double alpha) {
    CV_Assert(mask.size() == image.size());
    cv::Mat colored(image.size(), image.type(), color), blended;
    cv::addWeighted(image, 1 - alpha, colored, alpha, 0, blended);
    blended.copyTo(image, mask != 0);
}

cv::Mat visualize(const cv::Mat& image) {
    cv::Mat f, u8, rgb;
    image.convertTo(f, CV_32F);
    cv::normalize(f, f, 0, 255, cv::NORM_MINMAX);
    f.convertTo(u8, CV_8U);
    if (u8.channels() == 1) cv::cvtColor(u8, rgb, cv::COLOR_GRAY2RGB);
    else rgb = u8;
    return rgb;
}

}  // namespace pc::imgproc
