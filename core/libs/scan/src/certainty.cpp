// Säkerhetsgrad per skillnad: bekräftad, hög, trolig, möjlig – med skäl.
#include <algorithm>
#include <cmath>

#include "printcmp/scan/folds.hpp"
#include "printcmp/scan/certainty.hpp"

namespace pc::scan {

const std::vector<std::string> LEVELS = {"confirmed", "high", "probable", "possible"};

namespace {

int level_index(const std::string& l) { return (int)(std::find(LEVELS.begin(), LEVELS.end(), l) - LEVELS.begin()); }
const std::map<std::string, int> BASE = {{"confirmed", 95}, {"high", 80}, {"probable", 60}, {"possible", 30}};

bool near_fold(const json& d, const std::map<int, json>& inst_folds, double mm = 4.0) {
    if (!d.contains("bbox_mm") || d["bbox_mm"].is_null()) return false;
    double x = d["bbox_mm"][0], y = d["bbox_mm"][1], w = d["bbox_mm"][2], h = d["bbox_mm"][3];
    Pt c{x + w / 2, y + h / 2};
    double r = std::max(w, h) / 2;
    for (auto& i : d.value("instances", json::array())) {
        auto it = inst_folds.find(i.get<int>());
        if (it == inst_folds.end()) continue;
        for (auto& f : it->second) {
            Pt a{f["p0_mm"][0], f["p0_mm"][1]}, b{f["p1_mm"][0], f["p1_mm"][1]};
            if (point_line_dist(c, a, b) <= std::max(mm, f.value("band_mm", 0.0) + 1.0) + r) return true;
        }
    }
    return false;
}

std::string num(const json& v, const char* fmt) { return v.is_number() ? format(fmt, v.get<double>()) : "–"; }

}  // namespace

json assess(const json& d, int n_inst, const json& tol, const std::string& sample_kind, const std::map<int, json>& inst_folds,
            bool color_calibrated) {
    std::string kind = d["kind"];
    std::vector<std::string> reasons;
    int k = (int)d.value("instances", json::array()).size();
    json m = d.value("metrics", json::object());
    auto mget = [&](const char* key) { return m.contains(key) && m[key].is_number() ? m[key].get<double>() : 0.0; };
    double bonus = 0;
    std::string level;

    if (kind == "Uncertain") {
        std::string cause = d.value("cause", "");
        std::string sv = cause == "fold" ? "veck" : cause == "registration" ? "osäker registrering" : cause;
        return {{"level", "possible"}, {"score", 20}, {"reasons", {"Kan inte bedömas här (" + sv + ")"}}};
    }
    if (d.value("reason", "") == "instance_not_found")
        return {{"level", "confirmed"}, {"score", 100}, {"reasons", {"Etiketter saknas på arket"}}};
    if (sample_kind == "pdf") {
        if (d.contains("text_change"))
            for (size_t i = 0; i < d["text_change"].size() && i < 2; ++i) {
                const auto& tc = d["text_change"][i];
                if (tc.value("kind", "") == "Style") {
                    std::string s = "Textlager: samma text, " + num(tc.value("size_from", json()), "%g") + " → " +
                                    num(tc.value("size_to", json()), "%g") + " pt";
                    if (tc.value("font_from", "") != tc.value("font_to", ""))
                        s += ", " + tc.value("font_from", "") + " → " + tc.value("font_to", "");
                    reasons.push_back(s);
                } else {
                    reasons.push_back("Textlager: ”" + tc.value("from", "") + "” → ”" + tc.value("to", "") + "”");
                }
            }
        if (d.value("text_only", false)) reasons.push_back("Syns inte i tryck – bara i textlagret");
        reasons.push_back("PDF mot PDF: exakt rendering utan brus");
        if (kind == "Barcode" && d.contains("barcode") && !d["barcode"].value("read", json()).is_null())
            reasons.push_back("Avkodat innehåll skiljer: " + d["barcode"]["expected"].get<std::string>() + " → " +
                              d["barcode"]["read"].get<std::string>());
        return {{"level", "confirmed"}, {"score", 99}, {"reasons", reasons}};
    }
    double sdl = tol["struct_dl"], det = tol["delta_e"];
    if (kind == "Barcode") {
        const json& bc = d["barcode"];
        if (!bc.value("read", json()).is_null())
            return {{"level", "confirmed"}, {"score", 99},
                    {"reasons", {"Avkodat innehåll skiljer: väntat " + bc["expected"].get<std::string>() + ", läst " +
                                 bc["read"].get<std::string>()}}};
        level = "high";
        reasons.push_back("Koden kunde inte avkodas i provet");
    } else if (kind == "Braille") {
        json bi = d.value("braille", json::object());
        if (d.value("cause", "") == "embossing_missing") {
            level = "high";
            reasons.push_back("Ingen präglingssignal i hela raden");
        } else {
            double c = bi.value("contrast", 0.0), mg = bi.value("margin", 0.0);
            level = (mg >= 0.5 && c >= 2.5) ? "high" : (mg >= 0.2 ? "probable" : "possible");
            std::string rd = bi.value("read", ""), ex = bi.value("expected", "");
            auto strip = [](std::string s) {
                while (!s.empty() && s.back() == ' ') s.pop_back();
                while (!s.empty() && s.front() == ' ') s.erase(s.begin());
                return s.empty() ? std::string("–") : s;
            };
            reasons.push_back("Punktskrift läst ”" + strip(rd) + "”, väntat ”" + strip(ex) + "”" +
                              format(" (kontrast %.1f, marginal %.1f)", c, mg));
            bonus = 10 * std::min(1.0, mg);
        }
    } else if (kind == "Color") {
        double de = mget("delta_e");
        if (color_calibrated) level = de >= 2 * det ? "high" : "probable";
        else {
            level = de >= 2 * det ? "probable" : "possible";
            reasons.push_back("Färg mätt med okalibrerad scanner");
        }
        reasons.push_back(format("ΔE00 %.1f (tröskel %.0f)", de, det));
        bonus = std::min(10.0, de - det);
    } else {
        double r = mget("delta_l") / sdl, area = d.value("area_mm2", 0.0);
        level = (r >= 2.5 && area >= 0.1) ? "high" : (r >= 1.5 ? "probable" : "possible");
        reasons.push_back(format("Ljushetsavvikelse %.0f L* (%.1f × tröskeln), yta %.2f mm²", mget("delta_l"), r, area));
        bonus = 10 * std::min(1.0, r / 4);
        if (d.value("source", "") == "consensus") {
            // som Pythons max(level, "probable", key=LEVELS.index): högst "trolig"
            if (level_index(level) < level_index("probable")) level = "probable";
            reasons.push_back("Hittad bara vid jämförelse mellan etiketter");
        }
    }
    bool systematic = d.value("systematic", false);
    bool repeated = systematic && n_inst >= 3 && k >= std::max(3, (int)std::ceil(0.75 * n_inst)) && kind != "Color";
    if (repeated) {
        bool strong = level == "confirmed" || level == "high" || kind == "Braille" || kind == "Barcode" || mget("delta_l") >= 1.5 * sdl;
        level = strong ? "confirmed" : "high";
        reasons.insert(reasons.begin(), format("Samma avvikelse oberoende i %d av %d etiketter", k, n_inst));
    } else if (systematic) {
        reasons.insert(reasons.begin(), format("Avvikelsen finns i %d av %d etiketter", k, n_inst));
    }
    if (level != "confirmed" && !repeated && near_fold(d, inst_folds)) {
        level = LEVELS[std::min((int)LEVELS.size() - 1, level_index(level) + 1)];
        reasons.push_back("Nära ett veck");
    }
    return {{"level", level}, {"score", (int)pyround(std::min(99.0, BASE.at(level) + bonus))}, {"reasons", reasons}};
}

}  // namespace pc::scan
