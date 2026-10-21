// Objektkarta ur PDF:en. Text från textlagret, aldrig OCR.

#include <algorithm>
#include <cmath>
#include <numeric>

#include "printcmp/artwork/objmap.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/pdf/text.hpp"

namespace pc::artwork {

namespace {

bool inside_any(const Box& b, const std::vector<Box>& boxes, double pad) {
    double cx = b.cx(), cy = b.cy();
    for (const auto& o : boxes)
        if (o.x0 - pad <= cx && cx <= o.x1 + pad && o.y0 - pad <= cy && cy <= o.y1 + pad) return true;
    return false;
}

std::vector<Obj> group_outlined_text(std::vector<Obj> objs, double ppm, const std::vector<Box>& exclude) {
    std::vector<size_t> cand;
    for (size_t i = 0; i < objs.size(); ++i) {
        const auto& o = objs[i];
        if (o.kind != "Vector") continue;
        double w = o.bbox.w(), h = o.bbox.h();
        if (!(0.8 * ppm <= h && h <= 7.0 * ppm) || w > 1.6 * h || w < 0.12 * ppm) continue;
        if (inside_any(o.bbox, exclude, 1.0 * ppm)) continue;
        cand.push_back(i);
    }
    if (cand.size() < 3) return objs;
    std::map<size_t, size_t> parent;
    for (size_t i : cand) parent[i] = i;
    std::function<size_t(size_t)> find = [&](size_t a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    for (size_t ii = 0; ii < cand.size(); ++ii) {
        const Box& A = objs[cand[ii]].bbox;
        for (size_t jj = ii + 1; jj < cand.size(); ++jj) {
            const Box& B = objs[cand[jj]].bbox;
            double ha = A.h(), hb = B.h(), hm = std::max(ha, hb);
            if (hm > 3.0 * std::min(ha, hb)) continue;
            double ov = std::min(A.y1, B.y1) - std::max(A.y0, B.y0);
            if (ov < 0.5 * std::min(ha, hb)) continue;
            double gap = std::max(B.x0 - A.x1, A.x0 - B.x1);
            if (gap > 0.6 * hm) continue;
            parent[find(cand[ii])] = find(cand[jj]);
        }
    }
    std::map<size_t, std::vector<size_t>> groups;
    for (size_t i : cand) groups[find(i)].push_back(i);
    std::set<size_t> remove;
    std::vector<Obj> extra;
    for (auto& [root, members] : groups) {
        if (members.size() < 3) continue;
        std::vector<double> hs, bottoms;
        for (size_t i : members) {
            hs.push_back(objs[i].bbox.h());
            bottoms.push_back(objs[i].bbox.y1);
        }
        double med = median(bottoms), mh = median(hs);
        int on = 0;
        for (double b : bottoms) on += std::abs(b - med) <= 0.12 * mh;
        if ((double)on / bottoms.size() < 0.6) continue;
        Obj g;
        g.kind = "OutlinedText";
        g.bbox = objs[members[0]].bbox;
        for (size_t i : members) {
            g.bbox = g.bbox.unite(objs[i].bbox);
            g.glyphs.push_back(objs[i].bbox);
        }
        std::sort(g.glyphs.begin(), g.glyphs.end(), [](const Box& a, const Box& b) {
            return std::tie(a.x0, a.y0, a.x1, a.y1) < std::tie(b.x0, b.y0, b.x1, b.y1);
        });
        g.baseline_px = med;
        extra.push_back(g);
        remove.insert(members.begin(), members.end());
    }
    std::vector<Obj> out;
    for (size_t i = 0; i < objs.size(); ++i)
        if (!remove.count(i)) out.push_back(objs[i]);
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

}  // namespace

std::vector<Obj> build_objects(const pdf::Bytes& data, int page_index, const pdf::Geometry& geom,
                               const std::vector<Box>& exclude) {
    auto d = pdf::Document::open(data);
    std::vector<Obj> objs;
    double ppm = geom.ppm();
    {
        std::string word;
        bool have = false, have_last = false;
        double wb[4] = {0, 0, 0, 0}, last_r = 0, last_base = 0;
        auto flush = [&] {
            std::string s = text::strip(word);
            if (!s.empty() && have) {
                Obj o;
                o.kind = "Text";
                o.bbox = geom.box_px(wb[0], wb[1], wb[2], wb[3]);
                o.text = s;
                objs.push_back(o);
            }
            word.clear();
            have = false;
        };
        for (const auto& c : pdf::text_chars(*d, page_index)) {
            char32_t ch = c.code;
            double l = c.l, r = c.r, b = c.b, t = c.t;
            if (text::is_space(ch) || ch == U'\r' || ch == U'\n' || (r - l) <= 0) {
                flush();
                have_last = false;
                continue;
            }
            // skiljetecken har små rutor: jämför mot ordets höjd hittills
            double size = std::max({t - b, have ? wb[3] - wb[1] : 0.0, 1.0});
            if (have_last && (l - last_r > 0.6 * size || std::abs(b - last_base) > 0.6 * size)) flush();
            word += text::encode(ch);
            if (!have) {
                wb[0] = l;
                wb[1] = b;
                wb[2] = r;
                wb[3] = t;
                have = true;
            } else {
                wb[0] = std::min(wb[0], l);
                wb[1] = std::min(wb[1], b);
                wb[2] = std::max(wb[2], r);
                wb[3] = std::max(wb[3], t);
            }
            last_r = r;
            last_base = b;
            have_last = true;
        }
        flush();
    }
    for (const auto& po : pdf::page_objects(*d, page_index, 4)) {
        std::string kind;
        if (po.type == pdf::OBJ_IMAGE) kind = "Image";
        else if (po.type == pdf::OBJ_PATH || po.type == pdf::OBJ_SHADING) kind = "Vector";
        else continue;
        Box box = geom.box_px(po.l, po.b, po.r, po.t);
        double w = box.w(), h = box.h();
        if (std::max(w, h) < 2) continue;
        if (kind == "Vector" && inside_any(box, exclude, 0.5 * ppm)) kind = "Barcode";
        if (kind == "Vector" && std::min(w, h) <= 0.5 * ppm && std::max(w, h) >= std::max(3 * ppm, 8 * std::min(w, h))) {
            kind = "Rule";
            double t_min = 0.3 * ppm;   // streckade linjer kan ha nolltjocka rutor
            if (w < t_min) {
                box.x0 -= t_min / 2;
                box.x1 += t_min / 2;
            }
            if (h < t_min) {
                box.y0 -= t_min / 2;
                box.y1 += t_min / 2;
            }
        }
        Obj o;
        o.kind = kind;
        o.bbox = box;
        objs.push_back(o);
    }
    objs = group_outlined_text(objs, ppm, exclude);
    for (size_t i = 0; i < objs.size(); ++i) objs[i].id = (int)i + 1;
    return objs;
}

std::vector<const Obj*> overlapping(const std::vector<Obj>& objs, const Box& box, const std::set<std::string>& kinds,
                                    double min_frac) {
    double area = std::max(box.w() * box.h(), 1.0);
    std::vector<const Obj*> out;
    for (const auto& o : objs) {
        if (!kinds.empty() && !kinds.count(o.kind)) continue;
        double ix = std::max(0.0, std::min(box.x1, o.bbox.x1) - std::max(box.x0, o.bbox.x0));
        double iy = std::max(0.0, std::min(box.y1, o.bbox.y1) - std::max(box.y0, o.bbox.y0));
        if (ix * iy >= min_frac * area) out.push_back(&o);
    }
    return out;
}

}  // namespace pc::artwork
