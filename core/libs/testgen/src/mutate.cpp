// Felkatalog (avsnitt 9.3): mutationer på beskrivningsnivå med exakt facit.
//
// Varje mutation returnerar en ny spec och ett facitfel:
//   {kind, accept, element, detail, bbox_ref_mm[, category, table_cell, table_rule]}
// `kind` är förväntad klass; `accept` är klasser som räknas som träff i detektionen.
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "printcmp/testgen/cases.hpp"
#include "util.hpp"

namespace pc::testgen {

const std::vector<std::string> STRUCT_ANY = {"Missing", "Added", "Changed", "Moved", "Shape", "Color"};

namespace {

using Result = std::optional<std::pair<json, json>>;

const std::map<char32_t, char32_t> LOOKALIKE = {
    {U'O', U'0'}, {U'0', U'O'}, {U'o', U'e'}, {U'e', U'c'}, {U'c', U'o'}, {U'l', U'1'}, {U'1', U'l'}, {U'I', U'l'},
    {U'i', U'j'}, {U'a', U'o'}, {U'n', U'm'}, {U'm', U'n'}, {U'u', U'v'}, {U'v', U'u'}, {U'r', U'n'}, {U't', U'f'},
    {U'f', U't'}, {U'S', U'5'}, {U'5', U'S'}, {U'B', U'8'}, {U'8', U'B'}, {U's', U'z'}, {U'k', U'h'}, {U'h', U'b'},
    {U'd', U'b'}, {U'b', U'd'}, {U'g', U'q'}, {U'p', U'q'}, {U'E', U'F'}, {U'F', U'E'}, {U'H', U'N'}, {U'N', U'H'},
    {U'V', U'Y'}, {U'2', U'7'}, {U'3', U'8'}, {U'4', U'9'}, {U'6', U'8'}, {U'9', U'4'}, {U'7', U'1'}};
const std::map<char32_t, char32_t> DIACRITIC = {{U'å', U'a'}, {U'ä', U'a'}, {U'ö', U'o'}, {U'Å', U'A'},
                                                 {U'Ä', U'A'}, {U'Ö', U'O'}, {U'é', U'e'}, {U'ó', U'o'}};

json box_json(const Rect& r) { return json::array({r[0], r[1], r[2], r[3]}); }
Rect box_of(const json& j) { return {j[0], j[1], j[2], j[3]}; }

std::vector<json> of_types(const json& spec, std::initializer_list<const char*> types) {
    std::vector<json> out;
    for (auto& e : spec["elements"])
        for (auto t : types)
            if (e["type"] == t) out.push_back(e);
    return out;
}
std::vector<json> texts(const json& spec) {
    std::vector<json> out;
    for (auto& e : spec["elements"])
        if (e["type"] == "text" || e["type"] == "outlined") out.push_back(e);
    return out;
}
json& find(json& spec, const std::string& id) {
    for (auto& e : spec["elements"])
        if (e["id"] == id) return e;
    throw Error(ErrorCode::NotFound, "element saknas: " + id);
}

bool inside(const json& spec, const Rect& b, double margin) {
    double W = spec["size_mm"][0], H = spec["size_mm"][1];
    return b[0] >= margin && b[1] >= margin && b[0] + b[2] <= W - margin && b[1] + b[3] <= H - margin;
}

std::vector<std::string> split_space(const std::u32string& s) {   // Pythons str.split(" ")
    std::vector<std::string> out;
    std::u32string cur;
    for (char32_t c : s) {
        if (c == U' ') {
            out.push_back(u8(cur));
            cur.clear();
        } else cur += c;
    }
    out.push_back(u8(cur));
    return out;
}

Result swap_char(const json& spec, Rng& rng, const std::map<char32_t, char32_t>& table, bool line_end_always_check) {
    std::vector<std::pair<json, size_t>> cands;
    for (auto& e : texts(spec)) {
        auto t = u32(e["text"].get<std::string>());
        for (size_t i = 0; i < t.size(); ++i)
            if (table.count(t[i])) cands.push_back({e, i});
    }
    if (cands.empty()) return std::nullopt;
    auto [e, i] = cands[rng.index(cands.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    auto old_box = char_bbox_mm(el, i);
    auto t = u32(el["text"].get<std::string>());
    char32_t old = t[i], neu = table.at(old);
    t[i] = neu;
    el["text"] = u8(t);
    auto new_box = char_bbox_mm(el, i);
    Rect box = rect_union(old_box, new_box);
    // om bredden ändras flyttas resten av raden: facit täcker till radens slut
    double w_ref = line_end_always_check ? box[2] : new_box[2];
    if (std::fabs(old_box[2] - w_ref) > 0.05) box = rect_union(box, char_bbox_mm(el, i, t.size()));
    return std::make_pair(s, json{{"kind", "Changed"}, {"accept", STRUCT_ANY}, {"element", e["id"]},
                                  {"detail", {{"from", u8(old)}, {"to", u8(neu)}, {"index", int(i)}}},
                                  {"bbox_ref_mm", box_json(box)}});
}

Result char_change(const json& spec, Rng& rng) { return swap_char(spec, rng, LOOKALIKE, false); }
Result diacritic(const json& spec, Rng& rng) { return swap_char(spec, rng, DIACRITIC, true); }

Result word_delete(const json& spec, Rng& rng) {
    std::vector<json> cands;
    for (auto& e : texts(spec))
        if (split_space(u32(e["text"].get<std::string>())).size() >= 3) cands.push_back(e);
    if (cands.empty()) return std::nullopt;
    auto e = cands[rng.index(cands.size())];
    auto words = split_space(u32(e["text"].get<std::string>()));
    int k = int(rng.integers(1, int64_t(words.size())));
    size_t start = u32(text::join(std::vector<std::string>(words.begin(), words.begin() + k), " ")).size();
    json s = spec;
    json& el = find(s, e["id"]);
    size_t n_old = u32(el["text"].get<std::string>()).size();
    auto old_end = char_bbox_mm(el, start, n_old);
    std::string removed = words[k];
    words.erase(words.begin() + k);
    el["text"] = text::join(words, " ");
    size_t n_new = u32(el["text"].get<std::string>()).size();
    auto new_end = char_bbox_mm(el, std::min(start, n_new - 1), n_new);
    return std::make_pair(s, json{{"kind", "Missing"}, {"accept", STRUCT_ANY}, {"element", e["id"]},
                                  {"detail", {{"removed", removed}}}, {"bbox_ref_mm", box_json(rect_union(old_end, new_end))}});
}

Result append_text(const json& spec, Rng& rng) {
    double W = spec["size_mm"][0];
    const std::u32string pool = U"abcdefghkmnprstuvxyz0123456789";
    std::vector<std::pair<json, std::string>> cands;
    for (auto& e : texts(spec)) {
        int n = int(rng.integers(2, 6));
        std::u32string add = U" ";
        for (int k = 0; k < n; ++k) add += pool[rng.index(pool.size())];
        if (e["pos_mm"][0].get<double>() + text_width_mm(e["font"], e["size_pt"], u32(e["text"].get<std::string>()) + add) < W - 3)
            cands.push_back({e, u8(add)});
    }
    if (cands.empty()) return std::nullopt;
    auto [e, add] = cands[rng.index(cands.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    size_t n = u32(el["text"].get<std::string>()).size();
    el["text"] = el["text"].get<std::string>() + add;
    auto box = char_bbox_mm(el, n, u32(el["text"].get<std::string>()).size());
    return std::make_pair(s, json{{"kind", "Added"}, {"accept", STRUCT_ANY}, {"element", e["id"]},
                                  {"detail", {{"added", add}}}, {"bbox_ref_mm", box_json(box)}});
}

Result font_size(const json& spec, Rng& rng) {
    double W = spec["size_mm"][0];
    auto cands = texts(spec);
    rng.shuffle(cands);
    for (auto& e : cands) {
        double f = 1 + rng.sign() * rng.uniform(0.08, 0.15);
        json s = spec;
        json& el = find(s, e["id"]);
        auto old = element_bbox_mm(el);
        el["size_pt"] = round_to(el["size_pt"].get<double>() * f, 2);
        auto neu = element_bbox_mm(el);
        if (neu[0] + neu[2] < W - 2)
            return std::make_pair(s, json{{"kind", "Shape"}, {"accept", STRUCT_ANY}, {"element", e["id"]},
                                          {"detail", {{"factor", round_to(f, 3)}}}, {"bbox_ref_mm", box_json(rect_union(old, neu))}});
    }
    return std::nullopt;
}

Result move(const json& spec, Rng& rng) {
    auto movable = of_types(spec, {"text", "outlined", "circle", "poly", "ean13", "qr"});
    // of_types grupperar efter typ; behåll ritordningen
    std::vector<json> ordered;
    for (auto& e : spec["elements"])
        for (auto& m : movable)
            if (m["id"] == e["id"]) ordered.push_back(e);
    rng.shuffle(ordered);
    auto acc = STRUCT_ANY;
    acc.push_back("Barcode");
    for (auto& e : ordered) {
        double d = rng.uniform(0.3, 2.5), a = rng.uniform(0, 2 * M_PI);
        double dx = d * std::cos(a), dy = d * std::sin(a);
        json s = spec;
        json& el = find(s, e["id"]);
        auto old = element_bbox_mm(el);
        std::string t = el["type"];
        if (t == "text" || t == "outlined") el["pos_mm"] = {el["pos_mm"][0].get<double>() + dx, el["pos_mm"][1].get<double>() + dy};
        else if (t == "circle") el["center_mm"] = {el["center_mm"][0].get<double>() + dx, el["center_mm"][1].get<double>() + dy};
        else if (t == "poly")
            for (auto& p : el["points_mm"]) p = {p[0].get<double>() + dx, p[1].get<double>() + dy};
        else el["rect_mm"] = {el["rect_mm"][0].get<double>() + dx, el["rect_mm"][1].get<double>() + dy, el["rect_mm"][2], el["rect_mm"][3]};
        auto neu = element_bbox_mm(el);
        if (inside(spec, neu, 1.5))
            return std::make_pair(s, json{{"kind", "Moved"}, {"accept", acc}, {"element", e["id"]},
                                          {"detail", {{"dx_mm", round_to(dx, 3)}, {"dy_mm", round_to(dy, 3)}, {"shift_mm", round_to(d, 3)}}},
                                          {"bbox_ref_mm", box_json(rect_union(old, neu))}});
    }
    return std::nullopt;
}

Result remove(const json& spec, Rng& rng) {
    std::vector<json> cands;
    for (auto& e : spec["elements"]) {
        std::string t = e["type"];
        if ((t == "circle" || t == "poly" || t == "text") && e["id"] != "title") cands.push_back(e);
    }
    if (cands.empty()) return std::nullopt;
    auto e = cands[rng.index(cands.size())];
    json s = spec;
    json els = json::array();
    for (auto& x : s["elements"])
        if (x["id"] != e["id"]) els.push_back(x);
    s["elements"] = els;
    return std::make_pair(s, json{{"kind", "Missing"}, {"accept", STRUCT_ANY}, {"element", e["id"]}, {"detail", json::object()},
                                  {"bbox_ref_mm", box_json(element_bbox_mm(e))}});
}

Result color_shift(const json& spec, Rng& rng) {
    double W = spec["size_mm"][0], H = spec["size_mm"][1];
    std::vector<json> cands;
    for (auto& e : spec["elements"]) {
        std::string t = e["type"];
        if (!(t == "rect" || t == "circle" || t == "poly") || !e.contains("color")) continue;
        auto v = e["color"]["cmyk"].get<std::vector<double>>();
        auto b = element_bbox_mm(e);
        // färgfel på ytor > 40 % av etiketten kalibreras bort av självkalibreringen
        if (v[0] + v[1] + v[2] + v[3] > 0.05 && b[2] * b[3] < 0.4 * W * H) cands.push_back(e);
    }
    if (cands.empty()) return std::nullopt;
    auto e = cands[rng.index(cands.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    auto cmyk = el["color"]["cmyk"].get<std::vector<double>>();
    int ch = int(rng.integers(0, 3));
    double delta = rng.sign() * rng.uniform(0.12, 0.3);
    double orig = e["color"]["cmyk"][ch];
    cmyk[ch] = std::clamp(cmyk[ch] + delta, 0.0, 1.0);
    if (std::fabs(cmyk[ch] - orig) < 0.08) cmyk[ch] = std::clamp(orig - delta, 0.0, 1.0);
    el["color"]["cmyk"] = cmyk;
    return std::make_pair(s, json{{"kind", "Color"}, {"accept", STRUCT_ANY}, {"element", e["id"]},
                                  {"detail", {{"channel", std::string(1, "CMY"[ch])}, {"from", orig}, {"to", cmyk[ch]}}},
                                  {"bbox_ref_mm", box_json(element_bbox_mm(e))}});
}

Result barcode_digit(const json& spec, Rng& rng) {
    auto cands = of_types(spec, {"ean13"});
    if (cands.empty()) return std::nullopt;
    auto e = cands[0];
    json s = spec;
    json& el = find(s, e["id"]);
    std::string d = el["data"].get<std::string>().substr(0, 12);
    int i = int(rng.integers(1, 12));
    d[i] = char('0' + ((d[i] - '0') + int(rng.integers(1, 9))) % 10);
    el["data"] = d;
    return std::make_pair(s, json{{"kind", "Barcode"}, {"accept", {"Barcode", "Changed", "Shape", "Missing", "Added"}},
                                  {"element", e["id"]}, {"detail", {{"from", e["data"]}, {"to", d}}}, {"bbox_ref_mm", e["rect_mm"]}});
}

// ------------------------------------------------------------------ tabeller

Result cell_value(const json& spec, Rng& rng) {
    auto ts = of_types(spec, {"table"});
    if (ts.empty()) return std::nullopt;
    auto e = ts[rng.index(ts.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    std::vector<TableCell> cands;
    for (auto& c : table_layout(el).cells)
        if (c.row >= 1 && std::any_of(c.text.begin(), c.text.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) cands.push_back(c);
    if (cands.empty()) return std::nullopt;
    auto c = cands[rng.index(cands.size())];
    std::string txt = c.text;   // siffror och tecken: ASCII i cellerna som kan ändras
    std::vector<int> idx;
    for (int i = 0; i < int(txt.size()); ++i)
        if (txt[i] >= '0' && txt[i] <= '9') idx.push_back(i);
    int i = idx[rng.index(idx.size())];
    std::string nt = txt;
    nt[i] = char('0' + ((txt[i] - '0') + int(rng.integers(1, 9))) % 10);
    auto old_box = cell_text_bbox_mm(c);
    el["rows"][c.row][c.col] = nt;
    TableCell c2;
    for (auto& x : table_layout(el).cells)
        if (x.row == c.row && x.col == c.col) c2 = x;
    return std::make_pair(s, json{{"kind", "Changed"}, {"accept", STRUCT_ANY}, {"element", e["id"]}, {"category", "TableCell"},
                                  {"table_cell", {c.row, c.col}}, {"detail", {{"from", txt}, {"to", nt}}},
                                  {"bbox_ref_mm", box_json(rect_union(old_box, cell_text_bbox_mm(c2)))}});
}

Result rule_delete(const json& spec, Rng& rng) {
    std::vector<json> ts;
    for (auto& e : of_types(spec, {"table"}))
        if (e.value("rules", "grid") != "none") ts.push_back(e);
    if (ts.empty()) return std::nullopt;
    auto e = ts[rng.index(ts.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    std::vector<TableRuleGeom> hs;
    int n = int(el["rows"].size());
    for (auto& r : table_layout(el).rules)
        if (r.orient == 'h' && r.index > 0 && r.index < n) hs.push_back(r);
    if (hs.empty()) return std::nullopt;
    auto r = hs[rng.index(hs.size())];
    if (!el.contains("skip_rules")) el["skip_rules"] = json::array();
    el["skip_rules"].push_back(r.index);
    return std::make_pair(s, json{{"kind", "Missing"}, {"accept", {"Missing", "Changed", "Shape", "Moved"}}, {"element", e["id"]},
                                  {"category", "TableRule"}, {"table_rule", r.index}, {"detail", {{"rule", r.index}}},
                                  {"bbox_ref_mm", box_json(rule_bbox_mm(r, el.value("rule_w_mm", 0.15)))}});
}

Result row_shift(const json& spec, Rng& rng) {
    auto ts = of_types(spec, {"table"});
    if (ts.empty()) return std::nullopt;
    auto e = ts[rng.index(ts.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    int r = int(rng.integers(1, int64_t(el["rows"].size())));
    double dx = rng.sign() * rng.uniform(0.3, 1.0);
    std::vector<Rect> boxes;
    for (auto& c : table_layout(el).cells)
        if (c.row == r) boxes.push_back(cell_text_bbox_mm(c));
    if (!el.contains("row_dx_mm")) el["row_dx_mm"] = json::object();
    el["row_dx_mm"][std::to_string(r)] = dx;
    for (auto& c : table_layout(el).cells)
        if (c.row == r) boxes.push_back(cell_text_bbox_mm(c));
    Rect box = boxes[0];
    for (auto& b : boxes) box = rect_union(box, b);
    return std::make_pair(s, json{{"kind", "Moved"}, {"accept", STRUCT_ANY}, {"element", e["id"]}, {"category", "TableCell"},
                                  {"table_cell", {r, nullptr}}, {"detail", {{"row", r}, {"dx_mm", round_to(dx, 3)}}},
                                  {"bbox_ref_mm", box_json(box)}});
}

// ------------------------------------------------------------------ punktskrift

Result braille_char(const json& spec, Rng& rng) {
    auto bs = of_types(spec, {"braille"});
    if (bs.empty()) return std::nullopt;
    auto e = bs[rng.index(bs.size())];
    auto t = u32(e["text"].get<std::string>());
    std::vector<size_t> letters;
    for (size_t i = 0; i < t.size(); ++i)
        if (text::is_alpha(t[i])) letters.push_back(i);
    if (letters.empty()) return std::nullopt;
    size_t i = letters[rng.index(letters.size())];
    char32_t old = t[i];
    std::u32string pool;
    for (char32_t ch : std::u32string(U"abcdefghijklmnopqrstuvxyz"))
        if (ch != old) pool += ch;
    char32_t neu = pool[rng.index(pool.size())];
    json s = spec;
    json& el = find(s, e["id"]);
    auto nt = t;
    nt[i] = neu;
    el["text"] = u8(nt);
    int ci = braille_cell_count(u8(t.substr(0, i)));
    return std::make_pair(s, json{{"kind", "Braille"}, {"accept", {"Braille"}}, {"element", e["id"]}, {"category", "Braille"},
                                  {"detail", {{"from", u8(old)}, {"to", u8(neu)}, {"cell", ci}}},
                                  {"bbox_ref_mm", box_json(braille_cell_bbox_mm(el, ci))}});
}

// Teckenbyte i konturerad text (för riktade tester).
Result outlined_change(const json& spec, Rng& rng) {
    json only = spec;
    only["elements"] = json::array();
    for (auto& e : spec["elements"])
        if (e["type"] == "outlined") only["elements"].push_back(e);
    auto r = char_change(only, rng);
    if (!r) return std::nullopt;
    auto& [s_only, d] = *r;
    json s = spec;
    find(s, d["element"]) = find(s_only, d["element"]);
    d["category"] = "OutlinedText";
    return std::make_pair(s, d);
}

using Fn = std::function<Result(const json&, Rng&)>;
const std::vector<std::pair<std::string, Fn>>& catalog() {
    static const std::vector<std::pair<std::string, Fn>> c = {
        {"char_change", char_change}, {"diacritic", diacritic}, {"word_delete", word_delete}, {"append_text", append_text},
        {"font_size", font_size},     {"move", move},           {"delete", remove},           {"color_shift", color_shift},
        {"barcode_digit", barcode_digit}, {"cell_value", cell_value}, {"rule_delete", rule_delete}, {"row_shift", row_shift},
        {"braille_char", braille_char}};
    return c;
}

Fn lookup(const std::string& name) {
    for (auto& [n, f] : catalog())
        if (n == name) return f;
    if (name == "outlined_change") return outlined_change;
    throw Error(ErrorCode::InvalidArgument, "okänd mutation: " + name);
}

bool overlaps(const Rect& a, const Rect& b, double pad = 1.0) {
    return !(a[0] - pad > b[0] + b[2] || b[0] - pad > a[0] + a[2] || a[1] - pad > b[1] + b[3] || b[1] - pad > a[1] + a[3]);
}

}  // namespace

std::vector<std::string> mutation_names() {
    std::vector<std::string> n;
    for (auto& [k, _] : catalog()) n.push_back(k);
    return n;
}

std::pair<json, json> mutate_label(json spec, Rng& rng, int n, const std::vector<std::string>& allowed) {
    auto names = allowed.empty() ? mutation_names() : allowed;
    json defects = json::array();
    std::set<std::string> used;
    for (int tries = 0; int(defects.size()) < n && tries < 60; ++tries) {
        const auto& name = names[rng.index(names.size())];
        auto r = lookup(name)(spec, rng);
        if (!r) continue;
        auto& [s, d] = *r;
        std::string el = d["element"];
        if (used.count(el) || el == "lot") continue;
        Rect box = box_of(d["bbox_ref_mm"]);
        bool clash = false;
        for (auto& x : defects) clash |= overlaps(box, box_of(x["bbox_ref_mm"]));
        if (clash) continue;
        spec = s;
        used.insert(el);
        if (!d.contains("category"))
            for (auto& x : spec["elements"])
                if (x["id"] == el) {
                    if (x["type"] == "text") d["category"] = "Text";
                    else if (x["type"] == "outlined") d["category"] = "OutlinedText";
                }
        json rounded = json::array();
        for (auto& v : d["bbox_ref_mm"]) rounded.push_back(round_to(v.get<double>(), 3));
        d["bbox_ref_mm"] = rounded;
        d["id"] = "a" + std::to_string(defects.size() + 1);
        d["mutation"] = name;
        d["systematic"] = true;
        defects.push_back(d);
    }
    return {spec, defects};
}

}  // namespace pc::testgen
