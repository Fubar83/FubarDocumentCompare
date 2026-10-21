// Veck på arknivå: veckkandidater som återkommer på samma ställe i etiketterna (originalfel, inte
// veck) och svaga veck som bara syns som fynd på en linje över arket.
#include "printcmp/scan/sheet_folds.hpp"

#include <map>
#include <opencv2/imgproc.hpp>
#include <set>

namespace pc::scan {

namespace {

std::pair<double, double> line_params(Pt p0, Pt p1, Pt c) {
    double ang = std::fmod(std::atan2(p1.y - p0.y, p1.x - p0.x) * 180 / CV_PI + 360, 180);
    double a = ang * CV_PI / 180;
    return {ang, (p0.x - c.x) * -std::sin(a) + (p0.y - c.y) * std::cos(a)};
}

bool line_crosses(const Mat3& H, Pt mu, Pt e, double sppm, const cv::Mat& valid, double ppm, double margin_mm = 2.0) {
    Mat3 Hinv = H.inverse();
    int h = valid.rows, w = valid.cols;
    Pt a = Hinv.apply({(mu.x - 1e4 * e.x) * sppm, (mu.y - 1e4 * e.y) * sppm});
    Pt b = Hinv.apply({(mu.x + 1e4 * e.x) * sppm, (mu.y + 1e4 * e.y) * sppm});
    double dx = b.x - a.x, dy = b.y - a.y, n = std::hypot(dx, dy);
    if (n < 1) return false;
    dx /= n;
    dy /= n;
    double m = margin_mm * ppm;
    Pt c{w / 2.0, h / 2.0};
    double t0 = (c.x - a.x) * dx + (c.y - a.y) * dy;
    Pt foot{a.x + t0 * dx, a.y + t0 * dy};
    int nsamp = 4 * (w + h) / 10;
    int ok = 0, inside = 0;
    for (int i = 0; i < nsamp; ++i) {
        double t = -(w + h) + 2.0 * (w + h) * i / std::max(1, nsamp - 1);
        double x = foot.x + t * dx, y = foot.y + t * dy;
        if (x >= m && x < w - m && y >= m && y < h - m) {
            ++ok;
            inside += valid.at<uchar>((int)y, (int)x) > 0;
        }
    }
    return ok > 0 && (double)inside / ok > 0.5;
}

}  // namespace

void drop_systematic_fold_candidates(std::vector<SheetLabel>& labels, cv::Size ref_size, double ppm, double sppm) {
    struct Item { size_t si; scan::Fold f; double al, rl, as, rs; };
    std::vector<Item> items;
    Pt c_lab{ref_size.width / 2.0, ref_size.height / 2.0};
    for (size_t i = 0; i < labels.size(); ++i)
        for (auto& f : labels[i].folds) {
            auto [al, rl] = line_params(f.p0, f.p1, c_lab);
            Pt q0 = labels[i].H.apply(f.p0), q1 = labels[i].H.apply(f.p1);
            auto [as, rs] = line_params(q0, q1, {0, 0});
            items.push_back({i, f, al, rl, as, rs});
        }
    int need = std::max(3, (int)std::ceil(0.4 * labels.size()));
    std::set<size_t> drop;
    for (auto& it : items) {
        std::vector<size_t> same;
        for (size_t j = 0; j < items.size(); ++j) {
            double da = std::abs(items[j].al - it.al);
            da = std::min(da, 180 - da);
            if (da < 8 && std::abs(items[j].rl - it.rl) < 2.5 * ppm) same.push_back(j);
        }
        std::set<size_t> insts;
        for (size_t j : same) insts.insert(items[j].si);
        if ((int)insts.size() < need) continue;
        double lo = 1e18, hi = -1e18;
        for (size_t j : same) {
            double r = std::abs(items[j].as - it.as) > 90 ? -items[j].rs : items[j].rs;
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        if (hi - lo > 3.0 * sppm) drop.insert(same.begin(), same.end());
    }
    for (auto& s : labels) s.folds.clear();
    for (size_t k = 0; k < items.size(); ++k)
        if (!drop.count(k)) labels[items[k].si].folds.push_back(items[k].f);
}

std::vector<json> collinear_folds(std::vector<json> diffs, const std::vector<SheetLabel>& labels, const cv::Mat& valid, double ppm,
                                  double sppm, json& inst_out, double tol_mm, int min_inst, double band_mm) {
    std::map<int, const SheetLabel*> by_idx;
    for (auto& s : labels) by_idx[s.index] = &s;
    struct C { size_t i; int inst; Pt sheet, ref; bool has_axis; Pt axis; };
    std::vector<C> cand;
    for (size_t i = 0; i < diffs.size(); ++i) {
        const json& d = diffs[i];
        std::string k = d.value("kind", "");
        if (k == "Uncertain" || k == "Barcode" || !d.contains("bbox_mm") || d["bbox_mm"].is_null() || d.value("category", "") == "Braille")
            continue;
        if (d.value("area_mm2", 0.0) > 4.0) continue;
        auto it = by_idx.find(d.value("instance", -1));
        if (it == by_idx.end()) continue;
        double x = d["bbox_px"][0], y = d["bbox_px"][1], w = d["bbox_px"][2], h = d["bbox_px"][3];
        Pt cr{x + w / 2, y + h / 2};
        Pt q = it->second->H.apply(cr);
        C c{i, it->first, {q.x / sppm, q.y / sppm}, {cr.x / ppm, cr.y / ppm}, false, {0, 0}};
        if (std::max(w, h) > 2.5 * std::max(1.0, std::min(w, h))) {
            Pt v = w > h ? Pt{1, 0} : Pt{0, 1};
            const Mat3& H = it->second->H;
            Pt a{H(0, 0) * v.x + H(0, 1) * v.y, H(1, 0) * v.x + H(1, 1) * v.y};
            double n = std::hypot(a.x, a.y);
            c.has_axis = true;
            c.axis = {a.x / n, a.y / n};
        }
        cand.push_back(c);
    }
    int n_inst = (int)labels.size();
    std::vector<C> keep;
    for (auto& c : cand) {
        std::set<int> others;
        for (auto& o : cand)
            if (o.inst != c.inst && std::hypot(o.ref.x - c.ref.x, o.ref.y - c.ref.y) < 1.0) others.insert(o.inst);
        if ((int)others.size() < std::max(1, (n_inst - 1) / 2)) keep.push_back(c);
    }
    cand = keep;
    if ((int)cand.size() < min_inst) return diffs;
    std::vector<size_t> order(cand.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cand[a].inst < cand[b].inst; });
    std::set<size_t> used;
    std::vector<std::pair<Pt, Pt>> found;
    for (size_t ai : order)
        for (size_t bi : order) {
            if (bi <= ai || cand[ai].inst == cand[bi].inst || used.count(ai) || used.count(bi)) continue;
            Pt p = cand[ai].sheet, r = cand[bi].sheet;
            double dx = r.x - p.x, dy = r.y - p.y, n = std::hypot(dx, dy);
            if (n < 10) continue;
            dx /= n;
            dy /= n;
            std::vector<size_t> inl;
            for (size_t k = 0; k < cand.size(); ++k) {
                if (used.count(k)) continue;
                double dist = std::abs((cand[k].sheet.x - p.x) * -dy + (cand[k].sheet.y - p.y) * dx);
                if (dist >= tol_mm) continue;
                if (cand[k].has_axis && std::abs(cand[k].axis.x * dx + cand[k].axis.y * dy) <= 0.94) continue;
                inl.push_back(k);
            }
            std::set<int> insts;
            for (size_t k : inl) insts.insert(cand[k].inst);
            if ((int)insts.size() < min_inst) continue;
            int rep = 0;
            for (size_t k : inl) {
                std::set<int> others;
                for (size_t j : inl)
                    if (cand[j].inst != cand[k].inst && std::hypot(cand[j].ref.x - cand[k].ref.x, cand[j].ref.y - cand[k].ref.y) < 1.0)
                        others.insert(cand[j].inst);
                rep += (int)others.size() >= std::max(1, ((int)insts.size() - 1) / 2);
            }
            if (rep > 0.25 * inl.size()) continue;
            std::vector<cv::Point2d> P;
            for (size_t k : inl) P.push_back({cand[k].sheet.x, cand[k].sheet.y});
            cv::PCA pca(cv::Mat(P).reshape(1), cv::Mat(), cv::PCA::DATA_AS_ROW);
            Pt mu{pca.mean.at<double>(0), pca.mean.at<double>(1)}, e{pca.eigenvectors.at<double>(0, 0), pca.eigenvectors.at<double>(0, 1)};
            int crossed = 0;
            for (auto& s : labels) crossed += line_crosses(s.H, mu, e, sppm, valid, ppm);
            if (insts.size() < 0.5 * crossed) continue;
            found.push_back({mu, e});
            used.insert(inl.begin(), inl.end());
        }
    if (found.empty()) return diffs;
    std::set<size_t> drop;
    std::vector<json> zones;
    for (auto& [mu, e] : found)
        for (auto& s : labels) {
            Mat3 Hinv = s.H.inverse();
            Pt a = Hinv.apply({(mu.x - 1e4 * e.x) * sppm, (mu.y - 1e4 * e.y) * sppm});
            Pt b = Hinv.apply({(mu.x + 1e4 * e.x) * sppm, (mu.y + 1e4 * e.y) * sppm});
            scan::Fold f{a, b, band_mm * ppm};
            cv::Mat zone = cv::Mat::zeros(valid.size(), CV_8U);
            scan::draw_extended_band(zone, f);
            zone &= valid;
            if (!cv::countNonZero(zone)) continue;
            int idx = s.index;
            for (size_t i = 0; i < diffs.size(); ++i) {
                const json& d = diffs[i];
                std::string k = d.value("kind", "");
                if (d.value("instance", -1) != idx || k == "Uncertain" || k == "Barcode" || drop.count(i)) continue;
                if (d.value("area_mm2", 0.0) > 4.0) continue;
                int cx = std::min(valid.cols - 1, (int)(d["bbox_px"][0].get<double>() + d["bbox_px"][2].get<double>() / 2));
                int cy = std::min(valid.rows - 1, (int)(d["bbox_px"][1].get<double>() + d["bbox_px"][3].get<double>() / 2));
                if (zone.at<uchar>(cy, cx)) drop.insert(i);
            }
            std::vector<cv::Point> pts;
            cv::findNonZero(zone, pts);
            cv::Rect r = cv::boundingRect(pts);
            json z = scan::uncertain_zone(idx, r, ppm, "fold");
            z["area_mm2"] = pts.size() / (ppm * ppm);
            z["line_mm"] = {{f.p0.x / ppm, f.p0.y / ppm}, {f.p1.x / ppm, f.p1.y / ppm}};
            z["reason"] = "collinear_findings";
            zones.push_back(z);
            for (auto& io : inst_out)
                if (io["index"] == idx)
                    io["folds"].push_back({{"p0_mm", {f.p0.x / ppm, f.p0.y / ppm}}, {"p1_mm", {f.p1.x / ppm, f.p1.y / ppm}}, {"band_mm", band_mm}});
        }
    std::vector<json> out;
    for (size_t i = 0; i < diffs.size(); ++i)
        if (!drop.count(i)) out.push_back(diffs[i]);
    out.insert(out.end(), zones.begin(), zones.end());
    return out;
}


}  // namespace pc::scan
