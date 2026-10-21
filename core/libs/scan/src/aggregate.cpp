// Sammanställning av fynd över etiketterna.
#include "printcmp/scan/aggregate.hpp"

#include "printcmp/scan/severity.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace pc::scan {

std::vector<json> aggregate(std::vector<json> diffs, int n_instances, double ratio, double cluster_mm) {
    if (n_instances <= 1) {
        for (auto& d : diffs) {
            d["systematic"] = false;
            d["instances"] = json::array({d["instance"]});
        }
        return diffs;
    }
    std::vector<json> pool, rest;
    for (auto& d : diffs) (d["kind"] == "Uncertain" ? rest : pool).push_back(d);
    auto center = [](const json& d) {
        auto b = d["bbox_mm"];
        return Pt{b[0].get<double>() + b[2].get<double>() / 2, b[1].get<double>() + b[3].get<double>() / 2};
    };
    auto overlap = [](const json& a, const json& b, double pad) {
        double ax = a[0], ay = a[1], aw = a[2], ah = a[3], bx = b[0], by = b[1], bw = b[2], bh = b[3];
        return !(ax - pad > bx + bw || bx - pad > ax + aw || ay - pad > by + bh || by - pad > ay + ah);
    };
    std::stable_sort(pool.begin(), pool.end(), [](const json& a, const json& b) {
        return a.value("area_mm2", 0.0) > b.value("area_mm2", 0.0);
    });
    std::vector<std::vector<json>> clusters;
    for (auto& d : pool) {
        bool placed = false;
        for (auto& cl : clusters) {
            const json& ref = cl[0];
            Pt c1 = center(d), c2 = center(ref);
            bool nearby = std::hypot(c1.x - c2.x, c1.y - c2.y) <= cluster_mm || overlap(d["bbox_mm"], ref["bbox_mm"], 0.2);
            bool new_inst = std::none_of(cl.begin(), cl.end(), [&](const json& x) { return x["instance"] == d["instance"]; });
            if (nearby && new_inst) {
                cl.push_back(d);
                placed = true;
                break;
            }
        }
        if (!placed) clusters.push_back({d});
    }
    std::vector<json> out;
    int need = std::max(2, (int)std::ceil(ratio * n_instances));
    for (auto& cl : clusters) {
        std::set<int> insts;
        for (auto& d : cl) insts.insert(d["instance"].get<int>());
        std::set<int> unc;
        for (auto& u : rest)
            if (overlap(cl[0]["bbox_mm"], u["bbox_mm"], 0.0)) unc.insert(u["instance"].get<int>());
        for (int i : insts) unc.erase(i);
        if (insts.size() >= 2 && (int)(insts.size() + unc.size()) >= need) {
            insts.insert(unc.begin(), unc.end());
            std::vector<double> x0, y0, x1, y1;
            std::map<std::string, int> kinds;
            for (auto& d : cl) {
                auto b = d["bbox_mm"];
                x0.push_back(b[0]);
                y0.push_back(b[1]);
                x1.push_back(b[0].get<double>() + b[2].get<double>());
                y1.push_back(b[1].get<double>() + b[3].get<double>());
                kinds[d["kind"].get<std::string>()]++;
            }
            std::string kind = std::max_element(kinds.begin(), kinds.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
            json rep = *std::max_element(cl.begin(), cl.end(), [](const json& a, const json& b) {
                return a.value("confidence", 0.0) < b.value("confidence", 0.0);
            });
            double mx0 = median(x0), my0 = median(y0), mx1 = median(x1), my1 = median(y1);
            rep["kind"] = kind;
            rep["systematic"] = true;
            rep["instances"] = std::vector<int>(insts.begin(), insts.end());
            rep["bbox_mm"] = {mx0, my0, mx1 - mx0, my1 - my0};
            rep["instance"] = 0;
            int sev = 0;
            for (auto& d : cl) sev = std::max(sev, sev_index(d["severity"]));
            rep["severity"] = SEV[sev];
            out.push_back(rep);
        } else {
            for (auto& d : cl) {
                d["systematic"] = false;
                d["instances"] = json::array({d["instance"]});
                out.push_back(d);
            }
        }
    }
    for (auto& d : rest) {
        d["systematic"] = false;
        d["instances"] = json::array({d["instance"]});
        out.push_back(d);
    }
    return out;
}

json uncertain_zone(int instance, const cv::Rect& r, double ppm, const std::string& cause) {
    return {{"instance", instance}, {"kind", "Uncertain"}, {"severity", "Info"}, {"cause", cause},
            {"bbox_px", bbox_json_px(r)}, {"bbox_mm", {r.x / ppm, r.y / ppm, r.width / ppm, r.height / ppm}},
            {"area_mm2", r.area() / (ppm * ppm)}, {"objects", json::array()},
            {"metrics", {{"delta_l", nullptr}, {"delta_e", nullptr}, {"shift_mm", nullptr}}}, {"confidence", 0.5}};
}

}  // namespace pc::scan
