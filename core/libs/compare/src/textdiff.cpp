// Textjämförelse mellan två PDF:ers textlager på en sida.

#include <algorithm>
#include <set>
#include <cmath>

#include "printcmp/compare/textdiff.hpp"
#include "printcmp/pdf/text.hpp"
#include "printcmp/core/difflib.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/core/text.hpp"

namespace pc::compare {

namespace {

std::vector<TextWord> reading_order(std::vector<TextWord> words) {
    if (words.empty()) return words;
    std::stable_sort(words.begin(), words.end(), [](const TextWord& a, const TextWord& b) { return a.bbox.cy() < b.bbox.cy(); });
    std::vector<std::vector<TextWord>> lines;
    for (auto& w : words) {
        double cy = w.bbox.cy(), h = w.bbox.h();
        bool placed = false;
        for (auto& ln : lines) {
            double ly = 0, lh = 0;
            for (auto& x : ln) {
                ly += x.bbox.cy();
                lh += x.bbox.h();
            }
            ly /= ln.size();
            lh /= ln.size();
            if (std::abs(cy - ly) < 0.5 * std::max(h, lh)) {
                ln.push_back(w);
                placed = true;
                break;
            }
        }
        if (!placed) lines.push_back({w});
    }
    auto ly = [](const std::vector<TextWord>& ln) {
        double s = 0;
        for (auto& x : ln) s += x.bbox.cy();
        return s / ln.size();
    };
    std::stable_sort(lines.begin(), lines.end(), [&](auto& a, auto& b) { return ly(a) < ly(b); });
    std::vector<TextWord> out;
    for (auto& ln : lines) {
        std::stable_sort(ln.begin(), ln.end(), [](const TextWord& a, const TextWord& b) { return a.bbox.x0 < b.bbox.x0; });
        out.insert(out.end(), ln.begin(), ln.end());
    }
    return out;
}

Box map_box(const Box& b, const Mat3& Hinv) {
    Pt p[4] = {Hinv.apply({b.x0, b.y0}), Hinv.apply({b.x1, b.y0}), Hinv.apply({b.x1, b.y1}), Hinv.apply({b.x0, b.y1})};
    Box o{p[0].x, p[0].y, p[0].x, p[0].y};
    for (auto& q : p) o = o.unite({q.x, q.y, q.x, q.y});
    return o;
}

json mk(const std::string& kind, const Box& box, double ppm, const std::string& frm, const std::string& to, json extra = {}) {
    json d = {{"kind", kind},
              {"bbox_px", {(int)box.x0, (int)box.y0, (int)(box.x1 - box.x0), (int)(box.y1 - box.y0)}},
              {"bbox_mm", {box.x0 / ppm, box.y0 / ppm, box.w() / ppm, box.h() / ppm}},
              {"text", {{"from", frm}, {"to", to}}}};
    if (extra.is_object())
        for (auto& [k, v] : extra.items()) d["text"][k] = v;
    return d;
}

std::vector<json> merge_runs(std::vector<json> diffs, double ppm) {
    std::stable_sort(diffs.begin(), diffs.end(), [](const json& a, const json& b) {
        auto key = [](const json& d) {
            return std::make_tuple(d["kind"].get<std::string>(), (double)pyround(d["bbox_mm"][1].get<double>()),
                                   d["bbox_mm"][0].get<double>());
        };
        return key(a) < key(b);
    });
    std::vector<json> out;
    auto join_box = [&](json& p, const json& d) {
        double px = p["bbox_mm"][0], py = p["bbox_mm"][1], pw = p["bbox_mm"][2], ph = p["bbox_mm"][3];
        double x = d["bbox_mm"][0], y = d["bbox_mm"][1], w = d["bbox_mm"][2], h = d["bbox_mm"][3];
        double x0 = std::min(px, x), y0 = std::min(py, y), x1 = std::max(px + pw, x + w), y1 = std::max(py + ph, y + h);
        p["bbox_mm"] = {x0, y0, x1 - x0, y1 - y0};
        p["bbox_px"] = {(int)(x0 * ppm), (int)(y0 * ppm), (int)((x1 - x0) * ppm), (int)((y1 - y0) * ppm)};
    };
    for (auto& d : diffs) {
        if (!out.empty()) {
            json& p = out.back();
            double py = p["bbox_mm"][1], ph = p["bbox_mm"][3], y = d["bbox_mm"][1], h = d["bbox_mm"][3];
            double px = p["bbox_mm"][0], pw = p["bbox_mm"][2], x = d["bbox_mm"][0];
            bool same_line = std::abs((py + ph / 2) - (y + h / 2)) < 0.5 * std::max(ph, h);
            if (d["kind"] == "Style" && p["kind"] == "Style" && p["text"].value("size_to", json()) == d["text"].value("size_to", json()) &&
                p["text"].value("font_to", json()) == d["text"].value("font_to", json()) && same_line) {
                join_box(p, d);
                std::string t = p["text"]["from"].get<std::string>() + " " + d["text"]["from"].get<std::string>();
                p["text"]["from"] = t;
                p["text"]["to"] = t;
                continue;
            }
            if ((d["kind"] == "Added" || d["kind"] == "Missing") && p["kind"] == d["kind"] && same_line &&
                x - (px + pw) < 2.5 * std::max(ph, h)) {
                join_box(p, d);
                std::string key = d["kind"] == "Missing" ? "from" : "to";
                p["text"][key] = text::strip(p["text"][key].get<std::string>() + " " + d["text"][key].get<std::string>());
                continue;
            }
        }
        out.push_back(d);
    }
    return out;
}

}  // namespace

std::vector<TextWord> extract_words(const pdf::Bytes& data, int page_index, const pdf::Geometry& geom) {
    auto d = pdf::Document::open(data);
    std::vector<TextWord> words;
    std::string cur, font;
    bool have = false, have_last = false;
    double box[4] = {0, 0, 0, 0}, last_r = 0, last_b = 0;
    std::vector<double> sizes;
    static Regex subset("^[A-Z]{6}\\+");
    auto flush = [&] {
        std::string s = text::strip(cur);
        if (!s.empty() && have) {
            Pt p0 = geom.to_px(box[0], box[3]), p1 = geom.to_px(box[2], box[1]);
            words.push_back({text::nfc(s), {p0.x, p0.y, p1.x, p1.y}, sizes.empty() ? 0.0 : median(sizes), font});
        }
        cur.clear();
        have = false;
        sizes.clear();
        font.clear();
    };
    for (const auto& c : pdf::text_chars(*d, page_index)) {
        char32_t ch = c.code;
        double l = c.l, r = c.r, b = c.b, t = c.t;
        if (text::is_space(ch) || ch == U'\r' || ch == U'\n' || ch == 2 || (r - l) <= 0) {
            flush();
            have_last = false;
            continue;
        }
        double size = std::max({t - b, have ? box[3] - box[1] : 0.0, 1.0});
        if (have_last && (l - last_r > 0.6 * size || std::abs(b - last_b) > 0.6 * size)) flush();
        cur += text::encode(ch);
        if (!have) {
            box[0] = l;
            box[1] = b;
            box[2] = r;
            box[3] = t;
            have = true;
        } else {
            box[0] = std::min(box[0], l);
            box[1] = std::min(box[1], b);
            box[2] = std::max(box[2], r);
            box[3] = std::max(box[3], t);
        }
        sizes.push_back(c.font_size);
        if (font.empty() && !c.font_name.empty()) font = subset.replace(c.font_name, "");
        last_r = r;
        last_b = b;
        have_last = true;
    }
    flush();
    return reading_order(words);
}

std::vector<json> text_compare(const std::vector<TextWord>& ref, const std::vector<TextWord>& smp, const Mat3& H, double ppm,
                               double size_tol) {
    Mat3 Hinv = H.inverse();
    std::vector<Box> smp_in_ref;
    for (auto& w : smp) smp_in_ref.push_back(map_box(w.bbox, Hinv));
    std::vector<std::string> a, b;
    for (auto& w : ref) a.push_back(w.text);
    for (auto& w : smp) b.push_back(w.text);
    difflib::SequenceMatcher<std::string> sm(a, b);
    std::vector<json> diffs;
    std::vector<size_t> removed, added;
    for (const auto& op : sm.get_opcodes()) {
        if (op.tag == difflib::Opcode::Equal) {
            for (size_t k = 0; k < op.i2 - op.i1; ++k) {
                const auto& rw = ref[op.i1 + k];
                const auto& sw = smp[op.j1 + k];
                bool size_diff = rw.size > 0 && sw.size > 0 && std::abs(rw.size - sw.size) > size_tol * rw.size;
                bool font_diff = !rw.font.empty() && !sw.font.empty() && rw.font != sw.font;
                if (size_diff || font_diff)
                    diffs.push_back(mk("Style", rw.bbox.unite(smp_in_ref[op.j1 + k]), ppm, rw.text, sw.text,
                                       {{"font_from", rw.font}, {"font_to", sw.font}, {"size_from", round_to(rw.size, 2)},
                                        {"size_to", round_to(sw.size, 2)}}));
            }
        } else if (op.tag == difflib::Opcode::Replace) {
            if (op.i2 - op.i1 == op.j2 - op.j1) {
                for (size_t k = 0; k < op.i2 - op.i1; ++k) {
                    const auto& rw = ref[op.i1 + k];
                    const auto& sw = smp[op.j1 + k];
                    diffs.push_back(mk("Changed", rw.bbox.unite(smp_in_ref[op.j1 + k]), ppm, rw.text, sw.text,
                                       {{"diff", difflib::char_diff(rw.text, sw.text)}}));
                }
            } else {
                std::vector<std::string> fs, ts;
                Box u = ref[op.i1].bbox;
                for (size_t i = op.i1; i < op.i2; ++i) {
                    fs.push_back(ref[i].text);
                    u = u.unite(ref[i].bbox);
                }
                for (size_t j = op.j1; j < op.j2; ++j) {
                    ts.push_back(smp[j].text);
                    u = u.unite(smp_in_ref[j]);
                }
                std::string frm = text::join(fs, " "), to = text::join(ts, " ");
                diffs.push_back(mk("Changed", u, ppm, frm, to, {{"diff", difflib::char_diff(frm, to)}}));
            }
        } else if (op.tag == difflib::Opcode::Delete) {
            for (size_t i = op.i1; i < op.i2; ++i) removed.push_back(i);
        } else {
            for (size_t j = op.j1; j < op.j2; ++j) added.push_back(j);
        }
    }
    std::set<size_t> used_add;
    for (size_t i : removed) {
        const auto& rw = ref[i];
        auto it = std::find_if(added.begin(), added.end(), [&](size_t j) { return !used_add.count(j) && smp[j].text == rw.text; });
        if (it != added.end()) {
            size_t j = *it;
            used_add.insert(j);
            const Box& sb = smp_in_ref[j];
            double shift = std::hypot(sb.cx() - rw.bbox.cx(), sb.cy() - rw.bbox.cy()) / ppm;
            diffs.push_back(mk("Moved", rw.bbox.unite(sb), ppm, rw.text, rw.text,
                               {{"shift_mm", round_to(shift, 2)},
                                {"from_bbox_mm", {rw.bbox.x0 / ppm, rw.bbox.y0 / ppm, rw.bbox.w() / ppm, rw.bbox.h() / ppm}}}));
        } else {
            diffs.push_back(mk("Missing", rw.bbox, ppm, rw.text, ""));
        }
    }
    for (size_t j : added)
        if (!used_add.count(j)) diffs.push_back(mk("Added", smp_in_ref[j], ppm, "", smp[j].text));
    return merge_runs(diffs, ppm);
}

}  // namespace pc::compare
