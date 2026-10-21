#include "printcmp/imgproc/region.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pc::imgproc {

namespace {

// mask i rektangelns koordinater ur punkter i bildens koordinater
Region from_points(const std::vector<cv::Point>& pts, std::string label) {
    Region r;
    if (pts.empty()) return r;
    r.rect = cv::boundingRect(pts);
    r.mask = cv::Mat::zeros(r.rect.size(), CV_8U);
    std::vector<cv::Point> local;
    for (auto& p : pts) local.push_back(p - r.rect.tl());
    cv::fillPoly(r.mask, std::vector<std::vector<cv::Point>>{local}, 255, cv::LINE_8);
    r.label = std::move(label);
    return r;
}

}  // namespace

Region Region::box(const cv::Rect& rc, std::string label) {
    Region r;
    r.rect = rc;
    r.label = std::move(label);
    return r;
}

Region Region::polygon(const std::vector<cv::Point>& pts, std::string label) { return from_points(pts, std::move(label)); }

Region Region::circle(cv::Point c, int radius, std::string label) {
    Region r;
    r.rect = cv::Rect(c.x - radius, c.y - radius, 2 * radius + 1, 2 * radius + 1);
    r.mask = cv::Mat::zeros(r.rect.size(), CV_8U);
    cv::circle(r.mask, cv::Point(radius, radius), radius, 255, cv::FILLED, cv::LINE_8);
    r.label = std::move(label);
    return r;
}

Region Region::rotated(const cv::RotatedRect& rr, std::string label) {
    cv::Point2f p[4];
    rr.points(p);
    std::vector<cv::Point> pts;
    for (auto& q : p) pts.push_back(cv::Point(int(std::lround(q.x)), int(std::lround(q.y))));
    return from_points(pts, std::move(label));
}

Region Region::from_mask(const cv::Mat& full, std::string label) {
    CV_Assert(full.type() == CV_8U);
    Region r;
    std::vector<cv::Point> nz;
    cv::findNonZero(full, nz);
    if (nz.empty()) return r;
    r.rect = cv::boundingRect(nz);
    r.mask = (full(r.rect) != 0);
    r.label = std::move(label);
    return r;
}

Region Region::mm(double x, double y, double w, double h, double dpi, std::string label) {
    double ppm = dpi / 25.4;
    int x0 = int(std::lround(x * ppm)), y0 = int(std::lround(y * ppm));
    int x1 = int(std::lround((x + w) * ppm)), y1 = int(std::lround((y + h) * ppm));
    return box(cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)), std::move(label));
}

int Region::area() const { return has_mask() ? cv::countNonZero(mask) : rect.area(); }

bool Region::contains(cv::Point p) const {
    if (!rect.contains(p)) return false;
    return !has_mask() || mask.at<uchar>(p.y - rect.y, p.x - rect.x) != 0;
}

cv::Mat Region::full_mask(cv::Size size) const {
    cv::Mat m = cv::Mat::zeros(size, CV_8U);
    Region c = clipped(size);
    if (c.empty()) return m;
    if (c.has_mask()) c.mask.copyTo(m(c.rect));
    else m(c.rect).setTo(255);
    return m;
}

Region Region::clipped(cv::Size size) const {
    Region r = *this;
    cv::Rect in = rect & cv::Rect(0, 0, size.width, size.height);
    if (in.empty()) {
        r.rect = {};
        r.mask = {};
        return r;
    }
    if (has_mask()) r.mask = mask(cv::Rect(in.tl() - rect.tl(), in.size())).clone();
    r.rect = in;
    return r;
}

Region Region::translated(cv::Point o) const {
    Region r = *this;
    r.rect += o;
    return r;
}

RegionSet regions_from_mask(const cv::Mat& mask, int min_area) {
    CV_Assert(mask.type() == CV_8U);
    cv::Mat labels, stats, centroids;
    int n = cv::connectedComponentsWithStats(mask != 0, labels, stats, centroids, 8, CV_32S);
    RegionSet out;
    for (int i = 1; i < n; ++i) {
        if (stats.at<int>(i, cv::CC_STAT_AREA) < min_area) continue;
        cv::Rect r(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP), stats.at<int>(i, cv::CC_STAT_WIDTH),
                   stats.at<int>(i, cv::CC_STAT_HEIGHT));
        Region reg = Region::box(r, std::to_string(out.size() + 1));
        reg.mask = (labels(r) == i);
        out.push_back(std::move(reg));
    }
    return out;
}

cv::Mat mask_of(const RegionSet& regions, cv::Size size) {
    cv::Mat m = cv::Mat::zeros(size, CV_8U);
    for (auto& r : regions) m |= r.full_mask(size);
    return m;
}

RegionSet grid(const cv::Rect& area, int rows, int cols, int gap) {
    RegionSet out;
    if (rows <= 0 || cols <= 0) return out;
    double w = (area.width - (cols - 1) * gap) / double(cols), h = (area.height - (rows - 1) * gap) / double(rows);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            int x0 = area.x + int(std::lround(c * (w + gap))), y0 = area.y + int(std::lround(r * (h + gap)));
            int x1 = area.x + int(std::lround(c * (w + gap) + w)), y1 = area.y + int(std::lround(r * (h + gap) + h));
            out.push_back(Region::box(cv::Rect(x0, y0, x1 - x0, y1 - y0), std::to_string(r) + "," + std::to_string(c)));
        }
    return out;
}

RegionSet clip(const RegionSet& regions, cv::Size size) {
    RegionSet out;
    for (auto& r : regions) {
        auto c = r.clipped(size);
        if (!c.empty()) out.push_back(std::move(c));
    }
    return out;
}

double iou(const cv::Rect& a, const cv::Rect& b) {
    double inter = (a & b).area(), uni = a.area() + b.area() - inter;
    return uni > 0 ? inter / uni : 0.0;
}

}  // namespace pc::imgproc
