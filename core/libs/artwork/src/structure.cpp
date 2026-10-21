// Tabeller ur PDF-geometrin.
#include <algorithm>
#include <cmath>
#include <set>

#include "printcmp/artwork/objmap.hpp"
#include "printcmp/artwork/structure.hpp"
#include "printcmp/core/text.hpp"

namespace pc::artwork {

namespace {

std::vector<double> cluster(std::vector<double> vals, double tol) {
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

using Line = std::vector<const Obj*>;

std::vector<Line> text_lines(std::vector<const Obj*> words) {
    std::stable_sort(words.begin(), words.end(), [](auto a, auto b) { return a->bbox.cy() < b->bbox.cy(); });
    std::vector<Line> lines;
    for (auto w : words) {
        double cy = w->bbox.cy(), h = w->bbox.h();
        bool placed = false;
        size_t start = lines.size() > 3 ? lines.size() - 3 : 0;
        for (size_t k = start; k < lines.size(); ++k) {
            std::vector<double> cys, hs;
            for (auto o : lines[k]) {
                cys.push_back(o->bbox.cy());
                hs.push_back(o->bbox.h());
            }
            if (std::abs(cy - median(cys)) <= 0.35 * std::max(h, median(hs))) {
                lines[k].push_back(w);
                placed = true;
                break;
            }
        }
        if (!placed) lines.push_back({w});
    }
    for (auto& ln : lines) std::stable_sort(ln.begin(), ln.end(), [](auto a, auto b) { return a->bbox.x0 < b->bbox.x0; });
    auto med = [](const Line& ln) {
        std::vector<double> c;
        for (auto o : ln) c.push_back(o->bbox.cy());
        return median(c);
    };
    std::stable_sort(lines.begin(), lines.end(), [&](const Line& a, const Line& b) { return med(a) < med(b); });
    return lines;
}

std::vector<std::pair<double, double>> separators(const std::vector<Line>& rows, double x0, double x1, double gap_min) {
    int n = (int)std::ceil(x1 - x0) + 1;
    if (n <= 2) return {};
    std::vector<bool> occ(n, false);
    for (const auto& ln : rows)
        for (auto w : ln) {
            int a = (int)std::max(0.0, std::floor(w->bbox.x0 - x0));
            int b = (int)std::min<double>(n, std::ceil(w->bbox.x1 - x0));
            for (int k = a; k < b; ++k) occ[k] = true;
        }
    int lo = -1, hi = -1;
    for (int k = 0; k < n; ++k)
        if (occ[k]) {
            if (lo < 0) lo = k;
            hi = k;
        }
    if (lo < 0) return {};
    std::vector<std::pair<double, double>> seps;
    int i = lo;
    while (i <= hi) {
        if (!occ[i]) {
            int j = i;
            while (j <= hi && !occ[j]) ++j;
            if (j - i >= gap_min) seps.push_back({x0 + i, x0 + j});
            i = j;
        } else {
            ++i;
        }
    }
    return seps;
}

bool both_sides(const Line& ln, std::pair<double, double> sep) {
    bool l = false, r = false;
    for (auto w : ln) {
        l |= w->bbox.x1 <= sep.first + 1;
        r |= w->bbox.x0 >= sep.second - 1;
    }
    return l && r;
}

int searchsorted(const std::vector<double>& v, double x) {   // numpy side='left'
    return (int)(std::lower_bound(v.begin(), v.end(), x) - v.begin());
}

void fill_cells(Table& t, const std::vector<const Obj*>& words) {
    for (auto w : words) {
        double cx = w->bbox.cx(), cy = w->bbox.cy();
        if (!(t.bbox.x0 <= cx && cx <= t.bbox.x1 && t.bbox.y0 <= cy && cy <= t.bbox.y1)) continue;
        int r = std::clamp(searchsorted(t.row_bounds, cy) - 1, 0, t.n_rows() - 1);
        int c = std::clamp(searchsorted(t.col_bounds, cx) - 1, 0, t.n_cols() - 1);
        auto& cell = t.cells[{r, c}];
        cell = text::strip(cell + " " + w->text);
    }
}

int argmin_abs(const std::vector<double>& v, double x) {
    int best = 0;
    for (int i = 1; i < (int)v.size(); ++i)
        if (std::abs(x - v[i]) < std::abs(x - v[best])) best = i;
    return best;
}

}  // namespace

json Table::to_json(double ppm) const {
    json cells_j = json::array();
    for (int r = 0; r < n_rows(); ++r) {
        json row = json::array();
        for (int c = 0; c < n_cols(); ++c) {
            auto it = cells.find({r, c});
            row.push_back(it == cells.end() ? "" : it->second);
        }
        cells_j.push_back(row);
    }
    return {{"id", id}, {"kind", kind},
            {"bbox_mm", {round_to(bbox.x0 / ppm, 2), round_to(bbox.y0 / ppm, 2), round_to(bbox.w() / ppm, 2), round_to(bbox.h() / ppm, 2)}},
            {"rows", n_rows()}, {"cols", n_cols()}, {"rules", rules.size()}, {"cells", cells_j}};
}

std::vector<Table> detect_tables(const std::vector<Obj>& objs, double ppm) {
    std::vector<const Obj*> words, hr, vr;
    for (const auto& o : objs) {
        if (o.kind == "Text") words.push_back(&o);
        if (o.kind == "Rule") (o.bbox.w() >= o.bbox.h() ? hr : vr).push_back(&o);
    }
    std::vector<Table> tables;
    std::set<size_t> used_h;
    std::vector<size_t> order(hr.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return hr[a]->bbox.cy() < hr[b]->bbox.cy(); });

    for (size_t i : order) {
        if (used_h.count(i)) continue;
        const Box& a = hr[i]->bbox;
        std::vector<size_t> stack{i};
        for (size_t j : order) {
            if (used_h.count(j) || j == i) continue;
            const Box& b = hr[j]->bbox;
            double ov = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
            if (ov >= 0.6 * std::min(a.w(), b.w())) stack.push_back(j);
        }
        std::vector<double> cys;
        for (size_t k : stack) cys.push_back(hr[k]->bbox.cy());
        auto ys = cluster(cys, 0.4 * ppm);
        std::vector<std::vector<double>> groups{{ys[0]}};
        for (size_t k = 1; k < ys.size(); ++k) {
            if (ys[k] - groups.back().back() > 15 * ppm) groups.push_back({ys[k]});
            else groups.back().push_back(ys[k]);
        }
        for (const auto& g : groups) {
            std::vector<size_t> members;
            for (size_t k : stack) {
                double y = hr[k]->bbox.cy();
                if (g.front() - 0.5 * ppm <= y && y <= g.back() + 0.5 * ppm) members.push_back(k);
            }
            if (members.empty()) continue;
            std::vector<double> mx0, mx1;
            for (size_t k : members) {
                mx0.push_back(hr[k]->bbox.x0);
                mx1.push_back(hr[k]->bbox.x1);
            }
            double x0 = median(mx0), x1 = median(mx1), y0 = g.front(), y1 = g.back();
            std::vector<const Obj*> vin;
            for (auto v : vr)
                if (x0 - ppm <= v->bbox.cx() && v->bbox.cx() <= x1 + ppm &&
                    std::min(v->bbox.y1, y1) - std::max(v->bbox.y0, y0) >= 0.5 * (y1 - y0))
                    vin.push_back(v);
            std::vector<double> vcx;
            for (auto v : vin) vcx.push_back(v->bbox.cx());
            auto vxs = cluster(vcx, 0.4 * ppm);
            std::vector<double> inner_v;
            for (double x : vxs)
                if (x0 + 0.5 * ppm < x && x < x1 - 0.5 * ppm) inner_v.push_back(x);
            if (g.size() < 3 && !(g.size() >= 2 && vxs.size() >= 2)) continue;
            std::vector<double> row_bounds = g, col_bounds;
            std::string kind;
            if (!inner_v.empty()) {
                col_bounds.push_back(x0);
                col_bounds.insert(col_bounds.end(), inner_v.begin(), inner_v.end());
                col_bounds.push_back(x1);
                kind = "ruled_grid";
            } else {
                std::vector<const Obj*> inside;
                for (auto w : words)
                    if (x0 <= w->bbox.cx() && w->bbox.cx() <= x1 && y0 <= w->bbox.cy() && w->bbox.cy() <= y1) inside.push_back(w);
                auto rows_w = text_lines(inside);
                std::vector<double> hs;
                for (auto w : inside) hs.push_back(w->bbox.h());
                double gap_min = std::max(1.5 * ppm, inside.empty() ? ppm : median(hs));
                auto seps = rows_w.empty() ? std::vector<std::pair<double, double>>{} : separators(rows_w, x0, x1, gap_min);
                col_bounds.push_back(x0);
                for (auto& s : seps) col_bounds.push_back((s.first + s.second) / 2);
                col_bounds.push_back(x1);
                kind = vxs.empty() ? "ruled_horizontal" : "ruled_grid";
            }
            if ((row_bounds.size() - 1) * (col_bounds.size() - 1) < 2) continue;
            Table t;
            t.id = (int)tables.size() + 1;
            t.kind = kind;
            t.bbox = {x0, y0, x1, y1};
            t.row_bounds = row_bounds;
            t.col_bounds = col_bounds;
            for (size_t k : members)
                t.rules.push_back({hr[k]->bbox, 'h', hr[k]->id, argmin_abs(row_bounds, hr[k]->bbox.cy())});
            for (auto v : vin) t.rules.push_back({v->bbox, 'v', v->id, argmin_abs(col_bounds, v->bbox.cx())});
            fill_cells(t, words);
            tables.push_back(t);
            used_h.insert(members.begin(), members.end());
        }
    }

    // ---------------- utan linjer
    std::vector<const Obj*> free;
    for (auto w : words) {
        bool in = false;
        for (const auto& t : tables)
            if (t.bbox.x0 - ppm <= w->bbox.cx() && w->bbox.cx() <= t.bbox.x1 + ppm && t.bbox.y0 - ppm <= w->bbox.cy() &&
                w->bbox.cy() <= t.bbox.y1 + ppm)
                in = true;
        if (!in) free.push_back(w);
    }
    auto lines = text_lines(free);
    std::vector<double> cys, hs;
    for (const auto& ln : lines) {
        std::vector<double> c, h;
        for (auto o : ln) {
            c.push_back(o->bbox.cy());
            h.push_back(o->bbox.h());
        }
        cys.push_back(median(c));
        hs.push_back(median(h));
    }
    size_t i = 0;
    while (lines.size() >= 3 && i < lines.size() - 2) {
        bool have = false;
        size_t bj = 0;
        std::vector<std::pair<double, double>> bseps;
        double bx0 = 0, bx1 = 0, bstep = 0;
        for (size_t j = i + 2; j < lines.size(); ++j) {
            std::vector<double> sp;
            for (size_t k = i; k < j; ++k) sp.push_back(cys[k + 1] - cys[k]);
            std::vector<double> hseg(hs.begin() + i, hs.begin() + j + 1);
            double hm = median(hseg);
            double spmax = *std::max_element(sp.begin(), sp.end()), spmin = *std::min_element(sp.begin(), sp.end());
            if (spmax > 2.2 * hm || spmin < 0.8 * hm || (spmax - spmin) > 0.35 * median(sp)) break;
            if (*std::max_element(hseg.begin(), hseg.end()) > 1.6 * *std::min_element(hseg.begin(), hseg.end())) break;
            std::vector<Line> seg(lines.begin() + i, lines.begin() + j + 1);
            double x0 = 1e18, x1 = -1e18;
            for (const auto& ln : seg)
                for (auto w : ln) {
                    x0 = std::min(x0, w->bbox.x0);
                    x1 = std::max(x1, w->bbox.x1);
                }
            auto seps = separators(seg, x0, x1, std::max(1.5 * ppm, hm));
            std::vector<std::pair<double, double>> ok;
            for (auto& s : seps) {
                if (!both_sides(seg.front(), s) || !both_sides(seg.back(), s)) continue;
                size_t cnt = 0;
                for (const auto& ln : seg) cnt += both_sides(ln, s);
                if (cnt >= seg.size() - 1) ok.push_back(s);
            }
            if (ok.empty()) break;
            have = true;
            bj = j;
            bseps = ok;
            bx0 = x0;
            bx1 = x1;
            bstep = median(sp);
        }
        if (!have) {
            ++i;
            continue;
        }
        std::vector<double> rb{cys[i] - bstep / 2};
        for (size_t k = i; k < bj; ++k) rb.push_back((cys[k] + cys[k + 1]) / 2);
        rb.push_back(cys[bj] + bstep / 2);
        std::vector<double> cb{bx0 - 0.5 * ppm};
        for (auto& s : bseps) cb.push_back((s.first + s.second) / 2);
        cb.push_back(bx1 + 0.5 * ppm);
        Table t;
        t.id = (int)tables.size() + 1;
        t.kind = "unruled";
        t.bbox = {cb.front(), rb.front(), cb.back(), rb.back()};
        t.row_bounds = rb;
        t.col_bounds = cb;
        fill_cells(t, free);
        tables.push_back(t);
        i = bj + 1;
    }
    return tables;
}

std::optional<json> locate_in_table(const std::vector<Table>& tables, const Box& box, double ppm) {
    double cx = box.cx(), cy = box.cy();
    for (const auto& t : tables) {
        if (!(t.bbox.x0 - ppm <= cx && cx <= t.bbox.x1 + ppm && t.bbox.y0 - ppm <= cy && cy <= t.bbox.y1 + ppm)) continue;
        double bw = box.w(), bh = box.h();
        for (const auto& rl : t.rules) {
            double pad = 0.3 * ppm;
            double ix = std::max(0.0, std::min(box.x1, rl.bbox.x1 + pad) - std::max(box.x0, rl.bbox.x0 - pad));
            double iy = std::max(0.0, std::min(box.y1, rl.bbox.y1 + pad) - std::max(box.y0, rl.bbox.y0 - pad));
            bool thin = std::min(bw, bh) <= 0.8 * ppm;
            if (thin && ix * iy >= 0.6 * std::max(bw * bh, 1.0)) {
                if (rl.orient == 'h') {
                    int p = rl.position;
                    json between = (0 < p && p < t.n_rows()) ? json::array({p - 1, p}) : json(nullptr);
                    return json{{"table", t.id}, {"element", "rule"}, {"orient", "h"}, {"position", p}, {"between_rows", between}};
                }
                return json{{"table", t.id}, {"element", "rule"}, {"orient", "v"}, {"position", rl.position}};
            }
        }
        int r = std::clamp(searchsorted(t.row_bounds, cy) - 1, 0, t.n_rows() - 1);
        int c = std::clamp(searchsorted(t.col_bounds, cx) - 1, 0, t.n_cols() - 1);
        int c0 = searchsorted(t.col_bounds, box.x0 + 1) - 1, c1 = searchsorted(t.col_bounds, box.x1 - 1) - 1;
        auto cell = [&](int rr, int cc) {
            auto it = t.cells.find({rr, cc});
            return it == t.cells.end() ? std::string() : it->second;
        };
        return json{{"table", t.id}, {"element", "cell"}, {"row", r}, {"col", c0 == c1 ? json(c) : json(nullptr)},
                    {"row_label", cell(r, 0)}, {"cell_text", c0 == c1 ? json(cell(r, c)) : json(nullptr)}};
    }
    return std::nullopt;
}

}  // namespace pc::artwork
