#include "printcmp/imgproc/match.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pc::imgproc {

namespace {

int cv_method(MatchMethod m) {
    switch (m) {
        case MatchMethod::CorrNormed: return cv::TM_CCORR_NORMED;
        case MatchMethod::SqDiffNormed: return cv::TM_SQDIFF_NORMED;
        default: return cv::TM_CCOEFF_NORMED;
    }
}

// matchTemplate tar 8-bitars eller flyttal; gråskala om gray
cv::Mat prepare(const cv::Mat& m, bool gray) {
    cv::Mat out = m;
    if (gray && m.channels() == 3) cv::cvtColor(m, out, cv::COLOR_RGB2GRAY);
    else if (gray && m.channels() == 4) cv::cvtColor(m, out, cv::COLOR_RGBA2GRAY);
    else if (m.channels() == 4) cv::cvtColor(m, out, cv::COLOR_RGBA2RGB);
    if (out.depth() != CV_8U && out.depth() != CV_32F) {
        cv::Mat f;
        out.convertTo(f, CV_32F);
        out = f;
    }
    return out;
}

struct Variant {
    cv::Mat templ, mask;                  // mask tom = hela mallen
    std::vector<cv::Point2f> corners;     // ursprungsmallens hörn i variantens koordinater
    double angle, scale;
};

Variant make_variant(const cv::Mat& templ, const cv::Mat& user_mask, double angle, double scale) {
    Variant v{templ, user_mask, {}, angle, scale};
    std::vector<cv::Point2f> c = {{0, 0}, {float(templ.cols), 0}, {float(templ.cols), float(templ.rows)}, {0, float(templ.rows)}};
    if (scale != 1.0) {
        cv::resize(templ, v.templ, cv::Size(), scale, scale, scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
        if (!user_mask.empty()) cv::resize(user_mask, v.mask, v.templ.size(), 0, 0, cv::INTER_NEAREST);
        for (auto& p : c) p *= float(scale);
    }
    if (angle != 0.0) {
        cv::Point2f center(v.templ.cols / 2.0f, v.templ.rows / 2.0f);
        cv::Mat R = cv::getRotationMatrix2D(center, angle, 1.0);
        cv::Rect2f bb = cv::RotatedRect(center, v.templ.size(), float(-angle)).boundingRect2f();
        R.at<double>(0, 2) += bb.width / 2.0 - center.x;
        R.at<double>(1, 2) += bb.height / 2.0 - center.y;
        cv::Size size(int(std::ceil(bb.width)), int(std::ceil(bb.height)));
        cv::Mat base_mask = v.mask.empty() ? cv::Mat(v.templ.size(), CV_8U, cv::Scalar(255)) : v.mask;
        cv::Mat t, m;
        cv::warpAffine(v.templ, t, R, size, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
        cv::warpAffine(base_mask, m, R, size, cv::INTER_NEAREST, cv::BORDER_CONSTANT);
        v.templ = t;
        v.mask = m;
        cv::transform(c, c, R);
    }
    v.corners = c;
    return v;
}

}  // namespace

Region Match::region() const {
    std::vector<cv::Point> pts;
    for (auto& p : corners) pts.push_back(cv::Point(int(std::lround(p.x)), int(std::lround(p.y))));
    Region r = pts.empty() ? Region::box(box) : Region::polygon(pts);
    r.score = score;
    return r;
}

cv::Mat match_map(const cv::Mat& image, const cv::Mat& templ, MatchMethod method, const cv::Mat& mask) {
    cv::Mat img = prepare(image, true), t = prepare(templ, true), r;
    if (img.depth() != t.depth()) {
        img.convertTo(img, CV_32F);
        t.convertTo(t, CV_32F);
    }
    cv::matchTemplate(img, t, r, cv_method(method), mask);
    cv::patchNaNs(r, 0);
    if (method == MatchMethod::SqDiffNormed) r = 1.0 - r;
    return r;
}

std::vector<cv::Point> peaks(const cv::Mat& response, double threshold, int min_distance, int max_n) {
    CV_Assert(response.type() == CV_32F);
    int md = std::max(1, min_distance);
    cv::Mat dil;
    cv::dilate(response, dil, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * md + 1, 2 * md + 1)));
    cv::Mat is_max = (response >= dil) & (response >= threshold);
    std::vector<cv::Point> pts;
    cv::findNonZero(is_max, pts);
    std::sort(pts.begin(), pts.end(), [&](const cv::Point& a, const cv::Point& b) { return response.at<float>(a) > response.at<float>(b); });
    std::vector<cv::Point> out;
    for (auto& p : pts) {
        bool near = false;
        for (auto& q : out)
            if (std::abs(p.x - q.x) <= md && std::abs(p.y - q.y) <= md) {
                near = true;   // platåer ger flera lika maxima
                break;
            }
        if (near) continue;
        out.push_back(p);
        if (int(out.size()) >= max_n) break;
    }
    return out;
}

namespace {

// Kandidater för alla vinklar och skalor inom områdena (bildens och mallens upplösning).
std::vector<Match> candidates(const cv::Mat& image, const cv::Mat& templ, const cv::Mat& mask, const MatchOptions& opt,
                              const std::vector<Region>& areas, double threshold) {
    std::vector<Match> cand;
    for (double scale : opt.scales)
        for (double angle : opt.angles) {
            Variant v = make_variant(templ, mask, angle, scale);
            if (v.templ.empty()) continue;
            int md = std::max(1, std::min(v.templ.cols, v.templ.rows) / 2);
            for (auto& area : areas) {
                if (area.rect.width < v.templ.cols || area.rect.height < v.templ.rows) continue;
                cv::Mat r;
                cv::matchTemplate(image(area.rect), v.templ, r, cv_method(opt.method), v.mask);
                cv::patchNaNs(r, 0);
                if (opt.method == MatchMethod::SqDiffNormed) r = 1.0 - r;
                for (auto& p : peaks(r, threshold, md, opt.max_matches * 4)) {
                    Match m;
                    m.box = cv::Rect(area.rect.tl() + p, v.templ.size());
                    m.center = {m.box.x + v.templ.cols / 2.0, m.box.y + v.templ.rows / 2.0};
                    if (area.has_mask() && !area.contains(cv::Point(int(m.center.x), int(m.center.y)))) continue;
                    m.score = r.at<float>(p);
                    m.angle = angle;
                    m.scale = scale;
                    for (auto& c : v.corners) m.corners.push_back(c + cv::Point2f(float(m.box.x), float(m.box.y)));
                    cand.push_back(std::move(m));
                }
            }
        }
    return cand;
}

int pyramid_factor(const MatchOptions& opt, cv::Size t) {
    if (opt.pyramid > 0) return opt.pyramid;
    int f = 1;
    double smallest = std::min(t.width, t.height) * *std::min_element(opt.scales.begin(), opt.scales.end());
    while (smallest / (f * 2) >= 24 && f < 16) f *= 2;
    return f;
}

cv::Mat down(const cv::Mat& m, int f, bool nearest = false) {
    cv::Mat o;
    cv::resize(m, o, cv::Size(std::max(1, m.cols / f), std::max(1, m.rows / f)), 0, 0, nearest ? cv::INTER_NEAREST : cv::INTER_AREA);
    return o;
}

}  // namespace

std::vector<Match> find_template(const cv::Mat& image_in, const cv::Mat& templ_in, const MatchOptions& opt) {
    cv::Mat image = prepare(image_in, opt.gray), templ = prepare(templ_in, opt.gray);
    if (image.depth() != templ.depth()) {
        image.convertTo(image, CV_32F);
        templ.convertTo(templ, CV_32F);
    }
    std::vector<Region> areas;
    if (opt.search_in.empty()) areas.push_back(Region::box(cv::Rect(0, 0, image.cols, image.rows)));
    else areas = clip(opt.search_in, image.size());

    std::vector<Match> cand;
    int f = pyramid_factor(opt, templ.size());
    if (f <= 1) {
        cand = candidates(image, templ, opt.template_mask, opt, areas, opt.threshold);
    } else {
        // grovt: nedskalad bild och mall, lägre tröskel; sedan varje kandidat i full upplösning nära sitt läge
        std::vector<Region> small_areas;
        for (auto& a : areas) {
            Region s = Region::box(cv::Rect(a.rect.x / f, a.rect.y / f, std::max(1, a.rect.width / f), std::max(1, a.rect.height / f)));
            if (a.has_mask()) s.mask = down(a.mask, f, true);
            if (!s.mask.empty() && s.mask.size() != s.rect.size()) cv::resize(s.mask, s.mask, s.rect.size(), 0, 0, cv::INTER_NEAREST);
            small_areas.push_back(s);
        }
        cv::Mat small_mask = opt.template_mask.empty() ? cv::Mat() : down(opt.template_mask, f, true);
        auto coarse = candidates(down(image, f), down(templ, f), small_mask, opt, small_areas, std::max(0.3, opt.threshold - 0.25));
        int pad = 2 * f + 2;
        for (auto& c : coarse) {
            Variant v = make_variant(templ, opt.template_mask, c.angle, c.scale);
            cv::Rect win = cv::Rect(c.box.x * f - pad, c.box.y * f - pad, v.templ.cols + 2 * pad, v.templ.rows + 2 * pad) &
                           cv::Rect(0, 0, image.cols, image.rows);
            if (win.width < v.templ.cols || win.height < v.templ.rows) continue;
            cv::Mat r;
            cv::matchTemplate(image(win), v.templ, r, cv_method(opt.method), v.mask);
            cv::patchNaNs(r, 0);
            if (opt.method == MatchMethod::SqDiffNormed) r = 1.0 - r;
            double best;
            cv::Point p;
            cv::minMaxLoc(r, nullptr, &best, nullptr, &p);
            if (best < opt.threshold) continue;
            Match m;
            m.box = cv::Rect(win.tl() + p, v.templ.size());
            m.center = {m.box.x + v.templ.cols / 2.0, m.box.y + v.templ.rows / 2.0};
            m.score = best;
            m.angle = c.angle;
            m.scale = c.scale;
            for (auto& q : v.corners) m.corners.push_back(q + cv::Point2f(float(m.box.x), float(m.box.y)));
            bool inside = false;
            for (auto& a : areas) inside |= a.contains(cv::Point(int(m.center.x), int(m.center.y)));
            if (inside) cand.push_back(std::move(m));
        }
    }
    // bäst först; träffar som överlappar en bättre slås ihop (vinklar/skalor ger flera nära träffar)
    std::sort(cand.begin(), cand.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    std::vector<Match> out;
    for (auto& m : cand) {
        bool dup = false;
        for (auto& k : out)
            if (iou(m.box, k.box) > opt.max_overlap) {
                dup = true;
                break;
            }
        if (dup) continue;
        out.push_back(m);
        if (int(out.size()) >= opt.max_matches) break;
    }
    return out;
}

RegionSet to_regions(const std::vector<Match>& matches) {
    RegionSet out;
    for (auto& m : matches) out.push_back(m.region());
    return out;
}

}  // namespace pc::imgproc
