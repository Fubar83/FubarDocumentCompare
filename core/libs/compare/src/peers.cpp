// Etiketterna på ett ark jämförda med varandra (utan original): detektering mot bakgrunden, förlaga
// (medoid), passning mot förlagan, median som konsensus och avvikelser per etikett.
#include "printcmp/compare/peers.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <opencv2/imgproc.hpp>

#include "printcmp/compare/profiles.hpp"
#include "printcmp/image/image.hpp"
#include "printcmp/imgproc/transform.hpp"
#include "printcmp/scan/diff.hpp"
#include "printcmp/scan/folds.hpp"
#include "printcmp/scan/locate.hpp"
#include "printcmp/scan/normalize.hpp"
#include "printcmp/scan/register.hpp"

namespace pc::compare {

namespace {

using Quad = std::array<Pt, 4>;

constexpr double DETECT_DPI = 40.0, MATCH_DPI = 75.0;

double norm_skew(double a) { return std::fmod(std::fmod(a + 45.0, 90.0) + 90.0, 90.0) - 45.0; }

std::array<cv::Point2d, 4> cv_quad(const Quad& q) {
    std::array<cv::Point2d, 4> r;
    for (int k = 0; k < 4; ++k) r[k] = {q[k].x, q[k].y};
    return r;
}

// Hörnen vridna k kvartsvarv: etikettens övre vänstra hörn ligger där det k:te hörnet låg.
Quad shift(const Quad& q, int k) {
    Quad r;
    for (int c = 0; c < 4; ++c) r[c] = q[(c + k) % 4];
    return r;
}

double side(const Quad& q, int a, int b) { return std::hypot(q[b].x - q[a].x, q[b].y - q[a].y); }

Mat3 quad_H(cv::Size size, const Quad& q) {
    std::vector<cv::Point2f> from{{0, 0}, {(float)size.width - 1, 0}, {(float)size.width - 1, (float)size.height - 1}, {0, (float)size.height - 1}}, to;
    for (auto& c : q) to.emplace_back((float)c.x, (float)c.y);
    cv::Mat h = cv::getPerspectiveTransform(from, to);
    Mat3 H;
    for (int i = 0; i < 9; ++i) H.m[i] = h.at<double>(i / 3, i % 3);
    return H;
}

// Median per pixel av flera bilder med en kanal (CV_32F).
cv::Mat median_of(const std::vector<cv::Mat>& imgs) {
    cv::Mat med(imgs[0].size(), CV_32F);
    size_t n = imgs.size();
    std::vector<float> buf(n);
    std::vector<const float*> rows(n);
    for (int y = 0; y < med.rows; ++y) {
        for (size_t k = 0; k < n; ++k) rows[k] = imgs[k].ptr<float>(y);
        float* o = med.ptr<float>(y);
        for (int x = 0; x < med.cols; ++x) {
            for (size_t k = 0; k < n; ++k) buf[k] = rows[k][x];
            std::nth_element(buf.begin(), buf.begin() + n / 2, buf.end());
            float hi = buf[n / 2];
            o[x] = n % 2 ? hi : (hi + *std::max_element(buf.begin(), buf.begin() + n / 2)) / 2;
        }
    }
    return med;
}

cv::Mat median_multi(const std::vector<cv::Mat>& imgs) {
    std::vector<std::vector<cv::Mat>> ch(imgs[0].channels());
    for (auto& im : imgs) {
        std::vector<cv::Mat> parts;
        cv::split(im, parts);
        for (size_t c = 0; c < parts.size(); ++c) ch[c].push_back(parts[c]);
    }
    std::vector<cv::Mat> meds;
    for (auto& c : ch) meds.push_back(median_of(c));
    cv::Mat out;
    cv::merge(meds, out);
    return out;
}

double ncc(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat r;
    cv::matchTemplate(a, b, r, cv::TM_CCOEFF_NORMED);
    return r.at<float>(0, 0);
}

double ncc_masked(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask) {
    cv::Scalar ma, sa, mb, sb;
    cv::meanStdDev(a, ma, sa, mask);
    cv::meanStdDev(b, mb, sb, mask);
    if (sa[0] < 1e-6 || sb[0] < 1e-6) return 0;
    cv::Mat p = (a - ma[0]).mul(b - mb[0]);
    return cv::mean(p, mask)[0] / (sa[0] * sb[0]);
}

// Tänkbara bakgrundsfärger ur en ram längs kanten: medianen och de två vanligaste färgerna. Ett snett
// ark lämnar skannerlockets färg i hörnen; den är ofta lika ljus som etiketterna, så bakgrundspappret
// (mellan etiketterna) behöver inte vara medianen.
std::vector<cv::Vec3f> background_candidates(const cv::Mat& small) {
    int b = std::max(2, std::min(small.cols, small.rows) / 30);
    std::vector<cv::Vec3f> px;
    for (int y = 0; y < small.rows; ++y)
        for (int x = 0; x < small.cols; ++x)
            if (x < b || y < b || x >= small.cols - b || y >= small.rows - b) {
                auto p = small.at<cv::Vec3b>(y, x);
                px.push_back({(float)p[0], (float)p[1], (float)p[2]});
            }
    std::vector<cv::Vec3f> out;
    cv::Vec3f med;
    for (int c = 0; c < 3; ++c) {
        std::vector<float> v;
        for (auto& p : px) v.push_back(p[c]);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        med[c] = v[v.size() / 2];
    }
    out.push_back(med);
    cv::Mat data((int)px.size(), 3, CV_32F, px.data()), labels, centers;
    cv::kmeans(data, 2, labels, cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 20, 0.5), 3, cv::KMEANS_PP_CENTERS, centers);
    for (int k = 0; k < 2; ++k) {
        if (cv::countNonZero(labels == k) < 0.1 * px.size()) continue;
        cv::Vec3f c(centers.at<float>(k, 0), centers.at<float>(k, 1), centers.at<float>(k, 2));
        bool dup = false;
        for (auto& o : out) dup |= cv::norm(o - c) < 6;
        if (!dup) out.push_back(c);
    }
    return out;
}

// Förgrunden: det som skiljer sig från bakgrundsfärgen.
cv::Mat foreground(const cv::Mat& small, const cv::Vec3f& bg) {
    std::vector<cv::Mat> ch;
    cv::split(small, ch);
    cv::Mat dist(small.size(), CV_32F, cv::Scalar(0));
    for (int c = 0; c < 3; ++c) {
        cv::Mat f;
        ch[c].convertTo(f, CV_32F);
        dist = cv::max(dist, cv::abs(f - bg[c]));
    }
    cv::GaussianBlur(dist, dist, cv::Size(), 0.8);
    return dist > 14.0;
}

cv::Mat fill_holes(const cv::Mat& m) {
    cv::Mat pad;
    cv::copyMakeBorder(m, pad, 1, 1, 1, 1, cv::BORDER_CONSTANT, 0);
    cv::Mat flood = pad.clone();
    cv::floodFill(flood, cv::Point(0, 0), 255);
    cv::Mat filled = pad | ~flood;
    return filled(cv::Rect(1, 1, m.cols, m.rows)).clone();
}

struct Candidate {
    std::vector<cv::RotatedRect> rects;
    cv::Vec3f bg;
    double score = -1;       // andelen stora rektangulära komponenter som är lika stora
    double median_area = 0;  // px
};

// Komponenterna efter slutning (radien close_r) och erosion (erode_r): de som är ungefär lika stora och
// rektangulära. Erosionen skiljer etiketter som hänger ihop med smala broar (veckens skugga, det vita
// utanför ett snett ark); rektanglarna växer tillbaka med lika mycket.
Candidate components(const cv::Mat& fg, int close_r, int erode_r, double min_area_px) {
    cv::Mat m = fg.clone();
    if (close_r > 0) cv::morphologyEx(m, m, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * close_r + 1, 2 * close_r + 1)));
    m = fill_holes(m);
    cv::morphologyEx(m, m, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    if (erode_r > 0) cv::erode(m, m, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * erode_r + 1, 2 * erode_r + 1)));
    std::vector<std::vector<cv::Point>> cs;
    cv::findContours(m, cs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<std::pair<double, cv::RotatedRect>> big;
    for (auto& c : cs) {
        auto rr = cv::minAreaRect(c);
        double fill = cv::contourArea(c) / std::max(1.0, (double)rr.size.area());
        rr.size.width += 2.0f * erode_r;
        rr.size.height += 2.0f * erode_r;
        double a = rr.size.area();
        if (a < min_area_px || fill < 0.88) continue;
        big.push_back({a, rr});
    }
    Candidate out;
    if (big.empty()) return out;
    std::vector<double> areas;
    for (auto& [a, _] : big) areas.push_back(a);
    std::nth_element(areas.begin(), areas.begin() + areas.size() / 2, areas.end());
    out.median_area = areas[areas.size() / 2];
    for (auto& [a, rr] : big)
        if (std::abs(a - out.median_area) < 0.3 * out.median_area) out.rects.push_back(rr);
    out.score = (double)out.rects.size() / big.size();
    return out;
}

// Hörnen ur en roterad rektangel: snedheten −45..45°, övre vänster först, i helbildens px.
DetectedLabel to_label(const cv::RotatedRect& rr, double s) {
    double a = rr.angle, w = rr.size.width, h = rr.size.height;
    while (a > 45) a -= 90, std::swap(w, h);
    while (a < -45) a += 90, std::swap(w, h);
    double r = a * CV_PI / 180, ux = std::cos(r), uy = std::sin(r), vx = -uy, vy = ux;
    double cx = rr.center.x, cy = rr.center.y;
    Pt q[4] = {{cx - ux * w / 2 - vx * h / 2, cy - uy * w / 2 - vy * h / 2},
               {cx + ux * w / 2 - vx * h / 2, cy + uy * w / 2 - vy * h / 2},
               {cx + ux * w / 2 + vx * h / 2, cy + uy * w / 2 + vy * h / 2},
               {cx - ux * w / 2 + vx * h / 2, cy - uy * w / 2 + vy * h / 2}};
    DetectedLabel d;
    for (int k = 0; k < 4; ++k) d.corners[k] = {(q[k].x + 0.5) / s - 0.5, (q[k].y + 0.5) / s - 0.5};
    d.angle_deg = -a;   // bildens y nedåt: minAreaRect vrider medurs
    return d;
}

// Grovdetekteringen ger vinkeln på någon grad när: räta upp, mät resten och vrid hörnen kring mitten.
void refine(DetectedLabel& d, const cv::Mat& sheet) {
    cv::Size size((int)std::lround(side(d.corners, 0, 1)), (int)std::lround(side(d.corners, 1, 2)));
    if (size.width < 32 || size.height < 32) return;
    auto s = imgproc::estimate_skew(imgproc::warp_quad(sheet, cv_quad(d.corners), size), 3.0);
    if (s.confidence < 0.3 || std::abs(s.angle_deg) < 0.01) return;
    Pt c{0, 0};
    for (auto& p : d.corners) c.x += p.x / 4, c.y += p.y / 4;
    cv::Mat m = cv::getRotationMatrix2D(cv::Point2d(c.x, c.y), s.angle_deg, 1.0);
    for (auto& p : d.corners)
        p = {m.at<double>(0, 0) * p.x + m.at<double>(0, 1) * p.y + m.at<double>(0, 2),
             m.at<double>(1, 0) * p.x + m.at<double>(1, 1) * p.y + m.at<double>(1, 2)};
    d.angle_deg += s.angle_deg;
}


// Kanterna i högre upplösning: räta upp etiketten med en marginal runt om, mät var förgrunden (det som
// skiljer sig från bakgrunden) börjar och slutar i mitten av varje sida och flytta sidorna dit.
void refine_edges(DetectedLabel& d, const cv::Mat& sheet, const cv::Vec3f& bg, double dpi) {
    double s = std::min(1.0, 150.0 / dpi), margin = 3.0 * dpi / MM_PER_INCH;
    Pt c{0, 0};
    for (auto& p : d.corners) c.x += p.x / 4, c.y += p.y / 4;
    double w = side(d.corners, 0, 1), h = side(d.corners, 1, 2);
    if (w < 16 || h < 16) return;
    double ux = (d.corners[1].x - d.corners[0].x) / w, uy = (d.corners[1].y - d.corners[0].y) / w;
    double vx = (d.corners[3].x - d.corners[0].x) / h, vy = (d.corners[3].y - d.corners[0].y) / h;
    double W = w + 2 * margin, H = h + 2 * margin;
    auto at = [&](double a, double b) { return Pt{c.x + ux * a + vx * b, c.y + uy * a + vy * b}; };   // a, b från mitten
    Quad big{at(-W / 2, -H / 2), at(W / 2, -H / 2), at(W / 2, H / 2), at(-W / 2, H / 2)};
    cv::Size size((int)std::lround(W * s), (int)std::lround(H * s));
    cv::Mat crop = imgproc::warp_quad(sheet, cv_quad(big), size, imgproc::Interpolation::Linear);
    cv::Mat fg = foreground(crop, bg);
    // andel förgrund per kolumn (mittre 60 % av raderna) och per rad (mittre 60 % av kolumnerna)
    cv::Mat cols, rows;
    cv::reduce(fg(cv::Range((int)(0.2 * fg.rows), (int)(0.8 * fg.rows)), cv::Range::all()), cols, 0, cv::REDUCE_AVG, CV_32F);
    cv::reduce(fg(cv::Range::all(), cv::Range((int)(0.2 * fg.cols), (int)(0.8 * fg.cols))), rows, 1, cv::REDUCE_AVG, CV_32F);
    // Förgrunden kring mitten (luckor under 1 mm överbryggas): stannar vid mellanrummet till grannen. Om
    // etiketten själv har stora tomma partier (vit etikett på vit bakgrund) blir det för kort, och då
    // gäller första och sista raden med förgrund.
    int bridge = std::max(1, (int)std::lround(1.0 * dpi * s / MM_PER_INCH));
    auto around_center = [&](const cv::Mat& prof, int n, int& lo, int& hi) {
        const float* p = prof.ptr<float>();
        auto on = [&](int i) { return p[i] >= 0.5f * 255; };
        auto walk = [&](int from, int dir) {
            int last = from, gap = 0;
            for (int i = from; i >= 0 && i < n; i += dir) {
                if (on(i)) last = i, gap = 0;
                else if (++gap > bridge) break;
            }
            return last;
        };
        lo = walk(n / 2, -1), hi = walk(n / 2, 1);
        return hi > lo;
    };
    auto first_last = [](const cv::Mat& prof, int n, int& lo, int& hi) {
        const float* p = prof.ptr<float>();
        lo = 0, hi = n - 1;
        while (lo < n && p[lo] < 0.5f * 255) ++lo;
        while (hi > lo && p[hi] < 0.5f * 255) --hi;
        return hi > lo;
    };
    cv::Mat rows_t = rows.t();
    for (int method = 0; method < 2; ++method) {
        int x0, x1, y0, y1;
        bool ok = method == 0 ? around_center(cols, fg.cols, x0, x1) && around_center(rows_t, fg.rows, y0, y1)
                              : first_last(cols, fg.cols, x0, x1) && first_last(rows_t, fg.rows, y0, y1);
        if (!ok) continue;
        double a0 = x0 / s - W / 2, a1 = (x1 + 1) / s - W / 2, b0 = y0 / s - H / 2, b1 = (y1 + 1) / s - H / 2;
        // bara små rättelser: en stor ändring betyder att något annat än etiketten (smuts, skugga) tog över
        if (std::abs(a1 - a0 - w) > 0.15 * w || std::abs(b1 - b0 - h) > 0.15 * h) continue;
        d.corners = {at(a0, b0), at(a1, b0), at(a1, b1), at(a0, b1)};
        return;
    }
}
json box_json(const Box& b) { return {b.x0, b.y0, b.w(), b.h()}; }

}  // namespace

std::vector<DetectedLabel> detect_labels(const cv::Mat& sheet_rgb, double dpi, const PeerOptions& opt) {
    if (sheet_rgb.empty() || sheet_rgb.type() != CV_8UC3) throw Error(ErrorCode::InvalidArgument, "arket ska vara en RGB-bild");
    if (!(dpi > 0)) throw Error(ErrorCode::InvalidArgument, "arkets upplösning saknas");
    double s = std::min(1.0, DETECT_DPI / dpi), sppm = dpi * s / MM_PER_INCH;
    cv::Mat small;
    cv::resize(sheet_rgb, small, cv::Size(), s, s, cv::INTER_AREA);
    double min_px = opt.min_label_mm2 * sppm * sppm, img_area = (double)small.total();
    // Alla kombinationer av bakgrundsfärg, slutning och erosion; flest lika stora rektanglar vinner (med
    // känt antal: närmast det), sedan störst etiketter (inte delar av etiketter) och minst ingrepp.
    auto quality = [&](const Candidate& c, int step) {
        if (c.rects.empty()) return -1e18;
        double tie = 10.0 * c.median_area / img_area - 0.01 * step;
        if (opt.expected_count) return -1e6 * std::abs((double)c.rects.size() - *opt.expected_count) + 100.0 * c.score + tie;
        return (c.score >= 0.99 ? 1e6 : 0.0) + 1000.0 * c.rects.size() * c.score + tie;
    };
    Candidate best;
    double best_q = -1e18;
    for (auto& bg : background_candidates(small)) {
        cv::Mat fg = foreground(small, bg);
        int step = 0;
        for (double e_mm : {0.0, 1.5, 3.0, 4.5})
            for (double r_mm : {0.0, 0.5, 1.0, 1.5, 2.0, 3.0}) {
                auto c = components(fg, (int)std::lround(r_mm * sppm), (int)std::lround(e_mm * sppm), min_px);
                c.bg = bg;
                double q = quality(c, step++);
                if (q > best_q) best_q = q, best = c;
            }
    }
    std::vector<DetectedLabel> out;
    for (auto& rr : best.rects) {
        auto d = to_label(rr, s);
        refine(d, sheet_rgb);
        refine_edges(d, sheet_rgb, best.bg, dpi);
        out.push_back(d);
    }
    if (out.empty()) return out;

    // Bakgrunden räcker inte alltid (veckens skugga eller smuts binder ihop etiketter, etiketter som ligger
    // kant i kant). Den etikett som är mest lik de andras storlek blir mall och letas på hela arket som mot
    // ett original: alla vridningar, och arkets snedhet eftersom mallen är upprätad.
    std::vector<double> areas;
    for (auto& d : out) areas.push_back(side(d.corners, 0, 1) * side(d.corners, 1, 2));
    auto sorted = areas;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    size_t t = 0;
    for (size_t i = 1; i < out.size(); ++i)
        if (std::abs(areas[i] - sorted[sorted.size() / 2]) < std::abs(areas[t] - sorted[sorted.size() / 2])) t = i;
    cv::Size tsize((int)std::lround(side(out[t].corners, 0, 1)), (int)std::lround(side(out[t].corners, 1, 2)));
    if (tsize.width < 32 || tsize.height < 32) return out;
    cv::Mat tmpl = imgproc::warp_quad(sheet_rgb, cv_quad(out[t].corners), tsize, imgproc::Interpolation::Linear);
    cv::Mat inside(tsize, CV_8U, cv::Scalar(0));
    int b = std::max(1, (int)std::lround(1.0 * dpi / MM_PER_INCH));
    inside(cv::Rect(b, b, std::max(1, tsize.width - 2 * b), std::max(1, tsize.height - 2 * b))).setTo(1);
    double max_skew = std::abs(norm_skew(out[t].angle_deg)) + 2.0;
    auto [insts, info] = scan::locate(tmpl, inside, dpi, sheet_rgb, dpi, {0, 90, 180, 270}, max_skew, opt.expected_count);
    if (insts.size() < out.size()) return out;
    Pt rect[4] = {{0, 0}, {(double)tsize.width - 1, 0}, {(double)tsize.width - 1, (double)tsize.height - 1}, {0, (double)tsize.height - 1}};
    std::vector<DetectedLabel> located;
    std::vector<std::vector<cv::Point2f>> taken;
    std::stable_sort(insts.begin(), insts.end(), [](const scan::Instance& a, const scan::Instance& b) { return a.score > b.score; });
    for (auto& in : insts) {
        DetectedLabel d;
        for (int k = 0; k < 4; ++k) d.corners[k] = in.H0.apply(rect[k]);
        // etiketter ligger inte på varandra: en träff som täcker en bättre till mer än en tiondel är en halvträff
        std::vector<cv::Point2f> poly;
        for (auto& c : d.corners) poly.emplace_back((float)c.x, (float)c.y);
        bool overlap = false;
        for (auto& o : taken) {
            std::vector<cv::Point2f> inter;
            overlap |= cv::intersectConvexConvex(poly, o, inter) > 0.1 * cv::contourArea(poly);
        }
        if (overlap) continue;
        taken.push_back(poly);
        d.angle_deg = in.angle_deg;
        refine(d, sheet_rgb);
        // som detekteringen: övre vänster på arket först, snedheten −45..45°
        int q = (int)std::lround(std::fmod(std::fmod(d.angle_deg - norm_skew(d.angle_deg), 360.0) + 360.0, 360.0) / 90.0) % 4;
        d.corners = shift(d.corners, q);
        d.angle_deg = norm_skew(d.angle_deg);
        located.push_back(d);
    }
    return located;
}

PeerResult compare_peers(const cv::Mat& sheet_rgb, double dpi, const PeerOptions& opt) {
    auto found = detect_labels(sheet_rgb, dpi, opt);
    PeerResult res;
    res.dpi = dpi;
    if (found.empty()) {
        res.notes.push_back("Inga etiketter hittades");
        return res;
    }
    json tol = resolve_profile(opt.profile, opt.tolerance_overrides);
    double ppm = dpi / MM_PER_INCH;

    // ---------- gemensam storlek och läsordning
    size_t n = found.size();
    std::vector<double> ws, hs;
    int n_land = 0;
    for (auto& d : found) {
        double w = side(d.corners, 0, 1), h = side(d.corners, 1, 2);
        n_land += w >= h;
        ws.push_back(std::max(w, h));
        hs.push_back(std::min(w, h));
    }
    std::nth_element(ws.begin(), ws.begin() + n / 2, ws.end());
    std::nth_element(hs.begin(), hs.begin() + n / 2, hs.end());
    bool landscape = 2 * n_land >= (int)n, square = (ws[n / 2] - hs[n / 2]) < 0.05 * ws[n / 2];
    cv::Size size = landscape ? cv::Size((int)std::lround(ws[n / 2]), (int)std::lround(hs[n / 2]))
                              : cv::Size((int)std::lround(hs[n / 2]), (int)std::lround(ws[n / 2]));
    {
        std::vector<scan::Instance> insts;
        std::vector<double> skews;
        for (size_t i = 0; i < n; ++i) {
            scan::Instance in;
            in.score = (double)i;   // assign_grid numrerar om: ursprunget följer med i score
            for (auto& p : found[i].corners) in.center.x += p.x / 4, in.center.y += p.y / 4;
            insts.push_back(in);
            skews.push_back(norm_skew(found[i].angle_deg));
        }
        std::nth_element(skews.begin(), skews.begin() + n / 2, skews.end());
        res.sheet_angle_deg = skews[n / 2];
        insts = scan::assign_grid(insts, res.sheet_angle_deg, 0.4 * std::min(size.width, size.height));
        std::vector<DetectedLabel> ordered;
        res.labels.resize(n);
        for (size_t i = 0; i < n; ++i) {
            ordered.push_back(found[(size_t)insts[i].score]);
            res.labels[i].index = (int)i;
            res.labels[i].row = insts[i].row;
            res.labels[i].col = insts[i].col;
        }
        found = ordered;
    }

    // ---------- vridning och förlaga: korrelation mellan utsnitt i låg upplösning
    double ms = std::min(1.0, MATCH_DPI / dpi);
    cv::Size msize((int)std::lround(size.width * ms), (int)std::lround(size.height * ms));
    auto crop_small = [&](const Quad& q) {
        cv::Mat g = image::gray_f32(imgproc::warp_quad(sheet_rgb, cv_quad(q), size));
        cv::Mat out;
        cv::resize(g, out, msize, 0, 0, cv::INTER_AREA);
        cv::GaussianBlur(out, out, cv::Size(), 1.0);
        return out;
    };
    std::vector<std::vector<int>> cand(n);
    std::vector<std::vector<cv::Mat>> crops(n);
    for (size_t i = 0; i < n; ++i) {
        double w = side(found[i].corners, 0, 1), h = side(found[i].corners, 1, 2);
        if (square) cand[i] = {0, 1, 2, 3};
        else if ((w >= h) == landscape) cand[i] = {0, 2};
        else cand[i] = {3, 1};
        for (int k : cand[i]) crops[i].push_back(crop_small(shift(found[i].corners, k)));
    }
    // sim[i][j]: bästa korrelationen när j vrids mot i (i i sin första kandidat)
    std::vector<std::vector<double>> sim(n, std::vector<double>(n, 0));
    std::vector<std::vector<int>> best_k(n, std::vector<int>(n, 0));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) {
            if (i == j) {
                sim[i][j] = 1;
                best_k[i][j] = cand[j][0];
                continue;
            }
            double b = -2;
            for (size_t c = 0; c < cand[j].size(); ++c) {
                double v = ncc(crops[i][0], crops[j][c]);
                if (v > b) b = v, best_k[i][j] = cand[j][c];
            }
            sim[i][j] = b;
        }
    size_t master = 0;
    double best_sum = -1e9;
    for (size_t i = 0; i < n; ++i) {
        double s = std::accumulate(sim[i].begin(), sim[i].end(), 0.0);
        if (s > best_sum) best_sum = s, master = i;
    }
    std::vector<int> k(n);
    for (size_t j = 0; j < n; ++j) k[j] = best_k[master][j];
    // Utan original är "rätt upp" det som de flesta etiketterna visar
    {
        int best_d = 0, best_c = -1;
        for (int d : square ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{0, 2}) {
            int c = 0;
            for (size_t j = 0; j < n; ++j) c += (k[j] + d) % 4 == cand[j][0];
            if (c > best_c) best_c = c, best_d = d;
        }
        for (auto& kk : k) kk = (kk + best_d) % 4;
    }
    res.labels[master].master = true;

    // ---------- passning mot förlagan i full upplösning
    Quad mq = shift(found[master].corners, k[master]);
    cv::Mat master_rgb = imgproc::warp_quad(sheet_rgb, cv_quad(mq), size, imgproc::Interpolation::Linear);
    cv::Mat master_gs = scan::prepare_gray(master_rgb, ppm), sheet_gs = scan::prepare_gray(sheet_rgb, ppm);
    int border = std::max(2, (int)std::lround(1.0 * ppm));
    cv::Mat valid(size, CV_8U, cv::Scalar(0));
    if (size.width > 2 * border && size.height > 2 * border)
        valid(cv::Rect(border, border, size.width - 2 * border, size.height - 2 * border)).setTo(1);
    res.label_size = size;
    Pt rect[4] = {{0, 0}, {(double)size.width - 1, 0}, {(double)size.width - 1, (double)size.height - 1}, {0, (double)size.height - 1}};
    for (size_t j = 0; j < n; ++j) {
        Quad q = shift(found[j].corners, k[j]);
        Mat3 H0 = quad_H(size, q);
        auto reg = scan::register_images(master_gs, sheet_gs, H0, valid, ppm, tol, opt.seed, opt.local_registration);
        res.aligned_rgb.push_back(scan::resample(sheet_rgb, reg.H, reg.flow, size, cv::INTER_LANCZOS4));
        auto& L = res.labels[j];
        for (int c = 0; c < 4; ++c) L.corners[c] = reg.H.apply(rect[c]);
        L.registration_rms_px = std::isfinite(reg.rms_px) ? round_to(reg.rms_px, 2) : 0.0;
        double skew = std::atan2(L.corners[1].y - L.corners[0].y, L.corners[1].x - L.corners[0].x) * 180 / CV_PI;
        L.angle_deg = std::fmod(round_to(std::fmod(-skew + 720.0, 360.0), 2), 360.0);
    }

    // ---------- konsensus: median av rasterfria, färganpassade etiketter
    double sigma = std::clamp(0.55 * dpi / (70 * 2.54), 1.0, 3.0);
    std::vector<cv::Mat> soft;
    for (auto& a : res.aligned_rgb) soft.push_back(scan::descreen(image::to_float(a), sigma));
    if (n >= 3) {
        cv::Mat med_f = median_multi(soft);
        for (auto& s : soft) s = scan::apply_color(s, scan::fit_color(s, med_f, valid));
    }
    std::vector<cv::Mat> labs;
    for (auto& s : soft) labs.push_back(image::rgb2lab(cv::min(cv::max(s, 0.0), 1.0)));
    cv::Mat med_lab, med_L;
    if (n >= 3) {
        med_lab = median_multi(labs);
        std::vector<cv::Mat> f;
        for (auto& a : res.aligned_rgb) {
            cv::Mat x;
            a.convertTo(x, CV_32F);
            f.push_back(x);
        }
        median_multi(f).convertTo(res.consensus_rgb, CV_8U);
    } else {
        med_lab = labs[master];
        res.consensus_rgb = res.aligned_rgb[master].clone();
        res.notes.push_back(n == 1 ? "Bara en etikett: inget att jämföra med"
                                   : "Bara två etiketter: skillnaderna kan inte tillskrivas den ena av dem");
    }
    {
        std::vector<cv::Mat> ch;
        cv::split(med_lab, ch);
        med_L = ch[0];
    }

    int radius = (int)pyround(tol["edge_radius_mm"].get<double>() * ppm);
    double thr = 0.75 * tol["struct_dl"].get<double>(), min_area = tol["min_blob_mm2"].get<double>() * ppm * ppm;
    double de_thr = tol["delta_e"].get<double>(), min_color = tol["min_color_blob_mm2"].get<double>() * ppm * ppm;
    int cwin = std::max(1, (int)std::lround(tol["color_window_mm"].get<double>() * ppm));
    std::vector<PeerFinding> all;
    for (size_t j = 0; j < n; ++j) {
        auto& PL = res.labels[j];
        std::vector<cv::Mat> ch;
        cv::split(labs[j], ch);
        const cv::Mat& L = ch[0];
        PL.similarity = round_to(std::max(0.0, ncc_masked(L, med_L, valid)), 3);
        if (n == 1 || (n == 2 && j == master)) continue;
        cv::Mat dm;
        cv::GaussianBlur(cv::max(scan::range_dist(L, med_L, radius), scan::range_dist(med_L, L, radius)), dm, cv::Size(), 0.7);
        cv::Mat ok = valid.clone();
        // veck: långa raka band med avvikelse är osäkra zoner, inte fel
        if (tol["fold_detection"].get<bool>()) {
            for (auto& f : scan::detect_folds(med_L, L, valid, ppm, tol["fold_band_mm"])) {
                cv::Mat band(size, CV_8U, cv::Scalar(0));
                scan::draw_extended_band(band, f);
                cv::Rect r = cv::boundingRect(band);
                ok.setTo(0, band);
                PeerFinding pf;
                pf.label = (int)j;
                pf.kind = "Uncertain";
                pf.detail = "veck";
                pf.bbox_px = {(double)r.x, (double)r.y, (double)r.x + r.width, (double)r.y + r.height};
                all.push_back(pf);
            }
        }
        auto blobs = [&](const cv::Mat& mask, double min_px, const cv::Mat& score, bool color) {
            cv::Mat labels, stats, cents;
            int nc = cv::connectedComponentsWithStats(mask, labels, stats, cents, 8, CV_32S);
            for (int c = 1; c < nc; ++c) {
                int x = stats.at<int>(c, 0), y = stats.at<int>(c, 1), w = stats.at<int>(c, 2), h = stats.at<int>(c, 3), a = stats.at<int>(c, 4);
                if (a < min_px) continue;
                cv::Rect r(x, y, w, h);
                cv::Mat sub = labels(r) == c;
                double mx;
                cv::minMaxLoc(score(r), nullptr, &mx, nullptr, nullptr, sub);
                PeerFinding pf;
                pf.label = (int)j;
                pf.bbox_px = {(double)x, (double)y, (double)x + w, (double)y + h};
                pf.area_mm2 = a / (ppm * ppm);
                if (color) {
                    pf.kind = "Color";
                    pf.detail = format("ΔE00 %.1f", mx);
                } else {
                    auto sdl = image::masked_values(L(r) - med_L(r), sub);
                    int pos = 0;
                    for (float v : sdl) pos += v > 0;
                    double frac = sdl.empty() ? 0.5 : (double)pos / sdl.size();
                    pf.kind = frac > 0.75 ? "Missing" : (frac < 0.25 ? "Added" : "Shape");
                    pf.delta_l = round_to(mx, 1);
                }
                all.push_back(pf);
            }
        };
        cv::Mat m = (dm > thr) & (ok > 0);
        cv::morphologyEx(m, m, cv::MORPH_OPEN, cv::Mat::ones(2, 2, CV_8U));
        blobs(m, min_area, dm, false);
        cv::Mat de = image::delta_e_2000(labs[j], med_lab), de_s;
        cv::blur(de, de_s, cv::Size(cwin, cwin));
        // färgavvikelser bara där det inte redan finns ett strukturfynd (fönstret sprider det strukturella)
        cv::Mat near_struct;
        cv::dilate(m, near_struct, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * cwin + 1, 2 * cwin + 1)));
        cv::Mat cm = (de_s > de_thr) & (ok > 0) & ~near_struct;
        blobs(cm, min_color, de_s, true);
    }
    // Samma avvikelse på hälften av etiketterna eller fler är inget enskilt fel (medianen bär den)
    for (auto& f : all) {
        int same = 0;
        for (auto& e : all)
            if (e.kind == f.kind && std::abs(e.bbox_px.cx() - f.bbox_px.cx()) < 1.0 * ppm && std::abs(e.bbox_px.cy() - f.bbox_px.cy()) < 1.0 * ppm) ++same;
        if (f.kind != "Uncertain" && n >= 3 && 2 * same >= (int)n) continue;
        f.x_mm = round_to(f.bbox_px.x0 / ppm, 2);
        f.y_mm = round_to(f.bbox_px.y0 / ppm, 2);
        f.w_mm = round_to(f.bbox_px.w() / ppm, 2);
        f.h_mm = round_to(f.bbox_px.h() / ppm, 2);
        f.area_mm2 = round_to(f.area_mm2, 3);
        res.labels[f.label].findings.push_back(f);
    }
    res.sheet_angle_deg = round_to(res.sheet_angle_deg, 2);
    return res;
}

json PeerResult::to_json() const {
    json ls = json::array();
    for (auto& l : labels) {
        json corners = json::array();
        for (auto& c : l.corners) corners.push_back({round_to(c.x, 1), round_to(c.y, 1)});
        json fs = json::array();
        for (auto& f : l.findings)
            fs.push_back({{"label", f.label}, {"kind", f.kind}, {"bbox_px", box_json(f.bbox_px)},
                          {"bbox_mm", {f.x_mm, f.y_mm, f.w_mm, f.h_mm}}, {"area_mm2", f.area_mm2}, {"delta_l", f.delta_l},
                          {"detail", f.detail}});
        ls.push_back({{"index", l.index}, {"row", l.row}, {"col", l.col}, {"angle_deg", l.angle_deg}, {"corners", corners},
                      {"similarity", l.similarity}, {"registration_rms_px", l.registration_rms_px}, {"master", l.master},
                      {"findings", fs}});
    }
    return {{"dpi", dpi}, {"sheet_angle_deg", sheet_angle_deg}, {"label_size", {label_size.width, label_size.height}},
            {"labels", ls}, {"notes", notes}};
}

}  // namespace pc::compare
