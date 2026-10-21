// Klassning. Regler i ordning, första träff gäller.
#include <algorithm>
#include <cmath>
#include <map>
#include <opencv2/imgproc.hpp>

#include "printcmp/image/image.hpp"
#include "printcmp/scan/aggregate.hpp"
#include "printcmp/scan/diff.hpp"
#include "printcmp/scan/severity.hpp"
#include "printcmp/scan/classify.hpp"

namespace pc::scan {


namespace {

const std::map<std::string, std::string> MIN_SEV = {{"Text", "Major"}, {"OutlinedText", "Major"}, {"TableCell", "Major"},
                                                     {"TableRule", "Major"}, {"Barcode", "Critical"}, {"Braille", "Critical"}};
const std::vector<std::string> CAT_ORDER = {"Text", "OutlinedText", "Barcode", "Image", "Rule", "Vector"};

cv::Rect clip(cv::Rect r, cv::Size s) { return r & cv::Rect(0, 0, s.width, s.height); }

std::optional<std::pair<double, double>> tone_offset(const cv::Mat& rL, const cv::Mat& sL, const DiffMaps& maps,
                                                     const Blob& b, const std::vector<const artwork::Obj*>& hit, double ppm) {
    const artwork::Obj* o = nullptr;
    for (auto c : hit)
        if (c->kind == "Text" || c->kind == "OutlinedText" || c->kind == "Vector")
            if (!o || c->bbox.area() > o->bbox.area()) o = c;
    if (!o) return std::nullopt;
    int pad = (int)(0.3 * ppm);
    cv::Rect R = clip(cv::Rect(cv::Point((int)o->bbox.x0 - pad, (int)o->bbox.y0 - pad),
                               cv::Point((int)o->bbox.x1 + pad, (int)o->bbox.y1 + pad)), rL.size());
    auto blob_ref = image::masked_values(rL(b.bbox), b.mask);
    if (blob_ref.size() < 5 || R.empty()) return std::nullopt;
    cv::Mat bd = sL(b.bbox) - rL(b.bbox);
    double tone_ref = median(std::vector<double>(blob_ref.begin(), blob_ref.end()));
    cv::Mat Rr = rL(R), same = cv::abs(Rr - tone_ref) < 6.0, er;
    cv::erode(same, er, cv::Mat::ones(3, 3, CV_8U));
    same &= er;
    if (cv::countNonZero(same) < std::max<double>(50, 4.0 * blob_ref.size())) return std::nullopt;
    cv::Mat Dm = sL(R) - Rr;
    auto D = image::masked_values(Dm, same);
    std::vector<double> Dv(D.begin(), D.end());
    double off = median(Dv);
    if (std::abs(off) < 8.0) return std::nullopt;
    std::vector<double> dev;
    for (double v : Dv) dev.push_back(std::abs(v - off));
    double spread = median(dev);
    double bmed = image::masked_median(bd, b.mask);
    if (spread > 0.35 * std::abs(off) || std::abs(bmed - off) > 0.35 * std::abs(off) + 3.0) return std::nullopt;
    double de = image::masked_median(maps.delta_e(R), same);
    return std::make_pair(off, de);
}

bool near_text_line(const std::vector<artwork::Obj>& objs, const Box& box) {
    double cy = box.cy();
    for (const auto& o : objs) {
        if (o.kind != "Text" && o.kind != "OutlinedText") continue;
        double h = o.bbox.h();
        if (!(o.bbox.y0 - 0.2 * h <= cy && cy <= o.bbox.y1 + 0.2 * h)) continue;
        double gap = std::max({box.x0 - o.bbox.x1, o.bbox.x0 - box.x1, 0.0});
        if (gap <= 2.0 * h) return true;
    }
    return false;
}

std::string category_of(const std::vector<const artwork::Obj*>& hit, const std::optional<json>& tinfo) {
    if (tinfo) return (*tinfo)["element"] == "rule" ? "TableRule" : "TableCell";
    for (const auto& k : CAT_ORDER)
        for (auto o : hit)
            if (o->kind == k) return k;
    return "Background";
}

struct MoveResult { int dx, dy; double best, zero; };

std::optional<MoveResult> moved_test(const cv::Mat& src, const cv::Mat& dst, const cv::Rect& box, double ppm, double max_mm = 4.0) {
    int pad = (int)pyround(0.6 * ppm);
    cv::Rect pr = clip(cv::Rect(box.x - pad, box.y - pad, box.width + 2 * pad, box.height + 2 * pad), src.size());
    cv::Mat patch = src(pr);
    cv::Scalar mu, sd;
    cv::meanStdDev(patch, mu, sd);
    if (patch.total() < 16 || sd[0] < 6) return std::nullopt;
    int m = (int)pyround(max_mm * ppm);
    cv::Rect sr = clip(cv::Rect(pr.x - m, pr.y - m, pr.width + 2 * m, pr.height + 2 * m), src.size());
    cv::Mat search = dst(sr);
    if (search.rows < patch.rows || search.cols < patch.cols) return std::nullopt;
    cv::Mat res;
    cv::matchTemplate(search, patch, res, cv::TM_CCOEFF_NORMED);
    cv::patchNaNs(res, -1);
    double best;
    cv::Point loc;
    cv::minMaxLoc(res, nullptr, &best, nullptr, &loc);
    int zx = pr.x - sr.x, zy = pr.y - sr.y;
    double zero = (0 <= zy && zy < res.rows && 0 <= zx && zx < res.cols) ? res.at<float>(zy, zx) : -1.0;
    int dx = loc.x - zx, dy = loc.y - zy;
    cv::Rect own(pr.x + dx, pr.y + dy, patch.cols, patch.rows);
    if ((own & cv::Rect(0, 0, src.cols, src.rows)) == own) {
        cv::Mat o = src(own);
        cv::meanStdDev(o, mu, sd);
        if (sd[0] > 1e-3) {
            cv::Mat r2;
            cv::matchTemplate(o, patch, r2, cv::TM_CCOEFF_NORMED);
            if (r2.at<float>(0, 0) > best - 0.1) return std::nullopt;   // samma form fanns redan där
        }
    }
    return MoveResult{dx, dy, best, zero};
}

bool inside_word(const std::vector<const artwork::Obj*>& hit, const cv::Rect& b, double ppm) {
    double pad = 0.15 * ppm;
    for (auto o : hit) {
        if (o->kind != "Text" && o->kind != "OutlinedText") continue;
        if (b.x >= o->bbox.x0 - pad && b.y >= o->bbox.y0 - pad && b.x + b.width <= o->bbox.x1 + pad &&
            b.y + b.height <= o->bbox.y1 + pad && (double)b.area() < 0.4 * o->bbox.area())
            return true;
    }
    return false;
}

std::string presence_kind(const cv::Mat& rL, const cv::Mat& sL, const Blob& b, double ppm) {
    int pad = std::max(3, (int)pyround(1.0 * ppm));
    cv::Rect R = clip(cv::Rect(b.bbox.x - pad, b.bbox.y - pad, b.bbox.width + 2 * pad, b.bbox.height + 2 * pad), rL.size());
    cv::Mat m = cv::Mat::zeros(R.size(), CV_8U);
    b.mask.copyTo(m(cv::Rect(b.bbox.x - R.x, b.bbox.y - R.y, b.bbox.width, b.bbox.height)));
    cv::Mat dil;
    cv::dilate(m, dil, image::ellipse_kernel(pad));
    cv::Mat ring = (dil > 0) & (m == 0), inner = m > 0;
    cv::Mat rr = rL(R), ss = sL(R);
    if (cv::countNonZero(ring) < 10) {
        auto sdl = image::masked_values(ss - rr, inner);
        double lighter = 0.5;
        if (!sdl.empty()) {
            int pos = 0;
            for (float v : sdl) pos += v > 0;
            lighter = (double)pos / sdl.size();
        }
        return lighter > 0.75 ? "Missing" : (lighter < 0.25 ? "Added" : "Changed");
    }
    double bg_r = image::masked_median(rr, ring), bg_s = image::masked_median(ss, ring);
    double c_ref = image::masked_mean(cv::abs(rr - bg_r), inner), c_smp = image::masked_mean(cv::abs(ss - bg_s), inner);
    double hi = std::max(c_ref, c_smp), lo = std::min(c_ref, c_smp);
    if (hi < 1e-6 || lo > 0.45 * hi) return "Changed";
    return c_ref > c_smp ? "Missing" : "Added";
}

json bbox_mm(const cv::Rect& r, double ppm) { return {r.x / ppm, r.y / ppm, r.width / ppm, r.height / ppm}; }

std::vector<json> merge_moved(std::vector<json> diffs, double ppm) {
    static const std::set<std::string> K = {"Moved", "Missing", "Added", "Changed", "Shape"};
    std::set<size_t> used;
    std::vector<json> merged;
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (used.count(i) || !K.count(diffs[i]["kind"].get<std::string>())) continue;
        json a = diffs[i];
        for (size_t j = i + 1; j < diffs.size(); ++j) {
            const json& b = diffs[j];
            if (used.count(j) || !K.count(b["kind"].get<std::string>())) continue;
            if (!a.contains("shift_px") && !b.contains("shift_px")) continue;
            std::vector<int> s = a.contains("shift_px") ? a["shift_px"].get<std::vector<int>>()
                                                         : std::vector<int>{-b["shift_px"][0].get<int>(), -b["shift_px"][1].get<int>()};
            int ax = a["bbox_px"][0], ay = a["bbox_px"][1], aw = a["bbox_px"][2], ah = a["bbox_px"][3];
            int bx = b["bbox_px"][0], by = b["bbox_px"][1], bw = b["bbox_px"][2], bh = b["bbox_px"][3];
            int tx = ax + s[0], ty = ay + s[1];
            int ix = std::max(0, std::min(tx + aw, bx + bw) - std::max(tx, bx));
            int iy = std::max(0, std::min(ty + ah, by + bh) - std::max(ty, by));
            if (ix * iy > 0.25 * std::min(aw * ah, bw * bh)) {
                int x0 = std::min(ax, bx), y0 = std::min(ay, by), x1 = std::max(ax + aw, bx + bw), y1 = std::max(ay + ah, by + bh);
                cv::Rect u(x0, y0, x1 - x0, y1 - y0);
                a["kind"] = "Moved";
                a["bbox_px"] = bbox_json_px(u);
                a["bbox_mm"] = bbox_mm(u, ppm);
                a["area_mm2"] = a["area_mm2"].get<double>() + b["area_mm2"].get<double>();
                a["metrics"]["shift_mm"] = round_to(std::hypot(s[0], s[1]) / ppm, 3);
                a["shift_px"] = s;
                a["severity"] = SEV[std::max(sev_index(a["severity"]), sev_index(b["severity"]))];
                for (auto& o : b["objects"])
                    if (std::find(a["objects"].begin(), a["objects"].end(), o) == a["objects"].end()) a["objects"].push_back(o);
                used.insert(j);
            }
        }
        used.insert(i);
        merged.push_back(a);
    }
    for (size_t k = 0; k < diffs.size(); ++k)
        if (!used.count(k)) merged.push_back(diffs[k]);
    return merged;
}

std::vector<json> merge_object_color(std::vector<json> diffs, double ppm) {
    std::map<int, std::vector<json>> groups;
    std::vector<int> order;
    std::vector<json> out;
    for (auto& d : diffs) {
        if (d["kind"] == "Color" && !d["objects"].empty() && d.contains("tone_offset_L")) {
            int id = d["objects"][0]["id"];
            if (!groups.count(id)) order.push_back(id);
            groups[id].push_back(d);
        } else {
            out.push_back(d);
        }
    }
    for (int id : order) {
        auto& g = groups[id];
        if (g.size() == 1) {
            out.push_back(g[0]);
            continue;
        }
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        double area = 0;
        std::vector<double> des;
        const json* big = &g[0];
        for (auto& d : g) {
            x0 = std::min(x0, d["bbox_px"][0].get<int>());
            y0 = std::min(y0, d["bbox_px"][1].get<int>());
            x1 = std::max(x1, d["bbox_px"][0].get<int>() + d["bbox_px"][2].get<int>());
            y1 = std::max(y1, d["bbox_px"][1].get<int>() + d["bbox_px"][3].get<int>());
            area += d["area_mm2"].get<double>();
            des.push_back(d["metrics"]["delta_e"].get<double>());
            if (d["area_mm2"].get<double>() > (*big)["area_mm2"].get<double>()) big = &d;
        }
        json m = *big;
        cv::Rect u(x0, y0, x1 - x0, y1 - y0);
        m["bbox_px"] = bbox_json_px(u);
        m["bbox_mm"] = bbox_mm(u, ppm);
        m["area_mm2"] = area;
        m["metrics"]["delta_e"] = round_to(median(des), 2);
        m["parts"] = g.size();
        out.push_back(m);
    }
    return out;
}

std::vector<json> merge_table_rows(std::vector<json> diffs, double ppm) {
    std::map<std::pair<int, int>, std::vector<json>> groups;
    std::vector<std::pair<int, int>> order;
    std::vector<json> out;
    for (auto& d : diffs) {
        if (d["kind"] == "Moved" && d.contains("table") && d["table"].value("element", "") == "cell" && d.contains("shift_px")) {
            auto key = std::make_pair(d["table"]["table"].get<int>(), d["table"]["row"].get<int>());
            if (!groups.count(key)) order.push_back(key);
            groups[key].push_back(d);
        } else {
            out.push_back(d);
        }
    }
    for (auto& key : order) {
        auto& ds = groups[key];
        if (ds.size() == 1) {
            out.push_back(ds[0]);
            continue;
        }
        double mx = 0, my = 0;
        for (auto& d : ds) {
            mx += d["shift_px"][0].get<double>();
            my += d["shift_px"][1].get<double>();
        }
        mx /= ds.size();
        my /= ds.size();
        double dev = 0;
        for (auto& d : ds) dev = std::max(dev, std::hypot(d["shift_px"][0].get<double>() - mx, d["shift_px"][1].get<double>() - my));
        if (dev > 0.15 * ppm) {
            out.insert(out.end(), ds.begin(), ds.end());
            continue;
        }
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        double area = 0;
        json objs = json::array();
        for (auto& d : ds) {
            x0 = std::min(x0, d["bbox_px"][0].get<int>());
            y0 = std::min(y0, d["bbox_px"][1].get<int>());
            x1 = std::max(x1, d["bbox_px"][0].get<int>() + d["bbox_px"][2].get<int>());
            y1 = std::max(y1, d["bbox_px"][1].get<int>() + d["bbox_px"][3].get<int>());
            area += d["area_mm2"].get<double>();
            for (auto& o : d["objects"])
                if (objs.size() < 8) objs.push_back(o);
        }
        json m = ds[0];
        cv::Rect u(x0, y0, x1 - x0, y1 - y0);
        m["bbox_px"] = bbox_json_px(u);
        m["bbox_mm"] = bbox_mm(u, ppm);
        m["area_mm2"] = area;
        m["table"]["col"] = nullptr;
        m["table"]["cell_text"] = nullptr;
        m["table"]["row_shift"] = true;
        m["objects"] = objs;
        out.push_back(m);
    }
    return out;
}

}  // namespace

std::vector<json> classify(const std::vector<Blob>& blobs, const DiffMaps& maps, const std::vector<artwork::Obj>& objs,
                           const std::vector<barcode::Code>& ref_codes, const cv::Mat& smp_rgb_u8, const cv::Mat& fold_band,
                           const cv::Mat& confidence, double ppm, double dpi, const json& tol, int instance,
                           const std::vector<artwork::Table>& tables) {
    cv::Mat rL = maps.ref_L(), sL = maps.smp_L();
    std::vector<json> out;
    for (const auto& b : blobs) {
        const cv::Rect& r = b.bbox;
        Box box{(double)r.x, (double)r.y, (double)(r.x + r.width), (double)(r.y + r.height)};
        json d = {{"instance", instance}, {"kind", nullptr}, {"severity", "Minor"}, {"bbox_px", bbox_json_px(r)},
                  {"bbox_mm", bbox_mm(r, ppm)}, {"area_mm2", b.area_px / (ppm * ppm)}, {"objects", json::array()},
                  {"metrics", {{"delta_l", round_to(b.max_dl, 2)}, {"delta_e", round_to(b.mean_de, 2)}, {"shift_mm", nullptr}}},
                  {"confidence", 0.9}};
        auto hit = artwork::overlapping(objs, box, {"Text", "OutlinedText", "Image", "Vector", "Rule", "Barcode"}, 0.02);
        for (size_t k = 0; k < hit.size() && k < 8; ++k) {
            json o = {{"id", hit[k]->id}, {"kind", hit[k]->kind}};
            if (!hit[k]->text.empty()) o["text"] = hit[k]->text;
            d["objects"].push_back(o);
        }
        std::optional<json> tinfo;
        if (!tables.empty()) tinfo = artwork::locate_in_table(tables, box, ppm);
        if (tinfo) d["table"] = *tinfo;
        d["category"] = category_of(hit, tinfo);
        if (d["category"] == "Background" && near_text_line(objs, box)) d["category"] = "Text";
        std::string cat = d["category"];
        bool text_hit = cat == "Text" || cat == "OutlinedText" || cat == "TableCell";

        double fold_mean = fold_band.empty() ? 0.0 : cv::mean(fold_band(r) > 0, b.mask)[0] / 255.0;
        if (fold_mean > 0.3) {
            d["kind"] = "Uncertain";
            d["severity"] = "Info";
            d["cause"] = "fold";
            d["confidence"] = 0.5;
            out.push_back(d);
            continue;
        }
        if (!confidence.empty() && cv::mean(confidence(r), b.mask)[0] < 0.15) {
            d["kind"] = "Uncertain";
            d["severity"] = "Info";
            d["cause"] = "registration";
            d["confidence"] = 0.4;
            out.push_back(d);
            continue;
        }
        const barcode::Code* code_hit = nullptr;
        for (const auto& c : ref_codes)
            if (!(box.x1 < c.bbox.x0 || c.bbox.x1 < box.x0 || box.y1 < c.bbox.y0 || c.bbox.y1 < box.y0)) {
                code_hit = &c;
                break;
            }
        if (code_hit) {
            auto got = barcode::decode_region(smp_rgb_u8, code_hit->bbox, (int)(2 * ppm), dpi);
            const barcode::Code* match = nullptr;
            for (auto& g : got)
                if (g.data == code_hit->data) match = &g;
            if (match) {
                // läget mäts med faskorrelation mellan kodytorna (detektorns hörn är för grova)
                int pad = (int)(2 * ppm);
                cv::Rect cr = clip(cv::Rect(cv::Point((int)code_hit->bbox.x0 - pad, (int)code_hit->bbox.y0 - pad),
                                            cv::Point((int)code_hit->bbox.x1 + pad, (int)code_hit->bbox.y1 + pad)), rL.size());
                cv::Mat a_ = rL(cr).clone(), b_ = sL(cr).clone(), win;
                cv::createHanningWindow(win, a_.size(), CV_32F);
                double resp = 0;
                cv::Point2d sh = cv::phaseCorrelate(a_, b_, win, &resp);
                double shift_mm = resp > 0.2 ? std::hypot(sh.x, sh.y) / ppm : 0.0;
                if (shift_mm >= 0.25) {
                    Box u = code_hit->bbox.unite(match->bbox);
                    cv::Rect ur((int)u.x0, (int)u.y0, (int)(u.x1 - u.x0), (int)(u.y1 - u.y0));
                    d["kind"] = "Moved";
                    d["severity"] = "Major";
                    d["bbox_px"] = bbox_json_px(ur);
                    d["bbox_mm"] = {u.x0 / ppm, u.y0 / ppm, u.w() / ppm, u.h() / ppm};
                    d["barcode"] = {{"expected", code_hit->data}, {"read", match->data}};
                    d["metrics"]["shift_mm"] = round_to(shift_mm, 3);
                    bool dup = std::any_of(out.begin(), out.end(), [](const json& o) { return o["kind"] == "Moved" && o.contains("barcode"); });
                    if (!dup) out.push_back(d);
                }
                // formavvikelse inom en korrekt läst kod är en kvalitetsfråga (gradering), inte innehåll
                continue;
            }
            d["kind"] = "Barcode";
            d["severity"] = "Critical";
            d["barcode"] = {{"expected", code_hit->data}, {"read", got.empty() ? json(nullptr) : json(got[0].data)},
                            {"symbology", code_hit->symbology}};
            const Box& cb = code_hit->bbox;
            d["bbox_px"] = {(int)cb.x0, (int)cb.y0, (int)cb.w(), (int)cb.h()};
            d["bbox_mm"] = {cb.x0 / ppm, cb.y0 / ppm, cb.w() / ppm, cb.h() / ppm};
            out.push_back(d);
            continue;
        }
        double de_t = tol["delta_e"];
        if (b.kind_hint == "color") {
            d["kind"] = "Color";
            d["severity"] = b.mean_de > 2 * de_t ? "Major" : "Minor";
            out.push_back(d);
            continue;
        }
        auto tone = tone_offset(rL, sL, maps, b, hit, ppm);
        if (tone) {
            if (tone->second < de_t) continue;   // hela objektet har samma ljushetsförskjutning: ton, inte form
            d["kind"] = "Color";
            d["severity"] = tone->second > 2 * de_t ? "Major" : "Minor";
            d["metrics"]["delta_e"] = round_to(tone->second, 2);
            d["tone_offset_L"] = round_to(tone->first, 1);
            out.push_back(d);
            continue;
        }
        std::string kind = presence_kind(rL, sL, b, ppm);
        if ((kind == "Added" || kind == "Missing") && text_hit && inside_word(hit, r, ppm)) kind = "Changed";
        if (kind == "Changed" && !text_hit) kind = "Shape";
        std::optional<MoveResult> mt;
        if (cat != "TableRule" && cat != "Rule") mt = kind != "Added" ? moved_test(rL, sL, r, ppm) : moved_test(sL, rL, r, ppm);
        if (mt) {
            int dx = mt->dx, dy = mt->dy;
            if (kind == "Added") {
                dx = -dx;
                dy = -dy;
            }
            double shift_mm = std::hypot(dx, dy) / ppm;
            if (mt->best > 0.8 && mt->best - mt->zero > 0.1 && shift_mm >= 0.12) {
                kind = "Moved";
                d["metrics"]["shift_mm"] = round_to(shift_mm, 3);
                d["shift_px"] = {dx, dy};
            }
        }
        d["kind"] = kind;
        if (text_hit || d["area_mm2"].get<double>() > 4.0) d["severity"] = "Major";
        out.push_back(d);
    }
    for (auto& d : out) {
        std::string cat = d.value("category", "");
        if (MIN_SEV.count(cat) && d["kind"] != "Uncertain")
            d["severity"] = SEV[std::max(sev_index(d["severity"]), sev_index(MIN_SEV.at(cat)))];
    }
    out = merge_moved(out, ppm);
    out = merge_object_color(out, ppm);
    out = merge_table_rows(out, ppm);
    // en felaktig streckkod ger en rad
    std::set<std::string> seen;
    std::vector<json> dedup;
    for (auto& d : out) {
        if (d["kind"] == "Barcode") {
            std::string key = d["barcode"]["expected"];
            if (seen.count(key)) continue;
            seen.insert(key);
        }
        dedup.push_back(d);
    }
    out = dedup;
    std::vector<json> bad;
    for (auto& d : out)
        if (d["kind"] == "Barcode") bad.push_back(d);
    if (!bad.empty()) {
        std::vector<json> keep;
        for (auto& d : out) {
            if (d["kind"] != "Barcode" && d.contains("bbox_px")) {
                double cx = d["bbox_px"][0].get<double>() + d["bbox_px"][2].get<double>() / 2;
                double cy = d["bbox_px"][1].get<double>() + d["bbox_px"][3].get<double>() / 2;
                double pad = 1.5 * ppm;
                bool nearby = false;
                for (auto& b2 : bad) {
                    double bx = b2["bbox_px"][0], by = b2["bbox_px"][1], bw = b2["bbox_px"][2], bh = b2["bbox_px"][3];
                    if (bx - pad <= cx && cx <= bx + bw + pad && by - pad <= cy && cy <= by + bh + 4 * ppm) nearby = true;
                }
                if (nearby) continue;
            }
            keep.push_back(d);
        }
        out = keep;
    }
    return out;
}

}  // namespace pc::scan
