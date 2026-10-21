// Punktskrift: läsning ur PDF, cellnät, svensk grundnivå och kontroll.

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "printcmp/artwork/braille.hpp"
#include "printcmp/artwork/masks.hpp"
#include "printcmp/braille/braille.hpp"
#include "printcmp/pdf/text.hpp"
#include "printcmp/core/text.hpp"

namespace pc::artwork {

using braille::cells_to_text;
using braille::unicode_of;

namespace {

json diff_for_cell(const BrailleLine& ln, int k, const Cell& read, const Cell& missing, const Cell& extra, double ppm,
                   int instance) {
    Cell exp = ln.cells.count(k) ? ln.cells.at(k) : Cell{};
    Box b = ln.cell_box(k);
    return {{"instance", instance}, {"kind", "Braille"}, {"severity", "Critical"}, {"category", "Braille"},
            {"bbox_px", {(int)b.x0, (int)b.y0, (int)b.w(), (int)b.h()}},
            {"bbox_mm", {b.x0 / ppm, b.y0 / ppm, b.w() / ppm, b.h() / ppm}},
            {"area_mm2", b.w() * b.h() / (ppm * ppm)}, {"objects", json::array()},
            {"metrics", {{"delta_l", nullptr}, {"delta_e", nullptr}, {"shift_mm", nullptr}}}, {"confidence", 0.85},
            {"braille", {{"line", ln.index}, {"cell", k - ln.kmin},
                         {"expected", exp.empty() ? " " : cells_to_text({exp})},
                         {"read", read.empty() ? " " : cells_to_text({read})},
                         {"expected_unicode", unicode_of(exp)}, {"read_unicode", unicode_of(read)},
                         {"missing_dots", std::vector<int>(missing.begin(), missing.end())},
                         {"extra_dots", std::vector<int>(extra.begin(), extra.end())}}}};
}

std::vector<double> cluster_1d(std::vector<double> vals, double tol) {
    std::sort(vals.begin(), vals.end());
    std::vector<double> out, cur;
    for (double v : vals) {
        if (!cur.empty() && v - cur.back() > tol) {
            out.push_back(mean(cur));
            cur.clear();
        }
        cur.push_back(v);
    }
    if (!cur.empty()) out.push_back(mean(cur));
    return out;
}

}  // namespace


Pt BrailleLine::pos(int k, int n) const {
    int col = n <= 3 ? 0 : 1, row = (n - 1) % 3;
    return {origin + k * cell_pitch + col * pitch, top + row * pitch};
}

Box BrailleLine::cell_box(int k) const {
    double r = dot_r * 1.2;
    return {origin + k * cell_pitch - r, top - r, origin + k * cell_pitch + pitch + r, top + 2 * pitch + r};
}

Box BrailleLine::bbox() const {
    Box a = cell_box(kmin), b = cell_box(kmax);
    return {a.x0, a.y0, b.x1, b.y1};
}

std::vector<Cell> BrailleLine::cell_list() const {
    std::vector<Cell> v;
    for (int k = kmin; k <= kmax; ++k) v.push_back(cells.count(k) ? cells.at(k) : Cell{});
    return v;
}

std::pair<std::vector<Pt>, double> braille_dots_from_spots(const pdf::SpotAnalysis& spots, const pdf::Geometry& geom) {
    std::vector<Pt> centers;
    std::vector<double> radii;
    for (const auto& poly : spots.polygons("braille")) {
        if (poly.size() < 4) continue;
        double cx = 0, cy = 0;
        std::vector<Pt> px;
        for (const auto& p : poly) {
            Pt q = geom.to_px(p.x, p.y);
            px.push_back(q);
            cx += q.x;
            cy += q.y;
        }
        cx /= px.size();
        cy /= px.size();
        double r = 0;
        for (const auto& q : px) r += std::hypot(q.x - cx, q.y - cy);
        r /= px.size();
        if (r <= 0) continue;
        centers.push_back({cx, cy});
        radii.push_back(r);
    }
    return {centers, radii.empty() ? 0.0 : median(radii)};
}

std::pair<std::vector<Pt>, double> braille_dots_from_text(const pdf::Bytes& data, int page_index,
                                                          const pdf::Geometry& geom) {
    auto d = pdf::Document::open(data);
    std::vector<Pt> centers;
    std::vector<double> sizes;
    for (const auto& c : pdf::text_chars(*d, page_index)) {
        unsigned u = unsigned(c.code);
        if (u < 0x2800 || u > 0x28FF || !c.has_loose) continue;
        Pt p0 = geom.to_px(c.loose_l, c.loose_t), p1 = geom.to_px(c.loose_r, c.loose_b);
        double w = p1.x - p0.x, h = p1.y - p0.y;
        unsigned v = u - 0x2800;
        for (int nn = 1; nn <= 6; ++nn) {
            if (!(v & (1u << (nn - 1)))) continue;
            int col = nn <= 3 ? 0 : 1, row = (nn - 1) % 3;
            centers.push_back({p0.x + w * (0.3 + 0.4 * col), p0.y + h * (0.25 + 0.25 * row)});
        }
        sizes.push_back(w);
    }
    return {centers, sizes.empty() ? 0.0 : 0.08 * median(sizes)};
}

std::vector<BrailleLine> braille_decode(const std::vector<Pt>& pts, double dot_r, double ppm, const std::string& source) {
    if (pts.size() < 2) return {};
    std::vector<double> nn(pts.size(), 1e18);
    for (size_t i = 0; i < pts.size(); ++i)
        for (size_t j = 0; j < pts.size(); ++j)
            if (i != j) nn[i] = std::min(nn[i], std::hypot(pts[i].x - pts[j].x, pts[i].y - pts[j].y));
    std::vector<double> close;
    for (double v : nn)
        if (v < 4.0 * ppm) close.push_back(v);
    double p = close.empty() ? 2.5 * ppm : median(close);
    std::vector<double> ys;
    for (auto& q : pts) ys.push_back(q.y);
    auto rows = cluster_1d(ys, p / 3);
    std::vector<bool> used(rows.size(), false);
    std::vector<BrailleLine> lines;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (used[i]) continue;
        double top = rows[i];
        for (size_t j = 0; j < rows.size(); ++j)
            if (!used[j] && top - p / 3 <= rows[j] && rows[j] <= top + 2 * p + p / 3) used[j] = true;
        std::vector<Pt> lpts;
        for (auto& q : pts)
            if (q.y >= top - p / 3 && q.y <= top + 2 * p + p / 3) lpts.push_back(q);
        std::vector<double> xs;
        for (auto& q : lpts) xs.push_back(q.x);
        auto cols = cluster_1d(xs, p / 3);
        std::vector<double> cands;
        if (cols.size() >= 5)
            for (int k = 0; k <= 70; ++k) cands.push_back((2.1 + 0.01 * k) * p);
        else cands.push_back(2.4 * p);
        bool have = false;
        double best_cost = 0, best_c = 0, best_o = 0;
        std::vector<double> origins = cols;
        for (double c0 : cols) origins.push_back(c0 - p);
        for (double c : cands)
            for (double o0 : origins) {
                double cost = 0, kmin = 1e18;
                for (double cx : cols) {
                    double rel = cx - o0, k = std::nearbyint(rel / c), off = rel - k * c;
                    double res = std::min(std::abs(off), std::abs(off - p));
                    cost += res * res;
                    kmin = std::min(kmin, k);
                }
                if (kmin < 0) cost += 1e9;
                if (!have || cost < best_cost - 1e-6) {
                    have = true;
                    best_cost = cost;
                    best_c = c;
                    best_o = o0;
                }
            }
        BrailleLine ln;
        ln.index = (int)lines.size();
        ln.top = top;
        ln.origin = best_o;
        ln.pitch = p;
        ln.cell_pitch = best_c;
        ln.dot_r = dot_r;
        ln.source = source;
        for (auto& q : lpts) {
            int k = (int)pyround((q.x - best_o) / best_c);
            int col = std::abs((q.x - best_o) - k * best_c) < std::abs((q.x - best_o) - k * best_c - p) ? 0 : 1;
            int row = std::clamp((int)pyround((q.y - top) / p), 0, 2);
            ln.cells[k].insert(row + 1 + 3 * col);
        }
        ln.kmin = ln.cells.begin()->first;
        ln.kmax = ln.cells.rbegin()->first;
        lines.push_back(ln);
    }
    return lines;
}

std::vector<BrailleLine> braille_from_pdf(const pdf::SpotAnalysis& spots, const pdf::Bytes& data, int page,
                                          const pdf::Geometry& geom) {
    double ppm = geom.ppm();
    auto [dots, r] = braille_dots_from_spots(spots, geom);
    std::string src = "spot";
    if (dots.empty()) {
        std::tie(dots, r) = braille_dots_from_text(data, page, geom);
        src = "text";
    }
    if (dots.empty()) return {};
    return braille_decode(dots, r > 0 ? r : 0.75 * ppm, ppm, src);
}

cv::Mat braille_mask(cv::Size size, const std::vector<BrailleLine>& lines, double ppm, double margin_mm) {
    cv::Mat m = cv::Mat::zeros(size, CV_8U);
    for (const auto& ln : lines) {
        int rad = (int)std::lround(ln.dot_r + margin_mm * ppm);
        for (int k = ln.kmin; k <= ln.kmax; ++k)
            for (int n = 1; n <= 6; ++n) {
                Pt p = ln.pos(k, n);
                cv::circle(m, cv::Point((int)std::lround(p.x), (int)std::lround(p.y)), rad, 1, -1);
            }
    }
    return m;
}

BrailleScanResult braille_check_scan(const cv::Mat& resid_L, const std::vector<BrailleLine>& lines, double ppm,
                                     int instance, const cv::Mat& fold_band, double min_ratio, double min_level) {
    BrailleScanResult res;
    int h = resid_L.rows, w = resid_L.cols;
    cv::Mat wide;
    if (!fold_band.empty() && cv::countNonZero(fold_band)) {
        int k = (int)std::lround(1.5 * ppm);
        cv::dilate(fold_band, wide, cv::Mat::ones(2 * k + 1, 2 * k + 1, CV_8U));
    }
    for (const auto& ln : lines) {
        double r = ln.dot_r;
        Box bb = ln.bbox();
        int pad = (int)(3 * ln.pitch);
        int X0 = std::max(0, (int)bb.x0 - pad), Y0 = std::max(0, (int)bb.y0 - pad);
        int X1 = std::min(w, (int)bb.x1 + pad), Y1 = std::min(h, (int)bb.y1 + pad);
        if (X1 <= X0 || Y1 <= Y0) {
            res.reads.push_back(std::nullopt);
            continue;
        }
        cv::Mat sub;
        resid_L(cv::Rect(X0, Y0, X1 - X0, Y1 - Y0)).convertTo(sub, CV_32F);
        cv::Mat lp, hp, energy;
        cv::GaussianBlur(sub, lp, cv::Size(), 2.5 * r);
        hp = sub - lp;
        cv::GaussianBlur(cv::abs(hp), energy, cv::Size(), 0.6 * r);
        auto score = [&](double x, double y) -> double {
            int xi = (int)std::lround(x) - X0, yi = (int)std::lround(y) - Y0;
            if (0 <= xi && xi < energy.cols && 0 <= yi && yi < energy.rows) return energy.at<float>(yi, xi);
            return std::nan("");
        };
        auto peak_ratio = [&](double x, double y) {
            std::vector<double> ring;
            for (int a = 0; a < 8; ++a) {
                double ang = 2 * CV_PI * a / 8;
                double v = score(x + 1.5 * r * std::cos(ang), y + 1.5 * r * std::sin(ang));
                if (!std::isnan(v)) ring.push_back(v);
            }
            double c = score(x, y);
            return ring.empty() ? 0.0 : c / std::max(median(ring), 1e-6);
        };
        struct Pos { int k, n; double s; bool exp; };
        std::vector<Pos> positions;
        std::vector<double> pres, absn;
        std::set<int> unsure;
        for (int k = ln.kmin; k <= ln.kmax; ++k) {
            Cell exp = ln.cells.count(k) ? ln.cells.at(k) : Cell{};
            for (int n = 1; n <= 6; ++n) {
                Pt p = ln.pos(k, n);
                if (!wide.empty() && 0 <= (int)p.y && (int)p.y < h && 0 <= (int)p.x && (int)p.x < w &&
                    wide.at<uchar>((int)p.y, (int)p.x)) {
                    unsure.insert(k);
                    continue;
                }
                double s = score(p.x, p.y);
                if (std::isnan(s)) continue;
                bool e = exp.count(n) > 0;
                positions.push_back({k, n, s, e});
                (e ? pres : absn).push_back(s);
            }
        }
        // osäkra celler (veck) som sammanhängande zoner
        std::vector<std::vector<int>> runs;
        for (int k : unsure) {
            if (!runs.empty() && k == runs.back().back() + 1) runs.back().push_back(k);
            else runs.push_back({k});
        }
        for (const auto& run : runs) {
            Box a = ln.cell_box(run.front()), b = ln.cell_box(run.back());
            std::vector<int> cl;
            for (int k : run) cl.push_back(k - ln.kmin);
            res.diffs.push_back({{"instance", instance}, {"kind", "Uncertain"}, {"severity", "Info"}, {"category", "Braille"},
                                 {"cause", "fold"}, {"bbox_px", {(int)a.x0, (int)a.y0, (int)(b.x1 - a.x0), (int)(b.y1 - a.y0)}},
                                 {"bbox_mm", {a.x0 / ppm, a.y0 / ppm, (b.x1 - a.x0) / ppm, (b.y1 - a.y0) / ppm}},
                                 {"area_mm2", (b.x1 - a.x0) * (b.y1 - a.y0) / (ppm * ppm)}, {"objects", json::array()},
                                 {"metrics", {{"delta_l", nullptr}, {"delta_e", nullptr}, {"shift_mm", nullptr}}},
                                 {"confidence", 0.5}, {"braille", {{"line", ln.index}, {"cells", cl}}}});
        }
        if (pres.empty()) {
            res.reads.push_back(std::nullopt);
            continue;
        }
        double P = median(pres), A = absn.empty() ? 0.25 * P : median(absn);
        res.stats.push_back({{"line", ln.index}, {"present", round_to(P, 2)}, {"absent", round_to(A, 2)}});
        if (P < min_ratio * A || P < min_level) {
            Box b = ln.bbox();
            res.diffs.push_back({{"instance", instance}, {"kind", "Braille"}, {"severity", "Critical"}, {"category", "Braille"},
                                 {"cause", "embossing_missing"}, {"bbox_px", {(int)b.x0, (int)b.y0, (int)b.w(), (int)b.h()}},
                                 {"bbox_mm", {b.x0 / ppm, b.y0 / ppm, b.w() / ppm, b.h() / ppm}},
                                 {"area_mm2", b.w() * b.h() / (ppm * ppm)}, {"objects", json::array()},
                                 {"metrics", {{"delta_l", nullptr}, {"delta_e", nullptr}, {"shift_mm", nullptr}}},
                                 {"confidence", 0.7},
                                 {"braille", {{"line", ln.index}, {"expected_text", ln.text()}, {"read_text", nullptr}}}});
            res.reads.push_back(std::nullopt);
            continue;
        }
        double t_miss = A + 0.40 * (P - A), t_extra = A + 0.60 * (P - A);
        std::map<int, Cell> observed, missing, extra;
        for (const auto& q : positions) {
            Pt p = ln.pos(q.k, q.n);
            bool is_extra = !q.exp && q.s > t_extra && peak_ratio(p.x, p.y) > 1.4;
            if ((q.exp && q.s >= t_miss) || is_extra) observed[q.k].insert(q.n);
            if (q.exp && q.s < t_miss) missing[q.k].insert(q.n);
            if (is_extra) extra[q.k].insert(q.n);
        }
        std::string read;
        for (int k = ln.kmin; k <= ln.kmax; ++k)
            read += unsure.count(k) ? "~" : cells_to_text({observed.count(k) ? observed[k] : Cell{}});
        res.reads.push_back(read);
        std::set<int> ks;
        for (auto& [k, v] : missing) ks.insert(k);
        for (auto& [k, v] : extra) ks.insert(k);
        for (int k : ks) {
            if (unsure.count(k)) continue;
            json dc = diff_for_cell(ln, k, observed.count(k) ? observed[k] : Cell{}, missing.count(k) ? missing[k] : Cell{},
                                    extra.count(k) ? extra[k] : Cell{}, ppm, instance);
            std::vector<double> marg;
            for (const auto& q : positions) {
                if (q.k != k) continue;
                if (q.exp && q.s < t_miss) marg.push_back(((t_miss - q.s) / std::max(P - A, 1e-6)) / 0.4);
                if (!q.exp && extra.count(k) && extra[k].count(q.n))
                    marg.push_back(((q.s - t_extra) / std::max(P - A, 1e-6)) / 0.4);
            }
            dc["braille"]["contrast"] = round_to(P / std::max(A, 1e-6), 2);
            dc["braille"]["margin"] = round_to(marg.empty() ? 0.0 : *std::min_element(marg.begin(), marg.end()), 2);
            res.diffs.push_back(dc);
        }
    }
    return res;
}

std::pair<std::vector<json>, std::vector<std::string>> braille_compare_dots(const std::vector<BrailleLine>& lines,
                                                                            const std::vector<Pt>& sd, double ppm,
                                                                            int instance) {
    std::vector<json> diffs;
    std::vector<std::string> reads;
    std::vector<bool> used(sd.size(), false);
    for (const auto& ln : lines) {
        double tol = 0.35 * ln.pitch;
        std::map<int, Cell> observed;
        for (int k = ln.kmin - 1; k <= ln.kmax + 1; ++k)
            for (int n = 1; n <= 6; ++n) {
                Pt p = ln.pos(k, n);
                if (sd.empty()) continue;
                size_t j = 0;
                double best = 1e18;
                for (size_t q = 0; q < sd.size(); ++q) {
                    double dd = std::hypot(sd[q].x - p.x, sd[q].y - p.y);
                    if (dd < best) {
                        best = dd;
                        j = q;
                    }
                }
                if (best <= tol) {
                    observed[k].insert(n);
                    used[j] = true;
                }
            }
        int k0 = ln.kmin, k1 = ln.kmax;
        for (auto& [k, v] : observed) {
            k0 = std::min(k0, k);
            k1 = std::max(k1, k);
        }
        std::vector<Cell> rc;
        for (int k = k0; k <= k1; ++k) rc.push_back(observed.count(k) ? observed[k] : Cell{});
        reads.push_back(cells_to_text(rc));
        for (int k = k0; k <= k1; ++k) {
            Cell exp = ln.cells.count(k) ? ln.cells.at(k) : Cell{};
            Cell got = observed.count(k) ? observed[k] : Cell{};
            if (got != exp) {
                Cell miss, ext;
                std::set_difference(exp.begin(), exp.end(), got.begin(), got.end(), std::inserter(miss, miss.end()));
                std::set_difference(got.begin(), got.end(), exp.begin(), exp.end(), std::inserter(ext, ext.end()));
                diffs.push_back(diff_for_cell(ln, k, got, miss, ext, ppm, instance));
            }
        }
    }
    for (size_t j = 0; j < sd.size(); ++j) {
        if (used[j]) continue;
        double x = sd[j].x, y = sd[j].y, r = lines.empty() ? 0.75 * ppm : lines[0].dot_r;
        diffs.push_back({{"instance", instance}, {"kind", "Braille"}, {"severity", "Critical"}, {"category", "Braille"},
                         {"cause", "extra_dot_outside_cells"},
                         {"bbox_px", {(int)(x - 2 * r), (int)(y - 2 * r), (int)(4 * r), (int)(4 * r)}},
                         {"bbox_mm", {(x - 2 * r) / ppm, (y - 2 * r) / ppm, 4 * r / ppm, 4 * r / ppm}},
                         {"area_mm2", 16 * r * r / (ppm * ppm)}, {"objects", json::array()},
                         {"metrics", {{"delta_l", nullptr}, {"delta_e", nullptr}, {"shift_mm", nullptr}}},
                         {"confidence", 0.9}});
    }
    return {diffs, reads};
}

// ---------------------------------------------------------------- masker

Masks build_masks(cv::Size size, const pdf::Geometry& geom, const pdf::SpotAnalysis& spots,
                  const std::vector<UserMask>& user, double erode_mm, const cv::Mat& bmask) {
    int h = size.height, w = size.width;
    double ppm = geom.ppm();
    Masks M;
    cv::Mat inside = cv::Mat::zeros(size, CV_8U);
    auto polys = spots.polygons("dieline");
    std::vector<std::vector<Pt>> closed;
    for (auto& p : polys)
        if (p.size() >= 3) closed.push_back(p);
    std::stable_sort(closed.begin(), closed.end(),
                     [](const auto& a, const auto& b) { return pdf::polygon_area(a) > pdf::polygon_area(b); });
    M.has_dieline = !closed.empty();
    auto to_px = [&](const std::vector<Pt>& poly) {
        std::vector<cv::Point> out;
        for (auto& p : poly) {
            Pt q = geom.to_px(p.x, p.y);
            out.emplace_back((int)std::lround(q.x * 16), (int)std::lround(q.y * 16));
        }
        return out;
    };
    if (M.has_dieline) {
        std::vector<std::vector<cv::Point>> outer{to_px(closed[0])};
        cv::fillPoly(inside, outer, 1, cv::LINE_8, 4);
        for (size_t i = 1; i < closed.size(); ++i)
            if (pdf::polygon_area(closed[i]) > 0.02 * pdf::polygon_area(closed[0])) {
                std::vector<std::vector<cv::Point>> hole{to_px(closed[i])};
                cv::fillPoly(inside, hole, 0, cv::LINE_8, 4);
            }
    } else {
        inside.setTo(1);
    }
    M.inside = inside.clone();
    int er = std::max(1, (int)std::lround(erode_mm * ppm));
    cv::erode(inside, M.valid, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * er + 1, 2 * er + 1)),
              cv::Point(-1, -1), 1, cv::BORDER_CONSTANT, 0);
    if (!M.has_dieline) {
        M.valid.rowRange(0, std::min(er, h)).setTo(0);
        M.valid.rowRange(std::max(0, h - er), h).setTo(0);
        M.valid.colRange(0, std::min(er, w)).setTo(0);
        M.valid.colRange(std::max(0, w - er), w).setTo(0);
    }
    cv::Mat variable = cv::Mat::zeros(size, CV_8U);
    for (const auto& m : user) {
        int x0 = (int)std::floor(m.x * ppm), y0 = (int)std::floor(m.y * ppm);
        int x1 = (int)std::ceil((m.x + m.w) * ppm), y1 = (int)std::ceil((m.y + m.h) * ppm);
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(w, x1);
        y1 = std::min(h, y1);
        if (x1 > x0 && y1 > y0) variable(cv::Rect(x0, y0, x1 - x0, y1 - y0)).setTo(1);
    }
    int vm = std::max(1, (int)std::lround(0.5 * ppm));
    cv::dilate(variable, M.variable, cv::Mat::ones(2 * vm + 1, 2 * vm + 1, CV_8U));
    M.valid.setTo(0, M.variable);
    M.varnish = cv::Mat::zeros(size, CV_8U);
    for (auto& p : spots.polygons("varnish"))
        if (p.size() >= 3) {
            std::vector<std::vector<cv::Point>> pp{to_px(p)};
            cv::fillPoly(M.varnish, pp, 1, cv::LINE_8, 4);
        }
    if (!bmask.empty()) {
        M.valid.setTo(0, bmask);
        M.braille = bmask;
    }
    return M;
}

}  // namespace pc::artwork
