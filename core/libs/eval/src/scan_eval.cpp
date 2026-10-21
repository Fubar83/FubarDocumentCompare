// Utvärdering av scannade ark mot facit: träffar, falsklarm, klass, kategori, passning.
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "printcmp/eval/eval.hpp"

namespace pc::eval {

namespace {

using Box4 = std::array<double, 4>;

const std::map<std::string, std::set<std::string>> CLASS_OK = {
    {"char_change", {"Changed", "Shape", "Moved"}},   // bredare/smalare tecken flyttar resten av raden
    {"outlined_change", {"Changed", "Shape", "Moved"}},
    {"diacritic", {"Changed", "Missing", "Shape"}},
    {"word_delete", {"Missing", "Changed", "Moved"}},
    {"append_text", {"Added", "Changed"}},
    {"font_size", {"Shape", "Changed"}},
    {"move", {"Moved"}},
    {"delete", {"Missing"}},
    {"color_shift", {"Color"}},
    {"barcode_digit", {"Barcode"}},
    {"cell_value", {"Changed", "Shape"}},
    {"rule_delete", {"Missing"}},
    {"row_shift", {"Moved"}},
    {"braille_char", {"Braille"}},
};

Box4 B(const json& j) { return {j[0], j[1], j[2], j[3]}; }
Box4 expand(const Box4& b, double pad) { return {b[0] - pad, b[1] - pad, b[2] + 2 * pad, b[3] + 2 * pad}; }

double iou(const Box4& a, const Box4& b) {
    double ix = std::max(0.0, std::min(a[0] + a[2], b[0] + b[2]) - std::max(a[0], b[0]));
    double iy = std::max(0.0, std::min(a[1] + a[3], b[1] + b[3]) - std::max(a[1], b[1]));
    double inter = ix * iy, u = a[2] * a[3] + b[2] * b[3] - inter;
    return u > 0 ? inter / u : 0.0;
}
bool contains(const Box4& b, double x, double y) { return b[0] <= x && x <= b[0] + b[2] && b[1] <= y && y <= b[1] + b[3]; }

double match_score(const Box4& det, const Box4& truth, double pad = 0.3) {
    auto t = expand(truth, pad), d = expand(det, pad);
    double v = iou(d, t);
    if (v >= 0.1 || contains(t, det[0] + det[2] / 2, det[1] + det[3] / 2) || contains(d, truth[0] + truth[2] / 2, truth[1] + truth[3] / 2))
        return std::max(v, 0.1);
    return 0.0;
}

bool near_fold(const Box4& box, const json& lines, double band) {
    double cx = box[0] + box[2] / 2, cy = box[1] + box[3] / 2;
    for (auto& ln : lines) {
        double p0x = ln["p0_mm"][0], p0y = ln["p0_mm"][1], dx = ln["p1_mm"][0].get<double>() - p0x, dy = ln["p1_mm"][1].get<double>() - p0y;
        double t = ((cx - p0x) * dx + (cy - p0y) * dy) / std::max(dx * dx + dy * dy, 1e-9);
        double dist = std::hypot(p0x + t * dx - cx, p0y + t * dy - cy);
        if (dist <= std::max(band, ln.value("band_mm", 0.0)) + std::max(box[2], box[3]) / 2) return true;
    }
    return false;
}

// Rätt objektkategori (och tabellcell/-linje) för en träff.
bool category_ok(const json& t, const json& d) {
    std::string c = t["category"];
    json ti = d.contains("table") && !d["table"].is_null() ? d["table"] : json::object();
    if (c == "TableCell") {
        if (ti.value("element", "") != "cell") return false;
        auto r = t["table_cell"][0], col = t["table_cell"][1];
        return ti.value("row", json()) == r && (col.is_null() || ti.value("col", json()) == col || ti.value("col", json()).is_null());
    }
    if (c == "TableRule") return ti.value("element", "") == "rule" && ti.value("position", json()) == t.value("table_rule", json());
    if (c == "OutlinedText") return d.value("category", "") == "OutlinedText" && (d["severity"] == "Major" || d["severity"] == "Critical");
    if (c == "Braille") return d["kind"] == "Braille";
    return d.value("category", "") == c;
}

double homography_rms(const json& Hr, const json& Ht, double w, double h, int n = 8) {
    double s = 0;
    for (int iy = 0; iy < n; ++iy)
        for (int ix = 0; ix < n; ++ix) {
            double x = w * ix / (n - 1), y = h * iy / (n - 1);
            auto ap = [&](const json& H) {
                double X = H[0].get<double>() * x + H[1].get<double>() * y + H[2].get<double>();
                double Y = H[3].get<double>() * x + H[4].get<double>() * y + H[5].get<double>();
                double Z = H[6].get<double>() * x + H[7].get<double>() * y + H[8].get<double>();
                return std::pair<double, double>(X / Z, Y / Z);
            };
            auto [ax, ay] = ap(Hr);
            auto [bx, by] = ap(Ht);
            s += (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
        }
    return std::sqrt(s / (n * n));
}

double mean(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return s / v.size();
}

}  // namespace

std::vector<std::pair<int, int>> linear_sum_assignment(const std::vector<std::vector<double>>& cost) {
    // Ungerska metoden (potentialer), rader <= kolumner; transponera annars.
    int n = int(cost.size()), m = n ? int(cost[0].size()) : 0;
    if (!n || !m) return {};
    bool tr = n > m;
    auto C = [&](int i, int j) { return tr ? cost[j][i] : cost[i][j]; };
    if (tr) std::swap(n, m);
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> u(n + 1), v(m + 1);
    std::vector<int> p(m + 1), way(m + 1);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(m + 1, INF);
        std::vector<char> used(m + 1, 0);
        do {
            used[j0] = 1;
            int i0 = p[j0], j1 = 0;
            double delta = INF;
            for (int j = 1; j <= m; ++j)
                if (!used[j]) {
                    double cur = C(i0 - 1, j - 1) - u[i0] - v[j];
                    if (cur < minv[j]) minv[j] = cur, way[j] = j0;
                    if (minv[j] < delta) delta = minv[j], j1 = j;
                }
            for (int j = 0; j <= m; ++j)
                if (used[j]) u[p[j]] += delta, v[j] -= delta;
                else minv[j] -= delta;
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    std::vector<std::pair<int, int>> out;
    for (int j = 1; j <= m; ++j)
        if (p[j]) out.push_back(tr ? std::make_pair(j - 1, p[j] - 1) : std::make_pair(p[j] - 1, j - 1));
    std::sort(out.begin(), out.end());
    return out;
}

json evaluate_scan_case(const json& result, const json& truth) {
    double dpi = truth["dpi"], W = truth["label_size_mm"][0], H = truth["label_size_mm"][1];
    // instansmappning via rad/kolumn
    std::map<std::pair<int, int>, json> t_by_rc, r_by_rc;
    for (auto& i : truth["instances"]) t_by_rc[{i["row"], i["col"]}] = i;
    for (auto& i : result["instances"]) r_by_rc[{i["row"], i["col"]}] = i;
    std::map<int, int> inst_map;   // resultat-index -> facit-index
    std::vector<double> reg_err;
    for (auto& [rc, ti] : t_by_rc) {
        auto it = r_by_rc.find(rc);
        if (it == r_by_rc.end()) continue;
        inst_map[it->second["index"]] = ti["index"];
        if (ti.contains("folds") && !ti["folds"].empty()) continue;   // en transform kan inte beskriva ett veckat område
        reg_err.push_back(homography_rms(it->second["transform"]["h"], ti["transform_ref_to_sample"], W * dpi / 25.4, H * dpi / 25.4));
    }
    std::vector<int> t_instances;
    for (auto& i : truth["instances"]) t_instances.push_back(i["index"]);
    std::map<int, json> folds_by_inst;
    for (auto& u : truth.value("expect_uncertain", json::array())) folds_by_inst[u["instance"]] = u["lines"];
    // scanartefakter (damm på glaset): en detektion där är korrekt men inget tryckfel
    std::map<int, std::vector<Box4>> arts;
    for (auto& a : truth.value("scan_artifacts", json::array())) arts[a["instance"]].push_back(B(a["bbox_ref_mm"]));
    int scan_flagged = 0;

    struct Det {
        int inst;
        Box4 box;
        std::string kind;
        bool systematic;
        json id, severity, d;
    };
    std::vector<Det> dets;
    for (auto& d : result["differences"]) {
        if (!d.contains("bbox_mm") || d["bbox_mm"].is_null() || d["bbox_mm"].empty()) continue;
        for (auto& ri : d["instances"])
            if (inst_map.count(ri))
                dets.push_back({inst_map[ri], B(d["bbox_mm"]), d["kind"], d.value("systematic", false), d["id"], d["severity"], d});
    }
    struct Tru {
        int inst;
        Box4 box;
        std::string kind;
        std::set<std::string> accept;
        bool systematic;
        json id, mutation;
        std::string origin;
        json t;
    };
    std::vector<Tru> truths;
    for (auto& t : truth["defects"]) {
        std::vector<int> insts = t["systematic"].get<bool>() ? t_instances : t["instances"].get<std::vector<int>>();
        for (int ti : insts)
            truths.push_back({ti, B(t["bbox_ref_mm"]), t["kind"], t["accept"].get<std::set<std::string>>(), t["systematic"], t["id"],
                              t.value("mutation", json()), t["systematic"].get<bool>() ? "artwork" : "print", t});
    }

    std::vector<std::pair<const Tru*, const Det*>> hits;
    std::vector<const Det*> fa;
    int uncertain_ok = 0, uncertain_extra = 0;
    std::set<const Tru*> hit_t;
    for (int ti : t_instances) {
        std::vector<const Det*> D;
        std::vector<const Tru*> T;
        for (auto& d : dets)
            if (d.inst == ti) D.push_back(&d);
        for (auto& t : truths)
            if (t.inst == ti) T.push_back(&t);
        std::vector<std::vector<double>> S(T.size(), std::vector<double>(D.size(), 0.0));
        for (size_t a = 0; a < T.size(); ++a)
            for (size_t b = 0; b < D.size(); ++b) {
                if (D[b]->kind == "Uncertain" && !T[a]->systematic) continue;
                S[a][b] = (T[a]->accept.count(D[b]->kind) || D[b]->kind == "Uncertain") ? match_score(D[b]->box, T[a]->box) : 0.0;
            }
        std::set<size_t> matched_d;
        if (!T.empty() && !D.empty()) {
            std::vector<std::vector<double>> neg = S;
            for (auto& r : neg)
                for (auto& x : r) x = -x;
            for (auto [a, b] : linear_sum_assignment(neg))
                if (S[a][b] > 0) {
                    hits.push_back({T[a], D[b]});
                    hit_t.insert(T[a]);
                    matched_d.insert(b);
                }
        }
        for (auto* t : T)
            if (!hit_t.count(t)) hits.push_back({t, nullptr});
        // överskott: en detektion som överlappar ett redan träffat facit räknas inte som falsklarm
        for (size_t b = 0; b < D.size(); ++b) {
            if (matched_d.count(b)) continue;
            const auto* d = D[b];
            bool over = false;
            for (auto* t : T) over |= match_score(d->box, t->box, 0.6) > 0;
            if (over) continue;
            bool art = false;
            for (auto& a : arts[ti]) art |= match_score(d->box, a, 0.4) > 0;
            if (art) {
                ++scan_flagged;
                continue;
            }
            if (d->kind == "Uncertain") {
                if (near_fold(d->box, folds_by_inst.count(ti) ? folds_by_inst[ti] : json::array(), 3.0)) ++uncertain_ok;
                else ++uncertain_extra;
                continue;
            }
            fa.push_back(d);
        }
    }

    std::map<std::string, std::array<int, 2>> by_origin = {{"artwork", {0, 0}}, {"print", {0, 0}}};
    json by_mut = json::object();
    std::map<std::string, std::array<int, 2>> by_cat;
    std::vector<std::string> mut_order, cat_order;
    std::map<std::string, std::array<int, 2>> mut_counts;
    int cls_ok = 0, cls_n = 0, sys_ok = 0, sys_n = 0;
    json cls_misses = json::array(), cat_misses = json::array(), misses = json::array();
    for (auto& [t, d] : hits) {
        bool hit = d && d->kind != "Uncertain";
        std::string mk = t->mutation.is_null() ? "null" : t->mutation.get<std::string>();
        if (!mut_counts.count(mk)) mut_order.push_back(mk);
        by_origin[t->origin][1]++;
        mut_counts[mk][1]++;
        if (hit) {
            by_origin[t->origin][0]++;
            mut_counts[mk][0]++;
            if (t->origin == "artwork") {
                ++cls_n;
                auto it = CLASS_OK.find(mk);
                bool ok = it != CLASS_OK.end() ? it->second.count(d->kind) > 0 : d->kind == t->kind;
                cls_ok += ok;
                if (!ok) cls_misses.push_back({{"id", t->id}, {"mutation", t->mutation}, {"got", d->kind}});
            }
            ++sys_n;
            sys_ok += d->systematic == t->systematic;
            if (t->t.contains("category") && !t->t["category"].is_null()) {
                std::string c = t->t["category"];
                bool ok = category_ok(t->t, d->d);
                if (!by_cat.count(c)) cat_order.push_back(c);
                by_cat[c][1]++;
                by_cat[c][0] += ok;
                if (!ok)
                    cat_misses.push_back({{"id", t->id}, {"category", c}, {"got", d->d.value("category", json())},
                                          {"table", d->d.value("table", json())}, {"kind", d->kind}});
            }
        } else {
            misses.push_back({{"id", t->id}, {"inst", t->inst}, {"mutation", t->mutation}, {"box", t->box},
                              {"got", d ? json(d->kind) : json()}});
        }
    }
    for (auto& k : mut_order) by_mut[k] = mut_counts[k];
    json cats = json::object();
    for (auto& k : cat_order) cats[k] = by_cat[k];
    json fa_kinds = json::object();
    for (auto* d : fa) fa_kinds[d->kind] = fa_kinds.value(d->kind, 0) + 1;
    json fa_list = json::array();
    for (size_t i = 0; i < fa.size() && i < 50; ++i)
        fa_list.push_back({{"id", fa[i]->id}, {"inst", fa[i]->inst}, {"kind", fa[i]->kind},
                           {"box", {round_to(fa[i]->box[0], 2), round_to(fa[i]->box[1], 2), round_to(fa[i]->box[2], 2), round_to(fa[i]->box[3], 2)}}});
    if (cat_misses.size() > 20) cat_misses.erase(cat_misses.begin() + 20, cat_misses.end());
    int n_inst = int(t_instances.size());
    double rmax = 0;
    for (double e : reg_err) rmax = std::max(rmax, e);
    return {{"instances_expected", n_inst},
            {"instances_found", result["sheet"]["instances_found"]},
            {"instances_mapped", inst_map.size()},
            {"registration_rms_px", reg_err.empty() ? json() : json(mean(reg_err))},
            {"registration_rms_px_max", reg_err.empty() ? json() : json(rmax)},
            {"recall_artwork", by_origin["artwork"]},
            {"recall_print", by_origin["print"]},
            {"recall_by_mutation", by_mut},
            {"false_alarms", fa.size()},
            {"false_alarms_per_label", double(fa.size()) / std::max(1, n_inst)},
            {"false_alarm_kinds", fa_kinds},
            {"uncertain_ok", uncertain_ok},
            {"uncertain_extra", uncertain_extra},
            {"class_ok", {cls_ok, cls_n}},
            {"class_misses", cls_misses},
            {"scan_artifacts_flagged", scan_flagged},
            {"category_ok", cats},
            {"category_misses", cat_misses},
            {"systematic_ok", {sys_ok, sys_n}},
            {"misses", misses},
            {"false_alarm_list", fa_list}};
}

json aggregate_scan(const std::vector<json>& results) {
    auto ratio = [&](const char* key) {
        int a = 0, b = 0;
        for (auto& r : results) a += r[key][0].get<int>(), b += r[key][1].get<int>();
        return json{{"hit", a}, {"total", b}, {"rate", b ? json(double(a) / b) : json()}};
    };
    std::map<std::string, std::array<int, 2>> muts, cats;
    for (auto& r : results) {
        for (auto& [k, v] : r["recall_by_mutation"].items()) muts[k][0] += v[0].get<int>(), muts[k][1] += v[1].get<int>();
        json co = r.value("category_ok", json::object());   // inte items() på ett temporärt värde
        for (auto& [k, v] : co.items()) cats[k][0] += v[0].get<int>(), cats[k][1] += v[1].get<int>();
    }
    int labels = 0, found = 0, fa = 0, ue = 0;
    std::vector<double> reg;
    for (auto& r : results) {
        int e = r["instances_expected"];
        labels += e;
        found += std::min(r["instances_found"].get<int>(), e);
        fa += r["false_alarms"].get<int>();
        ue += r["uncertain_extra"].get<int>();
        if (!r["registration_rms_px"].is_null()) reg.push_back(r["registration_rms_px"]);
    }
    auto rates = [](const std::map<std::string, std::array<int, 2>>& m) {
        json o = json::object();
        for (auto& [k, v] : m) o[k] = {{"hit", v[0]}, {"total", v[1]}, {"rate", v[1] ? json(double(v[0]) / v[1]) : json()}};
        return o;
    };
    return {{"cases", results.size()},
            {"labels", labels},
            {"instances_found_rate", double(found) / std::max(1, labels)},
            {"recall_artwork", ratio("recall_artwork")},
            {"recall_print", ratio("recall_print")},
            {"classification", ratio("class_ok")},
            {"systematic_flag", ratio("systematic_ok")},
            {"false_alarms_per_label", double(fa) / std::max(1, labels)},
            {"uncertain_extra_per_label", double(ue) / std::max(1, labels)},
            {"registration_rms_px_mean", reg.empty() ? json() : json(mean(reg))},
            {"recall_by_mutation", rates(muts)},
            {"category", rates(cats)}};
}

}  // namespace pc::eval
