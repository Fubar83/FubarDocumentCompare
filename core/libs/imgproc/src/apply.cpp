#include "printcmp/imgproc/apply.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pc::imgproc {

namespace {

cv::Rect scaled(const cv::Rect& r, const Placement& p) {
    if (p.scale == 1.0) return r + p.offset;
    int x0 = int(std::lround(r.x * p.scale)), y0 = int(std::lround(r.y * p.scale));
    int x1 = int(std::lround((r.x + r.width) * p.scale)), y1 = int(std::lround((r.y + r.height) * p.scale));
    return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)) + p.offset;
}

// Skriv patch (redan i målrutans storlek) i dst på rutan, bara där masken (samma storlek) är satt.
void write(const cv::Mat& patch, cv::Mat& dst, const cv::Rect& at, const cv::Mat& mask) {
    cv::Rect in = at & cv::Rect(0, 0, dst.cols, dst.rows);
    if (in.empty()) return;
    cv::Rect local(in.tl() - at.tl(), in.size());
    cv::Mat src = fit_type(patch, dst.type())(local);
    if (mask.empty()) src.copyTo(dst(in));
    else src.copyTo(dst(in), mask(local));
}

cv::Mat resize_to(const cv::Mat& m, cv::Size size, bool nearest = false) {
    if (m.size() == size) return m;
    cv::Mat o;
    bool shrink = size.area() < m.size().area();
    cv::resize(m, o, size, 0, 0, nearest ? cv::INTER_NEAREST : shrink ? cv::INTER_AREA : cv::INTER_LINEAR);
    return o;
}

}  // namespace

cv::Mat fit_type(const cv::Mat& m, int type) {
    if (m.type() == type) return m;
    cv::Mat cur = m;
    int want_ch = CV_MAT_CN(type), ch = cur.channels();
    if (ch != want_ch) {
        cv::Mat c;
        if (ch == 1 && want_ch == 3) cv::cvtColor(cur, c, cv::COLOR_GRAY2RGB);
        else if (ch == 1 && want_ch == 4) cv::cvtColor(cur, c, cv::COLOR_GRAY2RGBA);
        else if (ch == 3 && want_ch == 1) cv::cvtColor(cur, c, cv::COLOR_RGB2GRAY);
        else if (ch == 4 && want_ch == 1) cv::cvtColor(cur, c, cv::COLOR_RGBA2GRAY);
        else if (ch == 4 && want_ch == 3) cv::cvtColor(cur, c, cv::COLOR_RGBA2RGB);
        else if (ch == 3 && want_ch == 4) cv::cvtColor(cur, c, cv::COLOR_RGB2RGBA);
        else CV_Error(cv::Error::StsBadArg, "kan inte konvertera kanalerna");
        cur = c;
    }
    if (cur.depth() != CV_MAT_DEPTH(type)) {
        cv::Mat c;
        cur.convertTo(c, CV_MAT_DEPTH(type));
        cur = c;
    }
    return cur;
}

cv::Mat filter_region(const Filter& f, const cv::Mat& src, const Region& region) {
    Region c = region.clipped(src.size());
    if (c.empty()) return {};
    int m = f.margin();
    cv::Rect ctx = cv::Rect(c.rect.x - m, c.rect.y - m, c.rect.width + 2 * m, c.rect.height + 2 * m) &
                   cv::Rect(0, 0, src.cols, src.rows);
    cv::Mat out = f.apply(src(ctx));
    CV_Assert(out.size() == ctx.size());
    return out(cv::Rect(c.rect.tl() - ctx.tl(), c.rect.size())).clone();
}

std::vector<cv::Mat> filter_regions(const Filter& f, const cv::Mat& src, const RegionSet& regions) {
    std::vector<cv::Mat> out;
    out.reserve(regions.size());
    for (auto& r : regions) out.push_back(filter_region(f, src, r));
    return out;
}

void apply_regions(const Filter& f, const cv::Mat& src, const RegionSet& regions, cv::Mat& dst, const Placement& place) {
    // filtrera allt först: dst kan vara src
    std::vector<std::pair<cv::Mat, Region>> results;
    for (auto& r : regions) {
        Region c = r.clipped(src.size());
        if (c.empty()) continue;
        results.push_back({filter_region(f, src, c), c});
    }
    if (dst.empty()) dst = results.empty() ? src.clone() : fit_type(src, results.front().first.type()).clone();
    for (auto& [img, c] : results) {
        cv::Rect at = scaled(c.rect, place);
        write(resize_to(img, at.size()), dst, at, c.has_mask() ? resize_to(c.mask, at.size(), true) : cv::Mat());
    }
}

cv::Mat apply_regions(const Filter& f, const cv::Mat& src, const RegionSet& regions) {
    cv::Mat dst;
    apply_regions(f, src, regions, dst);
    return dst;
}

void apply_to_targets(const Filter& f, const cv::Mat& src, const RegionSet& from, cv::Mat& dst, const RegionSet& to) {
    CV_Assert(from.size() == to.size());
    std::vector<cv::Mat> results = filter_regions(f, src, from);
    if (dst.empty()) dst = src.clone();
    for (size_t i = 0; i < from.size(); ++i) {
        if (results[i].empty() || to[i].empty()) continue;
        cv::Mat mask = to[i].has_mask() ? to[i].mask
                       : from[i].has_mask() ? resize_to(from[i].clipped(src.size()).mask, to[i].rect.size(), true)
                                            : cv::Mat();
        write(resize_to(results[i], to[i].rect.size()), dst, to[i].rect, mask);
    }
}

void paste(const cv::Mat& patch, cv::Mat& dst, const Region& to) {
    if (to.empty() || patch.empty()) return;
    write(resize_to(patch, to.rect.size()), dst, to.rect, to.mask);
}

}  // namespace pc::imgproc
