// Dokumentjämförelse via textlagret: omflödat (manus mot bok) eller fast layout.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

#include "printcmp/compare/profiles.hpp"
#include "printcmp/compare/document.hpp"
#include "printcmp/compare/datacheck.hpp"
#include "printcmp/core/difflib.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/core/text.hpp"

namespace pc::compare {

namespace {

using doc::Style;
using doc::Word;
using Opc = difflib::Opcode;

const std::map<std::string, int> SEV_RANK = {{"Critical", 0}, {"Major", 1}, {"Minor", 2}, {"Info", 3}};

const char* tag_name(Opc::Tag t) {
    switch (t) {
        case Opc::Equal: return "equal";
        case Opc::Replace: return "replace";
        case Opc::Delete: return "delete";
        default: return "insert";
    }
}

std::vector<Opc> opcodes(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if ((double)a.size() * b.size() <= 4'000'000) {
        difflib::SequenceMatcher<std::string> sm(a, b);
        return sm.get_opcodes();
    }
    // patience: unika ord i båda som ankare, längsta växande delföljd, difflib mellan ankarna
    std::unordered_map<std::string, int> ca, cb;
    for (auto& t : a) ca[t]++;
    for (auto& t : b) cb[t]++;
    std::unordered_map<std::string, size_t> pos_b;
    for (size_t j = 0; j < b.size(); ++j)
        if (cb[b[j]] == 1) pos_b[b[j]] = j;
    std::vector<std::pair<size_t, size_t>> pairs;
    for (size_t i = 0; i < a.size(); ++i)
        if (ca[a[i]] == 1 && pos_b.count(a[i])) pairs.push_back({i, pos_b[a[i]]});
    std::vector<size_t> tails, idx, prev(pairs.size(), SIZE_MAX);
    for (size_t k = 0; k < pairs.size(); ++k) {
        size_t j = pairs[k].second;
        size_t p = std::lower_bound(tails.begin(), tails.end(), j) - tails.begin();
        if (p == tails.size()) {
            tails.push_back(j);
            idx.push_back(k);
        } else {
            tails[p] = j;
            idx[p] = k;
        }
        prev[k] = p > 0 ? idx[p - 1] : SIZE_MAX;
    }
    std::vector<std::pair<size_t, size_t>> chain;
    size_t k = idx.empty() ? SIZE_MAX : idx.back();
    while (k != SIZE_MAX) {
        chain.push_back(pairs[k]);
        k = prev[k];
    }
    std::reverse(chain.begin(), chain.end());
    chain.push_back({a.size(), b.size()});
    std::vector<Opc> ops;
    size_t ai = 0, bj = 0;
    for (auto [i, j] : chain) {
        if (i > ai || j > bj) {
            std::vector<std::string> sa(a.begin() + ai, a.begin() + i), sb(b.begin() + bj, b.begin() + j);
            difflib::SequenceMatcher<std::string> sm(sa, sb);
            for (auto op : sm.get_opcodes()) ops.push_back({op.tag, op.i1 + ai, op.i2 + ai, op.j1 + bj, op.j2 + bj});
        }
        if (i < a.size()) ops.push_back({Opc::Equal, i, i + 1, j, j + 1});
        ai = i + 1;
        bj = j + 1;
    }
    std::vector<Opc> merged;
    for (auto& op : ops) {
        if (!merged.empty() && op.tag == Opc::Equal && merged.back().tag == Opc::Equal && merged.back().i2 == op.i1 &&
            merged.back().j2 == op.j1) {
            merged.back().i2 = op.i2;
            merged.back().j2 = op.j2;
        } else if (op.i1 < op.i2 || op.j1 < op.j2) {
            merged.push_back(op);
        }
    }
    return merged;
}

json loc(const std::vector<Word>& ws, size_t i1, size_t i2) {
    if (i1 >= i2) return nullptr;
    std::vector<std::pair<int, Box>> boxes;
    for (size_t i = i1; i < i2; ++i)
        for (auto& pb : ws[i].boxes()) boxes.push_back(pb);
    int p0 = boxes[0].first;
    Box u;
    bool first = true;
    std::set<int> pages;
    for (auto& [p, b] : boxes) {
        pages.insert(p + 1);
        if (p != p0) continue;
        u = first ? b : u.unite(b);
        first = false;
    }
    std::vector<std::string> t;
    for (size_t i = i1; i < i2; ++i) t.push_back(ws[i].text);
    return {{"page", p0 + 1}, {"bbox_mm", {round_to(u.x0, 2), round_to(u.y0, 2), round_to(u.w(), 2), round_to(u.h(), 2)}},
            {"pages", std::vector<int>(pages.begin(), pages.end())}, {"text", text::join(t, " ")}};
}

std::string joined(const std::vector<Word>& ws, size_t i1, size_t i2) {
    std::vector<std::string> t;
    for (size_t i = i1; i < i2 && i < ws.size(); ++i) t.push_back(ws[i].text);
    return text::join(t, " ");
}

json context(const std::vector<Word>& ws, size_t i1, size_t i2, size_t n = 6) {
    return {{"before", joined(ws, i1 > n ? i1 - n : 0, i1)}, {"after", joined(ws, i2, std::min(ws.size(), i2 + n))}};
}

std::string category(const std::string& frm, const std::string& to) {
    static Regex nondigit("\\D"), nonword("[\\W_]");
    if (nondigit.replace(frm, "") != nondigit.replace(to, "")) return "Number";
    if (nonword.replace(frm, "") == nonword.replace(to, "")) return "Punctuation";
    return "Text";
}

double color_dist(const std::string& a, const std::string& b) {
    if (a.size() != 7 || b.size() != 7) return 0.0;
    double s = 0;
    for (int k : {1, 3, 5}) {
        double d = std::stoi(a.substr(k, 2), nullptr, 16) - std::stoi(b.substr(k, 2), nullptr, 16);
        s += d * d;
    }
    return std::sqrt(s);
}

std::vector<std::string> style_delta(const Style& got, const Style& exp, double tol) {
    std::vector<std::string> d;
    if (got.family != exp.family) d.push_back("family");
    if (std::abs(got.size - exp.size) > tol) d.push_back("size");
    if (got.bold != exp.bold) d.push_back("bold");
    if (got.italic != exp.italic) d.push_back("italic");
    if (color_dist(got.color, exp.color) > 60) d.push_back("color");
    return d;
}

// Counter med insättningsordning (som Pythons Counter.most_common vid lika antal)
template <class K>
struct OrderedCounter {
    std::vector<K> order;
    std::map<K, double> n;
    void add(const K& k, double v = 1) {
        if (!n.count(k)) order.push_back(k);
        n[k] += v;
    }
    std::pair<K, double> most_common() const {
        K best = order[0];
        for (auto& k : order)
            if (n.at(k) > n.at(best)) best = k;
        return {best, n.at(best)};
    }
    double total() const {
        double s = 0;
        for (auto& [k, v] : n) s += v;
        return s;
    }
};

using Pairs = std::vector<std::pair<size_t, size_t>>;

std::pair<json, std::vector<json>> style_check(const std::vector<Word>& rw, const std::vector<Word>& sw, const Pairs& pairs,
                                               const std::string& layout) {
    if (pairs.empty()) return {json::array(), {}};
    using Key = Style::Key;
    std::map<Key, OrderedCounter<Key>> counts;
    std::vector<Key> role_order;
    std::map<Key, std::set<std::pair<int, int>>> lines;
    std::map<Key, int> words_n;
    std::map<Key, Style> rep;
    OrderedCounter<std::string> dummy;
    std::map<std::string, OrderedCounter<std::string>> fam_map, col_map;
    std::vector<double> ratios;
    for (auto [i, j] : pairs) {
        Key rk = rw[i].style.key(), sk = sw[j].style.key();
        if (!counts.count(rk)) role_order.push_back(rk);
        counts[rk].add(sk, std::max<double>(1, (double)text::length(rw[i].norm)));
        lines[rk].insert({rw[i].page, rw[i].line});
        words_n[rk]++;
        rep.emplace(sk, sw[j].style);
        rep.emplace(rk, rw[i].style);
        fam_map[rw[i].style.family].add(sw[j].style.family);
        col_map[rw[i].style.color].add(sw[j].style.color);
        if (rw[i].style.size > 0) ratios.push_back(sw[j].style.size / rw[i].style.size);
    }
    double body_ratio = ratios.empty() ? 1.0 : median(ratios);
    std::map<Key, Style> expected;
    std::map<Key, bool> strong;
    std::vector<json> smap;
    for (auto& rk : role_order) {
        auto& cnt = counts[rk];
        const Style& r = rep[rk];
        auto [sk, n] = cnt.most_common();
        double share = n / cnt.total();
        bool is_strong = words_n[rk] >= 5 && lines[rk].size() >= 2 && share >= 0.6;
        if (layout == "fixed") expected[rk] = r;
        else if (is_strong) expected[rk] = rep[sk];
        else {
            Style e = r;
            e.family = fam_map[r.family].most_common().first;
            e.size = round_to(r.size * body_ratio, 2);
            e.color = col_map[r.color].most_common().first;
            expected[rk] = e;
        }
        strong[rk] = is_strong;
        smap.push_back({{"reference", r.describe()}, {"sample", rep[sk].describe()}, {"words", words_n[rk]}, {"share", round_to(share, 3)}});
    }
    std::stable_sort(smap.begin(), smap.end(), [](const json& a, const json& b) { return a["words"].get<int>() > b["words"].get<int>(); });

    std::vector<json> out;
    std::set<Key> systematic_done;
    struct Run { size_t i1, i2, j1, j2; Style exp, got; std::vector<std::string> what; };
    std::optional<Run> run;
    auto close = [&] {
        if (!run) return;
        out.push_back({{"kind", "Style"}, {"severity", "Minor"}, {"category", "Style"}, {"ref", loc(rw, run->i1, run->i2)},
                       {"sample", loc(sw, run->j1, run->j2)}, {"context", context(rw, run->i1, run->i2)},
                       {"text", {{"from", joined(rw, run->i1, run->i2)}, {"to", joined(sw, run->j1, run->j2)}}},
                       {"style", {{"reference", rw[run->i1].style.describe()}, {"expected", run->exp.describe()},
                                  {"sample", run->got.describe()}, {"changed", run->what}}}});
        run.reset();
    };
    for (auto [i, j] : pairs) {
        const Style &r = rw[i].style, &s = sw[j].style;
        Key rk = r.key();
        const Style& exp = expected[rk];
        double tol = layout == "fixed" ? std::max(0.2, 0.05 * r.size)
                                       : (strong[rk] ? std::max(0.3, 0.04 * exp.size) : std::max(0.5, 0.35 * exp.size));
        auto what = style_delta(s, exp, tol);
        if (layout != "fixed" && strong[rk] && !systematic_done.count(rk)) {
            std::vector<std::string> sysd;
            if (exp.bold != r.bold) sysd.push_back("bold");
            if (exp.italic != r.italic) sysd.push_back("italic");
            if (!sysd.empty()) {
                systematic_done.insert(rk);
                out.push_back({{"kind", "StyleSystematic"}, {"severity", "Minor"}, {"category", "Style"}, {"ref", loc(rw, i, i + 1)},
                               {"sample", loc(sw, j, j + 1)}, {"text", {{"from", rw[i].text}, {"to", sw[j].text}}},
                               {"style", {{"reference", r.describe()}, {"sample", exp.describe()}, {"changed", sysd}, {"words", words_n[rk]}}}});
            }
        }
        if (!what.empty()) {
            if (run && run->i2 == i && run->j2 == j && run->what == what && run->exp.key() == exp.key() && run->got.key() == s.key() &&
                rw[i].page == rw[run->i1].page) {
                run->i2 = i + 1;
                run->j2 = j + 1;
            } else {
                close();
                run = Run{i, i + 1, j, j + 1, exp, s, what};
            }
        } else {
            close();
        }
    }
    close();
    return {smap, out};
}

using AddFn = std::function<json&(const std::string&, size_t, size_t, size_t, size_t, json)>;

Pairs fixed_text(const std::vector<Word>& rw, const std::vector<Word>& sw, const std::vector<std::string>& a,
                 const std::vector<std::string>& b, const AddFn& add, double tol) {
    auto lines = [](const std::vector<Word>& ws) {
        std::vector<std::pair<int, int>> keys;
        std::map<std::pair<int, int>, std::vector<size_t>> d;
        for (size_t k = 0; k < ws.size(); ++k) {
            auto key = std::make_pair(ws[k].page, ws[k].line);
            if (!d.count(key)) keys.push_back(key);
            d[key].push_back(k);
        }
        std::vector<std::vector<size_t>> out;
        for (auto& k : keys) out.push_back(d[k]);
        return out;
    };
    struct G { int page; double x0, x1, base; };
    auto geom = [](const std::vector<Word>& ws, const std::vector<size_t>& idx) {
        double x0 = 1e18, x1 = -1e18;
        std::vector<double> bs;
        for (size_t k : idx) {
            x0 = std::min(x0, ws[k].bbox.x0);
            x1 = std::max(x1, ws[k].bbox.x1);
            bs.push_back(ws[k].base);
        }
        return G{ws[idx[0]].page, x0, x1, median(bs)};
    };
    auto shift = [](const G& g1, const G& g2) {
        double c[3] = {g2.x0 - g1.x0, g2.x1 - g1.x1, (g2.x0 + g2.x1 - g1.x0 - g1.x1) / 2};
        double dx = c[0];
        for (double v : c)
            if (std::abs(v) < std::abs(dx)) dx = v;
        return std::make_pair(dx, g2.base - g1.base);
    };
    auto L1 = lines(rw), L2 = lines(sw);
    std::vector<std::string> s1, s2;
    std::vector<G> G1, G2;
    for (auto& L : L1) {
        std::vector<std::string> t;
        for (size_t i : L) t.push_back(a[i]);
        s1.push_back(text::join(t, " "));
        G1.push_back(geom(rw, L));
    }
    for (auto& L : L2) {
        std::vector<std::string> t;
        for (size_t j : L) t.push_back(b[j]);
        s2.push_back(text::join(t, " "));
        G2.push_back(geom(sw, L));
    }
    struct Cand { double r, negd; size_t p, q; };
    std::vector<Cand> cands;
    for (size_t p = 0; p < s1.size(); ++p) {
        auto cp = difflib::codepoints(s1[p]);
        for (size_t q = 0; q < s2.size(); ++q) {
            if (std::abs(G1[p].page - G2[q].page) > 1) continue;
            auto cq = difflib::codepoints(s2[q]);
            difflib::SequenceMatcher<char32_t> sm(cp, cq);
            if (sm.real_quick_ratio() < 0.6 || sm.quick_ratio() < 0.6) continue;
            double r = sm.ratio();
            if (r >= 0.6) {
                auto [dx, dy] = shift(G1[p], G2[q]);
                cands.push_back({round_to(r, 3), -std::hypot(dx, dy), p, q});
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        return std::tie(x.r, x.negd, x.p, x.q) > std::tie(y.r, y.negd, y.p, y.q);
    });
    std::map<size_t, size_t> pair_of;
    std::set<size_t> used;
    for (auto& c : cands)
        if (!pair_of.count(c.p) && !used.count(c.q)) {
            pair_of[c.p] = c.q;
            used.insert(c.q);
        }
    for (size_t p = 0; p < L1.size(); ++p) {
        if (pair_of.count(p)) continue;
        double bestd = 1e18;
        size_t bq = SIZE_MAX;
        for (size_t q = 0; q < L2.size(); ++q) {
            if (used.count(q) || G1[p].page != G2[q].page) continue;
            auto [dx, dy] = shift(G1[p], G2[q]);
            if (std::abs(dy) < 0.5 && std::abs(dx) < 1.0 && std::abs(dx) < bestd) {
                bestd = std::abs(dx);
                bq = q;
            }
        }
        if (bq != SIZE_MAX) {
            pair_of[p] = bq;
            used.insert(bq);
        }
    }
    Pairs equal_pairs;
    for (size_t p = 0; p < L1.size(); ++p) {
        const auto& L = L1[p];
        if (!pair_of.count(p)) {
            std::vector<std::string> t;
            for (size_t i : L) t.push_back(rw[i].text);
            add("Missing", L.front(), L.back() + 1, 0, 0,
                {{"category", category(s1[p], "")}, {"severity", "Major"}, {"text", {{"from", text::join(t, " ")}, {"to", ""}}}});
            continue;
        }
        size_t q = pair_of[p];
        const auto& M = L2[q];
        std::vector<std::string> la, lb;
        for (size_t i : L) la.push_back(a[i]);
        for (size_t j : M) lb.push_back(b[j]);
        difflib::SequenceMatcher<std::string> sm(la, lb);
        Pairs line_pairs;
        for (auto& op : sm.get_opcodes()) {
            size_t gi1 = op.i2 > op.i1 ? L[op.i1] : 0, gi2 = op.i2 > op.i1 ? L[op.i2 - 1] + 1 : 0;
            size_t gj1 = op.j2 > op.j1 ? M[op.j1] : 0, gj2 = op.j2 > op.j1 ? M[op.j2 - 1] + 1 : 0;
            if (op.tag == Opc::Equal) {
                for (size_t k = 0; k < op.i2 - op.i1; ++k) {
                    equal_pairs.push_back({L[op.i1 + k], M[op.j1 + k]});
                    line_pairs.push_back({L[op.i1 + k], M[op.j1 + k]});
                }
                continue;
            }
            std::vector<std::string> f, t, nf, nt;
            for (size_t k = op.i1; k < op.i2; ++k) {
                f.push_back(rw[L[k]].text);
                nf.push_back(a[L[k]]);
            }
            for (size_t k = op.j1; k < op.j2; ++k) {
                t.push_back(sw[M[k]].text);
                nt.push_back(b[M[k]]);
            }
            std::string frm = text::join(f, " "), to = text::join(t, " ");
            std::string kind = op.tag == Opc::Replace ? "Changed" : op.tag == Opc::Delete ? "Missing" : "Added";
            std::string cat = category(text::join(nf, " "), text::join(nt, " "));
            json tx = {{"from", frm}, {"to", to}};
            if (kind == "Changed") tx["diff"] = difflib::char_diff(frm, to);
            add(kind, gi1, gi2, gj1, gj2,
                {{"category", cat}, {"severity", (cat == "Number" && kind == "Changed") ? "Critical" : "Major"}, {"text", tx}});
        }
        auto [dx, dy] = shift(G1[p], G2[q]);
        if (!line_pairs.empty()) {
            std::vector<double> d;
            for (auto [i, j] : line_pairs) d.push_back(sw[j].base - rw[i].base);
            dy = median(d);   // median över ord i båda: en "rad" kan bestå av två texter med lite olika baslinje
        }
        if (G1[p].page != G2[q].page || std::hypot(dx, dy) > tol) {
            bool same = G1[p].page == G2[q].page;
            std::vector<std::string> f, t;
            for (size_t i : L) f.push_back(rw[i].text);
            for (size_t j : M) t.push_back(sw[j].text);
            add("Moved", L.front(), L.back() + 1, M.front(), M.back() + 1,
                {{"category", "Layout"}, {"severity", same ? "Minor" : "Major"},
                 {"text", {{"from", text::join(f, " ")}, {"to", text::join(t, " ")}}},
                 {"move", {{"shift_mm", {round_to(dx, 2), round_to(dy, 2)}}, {"distance_mm", round_to(std::hypot(dx, dy), 2)},
                           {"from_page", G1[p].page + 1}, {"to_page", G2[q].page + 1}, {"exact", true}}}});
        }
    }
    for (size_t q = 0; q < L2.size(); ++q)
        if (!used.count(q)) {
            std::vector<std::string> t;
            for (size_t j : L2[q]) t.push_back(sw[j].text);
            add("Added", 0, 0, L2[q].front(), L2[q].back() + 1,
                {{"category", category("", s2[q])}, {"severity", "Major"}, {"text", {{"from", ""}, {"to", text::join(t, " ")}}}});
        }
    return equal_pairs;
}

}  // namespace

json compare_documents(const doc::Document& ref, const doc::Document& smp, const DocumentOptions& opt) {
    auto vocab = doc::vocabulary({&ref, &smp});
    auto rw = doc::dehyphenate(ref.words, vocab);
    auto sw = doc::dehyphenate(smp.words, vocab);
    std::vector<std::string> a, b;
    for (auto& w : rw) a.push_back(w.norm);
    for (auto& w : sw) b.push_back(w.norm);
    std::vector<Opc> ops;
    if (opt.layout != "fixed") ops = opcodes(a, b);
    std::vector<json> findings;
    size_t mmw = (size_t)opt.min_move_words;

    AddFn add = [&](const std::string& kind, size_t i1, size_t i2, size_t j1, size_t j2, json extra) -> json& {
        json f = {{"kind", kind}, {"ref", loc(rw, i1, i2)}, {"sample", loc(sw, j1, j2)}};
        if (i2 > i1) f["context"] = context(rw, i1, i2);
        else if (j2 > j1) f["context"] = context(sw, j1, j2);
        for (auto& [k, v] : extra.items()) f[k] = v;
        findings.push_back(f);
        return findings.back();
    };
    auto join_range = [](const std::vector<std::string>& v, size_t i1, size_t i2) {
        return text::join(std::vector<std::string>(v.begin() + i1, v.begin() + i2), " ");
    };

    std::vector<std::array<size_t, 2>> removed, added;
    Pairs equal_pairs;
    std::map<size_t, size_t> rep_a, rep_b;
    for (size_t k_op = 0; k_op < ops.size(); ++k_op) {
        const auto& op = ops[k_op];
        if (op.tag == Opc::Equal) {
            for (size_t k = 0; k < op.i2 - op.i1; ++k) equal_pairs.push_back({op.i1 + k, op.j1 + k});
        } else if (op.tag == Opc::Delete) {
            removed.push_back({op.i1, op.i2});
        } else if (op.tag == Opc::Insert) {
            added.push_back({op.j1, op.j2});
        } else {
            std::vector<std::string> sa(a.begin() + op.i1, a.begin() + op.i2), sb(b.begin() + op.j1, b.begin() + op.j2);
            difflib::SequenceMatcher<std::string> sm(sa, sb);
            double sim = sm.ratio();
            bool big = op.i2 - op.i1 >= mmw && op.j2 - op.j1 >= mmw;
            if (big && sim < 0.5) {
                removed.push_back({op.i1, op.i2});
                added.push_back({op.j1, op.j2});
                for (size_t i = op.i1; i < op.i2; ++i) rep_a[i] = k_op;
                for (size_t j = op.j1; j < op.j2; ++j) rep_b[j] = k_op;
            } else {
                std::string frm = joined(rw, op.i1, op.i2), to = joined(sw, op.j1, op.j2);
                std::string nf = join_range(a, op.i1, op.i2), nt = join_range(b, op.j1, op.j2);
                if (nf == nt) continue;
                std::string cat = category(nf, nt);
                add("Changed", op.i1, op.i2, op.j1, op.j2,
                    {{"category", cat}, {"severity", cat == "Number" ? "Critical" : "Major"},
                     {"text", {{"from", frm}, {"to", to}, {"diff", difflib::char_diff(frm, to)}}}});
            }
        }
    }

    // --- flyttade stycken
    std::set<size_t> rem_idx, add_idx;
    for (auto& r : removed)
        for (size_t i = r[0]; i < r[1]; ++i) rem_idx.insert(i);
    for (auto& r : added)
        for (size_t j = r[0]; j < r[1]; ++j) add_idx.insert(j);
    auto regions = [](std::vector<std::array<size_t, 2>> runs, size_t gap = 2) {
        std::sort(runs.begin(), runs.end());
        std::vector<std::array<size_t, 2>> out;
        for (auto& r : runs) {
            if (!out.empty() && r[0] <= out.back()[1] + gap) out.back()[1] = std::max(out.back()[1], r[1]);
            else out.push_back(r);
        }
        return out;
    };
    auto uniq = [](std::vector<std::array<size_t, 2>> xs) {
        std::vector<std::array<size_t, 2>> out;
        std::set<std::array<size_t, 2>> seen;
        for (auto& x : xs)
            if (seen.insert(x).second) out.push_back(x);
        return out;
    };
    auto rc = removed;
    auto rr = regions(removed);
    rc.insert(rc.end(), rr.begin(), rr.end());
    auto rem_cands = uniq(rc);
    std::stable_sort(rem_cands.begin(), rem_cands.end(),
                     [](auto& x, auto& y) { return (long)x[0] - (long)x[1] < (long)y[0] - (long)y[1]; });
    auto ac = added;
    auto ar = regions(added);
    ac.insert(ac.end(), ar.begin(), ar.end());
    auto add_cands = uniq(ac);
    struct Move { size_t i1, i2, j1, j2; double ratio; };
    std::vector<Move> moved;
    auto live = [](const std::array<size_t, 2>& r, const std::set<size_t>& s) {
        std::vector<size_t> v;
        for (size_t i = r[0]; i < r[1]; ++i)
            if (s.count(i)) v.push_back(i);
        return v;
    };
    for (auto& rcand : rem_cands) {
        auto live_a = live(rcand, rem_idx);
        if (live_a.size() < mmw) continue;
        std::vector<std::string> la;
        for (size_t i : live_a) la.push_back(a[i]);
        size_t best_got = 0;
        bool have = false;
        std::vector<size_t> bia, bjb;
        double bratio = 0;
        for (auto& acand : add_cands) {
            auto live_b = live(acand, add_idx);
            if (live_b.size() < mmw) continue;
            std::vector<std::string> lb;
            for (size_t j : live_b) lb.push_back(b[j]);
            difflib::SequenceMatcher<std::string> sm(la, lb);
            size_t got = 0;
            std::vector<size_t> ia, jb;
            for (auto& m : sm.get_matching_blocks()) {
                got += m.size;
                for (size_t t = 0; t < m.size; ++t) {
                    ia.push_back(live_a[m.a + t]);
                    jb.push_back(live_b[m.b + t]);
                }
            }
            if (got >= 0.8 * la.size() && (!have || got > best_got)) {
                have = true;
                best_got = got;
                bia = ia;
                bjb = jb;
                bratio = sm.ratio();
            }
        }
        if (!have) continue;
        size_t mi1 = *std::min_element(bia.begin(), bia.end()), mi2 = *std::max_element(bia.begin(), bia.end()) + 1;
        size_t mj1 = *std::min_element(bjb.begin(), bjb.end()), mj2 = *std::max_element(bjb.begin(), bjb.end()) + 1;
        for (size_t i = mi1; i < mi2; ++i) rem_idx.erase(i);
        for (size_t j = mj1; j < mj2; ++j) add_idx.erase(j);
        // rotation: lika ord precis utanför flyttens ändar hör till flytten
        for (int it = 0; it < 3; ++it) {
            bool found = false;
            for (long i : {(long)mi1 - 1, (long)mi2}) {
                if (i < 0 || !rem_idx.count((size_t)i)) continue;
                for (long j : {(long)mj1 - 1, (long)mj2}) {
                    if (j < 0 || !add_idx.count((size_t)j) || a[i] != b[j]) continue;
                    rem_idx.erase(i);
                    add_idx.erase(j);
                    mi1 = std::min(mi1, (size_t)i);
                    mi2 = std::max(mi2, (size_t)i + 1);
                    mj1 = std::min(mj1, (size_t)j);
                    mj2 = std::max(mj2, (size_t)j + 1);
                    found = true;
                    break;
                }
                if (found) break;
            }
            if (!found) break;
        }
        moved.push_back({mi1, mi2, mj1, mj2, bratio});
    }
    // andra varvet: längsta gemensamma block i taget (stycken flyttade i bitar)
    while (true) {
        size_t best = 0;
        std::vector<size_t> bia, bjb;
        for (auto& rcand : rem_cands) {
            auto live_a = live(rcand, rem_idx);
            if (live_a.size() < mmw) continue;
            std::vector<std::string> la;
            for (size_t i : live_a) la.push_back(a[i]);
            for (auto& acand : add_cands) {
                auto live_b = live(acand, add_idx);
                if (live_b.size() < mmw) continue;
                std::vector<std::string> lb;
                for (size_t j : live_b) lb.push_back(b[j]);
                difflib::SequenceMatcher<std::string> sm(la, lb);
                auto m = sm.find_longest_match(0, la.size(), 0, lb.size());
                if (m.size >= mmw && m.size > best) {
                    best = m.size;
                    bia.assign(live_a.begin() + m.a, live_a.begin() + m.a + m.size);
                    bjb.assign(live_b.begin() + m.b, live_b.begin() + m.b + m.size);
                }
            }
        }
        if (!best) break;
        for (size_t i : bia) rem_idx.erase(i);
        for (size_t j : bjb) add_idx.erase(j);
        moved.push_back({*std::min_element(bia.begin(), bia.end()), *std::max_element(bia.begin(), bia.end()) + 1,
                         *std::min_element(bjb.begin(), bjb.end()), *std::max_element(bjb.begin(), bjb.end()) + 1, 1.0});
    }
    auto runs_of = [](const std::set<size_t>& idx) {
        std::vector<std::array<size_t, 2>> out;
        for (size_t x : idx) {
            if (!out.empty() && x == out.back()[1]) out.back()[1] = x + 1;
            else out.push_back({x, x + 1});
        }
        return out;
    };
    removed = runs_of(rem_idx);
    added = runs_of(add_idx);
    // rester av ett ersättningsblock = en ändring
    for (auto it = removed.begin(); it != removed.end();) {
        auto r = *it;
        auto ka = rep_a.find(r[0]);
        bool all = ka != rep_a.end();
        for (size_t i = r[0]; all && i < r[1]; ++i) all = rep_a.count(i) && rep_a[i] == ka->second;
        if (!all) {
            ++it;
            continue;
        }
        auto sit = std::find_if(added.begin(), added.end(), [&](auto& s) {
            for (size_t j = s[0]; j < s[1]; ++j)
                if (!rep_b.count(j) || rep_b[j] != ka->second) return false;
            return true;
        });
        if (sit == added.end()) {
            ++it;
            continue;
        }
        auto s = *sit;
        added.erase(sit);
        it = removed.erase(it);
        std::string frm = joined(rw, r[0], r[1]), to = joined(sw, s[0], s[1]);
        std::string cat = category(join_range(a, r[0], r[1]), join_range(b, s[0], s[1]));
        add("Changed", r[0], r[1], s[0], s[1],
            {{"category", cat}, {"severity", cat == "Number" ? "Critical" : "Major"},
             {"text", {{"from", frm}, {"to", to}, {"diff", difflib::char_diff(frm, to)}}}});
    }
    for (auto& m : moved) {
        std::string frm = joined(rw, m.i1, m.i2), to = joined(sw, m.j1, m.j2);
        json tx = {{"from", frm}, {"to", to}, {"words", m.i2 - m.i1}};
        if (m.ratio <= 0.999) tx["diff"] = difflib::char_diff(frm, to);
        add("Moved", m.i1, m.i2, m.j1, m.j2,
            {{"category", "Text"}, {"severity", "Major"}, {"text", tx},
             {"move", {{"ref_index", m.i1}, {"sample_index", m.j1}, {"exact", m.ratio > 0.999}}}});
    }
    for (auto& r : removed) {
        std::string frm = joined(rw, r[0], r[1]);
        add("Missing", r[0], r[1], 0, 0, {{"category", category(frm, "")}, {"severity", "Major"}, {"text", {{"from", frm}, {"to", ""}}}});
    }
    for (auto& r : added) {
        std::string to = joined(sw, r[0], r[1]);
        add("Added", 0, 0, r[0], r[1], {{"category", category("", to)}, {"severity", "Major"}, {"text", {{"from", ""}, {"to", to}}}});
    }
    if (opt.layout == "fixed") equal_pairs = fixed_text(rw, sw, a, b, add, opt.move_tol_mm);

    auto [style_map, style_findings] = style_check(rw, sw, equal_pairs, opt.layout);
    findings.insert(findings.end(), style_findings.begin(), style_findings.end());

    for (auto [side, d] : {std::make_pair(std::string("reference"), &ref), std::make_pair(std::string("sample"), &smp)})
        for (auto& iss : doc::page_number_issues(*d)) {
            json l = {{"page", iss.page}, {"bbox_mm", nullptr}, {"pages", {iss.page}}, {"text", iss.printed}};
            findings.push_back({{"kind", "PageNumber"}, {"severity", "Minor"}, {"category", "PageNumber"}, {"side", side},
                                {"ref", side == "reference" ? l : json(nullptr)}, {"sample", side == "sample" ? l : json(nullptr)},
                                {"text", {{"from", iss.expected}, {"to", iss.printed}}}});
        }
    for (auto& f : findings) {
        if (!f.contains("category")) f["category"] = "Text";
        bool exact = f["kind"] != "Moved" || !f.contains("move") || f["move"].value("exact", true);
        f["certainty"] = {{"level", exact ? "confirmed" : "high"}, {"score", exact ? 99 : 85}, {"reasons", {"Textlagret i båda filerna"}}};
    }
    auto sort_key = [](const json& f) {
        const json& l = !f["ref"].is_null() ? f["ref"] : f["sample"];
        int page = l.is_null() ? 0 : l["page"].get<int>();
        double y = (l.is_null() || l["bbox_mm"].is_null()) ? 0.0 : l["bbox_mm"][1].get<double>();
        auto it = SEV_RANK.find(f["severity"].get<std::string>());
        return std::make_tuple(it == SEV_RANK.end() ? 9 : it->second, page, y);
    };
    std::stable_sort(findings.begin(), findings.end(), [&](const json& x, const json& y) { return sort_key(x) < sort_key(y); });
    for (size_t k = 0; k < findings.size(); ++k) findings[k]["id"] = k + 1;
    int hyp_r = 0, hyp_s = 0;
    for (auto& w : rw) hyp_r += w.hyphenated;
    for (auto& w : sw) hyp_s += w.hyphenated;
    auto labels = [](const doc::Document& d) {
        json v = json::array();
        for (auto& l : d.page_labels) v.push_back(l ? json(*l) : json(nullptr));
        return v;
    };
    size_t matched = equal_pairs.size();
    return {{"findings", findings}, {"style_map", style_map},
            {"stats", {{"ref_words", rw.size()}, {"sample_words", sw.size()}, {"matched_words", matched},
                       {"match_ratio", round_to(2.0 * matched / std::max<size_t>(1, rw.size() + sw.size()), 4)},
                       {"ref_pages", ref.pages.size()}, {"sample_pages", smp.pages.size()},
                       {"ref_furniture_words", ref.furniture.size()}, {"sample_furniture_words", smp.furniture.size()},
                       {"hyphenations_joined", {{"reference", hyp_r}, {"sample", hyp_s}}}}},
            {"page_labels", {{"reference", labels(ref)}, {"sample", labels(smp)}}}};
}

json run_document_job(const std::string& ref_path, const std::string& smp_path, const DocumentOptions& opt, const JobContext& ctx) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    ctx.progress(Stage::Render, 0.0);
    doc::LoadOptions lr, ls;
    lr.typography = ls.typography = opt.typography_normalize;
    lr.ignore_case = ls.ignore_case = opt.ignore_case;
    lr.furniture = ls.furniture = opt.furniture_ignore;
    lr.first_page = opt.ref_first;
    lr.last_page = opt.ref_last;
    ls.first_page = opt.smp_first;
    ls.last_page = opt.smp_last;
    auto ref = doc::load_file(ref_path, lr);
    ctx.progress(Stage::Render, 0.5);
    ctx.check();
    auto smp = doc::load_file(smp_path, ls);
    double t_load = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    ctx.progress(Stage::Diff, 0.0);
    json res = compare_documents(ref, smp, opt);
    double t_cmp = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() - t_load;
    json cmp = {{"mode", "document"}, {"layout", opt.layout}, {"furniture", opt.furniture_ignore ? "ignore" : "compare"},
                {"typography", opt.typography_normalize ? "normalize" : "strict"}, {"ignore_case", opt.ignore_case},
                {"min_move_words", opt.min_move_words}, {"move_tol_mm", opt.move_tol_mm}};
    json result = {{"schema", "printcmp.docresult/1"}, {"engine", {{"printcmp", VERSION}, {"core", "native"}}},
                   {"reference", {{"path", ref_path}}}, {"sample", {{"path", smp_path}}}, {"compare", cmp}};
    for (auto& [k, v] : res.items()) result[k] = v;
    if (opt.data_check) {
        ctx.progress(Stage::DataCheck, 0.0);
        const doc::Document& d = opt.data_check->source == "reference" ? ref : smp;
        result["data_check"] = run_data_check(*opt.data_check, &d, {});
        auto fn = std::filesystem::u8path(d.path).filename().u8string();
        result["data_check"]["checked"] = std::string(fn.begin(), fn.end());
    }
    json by_kind = json::object(), by_sev = json::object();
    for (auto& f : result["findings"]) {
        by_kind[f["kind"].get<std::string>()] = by_kind.value(f["kind"].get<std::string>(), 0) + 1;
        by_sev[f["severity"].get<std::string>()] = by_sev.value(f["severity"].get<std::string>(), 0) + 1;
    }
    result["summary"] = {{"differences", result["findings"].size()}, {"by_kind", by_kind}, {"by_severity", by_sev}};
    result["timing_ms"] = {{"load", (int)t_load}, {"compare", (int)t_cmp}};
    ctx.progress(Stage::Done, 1.0);
    return result;
}

}  // namespace pc::compare
