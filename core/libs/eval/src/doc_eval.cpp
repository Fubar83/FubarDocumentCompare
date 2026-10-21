// Utvärdering av dokumentjämförelse och datakontroll mot facit.
//
// En skillnad matchar en facitändring om typen stämmer och texten överlappar (ord, efter samma
// normalisering som jämförelsen). En facitändring kan förklara flera skillnader (t.ex. två rader
// som bytt plats); de räknas som dubbletter, inte falsklarm. Det som inte matchar är falsklarm.
#include <map>
#include <set>

#include "printcmp/core/text.hpp"
#include "printcmp/doc/doctext.hpp"
#include "printcmp/eval/eval.hpp"

namespace pc::eval {

namespace {

const std::map<std::string, std::set<std::string>> ACCEPT = {
    {"Changed", {"Changed"}}, {"Missing", {"Missing", "Changed"}}, {"Added", {"Added", "Changed"}},
    {"Moved", {"Moved"}},     {"Style", {"Style", "StyleSystematic"}}, {"PageNumber", {"PageNumber"}}};
const std::map<std::string, std::set<std::string>> ATTR_OK = {
    {"bold", {"bold", "family"}}, {"italic", {"italic"}}, {"size", {"size"}}, {"family", {"family"}}, {"color", {"color"}}};

std::vector<std::string> toks(const std::string& s) { return text::split_ws(doc::normalize_text(s)); }

// andel av a:s ord som finns i b (ordningsoberoende)
double overlap(const std::string& a, const std::string& b) {
    auto ta = toks(a), tb = toks(b);
    if (ta.empty()) return 0.0;
    std::multiset<std::string> pool(tb.begin(), tb.end());
    int hit = 0;
    for (auto& t : ta) {
        auto it = pool.find(t);
        if (it != pool.end()) {
            pool.erase(it);
            ++hit;
        }
    }
    return double(hit) / ta.size();
}

std::string str(const json& j, const char* k) {
    if (!j.contains(k) || j[k].is_null()) return "";
    return j[k].get<std::string>();
}

bool matches(const json& m, const json& f) {
    std::string mk = m["kind"];
    auto acc = ACCEPT.find(mk);
    if (acc == ACCEPT.end() || !acc->second.count(f["kind"].get<std::string>())) return false;
    json t = f.contains("text") && !f["text"].is_null() ? f["text"] : json::object();
    std::string frm = str(t, "from"), to = str(t, "to");
    std::string ref = str(m, "ref_text"), smp = str(m, "smp_text");
    if (mk == "PageNumber") {
        json loc = f.contains("sample") && !f["sample"].is_null() ? f["sample"] : json::object();
        return loc.value("page", json()) == m.value("page", json());
    }
    if (mk == "Changed") return (!ref.empty() && overlap(ref, frm) == 1.0) || (!smp.empty() && overlap(smp, to) == 1.0);
    if (mk == "Missing") return overlap(ref, frm) >= 0.6 || (!toks(frm).empty() && overlap(frm, ref) == 1.0);
    if (mk == "Added") return overlap(smp, to) == 1.0;
    if (mk == "Moved") {
        std::vector<std::string> texts = {ref};
        if (!str(m, "other_text").empty()) texts.push_back(str(m, "other_text"));
        for (auto& x : texts)
            if (!x.empty() && (overlap(frm, x) >= 0.6 || overlap(x, frm) >= 0.6)) return true;
        return false;
    }
    if (mk == "Style") {
        if (!(overlap(frm, ref) >= 0.6 || overlap(ref, frm) >= 0.6)) return false;
        std::set<std::string> changed;
        if (f.contains("style") && f["style"].is_object())
            for (auto& c : f["style"].value("changed", json::array())) changed.insert(c);
        std::string attr = str(m, "attr");
        auto ok = ATTR_OK.count(attr) ? ATTR_OK.at(attr) : std::set<std::string>{attr};
        for (auto& c : changed)
            if (ok.count(c)) return true;
        return false;
    }
    return false;
}

}  // namespace

json evaluate_doc_case(const json& res, const json& truth) {
    const auto& muts = truth["mutations"];
    std::map<std::string, std::vector<json>> found;
    std::vector<json> fa;
    int dup = 0;
    for (auto& f : res["findings"]) {
        std::vector<std::string> hits;
        for (auto& m : muts)
            if (matches(m, f)) hits.push_back(m["id"]);
        if (hits.empty()) {
            fa.push_back(f);
            continue;
        }
        std::string first_new;
        for (auto& h : hits)
            if (found[h].empty()) {
                first_new = h;
                break;
            }
        if (first_new.empty()) {
            ++dup;
            found[hits[0]].push_back(f["id"]);
        } else found[first_new].push_back(f["id"]);
    }
    std::vector<std::string> order;
    std::map<std::string, std::array<int, 2>> by_mut;
    json misses = json::array();
    int n_found = 0;
    for (auto& m : muts) {
        std::string k = m["mutation"];
        if (!by_mut.count(k)) order.push_back(k);
        by_mut[k][1]++;
        if (!found[m["id"]].empty()) {
            by_mut[k][0]++;
            ++n_found;
        } else {
            json x = json::object();
            for (const char* key : {"id", "mutation", "kind", "ref_text", "smp_text", "page"}) x[key] = m.value(key, json());
            misses.push_back(x);
        }
    }
    json bm = json::object();
    for (auto& k : order) bm[k] = by_mut[k];
    json fal = json::array();
    for (auto& f : fa)
        fal.push_back({{"kind", f["kind"]}, {"text", f.contains("text") && !f["text"].is_null() ? f["text"] : json::object()},
                       {"style", f.value("style", json())}});
    json out = {{"type", truth["type"]},
                {"profile", truth.contains("profile") ? truth["profile"] : truth.value("template", json())},
                {"mutations", muts.size()},
                {"found", n_found},
                {"false_alarms", fa.size()},
                {"duplicates", dup},
                {"by_mutation", bm},
                {"misses", misses},
                {"false_alarm_list", fal}};
    if (truth.contains("data_check") && res.contains("data_check") && !res["data_check"].is_null()) {
        const auto& dct = truth["data_check"];
        const auto& dc = res["data_check"];
        std::map<std::string, std::string> items;
        for (auto& it : dc["items"]) items[it["field"]] = it["status"];
        int ok = 0, n = 0;
        json wrong = json::array();
        for (auto& [k, v] : dct["items"].items()) {
            ++n;
            std::string got = items.count(k) ? items[k] : "";
            if (got == v.get<std::string>()) ++ok;
            else wrong.push_back({k, got.empty() ? json() : json(got), v});
        }
        out["data"] = {{"items", n}, {"correct", ok}, {"variant_ok", dc["variant"] == dct["variant"]}, {"wrong", wrong}};
    }
    return out;
}

json aggregate_doc(const std::vector<json>& results) {
    json agg = {{"cases", results.size()}};
    for (const char* typ : {"book", "label"}) {
        std::vector<const json*> rs;
        for (auto& r : results)
            if (r["type"] == typ) rs.push_back(&r);
        if (rs.empty()) continue;
        std::map<std::string, std::array<int, 2>> bm;
        int found = 0, muts = 0, fa = 0, dup = 0;
        for (auto* r : rs) {
            for (auto& [k, v] : (*r)["by_mutation"].items()) bm[k][0] += v[0].get<int>(), bm[k][1] += v[1].get<int>();
            found += (*r)["found"].get<int>();
            muts += (*r)["mutations"].get<int>();
            fa += (*r)["false_alarms"].get<int>();
            dup += (*r)["duplicates"].get<int>();
        }
        json bmj = json::object();
        for (auto& [k, v] : bm) bmj[k] = {{"hit", v[0]}, {"total", v[1]}};
        json a = {{"cases", rs.size()}, {"found", found}, {"mutations", muts}, {"false_alarms", fa}, {"duplicates", dup}, {"by_mutation", bmj}};
        int items = 0, correct = 0, vok = 0, files = 0;
        for (auto* r : rs)
            if (r->contains("data")) {
                items += (*r)["data"]["items"].get<int>();
                correct += (*r)["data"]["correct"].get<int>();
                vok += (*r)["data"]["variant_ok"].get<bool>();
                ++files;
            }
        if (files) a["data"] = {{"items", items}, {"correct", correct}, {"variants_ok", vok}, {"files", files}};
        agg[typ] = a;
    }
    return agg;
}

}  // namespace pc::eval
