#include "printcmp/doc/doctext.hpp"


#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "printcmp/core/regex.hpp"
#include "printcmp/pdf/text.hpp"
#include "printcmp/core/text.hpp"

namespace pc::doc {

namespace {

const std::u32string HYPHENS = U"-­‐‑\x02";

const Regex& page_num_rx() {
    static Regex r(
        "^\\W*(?:(?:sida|page|s\\.|p\\.)\\s*)?(\\d{1,4}|[ivxlcdm]{1,7})(?:\\s*(?:\\(|av|of|/)\\s*\\d{1,4}\\)?)?\\W*$",
        true);
    return r;
}

std::string page_num_match(const std::string& t) {
    Regex::Match m;
    if (page_num_rx().full_match(t, &m) && m.groups.size() > 1) return m.groups[1];
    return "";
}

std::string sig_of(const std::string& t) {
    static Regex digits("\\d+");
    return text::strip(digits.replace(text::casefold(t), "#"));
}

double pymedian(const std::vector<double>& v) { return median(v); }

Style style_of(const std::string& name, int flags, int weight, double size, int r, int g, int b) {
    static Regex split("[-,]");
    static Regex suffix(
        "(Bold|Italic|Oblique|Regular|Roman|Book|Medium|Semibold|SemiBold|Light|Black|Heavy|Demi|MT|PS|It|BoldItalic|BoldOblique)+$");
    static Regex boldrx("Bold|Black|Heavy|Semibold|SemiBold|Demi");
    static Regex italrx("Italic|Oblique");
    Regex::Match m;
    std::string first = name, rest;
    if (split.search(name, m)) {
        first = name.substr(0, m.start);
        rest = name.substr(m.start);
    }
    std::string fam = first.empty() ? name : first;
    std::string stripped = suffix.replace(fam, "");
    if (!stripped.empty()) fam = stripped;
    Style s;
    s.family = fam;
    s.size = round_to(size, 2);
    s.bold = weight >= 600 || boldrx.search(name);
    s.italic = (flags & 64) || italrx.search(name + rest);
    s.color = format("#%02x%02x%02x", r & 255, g & 255, b & 255);
    return s;
}

std::string font_name(const pdf::TextChar& c) {
    static Regex subset("^[A-Z]{6}\\+");
    return subset.replace(c.font_name, "");
}

std::pair<std::vector<Word>, std::pair<double, double>> page_words(const pdf::Document& d, int pi) {
    auto [w_pt, h_pt] = pdf::page_size(d, pi);
    std::vector<Word> words;
    std::string cur;
    bool have_box = false, have_last = false, have_style = false;
    double box[4] = {0, 0, 0, 0};
    double last_r = 0, last_b = 0, last_h = 0;
    Style st;
    std::vector<double> bases;
    std::map<std::tuple<std::string, int, int, double, unsigned, unsigned, unsigned>, Style> cache;

    auto flush = [&] {
        std::string s = text::strip(cur);
        if (!s.empty() && have_box) {
            Word w;
            w.text = text::nfc(s);
            w.page = pi;
            w.bbox = {box[0] * PT_MM, (h_pt - box[3]) * PT_MM, box[2] * PT_MM, (h_pt - box[1]) * PT_MM};
            w.style = st;
            w.base = (h_pt - pymedian(bases)) * PT_MM;
            words.push_back(std::move(w));
        }
        cur.clear();
        have_box = false;
        have_style = false;
        bases.clear();
    };

    for (const auto& c : pdf::text_chars(d, pi)) {
        unsigned u = unsigned(c.code);
        char32_t ch = c.code;
        bool generated = c.generated;
        if (u == 2 || (HYPHENS.find(ch) != std::u32string::npos && c.hyphen)) {
            // avstavningsbindestreck som PDFium markerat: behåll som "-" i ordet
            if (!cur.empty()) cur += "-";
            flush();
            have_last = false;
            continue;
        }
        if (u == 0 || text::is_space(ch) || (generated && !text::is_alnum(ch))) {
            flush();
            have_last = false;
            continue;
        }
        double l = c.l, r = c.r, b = c.b, t = c.t;
        double fs = c.font_size;
        if (c.has_matrix) {
            const double* M = c.matrix;
            double sc = std::sqrt(std::abs(M[0] * M[3] - M[1] * M[2]));
            fs *= sc != 0 ? sc : 1.0;
        }
        double hgt = std::max({t - b, 0.3 * fs, 0.5});
        // mellanrum mäts på stegbredden (lösa rutor), radbyte på den lösa rutans underkant
        double ll = l, lr = r, lb = b;
        if (c.has_loose) {
            ll = c.loose_l;
            lr = c.loose_r;
            lb = c.loose_b;
        }
        if (have_last && (ll - last_r > 0.2 * fs || std::abs(lb - last_b) > 0.6 * std::max(hgt, last_h))) flush();
        if (!have_style) {
            int flags = c.font_flags;
            std::string name = font_name(c);
            int weight = c.font_weight;
            unsigned R = c.fill_r, G = c.fill_g, B = c.fill_b;
            auto key = std::make_tuple(name, flags, weight, round_to(fs, 2), R, G, B);
            auto it = cache.find(key);
            st = it != cache.end() ? it->second : (cache[key] = style_of(name, flags, weight, fs, R, G, B));
            have_style = true;
        }
        cur += text::encode(ch);
        bases.push_back(lb);
        if (!have_box) {
            box[0] = l;
            box[1] = b;
            box[2] = r;
            box[3] = t;
            have_box = true;
        } else {
            box[0] = std::min(box[0], l);
            box[1] = std::min(box[1], b);
            box[2] = std::max(box[2], r);
            box[3] = std::max(box[3], t);
        }
        last_r = lr;
        last_b = lb;
        last_h = hgt;
        have_last = true;
    }
    flush();
    return {words, {w_pt * PT_MM, h_pt * PT_MM}};
}

int py_round(double v) { return static_cast<int>(std::nearbyint(v)); }

std::vector<std::vector<Word>> columns(std::vector<Word> words) {
    if (words.size() < 80) return {words};
    double x0 = 1e18, x1 = -1e18;
    for (auto& w : words) {
        x0 = std::min(x0, w.bbox.x0);
        x1 = std::max(x1, w.bbox.x1);
    }
    if (x1 - x0 < 30) return {words};
    const double step = 0.5;
    std::vector<int> bins(static_cast<size_t>((x1 - x0) / step) + 2, 0);
    for (auto& w : words) {
        int a = static_cast<int>((w.bbox.x0 - x0) / step), b = static_cast<int>((w.bbox.x1 - x0) / step) + 1;
        for (int k = std::max(0, a); k < std::min<int>(b, (int)bins.size()); ++k) bins[k]++;
    }
    int lo = static_cast<int>(0.2 * bins.size()), hi = static_cast<int>(0.8 * bins.size());
    int limit = std::max(1, static_cast<int>(0.01 * words.size()));
    bool have_best = false;
    double best_c = 0, best_w = 0;
    int run_start = -1;
    for (int k = lo; k <= hi; ++k) {
        bool free = bins[k] <= limit;
        if (free && run_start < 0) run_start = k;
        if ((!free || k == hi) && run_start >= 0) {
            double width = (k - run_start) * step;
            if (width >= 4.0 && (!have_best || width > best_w)) {
                best_c = (run_start + k) / 2.0 * step + x0;
                best_w = width;
                have_best = true;
            }
            run_start = -1;
        }
    }
    if (!have_best) return {words};
    std::vector<Word> left, right;
    for (auto& w : words) ((w.bbox.x0 + w.bbox.x1) / 2 < best_c ? left : right).push_back(w);
    auto n_lines = [](const std::vector<Word>& ws) {
        std::set<int> s;
        for (auto& w : ws) s.insert(py_round(w.base));
        return s.size();
    };
    if (std::min(left.size(), right.size()) < 0.1 * words.size() || std::min(n_lines(left), n_lines(right)) < 3 ||
        n_lines(words) < 15)
        return {words};
    auto a = columns(left), b = columns(right);
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<std::vector<Word>> lines_of(std::vector<Word> ws) {
    std::stable_sort(ws.begin(), ws.end(), [](const Word& a, const Word& b) { return a.base < b.base; });
    std::vector<std::vector<Word>> lines;
    for (auto& w : ws) {
        double h = w.style.size * PT_MM;
        bool placed = false;
        for (int k = (int)lines.size() - 1; k >= std::max(0, (int)lines.size() - 3); --k) {
            auto& ln = lines[k];
            std::vector<double> bs;
            double lh = 0;
            for (auto& x : ln) {
                bs.push_back(x.base);
                lh = std::max(lh, x.style.size * PT_MM);
            }
            if (std::abs(w.base - median(bs)) < 0.4 * std::max(h, lh)) {
                ln.push_back(w);
                placed = true;
                break;
            }
        }
        if (!placed) lines.push_back({w});
    }
    auto med = [](const std::vector<Word>& ln) {
        std::vector<double> b;
        for (auto& x : ln) b.push_back(x.base);
        return median(b);
    };
    std::stable_sort(lines.begin(), lines.end(), [&](const auto& a, const auto& b) { return med(a) < med(b); });
    for (auto& ln : lines)
        std::stable_sort(ln.begin(), ln.end(), [](const Word& a, const Word& b) { return a.bbox.x0 < b.bbox.x0; });
    return lines;
}

std::set<std::pair<int, int>> find_furniture(const std::vector<std::vector<std::vector<Word>>>& per_page,
                                             const std::vector<std::pair<double, double>>& sizes) {
    std::vector<double> all_sizes;
    for (auto& lines : per_page)
        for (auto& ln : lines)
            for (auto& w : ln) all_sizes.push_back(w.style.size);
    double body_size = all_sizes.empty() ? 10.0 : median(all_sizes);
    auto line_y = [](const std::vector<Word>& ln) {
        double s = 0;
        for (auto& w : ln) s += (w.bbox.y0 + w.bbox.y1) / 2;
        return s / ln.size();
    };
    std::vector<double> gaps;
    for (size_t pi = 0; pi < per_page.size(); ++pi) {
        std::vector<double> ys;
        for (auto& ln : per_page[pi]) ys.push_back(line_y(ln));
        std::sort(ys.begin(), ys.end());
        for (size_t k = 1; k < ys.size(); ++k) {
            double gp = ys[k] - ys[k - 1];
            if (gp > 0.5 && gp < 0.1 * sizes[pi].second) gaps.push_back(gp);
        }
    }
    double lead = gaps.empty() ? 5.0 : median(gaps);
    struct Cand { int pi, k; std::string t; };
    std::vector<Cand> cands;
    std::set<std::pair<int, int>> isolated_small;
    for (size_t pi = 0; pi < per_page.size(); ++pi) {
        const auto& lines = per_page[pi];
        if (lines.empty()) continue;
        double h = sizes[pi].second;
        std::vector<double> ys;
        for (auto& ln : lines) ys.push_back(line_y(ln));
        for (size_t k = 0; k < lines.size(); ++k) {
            if (lines[k].size() > 12) continue;
            if (!(ys[k] < 0.12 * h || ys[k] > 0.88 * h)) continue;
            bool top = ys[k] < 0.5 * h;
            double nearby = h;
            for (size_t j = 0; j < ys.size(); ++j) {
                if (j == k) continue;
                if (top ? ys[j] >= ys[k] - 0.5 : ys[j] <= ys[k] + 0.5) nearby = std::min(nearby, std::abs(ys[j] - ys[k]));
            }
            if (nearby <= 1.2 * lead) continue;
            std::vector<std::string> parts;
            double mx = 0;
            for (auto& w : lines[k]) {
                parts.push_back(w.text);
                mx = std::max(mx, w.style.size);
            }
            cands.push_back({(int)pi, (int)k, text::join(parts, " ")});
            if (mx < 0.92 * body_size) isolated_small.insert({(int)pi, (int)k});
        }
    }
    std::unordered_map<std::string, int> count;
    for (auto& c : cands) count[sig_of(c.t)]++;
    int n_pages = (int)per_page.size();
    int need = n_pages >= 3 ? std::max(2, (int)(0.3 * n_pages)) : 1000000000;
    std::set<std::pair<int, int>> out;
    for (auto& c : cands) {
        int cnt = count[sig_of(c.t)];
        if (!page_num_match(c.t).empty() || cnt >= need || (n_pages >= 2 && isolated_small.count({c.pi, c.k})))
            out.insert({c.pi, c.k});
        else if (n_pages >= 3 && cnt >= 2 && text::length(c.t) < 80)
            out.insert({c.pi, c.k});
    }
    return out;
}

}  // namespace

Style::Key Style::key(double step) const {
    return {family, std::nearbyint(size / step) * step, bold, italic, color};
}

std::string Style::describe() const {
    std::string em;
    if (bold) em += "fet";
    if (italic) em += em.empty() ? "kursiv" : " kursiv";
    bool dark = false;
    if (color.size() == 7) {
        int s = std::stoi(color.substr(1, 2), nullptr, 16) + std::stoi(color.substr(3, 2), nullptr, 16) +
                std::stoi(color.substr(5, 2), nullptr, 16);
        dark = s < 150;   // svart (även CMYK-svart som renderas #231f20) skrivs inte ut
    }
    std::string col = (color.empty() || dark) ? "" : " " + color;
    return format("%s %.1f pt", family.c_str(), size) + (em.empty() ? "" : " " + em) + col;
}

std::string normalize_text(const std::string& s0, bool typography, bool ignore_case) {
    std::string s = text::nfkc(s0);
    s = text::replace_all(s, "­", "");
    if (typography) {
        static const std::vector<std::pair<std::string, std::string>> map = {
            {"’", "'"}, {"‘", "'"}, {"‚", "'"}, {"′", "'"},
            {"“", "\""}, {"”", "\""}, {"„", "\""}, {"«", "\""}, {"»", "\""},
            {"–", "-"}, {"—", "-"}, {"−", "-"}, {"‐", "-"}, {"‑", "-"},
            {" ", " "}, {" ", " "}, {" ", " "}, {"…", "..."}};
        for (auto& [a, b] : map) s = text::replace_all(s, a, b);
    }
    if (ignore_case) s = text::casefold(s);
    return s;
}

Document load(const pdf::Bytes& data, const LoadOptions& opt, const std::string& path) {
    auto d = pdf::Document::open(data);
    int n = d->page_count();
    int p0 = opt.first_page > 0 ? std::max(1, opt.first_page) : 1;
    int p1 = opt.last_page > 0 ? std::min(n, opt.last_page) : n;
    std::vector<std::vector<std::vector<Word>>> per_page;
    std::vector<std::pair<double, double>> sizes;
    for (int pi = p0 - 1; pi < p1; ++pi) {
        auto [ws, sz] = page_words(*d, pi);
        sizes.push_back(sz);
        std::vector<std::vector<Word>> lines;
        for (auto& col : columns(ws)) {
            auto ls = lines_of(col);
            lines.insert(lines.end(), ls.begin(), ls.end());
        }
        for (size_t k = 0; k < lines.size(); ++k)
            for (auto& w : lines[k]) w.line = (int)k;
        per_page.push_back(std::move(lines));
    }
    for (auto& lines : per_page)
        for (auto& ln : lines)
            for (auto& w : ln) w.norm = normalize_text(w.text, opt.typography, opt.ignore_case);
    std::set<std::pair<int, int>> furn;
    if (opt.furniture) furn = find_furniture(per_page, sizes);
    Document out;
    out.path = path;
    out.pages = sizes;
    std::vector<std::string> exact(per_page.size());
    std::vector<std::vector<std::pair<std::string, std::string>>> edge(per_page.size());
    static Regex num14("\\d{1,4}");
    for (size_t pi = 0; pi < per_page.size(); ++pi) {
        for (size_t k = 0; k < per_page[pi].size(); ++k) {
            auto& ln = per_page[pi][k];
            if (!furn.count({(int)pi, (int)k})) {
                out.words.insert(out.words.end(), ln.begin(), ln.end());
                continue;
            }
            std::vector<std::string> parts;
            for (auto& w : ln) {
                w.role = "furniture";
                parts.push_back(w.text);
            }
            out.furniture.insert(out.furniture.end(), ln.begin(), ln.end());
            std::string txt = text::join(parts, " ");
            std::string m = page_num_match(txt);
            if (!m.empty() && exact[pi].empty()) {
                exact[pi] = m;
            } else if (ln.size() > 1) {
                for (const Word* w : {&ln.front(), &ln.back()})
                    if (num14.full_match(w->text)) edge[pi].push_back({sig_of(txt), w->text});
            }
        }
    }
    std::map<std::string, std::set<std::string>> vals;
    for (auto& c : edge)
        for (auto& [s, v] : c) vals[s].insert(v);
    for (size_t pi = 0; pi < per_page.size(); ++pi) {
        std::optional<std::string> lab;
        if (!exact[pi].empty()) lab = exact[pi];
        else
            for (auto& [s, v] : edge[pi])
                if (vals[s].size() > 1) {
                    lab = v;
                    break;
                }
        out.page_labels.push_back(lab);
    }
    return out;
}

Document load_file(const std::string& path, const LoadOptions& opt) { return load(read_file_bytes(path), opt, path); }

std::vector<Word> dehyphenate(const std::vector<Word>& words, const std::set<std::string>& vocab) {
    std::vector<Word> out;
    size_t i = 0;
    while (i < words.size()) {
        const Word& w = words[i];
        const Word* nxt = i + 1 < words.size() ? &words[i + 1] : nullptr;
        auto wc = text::decode(w.norm);
        bool ends_hyphen = wc.size() > 1 && (HYPHENS.find(wc.back()) != std::u32string::npos);
        if (nxt && ends_hyphen && (nxt->page != w.page || nxt->line != w.line)) {
            auto nc = text::decode(nxt->norm);
            if (!nc.empty() && text::is_lower(nc[0])) {
                std::string head = text::encode(std::vector<char32_t>(wc.begin(), wc.end() - 1));
                bool keep = vocab.count(w.norm + nxt->norm) > 0;
                auto tc = text::decode(w.text);
                Word m;
                m.text = (keep ? w.text : text::encode(std::vector<char32_t>(tc.begin(), tc.end() - (tc.empty() ? 0 : 1)))) +
                         nxt->text;
                m.page = w.page;
                m.bbox = w.bbox;
                m.style = w.style;
                m.line = w.line;
                m.norm = (keep ? head + "-" : head) + nxt->norm;
                m.role = w.role;
                m.parts = {{w.page, w.bbox}, {nxt->page, nxt->bbox}};
                m.hyphenated = true;
                m.base = w.base;
                out.push_back(std::move(m));
                i += 2;
                continue;
            }
        }
        out.push_back(w);
        ++i;
    }
    return out;
}

std::set<std::string> vocabulary(const std::vector<const Document*>& docs) {
    std::set<std::string> v;
    for (auto* d : docs)
        for (auto& w : d->words) {
            auto c = text::decode(w.norm);
            if (c.size() > 2 && std::find(c.begin() + 1, c.end() - 1, U'-') != c.end() - 1) v.insert(w.norm);
        }
    return v;
}

std::vector<PageIssue> page_number_issues(const Document& d) {
    std::vector<std::pair<int, std::vector<int>>> runs;
    for (size_t pi = 0; pi < d.page_labels.size(); ++pi) {
        const auto& lab = d.page_labels[pi];
        if (!lab || !text::is_digit_str(*lab)) continue;
        int off = std::stoi(*lab) - (int)pi;
        if (!runs.empty() && runs.back().first == off) runs.back().second.push_back((int)pi);
        else runs.push_back({off, {(int)pi}});
    }
    std::vector<PageIssue> issues;
    size_t k = 1;
    while (k < runs.size()) {
        int prev_off = runs[k - 1].first;
        auto& [off, pages] = runs[k];
        int pi = pages[0];
        issues.push_back({pi + 1, std::to_string(pi + off), std::to_string(pi + prev_off)});
        if (pages.size() == 1 && k + 1 < runs.size() && runs[k + 1].first == prev_off) k += 2;
        else k += 1;
    }
    return issues;
}

}  // namespace pc::doc
