// HTML-rapport för dokumentjämförelse.
#include <filesystem>
#include <map>
#include <opencv2/imgproc.hpp>

#include "printcmp/core/text.hpp"
#include "printcmp/image/image.hpp"
#include "printcmp/report/report.hpp"

namespace pc::report {

namespace {

const std::map<std::string, std::string> KIND_SV = {{"Changed", "Ändrat"}, {"Added", "Tillagt"}, {"Missing", "Saknas"}, {"Moved", "Flyttat"},
                                                    {"Style", "Stil"}, {"StyleSystematic", "Stil (hela rollen)"}, {"PageNumber", "Sidnummer"}};
const std::map<std::string, std::string> KIND_COL = {{"Changed", "var(--major)"}, {"Added", "var(--pass)"}, {"Missing", "var(--crit)"},
                                                     {"Moved", "#7c3aed"}, {"Style", "var(--minor)"}, {"StyleSystematic", "var(--minor)"},
                                                     {"PageNumber", "var(--info)"}};
const std::map<std::string, std::string> SEV = {{"Critical", "crit"}, {"Major", "major"}, {"Minor", "minor"}, {"Info", "info"}};

const char* DOC_CSS = R"(
:root{--bg:#fafaf9;--fg:#1c1917;--muted:#78716c;--card:#fff;--line:#e7e5e4;--crit:#be123c;--major:#dc2626;--minor:#d97706;--info:#64748b;--pass:#16a34a}
@media (prefers-color-scheme:dark){:root{--bg:#0c0a09;--fg:#e7e5e4;--muted:#a8a29e;--card:#1c1917;--line:#292524}}
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:1200px;margin:0 auto;padding:24px 16px 64px} h1{font-size:22px;margin:0 0 4px} h2{font-size:16px;margin:32px 0 12px}
.muted{color:var(--muted)} .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin:16px 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px} .card b{display:block;font-size:22px}
img{max-width:100%;display:block;border-radius:6px;border:1px solid var(--line);background:#fff}
table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}
th,td{text-align:left;vertical-align:top;padding:10px;border-bottom:1px solid var(--line)} th{font-size:12px;color:var(--muted);font-weight:600}
.sev{font-size:12px;font-weight:600;padding:2px 8px;border-radius:99px;color:#fff;white-space:nowrap}
.sev.crit{background:var(--crit)} .sev.major{background:var(--major)} .sev.minor{background:var(--minor)} .sev.info{background:var(--info)}
.kind{border-left:4px solid var(--k);padding-left:6px;font-weight:600;white-space:nowrap}
del{color:var(--crit);background:color-mix(in srgb,var(--crit) 12%,transparent)} ins{color:var(--pass);text-decoration:none;font-weight:600;background:color-mix(in srgb,var(--pass) 12%,transparent)}
.ctx{color:var(--muted)} .txt{max-width:520px;overflow-wrap:anywhere}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:8px;min-width:260px} figure{margin:0} figcaption{font-size:12px;color:var(--muted);margin-top:4px}
.pill{display:inline-block;padding:1px 8px;border-radius:99px;font-size:12px;font-weight:600;color:#fff}
code{font:12px ui-monospace,Consolas,monospace}
@media (max-width:760px){table,thead,tbody,tr,td,th{display:block} thead{display:none} td{border:0;padding:4px 10px} tr{border-bottom:1px solid var(--line);padding:8px 0} .pair{grid-template-columns:1fr}}
)";

std::string esc(const json& v) { return html_escape(v.is_string() ? v.get<std::string>() : v.is_null() ? "" : v.dump()); }

class Pages {
public:
    explicit Pages(const std::string& path) {
        try {
            doc_ = pdf::Document::open_file(path);
        } catch (...) {
        }
    }
    cv::Mat crop(int page, const json& bb, double pad_mm = 4.0, double pad_x_mm = 22.0) {
        if (!doc_ || bb.is_null() || page < 1 || page > doc_->page_count()) return {};
        if (!cache_.count(page)) {
            if (cache_.size() >= 8) cache_.erase(cache_.begin());
            double w_pt = pdf::page_size(*doc_, page - 1).width_pt;
            double dpi = w_pt < 120 / MM_PER_INCH * 72 ? 400 : 150;   // etiketter i högre upplösning
            cache_[page] = {pdf::render_page(*doc_, page - 1, dpi), dpi};
        }
        auto& [img, dpi] = cache_[page];
        double ppm = dpi / MM_PER_INCH;
        pad_x_mm = std::min(pad_x_mm, 0.08 * img.cols / ppm);
        double x = bb[0], y = bb[1], w = bb[2], h = bb[3];
        int x0 = (int)std::max(0.0, (x - pad_x_mm) * ppm), y0 = (int)std::max(0.0, (y - pad_mm) * ppm);
        int x1 = (int)std::min<double>(img.cols, (x + w + pad_x_mm) * ppm), y1 = (int)std::min<double>(img.rows, (y + h + pad_mm) * ppm);
        if (x1 <= x0 || y1 <= y0) return {};
        cv::Mat c = img(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone();
        cv::rectangle(c, cv::Point((int)(x * ppm) - x0 - 2, (int)(y * ppm) - y0 - 2),
                      cv::Point((int)((x + w) * ppm) - x0 + 2, (int)((y + h) * ppm) - y0 + 2), cv::Scalar(220, 38, 38), 2);
        if (c.cols > 640) cv::resize(c, c, cv::Size(), 640.0 / c.cols, 640.0 / c.cols, cv::INTER_AREA);
        return c;
    }

private:
    std::shared_ptr<pdf::Document> doc_;
    std::map<int, std::pair<cv::Mat, double>> cache_;
};

std::string diff_html(const json& f) {
    const json t = f.value("text", json::object());
    std::string k = f["kind"], body;
    if (k == "Changed" && t.contains("diff")) {
        std::string s = t["diff"];
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '[') {
                size_t j = s.find(']', i);
                std::string inner = s.substr(i + 1, j == std::string::npos ? std::string::npos : j - i - 1);
                size_t arrow = inner.find("→");
                std::string a = inner.substr(0, arrow), b = arrow == std::string::npos ? "" : inner.substr(arrow + 3);
                if (!a.empty()) body += "<del>" + html_escape(a) + "</del>";
                if (!b.empty()) body += "<ins>" + html_escape(b) + "</ins>";
                i = j == std::string::npos ? s.size() : j + 1;
            } else {
                size_t j = s.find('[', i);
                if (j == std::string::npos) j = s.size();
                body += html_escape(s.substr(i, j - i));
                i = j;
            }
        }
    } else if (k == "Missing") {
        body = "<del>" + esc(t.value("from", json(""))) + "</del>";
    } else if (k == "Added") {
        body = "<ins>" + esc(t.value("to", json(""))) + "</ins>";
    } else if (k == "PageNumber") {
        body = "sida " + esc(t.value("to", json())) + " där " + esc(t.value("from", json())) + " väntades";
    } else {
        std::string s = t.value("from", "");
        body = html_escape(s.empty() ? t.value("to", "") : s);
    }
    if (f.contains("context") && (k == "Changed" || k == "Missing" || k == "Added"))
        body = "<span class=ctx>…" + esc(f["context"].value("before", json(""))) + "</span> " + body + " <span class=ctx>" +
               esc(f["context"].value("after", json(""))) + "…</span>";
    std::string extra;
    if (f.contains("style")) {
        const json& st = f["style"];
        std::vector<std::string> ch;
        for (auto& c : st.value("changed", json::array())) ch.push_back(c);
        extra = "<div class=muted>" + esc(st.value("reference", json(""))) + " → <b>" + esc(st.value("sample", json(""))) + "</b>" +
                (st.contains("expected") && st["expected"] != st.value("sample", json()) ? " (väntat " + esc(st["expected"]) + ")" : "") +
                " · " + html_escape(text::join(ch, ", ")) + "</div>";
    }
    if (f.contains("move")) {
        const json& mv = f["move"];
        if (mv.contains("distance_mm"))
            extra = format("<div class=muted>flyttat %.1f mm (dx %+.1f, dy %+.1f)", mv["distance_mm"].get<double>(),
                           mv["shift_mm"][0].get<double>(), mv["shift_mm"][1].get<double>()) +
                    (mv["from_page"] != mv["to_page"] ? format(", sida %d → %d", mv["from_page"].get<int>(), mv["to_page"].get<int>()) : "") +
                    "</div>";
        else
            extra = "<div class=muted>" + esc(t.value("words", json(""))) + " ord flyttade" + (mv.value("exact", true) ? "" : " (med ändringar)") + "</div>";
    }
    return "<div class=txt>" + body + "</div>" + extra;
}

std::string where(const json& l) {
    if (l.is_null()) return "–";
    json pages = l.value("pages", json::array({l["page"]}));
    std::string s = "s. " + pages.front().dump();
    if (pages.size() > 1) s += "–" + pages.back().dump();
    return s;
}

}  // namespace

std::string document_html(const json& result) {
    const json& st = result["stats"];
    const json& fs = result["findings"];
    std::map<std::string, int> counts;
    for (auto& f : fs) counts[f["kind"].get<std::string>()]++;
    Pages refp(result["reference"]["path"]), smpp(result["sample"]["path"]);
    std::string rows;
    size_t k = 0;
    for (auto& f : fs) {
        std::string imgs;
        if (k++ < 150) {
            std::string figs;
            for (auto [side, pages, name] : {std::make_tuple("ref", &refp, "Referens"), std::make_tuple("sample", &smpp, "Prov")}) {
                const json& l = f[side];
                if (l.is_null() || l["bbox_mm"].is_null()) continue;
                cv::Mat c = pages->crop(l["page"], l["bbox_mm"]);
                if (c.empty()) continue;
                figs += std::string("<figure><img loading=lazy src='") + image::png_data_uri(c) + "' alt='" + name + "'><figcaption>" + name + " " +
                        where(l) + "</figcaption></figure>";
            }
            if (!figs.empty()) imgs = "<div class=pair>" + figs + "</div>";
        }
        std::string kind = f["kind"];
        auto col = KIND_COL.count(kind) ? KIND_COL.at(kind) : "var(--info)";
        rows += "<tr><td>" + f["id"].dump() + "</td><td><span class=kind style='--k:" + col + "'>" + (KIND_SV.count(kind) ? KIND_SV.at(kind) : kind) +
                "</span><div class=muted>" + esc(f.value("category", json(""))) + "</div></td><td><span class='sev " +
                (SEV.count(f["severity"]) ? SEV.at(f["severity"]) : "info") + "'>" + f["severity"].get<std::string>() + "</span></td><td>" +
                where(f["ref"]) + "<br>" + where(f["sample"]) + "</td><td>" + diff_html(f) + "</td><td>" + imgs + "</td></tr>";
    }
    std::string smap;
    size_t n = 0;
    for (auto& m : result.value("style_map", json::array())) {
        if (n++ >= 20) break;
        smap += "<tr><td>" + esc(m["reference"]) + "</td><td>" + esc(m["sample"]) + "</td><td>" + m["words"].dump() + "</td><td>" +
                format("%.0f %%", m["share"].get<double>() * 100) + "</td></tr>";
    }
    std::string cards;
    for (auto kk : {"Changed", "Missing", "Added", "Moved", "Style"})
        cards += "<div class=card><b style='color:" + KIND_COL.at(kk) + "'>" + std::to_string(counts[kk]) + "</b>" + text::lower(KIND_SV.at(kk)) + "</div>";
    auto fname = [](const json& p) {
        auto u = std::filesystem::u8path(p.get<std::string>()).filename().u8string();
        return html_escape(std::string(u.begin(), u.end()));
    };
    std::string mode = result["compare"]["layout"] == "fixed" ? "fast layout" : "omflödad text (layout ignoreras)";
    bool typo = result["compare"].value("typography", "normalize") == "normalize";
    std::string doc = std::string("<!doctype html>\n<html lang=\"sv\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                                  "<title>Dokumentjämförelse</title><style>") + DOC_CSS + "</style></head><body><main>\n<h1>Dokumentjämförelse</h1>\n"
        "<div class=muted>Referens: " + fname(result["reference"]["path"]) + format(" (%d s.)", st["ref_pages"].get<int>()) + " · Prov: " +
        fname(result["sample"]["path"]) + format(" (%d s.)", st["sample_pages"].get<int>()) + " · " + mode + " · printcmp " +
        result["engine"]["printcmp"].get<std::string>() + " (native)</div>\n<div class=cards>\n" +
        format("<div class=card><b>%.1f %%</b>av orden lika</div>", st["match_ratio"].get<double>() * 100) + cards +
        format("<div class=card><b>%d</b>avstavningar lagade i provet</div>", st["hyphenations_joined"]["sample"].get<int>()) + "</div>\n" +
        format("<p class=muted>Sidhuvud, sidfot och sidnummer (%d ord i provet) jämförs inte som text; sidnumreringen kontrolleras för sig. "
               "Citattecken, streck och ligaturer normaliseras%s.</p>\n", st["sample_furniture_words"].get<int>(), typo ? "" : " inte") +
        format("<h2>Skillnader (%d)</h2>\n", (int)fs.size()) +
        "<table><thead><tr><th>#</th><th>Typ</th><th>Allvar</th><th>Ref / prov</th><th>Text</th><th>Utsnitt</th></tr></thead>\n<tbody>" +
        (rows.empty() ? std::string("<tr><td colspan=6>Inga skillnader.</td></tr>") : rows) +
        "</tbody></table>\n<h2>Stilroller</h2><p class=muted>Hur referensens stilar är satta i provet (mest använda först).</p>\n"
        "<table><thead><tr><th>Referens</th><th>Prov</th><th>Ord</th><th>Andel</th></tr></thead><tbody>" + smap + "</tbody></table>\n" +
        data_check_html(result.value("data_check", json())) + "\n</main></body></html>";
    return doc;
}

}  // namespace pc::report
