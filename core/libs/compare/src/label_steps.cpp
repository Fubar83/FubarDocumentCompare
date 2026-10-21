// Steg i etikettjobbet utanför pipelinen: konsensus mellan etiketterna, textlänkning, sammanfattning.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <set>

#include "printcmp/compare/profiles.hpp"
#include "printcmp/scan/aggregate.hpp"
#include "printcmp/scan/certainty.hpp"
#include "printcmp/scan/classify.hpp"
#include "printcmp/scan/diff.hpp"
#include "printcmp/scan/folds.hpp"
#include "printcmp/scan/normalize.hpp"
#include "printcmp/scan/register.hpp"
#include "printcmp/scan/sheet_folds.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/image/image.hpp"

#include "label_steps.hpp"

namespace pc::compare::detail {

json layer_json(const pdf::LayerInfo& info, const std::map<std::string, bool>& decision) {
    json ocgs = json::array();
    for (auto& g : info.ocgs) ocgs.push_back({{"name", g.name}, {"default_on", g.default_on}, {"used_on_page", g.used_on_page}});
    json compared = json::object();
    for (auto& [k, v] : decision) compared[k] = v;
    return {{"ocgs", ocgs}, {"spots", info.spots}, {"compared", compared}};
}

std::vector<json> consensus(const std::vector<cv::Mat>& aligned_L, const std::vector<cv::Mat>& bands, const cv::Mat& valid,
                            const std::vector<json>& existing, const std::vector<scan::Instance>& insts, double ppm, const json& tol) {
    std::vector<cv::Mat> clean;
    for (size_t i = 0; i < aligned_L.size(); ++i)
        if (!cv::countNonZero(bands[i])) clean.push_back(aligned_L[i]);
    if (clean.size() < 3) return {};
    cv::Mat med(clean[0].size(), CV_32F);
    std::vector<float> buf(clean.size());
    for (int y = 0; y < med.rows; ++y)
        for (int x = 0; x < med.cols; ++x) {
            for (size_t k = 0; k < clean.size(); ++k) buf[k] = clean[k].at<float>(y, x);
            size_t n = buf.size();
            std::nth_element(buf.begin(), buf.begin() + n / 2, buf.end());
            float hi = buf[n / 2];
            med.at<float>(y, x) = n % 2 ? hi : (hi + *std::max_element(buf.begin(), buf.begin() + n / 2)) / 2;
        }
    int radius = (int)pyround(tol["edge_radius_mm"].get<double>() * ppm);
    double thr = 0.75 * tol["struct_dl"].get<double>(), min_area = tol["min_blob_mm2"].get<double>() * ppm * ppm;
    std::vector<json> out;
    for (size_t i = 0; i < aligned_L.size(); ++i) {
        const cv::Mat& L = aligned_L[i];
        cv::Mat dm;
        cv::GaussianBlur(cv::max(scan::range_dist(L, med, radius), scan::range_dist(med, L, radius)), dm, cv::Size(), 0.7);
        cv::Mat m = (dm > thr) & (valid > 0) & (bands[i] == 0);
        cv::morphologyEx(m, m, cv::MORPH_OPEN, cv::Mat::ones(2, 2, CV_8U));
        for (auto& d : existing)
            if (d.value("instance", -1) == insts[i].index && d.contains("bbox_px") && !d["bbox_px"].is_null()) {
                int x = d["bbox_px"][0], y = d["bbox_px"][1], w = d["bbox_px"][2], h = d["bbox_px"][3], pad = (int)(ppm * 0.5);
                cv::Rect r = cv::Rect(x - pad, y - pad, w + 2 * pad, h + 2 * pad) & cv::Rect(0, 0, m.cols, m.rows);
                if (!r.empty()) m(r).setTo(0);
            }
        cv::Mat labels, stats, cents;
        int n = cv::connectedComponentsWithStats(m, labels, stats, cents, 8, CV_32S);
        for (int k = 1; k < n; ++k) {
            int x = stats.at<int>(k, 0), y = stats.at<int>(k, 1), w = stats.at<int>(k, 2), h = stats.at<int>(k, 3), a = stats.at<int>(k, 4);
            if (a < min_area) continue;
            cv::Rect r(x, y, w, h);
            cv::Mat sub = labels(r) == k;
            auto sdl = image::masked_values(L(r) - med(r), sub);
            int pos = 0;
            for (float v : sdl) pos += v > 0;
            double frac = sdl.empty() ? 0.5 : (double)pos / sdl.size();
            std::string kind = frac > 0.75 ? "Missing" : (frac < 0.25 ? "Added" : "Shape");
            double mx;
            cv::minMaxLoc(dm(r), nullptr, &mx, nullptr, nullptr, sub);
            out.push_back({{"instance", insts[i].index}, {"kind", kind}, {"severity", "Minor"}, {"source", "consensus"},
                           {"bbox_px", {x, y, w, h}}, {"bbox_mm", {x / ppm, y / ppm, w / ppm, h / ppm}}, {"area_mm2", a / (ppm * ppm)},
                           {"objects", json::array()}, {"metrics", {{"delta_l", round_to(mx, 2)}, {"delta_e", nullptr}, {"shift_mm", nullptr}}},
                           {"confidence", 0.7}});
        }
    }
    std::vector<json> keep;
    for (auto& d : out) {
        double cx = d["bbox_mm"][0].get<double>() + d["bbox_mm"][2].get<double>() / 2;
        double cy = d["bbox_mm"][1].get<double>() + d["bbox_mm"][3].get<double>() / 2;
        int n_same = 0;
        for (auto& e : out)
            if (std::abs(e["bbox_mm"][0].get<double>() + e["bbox_mm"][2].get<double>() / 2 - cx) < 1.0 &&
                std::abs(e["bbox_mm"][1].get<double>() + e["bbox_mm"][3].get<double>() / 2 - cy) < 1.0)
                ++n_same;
        if (n_same <= std::max(1, (int)(0.25 * aligned_L.size()))) keep.push_back(d);
    }
    return keep;
}

std::vector<json> link_text(std::vector<json> diffs, const std::vector<json>& tdiffs, int instance) {
    auto ov = [](const json& a, const json& b, double pad = 0.3) {
        double ax = a[0], ay = a[1], aw = a[2], ah = a[3], bx = b[0], by = b[1], bw = b[2], bh = b[3];
        return !(ax - pad > bx + bw || bx - pad > ax + aw || ay - pad > by + bh || by - pad > ay + ah);
    };
    std::set<size_t> used;
    for (auto& d : diffs) {
        if (d.value("instance", -1) != instance || !d.contains("bbox_mm") || d["bbox_mm"].is_null() || d["kind"] == "Uncertain") continue;
        std::vector<size_t> hits;
        for (size_t k = 0; k < tdiffs.size(); ++k)
            if (ov(d["bbox_mm"], tdiffs[k]["bbox_mm"])) hits.push_back(k);
        if (hits.empty()) continue;
        json tc = json::array();
        bool edit = false;
        for (size_t n = 0; n < hits.size() && n < 6; ++n) {
            json t = tdiffs[hits[n]]["text"];
            t["kind"] = tdiffs[hits[n]]["kind"];
            tc.push_back(t);
        }
        for (size_t k : hits) {
            std::string kk = tdiffs[k]["kind"];
            edit |= kk == "Changed" || kk == "Added" || kk == "Missing";
        }
        d["text_change"] = tc;
        std::string cat = d.value("category", "");
        if (cat.empty() || cat == "Background" || cat == "Vector" || cat == "Image") d["category"] = "Text";
        if ((d["kind"] == "Moved" || d["kind"] == "Shape") && edit) d["kind"] = "Changed";
        used.insert(hits.begin(), hits.end());
    }
    for (size_t k = 0; k < tdiffs.size(); ++k) {
        if (used.count(k)) continue;
        const json& t = tdiffs[k];
        json txt = t["text"];
        txt["kind"] = t["kind"];
        diffs.push_back({{"instance", instance}, {"kind", t["kind"] == "Style" ? "Changed" : t["kind"]}, {"severity", "Minor"},
                         {"category", "TextLayer"}, {"source", "text"}, {"bbox_px", t["bbox_px"]}, {"bbox_mm", t["bbox_mm"]},
                         {"area_mm2", t["bbox_mm"][2].get<double>() * t["bbox_mm"][3].get<double>()}, {"objects", json::array()},
                         {"metrics", {{"delta_l", nullptr}, {"delta_e", nullptr}, {"shift_mm", txt.value("shift_mm", json())}}},
                         {"confidence", 0.9}, {"text_change", json::array({txt})}, {"text_only", true}});
    }
    return diffs;
}

json summary(const std::vector<json>& diffs, const json& insts) {
    json by_kind = json::object(), by_sev = json::object();
    int systematic = 0;
    for (auto& d : diffs) {
        by_kind[d["kind"].get<std::string>()] = by_kind.value(d["kind"].get<std::string>(), 0) + 1;
        by_sev[d["severity"].get<std::string>()] = by_sev.value(d["severity"].get<std::string>(), 0) + 1;
        systematic += d.value("systematic", false);
    }
    int f = 0, w = 0, p = 0;
    for (auto& i : insts) {
        std::string s = i["status"];
        f += s == "fail";
        w += s == "warn";
        p += s == "pass";
    }
    return {{"differences", diffs.size()}, {"by_kind", by_kind}, {"by_severity", by_sev}, {"systematic", systematic},
            {"instances_fail", f}, {"instances_warn", w}, {"instances_pass", p}};
}


}  // namespace pc::compare::detail
