// Datakontroll: förväntade uppgifter ur en fil mot textlager och streckkoder.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "printcmp/compare/datacheck.hpp"
#include "printcmp/barcode/barcode.hpp"
#include "printcmp/pdf/document.hpp"
#include "printcmp/core/difflib.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/core/text.hpp"

namespace pc::compare {

namespace {

const Regex& barcode_field() {
    static Regex r("ean|gtin|upc|barcode|streckkod|qr|datamatrix|code ?128", true);
    return r;
}

DataItem make_item(const std::string& f0, const std::string& v0) {
    std::string field = text::strip(f0), value = text::strip(v0);
    bool rx = text::length(value) > 2 && text::starts_with(value, "/") && text::ends_with(value, "/");
    std::string v = rx ? value.substr(1, value.size() - 2) : value;
    static Regex code("\\d{8}|\\d{12,14}");
    bool is_code = barcode_field().search(field) || code.full_match(v);
    return {field.empty() ? v : field, v, rx, is_code};
}

std::vector<std::vector<std::string>> parse_csv(const std::string& txt, char delim) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    for (size_t i = 0; i < txt.size(); ++i) {
        char c = txt[i];
        if (quoted) {
            if (c == '"' && i + 1 < txt.size() && txt[i + 1] == '"') {
                cell += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cell += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == delim) {
            row.push_back(cell);
            cell.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < txt.size() && txt[i + 1] == '\n') ++i;
            row.push_back(cell);
            cell.clear();
            if (!(row.size() == 1 && row[0].empty())) rows.push_back(row);
            row.clear();
        } else {
            cell += c;
        }
    }
    if (!cell.empty() || !row.empty()) {
        row.push_back(cell);
        rows.push_back(row);
    }
    return rows;
}

std::vector<std::vector<DataItem>> rows_from_objects(const json& rows) {
    std::vector<std::vector<DataItem>> out;
    for (auto& r : rows) {
        std::vector<DataItem> items;
        for (auto& [k, v] : r.items()) {
            if (k.empty() || v.is_null() || (v.is_string() && v.get<std::string>().empty())) continue;
            items.push_back(make_item(k, v.is_string() ? v.get<std::string>() : v.dump()));
        }
        out.push_back(items);
    }
    return out;
}

struct Span { size_t a, b; const doc::Word* w; };
struct Page { int page; std::string text; std::vector<Span> spans; };

std::vector<Page> pages_of(const doc::Document& d, bool ignore_case, std::vector<doc::Word>& storage) {
    std::vector<doc::Word> all = d.words;
    all.insert(all.end(), d.furniture.begin(), d.furniture.end());
    std::stable_sort(all.begin(), all.end(), [](const doc::Word& x, const doc::Word& y) {
        return std::make_tuple(x.page, x.line, x.bbox.x0) < std::make_tuple(y.page, y.line, y.bbox.x0);
    });
    storage = doc::dehyphenate(all, doc::vocabulary({&d}));
    std::vector<Page> out;
    for (auto& w : storage) {
        if (out.empty() || out.back().page != w.page) out.push_back({w.page, "", {}});
        auto& pg = out.back();
        if (!pg.text.empty()) pg.text += " ";
        std::string t = doc::normalize_text(w.text, true, ignore_case);
        pg.spans.push_back({pg.text.size(), pg.text.size() + t.size(), &w});
        pg.text += t;
    }
    return out;
}

std::string norm_value(const std::string& v, bool ignore_case) {
    static Regex ws("\\s+");
    return text::strip(ws.replace(doc::normalize_text(v, true, ignore_case), " "));
}

json locate(const Page& pg, size_t s, size_t e) {
    std::vector<const doc::Word*> ws;
    for (auto& sp : pg.spans)
        if (sp.b > s && sp.a < e) ws.push_back(sp.w);
    if (ws.empty()) return {{"page", pg.page + 1}, {"bbox_mm", nullptr}, {"text", ""}};
    Box u;
    bool first = true;
    for (auto* w : ws)
        for (auto& [p, b] : w->boxes())
            if (p == pg.page) {
                u = first ? b : u.unite(b);
                first = false;
            }
    if (first) u = ws[0]->bbox;
    std::vector<std::string> t;
    for (auto* w : ws) t.push_back(w->text);
    return {{"page", pg.page + 1}, {"bbox_mm", {round_to(u.x0, 2), round_to(u.y0, 2), round_to(u.w(), 2), round_to(u.h(), 2)}},
            {"text", text::join(t, " ")}};
}

char32_t cp_before(const std::string& s, size_t k) {
    if (k == 0) return U' ';
    size_t p = k - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return text::decode(s.substr(p, k - p))[0];
}

char32_t cp_at(const std::string& s, size_t k) {
    if (k >= s.size()) return U' ';
    size_t e = k + 1;
    while (e < s.size() && (static_cast<unsigned char>(s[e]) & 0xC0) == 0x80) ++e;
    return text::decode(s.substr(k, e - k))[0];
}

std::string digits_only(const std::string& s) {
    static Regex nd("\\D");
    return nd.replace(s, "");
}

json find_item(const DataItem& it, const std::vector<Page>& pages, bool ignore_case) {
    if (it.regex) {
        Regex rx(it.value, ignore_case);
        json hits = json::array();
        for (auto& pg : pages)
            for (auto& m : rx.find_all(pg.text)) {
                json l = locate(pg, m.start, m.end);
                l["match"] = m.groups[0];
                hits.push_back(l);
            }
        json locs = json::array();
        for (size_t i = 0; i < hits.size() && i < 10; ++i) locs.push_back(hits[i]);
        return {{"status", hits.empty() ? "missing" : "found"}, {"count", hits.size()}, {"locations", locs}};
    }
    std::string v = norm_value(it.value, ignore_case);
    if (v.empty()) return {{"status", "missing"}, {"count", 0}, {"locations", json::array()}};
    auto vc = text::decode(v);
    json hits = json::array();
    for (auto& pg : pages) {
        size_t start = 0;
        while (true) {
            size_t k = pg.text.find(v, start);
            if (k == std::string::npos) break;
            char32_t before = cp_before(pg.text, k), after = cp_at(pg.text, k + v.size());
            if (!(text::is_alnum(before) && text::is_alnum(vc.front())) && !(text::is_alnum(after) && text::is_alnum(vc.back())))
                hits.push_back(locate(pg, k, k + v.size()));
            start = k + 1;
        }
    }
    if (!hits.empty()) {
        json locs = json::array();
        for (size_t i = 0; i < hits.size() && i < 10; ++i) locs.push_back(hits[i]);
        return {{"status", "found"}, {"count", hits.size()}, {"locations", locs}};
    }
    // nästan: samma antal ord (±1) från varje ord, bästa likhet
    size_t n_words = 1;
    for (char32_t c : vc) n_words += c == U' ';
    double best_r = -1;
    const Page* best_pg = nullptr;
    size_t bs = 0, be = 0;
    std::string best_cand;
    for (auto& pg : pages)
        for (size_t si = 0; si < pg.spans.size(); ++si)
            for (long n : {(long)n_words - 1, (long)n_words, (long)n_words + 1}) {
                if (n < 1 || si + n > pg.spans.size()) continue;
                size_t s = pg.spans[si].a, e = pg.spans[si + n - 1].b;
                std::string cand = pg.text.substr(s, e - s);
                auto cc = text::decode(cand);
                difflib::SequenceMatcher<char32_t> sm(vc, cc);
                if (sm.real_quick_ratio() < 0.7 || sm.quick_ratio() < 0.7) continue;
                double r = sm.ratio();
                if (r > best_r) {
                    best_r = r;
                    best_pg = &pg;
                    bs = s;
                    be = e;
                    best_cand = cand;
                }
            }
    if (best_pg && best_r >= 0.75) {
        json res = {{"status", "near"}, {"count", 0}, {"similarity", round_to(best_r, 3)}, {"found_text", best_cand},
                    {"diff", difflib::char_diff(v, best_cand)}, {"locations", {locate(*best_pg, bs, be)}}};
        static Regex other("[\\d\\s.,]");
        if (!digits_only(v).empty() && digits_only(v) == digits_only(best_cand) && other.replace(v, "") == other.replace(best_cand, ""))
            res["note"] = "samma siffror, annan formatering";
        return res;
    }
    return {{"status", "missing"}, {"count", 0}, {"locations", json::array()}};
}

bool ean_check_ok(const std::string& code) {
    // 12 siffror i en datafil är oftast en EAN-13 utan kontrollsiffra – inget att kontrollera
    if (!text::is_digit_str(code) || (code.size() != 8 && code.size() != 13 && code.size() != 14)) return true;
    int s = 0;
    size_t nb = code.size() - 1;
    for (size_t k = 0; k < nb; ++k) s += (code[k] - '0') * ((nb - k) % 2 == 1 ? 3 : 1);
    return (10 - s % 10) % 10 == code.back() - '0';
}

std::optional<json> check_barcode(const DataItem& it, const std::vector<json>& codes) {
    if (!it.barcode || codes.empty()) return std::nullopt;
    static Regex ws("\\s");
    std::string v = ws.replace(it.value, "");
    auto lstrip0 = [](std::string s) {
        size_t p = s.find_first_not_of('0');
        return p == std::string::npos ? std::string() : s.substr(p);
    };
    bool vd = text::is_digit_str(v);
    for (auto& c : codes) {
        std::string d = c["data"];
        bool dd = text::is_digit_str(d);
        if (d == v || (vd && dd && (lstrip0(d) == lstrip0(v) || (d.size() == v.size() + 1 && text::starts_with(d, v)))))
            return json{{"status", "found"}, {"data", d}, {"symbology", c.value("symbology", json())}, {"source", c.value("source", json())}};
    }
    for (auto& c : codes) {
        std::string d = c["data"];
        if (text::is_digit_str(d) == vd && std::abs((long)d.size() - (long)v.size()) <= 1)
            return json{{"status", "mismatch"}, {"data", d}, {"symbology", c.value("symbology", json())},
                        {"source", c.value("source", json())}, {"diff", difflib::char_diff(v, d)}};
    }
    return std::nullopt;
}

}  // namespace

std::vector<std::vector<DataItem>> load_expected(const std::string& path) {
    auto p = std::filesystem::u8path(path);
    std::string suf = text::lower(p.extension().string());
    std::string txt = read_file(path);
    if (text::starts_with(txt, "\xEF\xBB\xBF")) txt = txt.substr(3);
    if (suf == ".csv") {
        std::string first = txt.substr(0, txt.find('\n'));
        char delim = ',';
        size_t best = 0;
        for (char c : {',', ';', '\t'}) {
            size_t n = std::count(first.begin(), first.end(), c);
            if (n > best) {
                best = n;
                delim = c;
            }
        }
        auto rows = parse_csv(txt, delim);
        if (rows.empty()) return {};
        json objs = json::array();
        for (size_t r = 1; r < rows.size(); ++r) {
            json o = json::object();
            for (size_t c = 0; c < rows[0].size(); ++c) o[rows[0][c]] = c < rows[r].size() ? json(rows[r][c]) : json(nullptr);
            objs.push_back(o);
        }
        return rows_from_objects(objs);
    }
    if (suf == ".json") {
        json data = json::parse(txt);
        if (data.is_object()) return rows_from_objects(json::array({data}));
        if (!data.empty() && data[0].is_object()) {
            bool fv = true;
            for (auto& [k, v] : data[0].items()) fv &= (k == "field" || k == "value");
            if (fv) {
                std::vector<DataItem> items;
                for (auto& d : data)
                    items.push_back(make_item(d.value("field", ""), d["value"].is_string() ? d["value"].get<std::string>() : d["value"].dump()));
                return {items};
            }
        }
        return rows_from_objects(data);
    }
    std::vector<DataItem> items;
    static Regex kv("^([^:=/]{1,40}?)\\s*[:=]\\s+(.+)$");
    size_t pos = 0;
    while (pos <= txt.size()) {
        size_t e = txt.find('\n', pos);
        std::string ln = txt.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? txt.size() + 1 : e + 1;
        std::string s = text::strip(ln);
        if (s.empty() || s[0] == '#') continue;
        Regex::Match m;
        if (kv.full_match(s, &m)) items.push_back(make_item(m.groups[1], m.groups[2]));
        else items.push_back(make_item("", s));
    }
    return {items};
}

std::vector<json> codes_from_pdf(const std::string& path, int max_pages, double dpi) {
    auto d = pdf::Document::open_file(path);
    std::vector<json> out;
    for (int pi = 0; pi < std::min(d->page_count(), max_pages); ++pi) {
        cv::Mat rgb = pdf::render_page(*d, pi, dpi);
        for (auto& c : barcode::detect(rgb, dpi))
            out.push_back({{"data", c.data}, {"symbology", c.symbology}, {"source", "page " + std::to_string(pi + 1)}});
    }
    return out;
}

json data_check(const std::vector<std::vector<DataItem>>& variants, const doc::Document* d, const std::vector<json>& codes,
                const DataCheckOptions& opt) {
    std::vector<doc::Word> storage;
    std::vector<Page> pages;
    if (d) pages = pages_of(*d, opt.ignore_case, storage);
    auto run = [&](const std::vector<DataItem>& items) {
        json res = json::array();
        for (auto& it : items) {
            json r = pages.empty() ? json{{"status", "missing"}, {"count", 0}, {"locations", json::array()}} : find_item(it, pages, opt.ignore_case);
            if (auto bc = check_barcode(it, codes)) {
                r["barcode"] = *bc;
                if ((*bc)["status"] == "found") r["status"] = "found";
                else if (r["status"] != "found") r["status"] = "mismatch";
            }
            if (it.barcode && text::is_digit_str(it.value) && !ean_check_ok(it.value))
                r["warning"] = "kontrollsiffran i det förväntade värdet stämmer inte";
            json o = {{"field", it.field}, {"expected", it.value}, {"regex", it.regex}};
            for (auto& [k, v] : r.items()) o[k] = v;
            res.push_back(o);
        }
        return res;
    };
    int chosen = 0;
    std::string how = "only";
    if (variants.size() > 1) {
        if (opt.row) {
            chosen = std::clamp(*opt.row, 0, (int)variants.size() - 1);
            how = "row";
        } else if (!opt.row_match.empty() && opt.row_match.find('=') != std::string::npos) {
            std::string col = text::strip(opt.row_match.substr(0, opt.row_match.find('=')));
            std::string val = text::strip(opt.row_match.substr(opt.row_match.find('=') + 1));
            for (size_t k = 0; k < variants.size(); ++k)
                if (std::any_of(variants[k].begin(), variants[k].end(), [&](auto& i) { return i.field == col && i.value == val; })) {
                    chosen = (int)k;
                    break;
                }
            how = "match";
        } else {
            // bäst stämmande: summan av likheter (finns = 1, nästan = likheten); vid lika den första
            double best = -1;
            for (size_t k = 0; k < variants.size(); ++k) {
                auto rr = run(variants[k]);
                double s = 0;
                for (auto& x : rr) {
                    if (x["status"] == "found") s += 1.0;
                    else if (x["status"] == "near") s += x.value("similarity", 0.0);
                }
                s /= std::max<size_t>(1, rr.size());
                if (s > best) {
                    best = s;
                    chosen = (int)k;
                }
            }
            how = "best";
        }
    }
    json items = variants.empty() ? json::array() : run(variants[chosen]);
    json summary = {{"found", 0}, {"near", 0}, {"missing", 0}, {"mismatch", 0}};
    for (auto& x : items) summary[x["status"].get<std::string>()] = summary[x["status"].get<std::string>()].get<int>() + 1;
    return {{"variant", chosen}, {"variant_selection", how}, {"variants", variants.size()}, {"items", items}, {"summary", summary},
            {"ok", summary["found"].get<size_t>() == items.size()}};
}

json run_data_check(const DataCheckOptions& opt, const doc::Document* d, std::vector<json> codes) {
    auto variants = load_expected(opt.path);
    bool any_code = false;
    for (auto& v : variants)
        for (auto& i : v) any_code |= i.barcode;
    if (codes.empty() && d && !d->path.empty() && any_code) codes = codes_from_pdf(d->path);
    json res = data_check(variants, d, codes, opt);
    res["path"] = opt.path;
    return res;
}

}  // namespace pc::compare
