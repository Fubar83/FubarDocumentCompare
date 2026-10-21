// HTML-rapporter och bildlager.
#include "printcmp/report/report.hpp"

#include <filesystem>
#include <map>
#include <opencv2/imgproc.hpp>

#include "printcmp/core/text.hpp"
#include "printcmp/image/image.hpp"

namespace pc::report {

namespace {

const std::map<std::string, std::string> SEV_CLASS = {{"Critical", "crit"}, {"Major", "major"}, {"Minor", "minor"}, {"Info", "info"}};
const std::map<std::string, std::string> KIND_SV = {
    {"Missing", "Saknas"}, {"Added", "Tillagt"}, {"Changed", "Ändrat"}, {"Moved", "Flyttat"}, {"Color", "Färg"}, {"Shape", "Form"},
    {"Barcode", "Streckkod"}, {"Uncertain", "Osäker zon"}, {"Braille", "Punktskrift"}, {"Style", "Stil"},
    {"StyleSystematic", "Stil (hela rollen)"}, {"PageNumber", "Sidnummer"}};
const std::map<std::string, std::string> CAT_SV = {
    {"Text", "Text"}, {"OutlinedText", "Konturerad text"}, {"TableCell", "Tabellcell"}, {"TableRule", "Tabellinje"},
    {"Barcode", "Streckkod"}, {"Braille", "Punktskrift"}, {"Image", "Bild"}, {"Vector", "Grafik"}, {"Rule", "Linje"},
    {"Background", "Bakgrund"}, {"TextLayer", "Textlager"}};
const std::map<std::string, std::string> STATUS_SV = {{"pass", "Godkänd"}, {"warn", "Varning"}, {"fail", "Fel"}};
const std::vector<std::string> LEVELS = {"confirmed", "high", "probable", "possible"};
const std::map<std::string, std::pair<std::string, std::string>> LEVEL_SV = {
    {"confirmed", {"Bekräftade avvikelser", "Oberoende belägg: avkodat innehåll skiljer, eller samma tydliga avvikelse på samma ställe i (nästan) alla etiketter."}},
    {"high", {"Mycket sannolika", "Tydlig signal långt över toleransen, utanför veck."}},
    {"probable", {"Troliga", "Signal över toleransen men nära den, eller i ett område med sämre underlag."}},
    {"possible", {"Möjliga – kontrollera manuellt", "Osäkra zoner (veck, registrering), svag signal eller färg uppmätt med okalibrerad scanner."}}};
const std::map<std::string, std::string> LEVEL_COLOR = {{"confirmed", "#be123c"}, {"high", "#dc2626"}, {"probable", "#d97706"}, {"possible", "#64748b"}};
const std::map<std::string, cv::Scalar> KIND_RGB = {
    {"Missing", {220, 38, 38}}, {"Added", {234, 88, 12}}, {"Changed", {192, 38, 211}}, {"Moved", {37, 99, 235}}, {"Color", {22, 163, 74}},
    {"Shape", {147, 51, 234}}, {"Barcode", {190, 18, 60}}, {"Uncertain", {148, 163, 184}}, {"Braille", {13, 148, 136}}};
const std::map<std::string, cv::Scalar> STATUS_RGB = {{"pass", {22, 163, 74}}, {"warn", {217, 119, 6}}, {"fail", {220, 38, 38}}};

std::string esc(const json& v) { return html_escape(v.is_string() ? v.get<std::string>() : v.is_null() ? "" : v.dump()); }
std::string esc(const std::string& s) { return html_escape(s); }

std::string rgb_css(const cv::Scalar& c) { return format("rgb(%d, %d, %d)", (int)c[0], (int)c[1], (int)c[2]); }
cv::Scalar kind_rgb(const std::string& k) {
    auto it = KIND_RGB.find(k);
    return it == KIND_RGB.end() ? cv::Scalar(0, 0, 0) : it->second;
}
std::string sv(const std::map<std::string, std::string>& m, const std::string& k) {
    auto it = m.find(k);
    return it == m.end() ? k : it->second;
}

cv::Mat thumb(const cv::Mat& img, int max_w, int max_h = 220) {
    double s = std::min({1.0, (double)max_w / img.cols, (double)max_h / img.rows});
    if (s >= 1.0) return img;
    cv::Mat o;
    cv::resize(img, o, cv::Size(std::max(1, (int)(img.cols * s)), std::max(1, (int)(img.rows * s))), 0, 0, cv::INTER_AREA);
    return o;
}

std::pair<cv::Mat, cv::Rect> crop_mm(const cv::Mat& img, const json& bb, double dpi, double pad_mm = 2.0, double min_mm = 6.0) {
    double ppm = dpi / MM_PER_INCH;
    double x = bb[0], y = bb[1], w = bb[2], h = bb[3];
    double cx = x + w / 2, cy = y + h / 2, hw = std::max(w / 2 + pad_mm, min_mm / 2), hh = std::max(h / 2 + pad_mm, min_mm / 2);
    int x0 = (int)std::max(0.0, (cx - hw) * ppm), y0 = (int)std::max(0.0, (cy - hh) * ppm);
    int x1 = (int)std::min<double>(img.cols, (cx + hw) * ppm), y1 = (int)std::min<double>(img.rows, (cy + hh) * ppm);
    if (x1 <= x0 || y1 <= y0) return {cv::Mat(), {}};
    cv::Mat c = img(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone();
    cv::Rect b((int)(x * ppm) - x0, (int)(y * ppm) - y0, (int)((x + w) * ppm) - (int)(x * ppm), (int)((y + h) * ppm) - (int)(y * ppm));
    return {c, b};
}

const char* LABEL_CSS = R"(
:root{--bg:#fafaf9;--fg:#1c1917;--muted:#78716c;--card:#fff;--line:#e7e5e4;--crit:#be123c;--major:#dc2626;--minor:#d97706;--info:#64748b;--pass:#16a34a}
@media (prefers-color-scheme:dark){:root{--bg:#0c0a09;--fg:#e7e5e4;--muted:#a8a29e;--card:#1c1917;--line:#292524}}
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:1200px;margin:0 auto;padding:24px 16px 64px} h1{font-size:22px;margin:0 0 4px} h2{font-size:16px;margin:32px 0 12px}
.muted{color:var(--muted)} .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin:16px 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px} .card b{display:block;font-size:22px}
img{max-width:100%;display:block;border-radius:6px;border:1px solid var(--line)}
table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}
th,td{text-align:left;vertical-align:top;padding:10px;border-bottom:1px solid var(--line)} th{font-size:12px;color:var(--muted);font-weight:600}
td.num{color:var(--muted)} .sev{font-size:12px;font-weight:600;padding:2px 8px;border-radius:99px;color:#fff;white-space:nowrap}
.sev.crit{background:var(--crit)} .sev.major{background:var(--major)} .sev.minor{background:var(--minor)} .sev.info{background:var(--info)}
.kind{border-left:4px solid var(--k);padding-left:6px;font-weight:600;white-space:nowrap}
.pair{display:flex;gap:8px;flex-wrap:wrap} figure{margin:0} figcaption{font-size:12px;color:var(--muted);margin-top:4px}
.layer{display:inline-block;padding:1px 8px;margin:2px;border-radius:99px;border:1px solid var(--line);font-size:12px}
.layer.off{color:var(--muted);text-decoration:line-through} del{color:var(--major)} ins{color:var(--pass);text-decoration:none;font-weight:600}
.levels{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin:8px 0 16px}
.lv{display:block;text-decoration:none;color:var(--fg);background:var(--card);border:1px solid var(--line);border-left:5px solid var(--c);border-radius:10px;padding:10px 12px}
.lv b{display:block;font-size:22px;color:var(--c)}
h3{font-size:15px;margin:28px 0 4px;padding-left:10px;border-left:5px solid var(--c)} .lvdesc{margin:0 0 10px 15px}
.cert{display:flex;align-items:center;gap:8px;min-width:120px} .bar{flex:1;height:8px;background:var(--line);border-radius:99px;overflow:hidden}
.bar i{display:block;height:100%} ul.why{margin:6px 0 0;padding-left:16px;font-size:12px;color:var(--muted);max-width:260px}
.gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:16px}
.inst{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px}
.st.pass{color:var(--pass)} .st.warn{color:var(--minor)} .st.fail{color:var(--major)}
.pill{display:inline-block;padding:1px 8px;border-radius:99px;font-size:12px;font-weight:600;color:#fff} code{font:12px ui-monospace,Consolas,monospace}
@media (max-width:760px){table,thead,tbody,tr,td,th{display:block} thead{display:none} td{border:0;padding:4px 10px} tr{border-bottom:1px solid var(--line);padding:8px 0}}
)";

std::string contents(const json& result) {
    const json& ref = result["reference"];
    std::vector<std::string> rows;
    for (auto& t : ref.value("tables", json::array())) {
        std::string k = t["kind"];
        std::string kind = k == "ruled_grid" ? "med rutnät" : k == "ruled_horizontal" ? "med vågräta linjer" : "utan linjer";
        std::vector<std::string> first;
        if (!t["cells"].empty())
            for (auto& c : t["cells"][0])
                if (!c.get<std::string>().empty()) first.push_back(esc(c));
        rows.push_back(format("<li>Tabell %d (%s): %d rader × %d kolumner", t["id"].get<int>(), kind.c_str(), t["rows"].get<int>(),
                              t["cols"].get<int>()) +
                       (t["rules"].get<int>() ? ", " + std::to_string(t["rules"].get<int>()) + " linjer" : "") +
                       " – <span class=muted>" + text::join(first, " · ") + "</span></li>");
    }
    for (auto& b : ref.value("braille", json::array()))
        rows.push_back("<li>Punktskrift: ”" + esc(b["text"]) + "” " + b["unicode"].get<std::string>() +
                       format(" <span class=muted>(%d celler, punktavstånd %g mm, cellavstånd %g mm)</span></li>", b["cells"].get<int>(),
                              b["dot_pitch_mm"].get<double>(), b["cell_pitch_mm"].get<double>()));
    if (!ref.value("outlined_text_mm", json::array()).empty())
        rows.push_back("<li>Konturerad text: " + std::to_string(ref["outlined_text_mm"].size()) + " rad(er)</li>");
    for (auto& c : ref.value("barcodes", json::array())) rows.push_back("<li>Streckkod " + esc(c["symbology"]) + ": " + esc(c["data"]) + "</li>");
    json lay = ref.value("layers", json::object());
    if (!lay.value("ocgs", json::array()).empty()) {
        json comp = lay.value("compared", json::object());
        std::string items;
        for (auto& g : lay["ocgs"]) {
            bool on = comp.value(g["name"].get<std::string>(), true);
            items += std::string("<span class=\"layer ") + (on ? "on" : "off") + "\">" + (on ? "✓" : "✗") + " " + esc(g["name"]) + "</span> ";
        }
        rows.push_back("<li>PDF-lager (✓ jämförs, ✗ uteslutet): " + items + "</li>");
    }
    if (!lay.value("spots", json::array()).empty()) {
        std::vector<std::string> s;
        for (auto& x : lay["spots"]) s.push_back(esc(x));
        rows.push_back("<li>Dekorfärger: " + text::join(s, ", ") + "</li>");
    }
    if (rows.empty()) return "";
    return "<h2>Identifierat i originalet</h2><ul>" + text::join(rows, "") + "</ul>";
}

}  // namespace

void write_json(const std::string& path, const json& result) { write_file(path, result.dump(2, ' ', false, json::error_handler_t::replace)); }

void write_layers(const std::string& dir, const compare::LabelResult& r, double layer_dpi) {
    std::filesystem::create_directories(std::filesystem::u8path(dir));
    double s = std::min(1.0, layer_dpi / r.dpi);
    auto sc = [&](const cv::Mat& m) {
        if (s >= 1) return m;
        cv::Mat o;
        cv::resize(m, o, cv::Size(), s, s, cv::INTER_AREA);
        return o;
    };
    for (size_t k = 0; k < r.aligned.size(); ++k) {
        int idx = r.instances[k].index;
        cv::Mat ref = sc(r.ref_rgb), smp = sc(r.aligned[k]), ov = smp.clone();
        for (auto& d : r.result["differences"]) {
            if (!d.contains("bbox_px") || d["bbox_px"].is_null()) continue;
            if (std::none_of(d["instances"].begin(), d["instances"].end(), [&](const json& x) { return x == idx; })) continue;
            int x = (int)pyround(d["bbox_px"][0].get<double>() * s), y = (int)pyround(d["bbox_px"][1].get<double>() * s);
            int w = (int)pyround(d["bbox_px"][2].get<double>() * s), h = (int)pyround(d["bbox_px"][3].get<double>() * s);
            cv::rectangle(ov, cv::Point(x - 3, y - 3), cv::Point(x + w + 3, y + h + 3), kind_rgb(d["kind"]), 2);
        }
        std::string base = dir + format("/inst%03d_", idx);
        image::write_png(base + "ref.png", ref);
        image::write_png(base + "sample.png", smp);
        image::write_png(base + "overlay.png", ov);
    }
}

std::string data_check_html(const json& dc, const std::string& title) {
    if (dc.is_null() || !dc.is_object()) return "";
    static const std::map<std::string, std::pair<std::string, std::string>> ST = {
        {"found", {"Finns", "var(--pass)"}}, {"near", {"Nästan", "var(--minor)"}}, {"missing", {"Saknas", "var(--crit)"}},
        {"mismatch", {"Fel kod", "var(--crit)"}}};
    std::string rows;
    for (auto& it : dc["items"]) {
        auto [lab, col] = ST.at(it["status"].get<std::string>());
        std::string where = it.contains("locations") && !it["locations"].empty() ? "s. " + std::to_string(it["locations"][0]["page"].get<int>()) : "";
        std::string got, status = it["status"];
        if (status == "near") {
            got = "<code>" + esc(it.value("found_text", "")) + "</code><br><span class=muted>" + esc(it.value("diff", "")) +
                  (it.contains("note") ? " · " + esc(it["note"]) : "") + "</span>";
        } else if (status == "found") {
            std::vector<std::string> parts;
            if (it.value("count", 0)) parts.push_back(std::to_string(it["count"].get<int>()) + " st i texten");
            if (it.contains("barcode")) parts.push_back("streckkod " + esc(it["barcode"]["data"]));
            got = text::join(parts, " · ");
        } else if (status == "mismatch") {
            got = "streckkod <code>" + esc(it["barcode"]["data"]) + "</code><br><span class=muted>" + esc(it["barcode"].value("diff", "")) + "</span>";
        }
        std::string warn = it.contains("warning") ? "<div class=muted>" + esc(it["warning"]) + "</div>" : "";
        rows += "<tr><td><span class=pill style='background:" + col + "'>" + lab + "</span></td><td>" + esc(it["field"]) +
                "</td><td><code>" + esc(it["expected"]) + "</code>" + warn + "</td><td>" + got + "</td><td>" + where + "</td></tr>";
    }
    const json& s = dc["summary"];
    std::string var;
    if (dc["variants"].get<int>() > 1) {
        std::string how = dc["variant_selection"] == "best" ? "bäst stämmande" : "vald";
        var = format(" · variant %d av %d (%s)", dc["variant"].get<int>() + 1, dc["variants"].get<int>(), how.c_str());
    }
    std::string fname;
    if (dc.contains("path")) {
        auto u = std::filesystem::u8path(dc["path"].get<std::string>()).filename().u8string();
        fname = std::string(u.begin(), u.end());
    }
    return "<h2 id=data>" + title + "</h2><p class=muted>" + esc(fname) + " · " +
           format("%d finns, %d nästan, %d saknas, %d fel kod", s["found"].get<int>(), s["near"].get<int>(), s["missing"].get<int>(),
                  s["mismatch"].get<int>()) +
           var + (dc.contains("checked") ? " · kontrollerat i " + esc(dc["checked"]) : "") +
           "</p><table><thead><tr><th>Utfall</th><th>Fält</th><th>Förväntat</th><th>Hittat</th><th>Var</th></tr></thead><tbody>" + rows +
           "</tbody></table>";
}

void write_label_html(const std::string& path, const compare::LabelResult& r) {
    const json& result = r.result;
    // arköversikt
    double s = std::min(1.0, 900.0 / r.sample_rgb.cols);
    cv::Mat sheet;
    cv::resize(r.sample_rgb, sheet, cv::Size(), s, s, cv::INTER_AREA);
    double w = r.ref_rgb.cols, h = r.ref_rgb.rows;
    for (auto& io : result["instances"]) {
        Mat3 H;
        for (int k = 0; k < 9; ++k) H.m[k] = io["transform"]["h"][k];
        std::vector<cv::Point> poly;
        double cx = 0, cy = 0;
        for (Pt c : {Pt{0, 0}, Pt{w, 0}, Pt{w, h}, Pt{0, h}}) {
            Pt p = H.apply(c);
            poly.emplace_back((int)pyround(p.x * s), (int)pyround(p.y * s));
            cx += p.x * s / 4;
            cy += p.y * s / 4;
        }
        cv::Scalar col = STATUS_RGB.at(io["status"].get<std::string>());
        cv::polylines(sheet, poly, true, col, 2, cv::LINE_AA);
        cv::putText(sheet, std::to_string(io["index"].get<int>()), cv::Point((int)cx - 8, (int)cy + 8), cv::FONT_HERSHEY_SIMPLEX, 0.8, col, 2, cv::LINE_AA);
    }
    std::map<int, size_t> inst_pos;
    for (size_t k = 0; k < r.instances.size(); ++k) inst_pos[r.instances[k].index] = k;
    std::map<std::string, std::vector<std::string>> rows_by_level;
    for (auto& d : result["differences"]) {
        std::string thumbs;
        if (d.contains("bbox_mm") && !d["bbox_mm"].is_null() && !d["instances"].empty() && d["kind"] != "Uncertain") {
            int inst = d["instances"][0];
            if (inst_pos.count(inst)) {
                auto [rc, b] = crop_mm(r.ref_rgb, d["bbox_mm"], r.dpi);
                auto [sc, b2] = crop_mm(r.aligned[inst_pos[inst]], d["bbox_mm"], r.dpi);
                if (!rc.empty() && !sc.empty()) {
                    cv::Scalar col = kind_rgb(d["kind"]);
                    for (cv::Mat* im : {&rc, &sc})
                        cv::rectangle(*im, cv::Point(b.x - 2, b.y - 2), cv::Point(b.x + b.width + 2, b.y + b.height + 2), col, 1, cv::LINE_AA);
                    thumbs = "<div class=\"pair\"><figure><img src=\"" + image::png_data_uri(thumb(rc, 260)) +
                             "\"><figcaption>Referens</figcaption></figure><figure><img src=\"" + image::png_data_uri(thumb(sc, 260)) +
                             "\"><figcaption>Prov (instans " + std::to_string(inst) + ")</figcaption></figure></div>";
                }
            }
        }
        std::string where = "–";
        if (d.contains("bbox_mm") && !d["bbox_mm"].is_null())
            where = format("x %.1f, y %.1f mm<br><span class=muted>%.1f × %.1f mm</span>", d["bbox_mm"][0].get<double>(),
                           d["bbox_mm"][1].get<double>(), d["bbox_mm"][2].get<double>(), d["bbox_mm"][3].get<double>());
        std::vector<std::string> il;
        for (auto& x : d["instances"]) il.push_back(std::to_string(x.get<int>()));
        std::string insts_txt = (d.value("systematic", false) ? "Systematisk: " : "") + (il.empty() ? std::string("–") : text::join(il, ", "));
        if (d.value("reason", "") == "instance_not_found")
            insts_txt = format("Hittade %d av %d instanser", d["found"].get<int>(), d["expected"].get<int>());
        std::vector<std::string> details;
        std::vector<std::string> texts;
        for (auto& o : d.value("objects", json::array()))
            if (o.contains("text") && texts.size() < 6) texts.push_back(o["text"]);
        if (!texts.empty()) details.push_back("Text: " + esc(text::join(texts, " ")));
        json m = d.value("metrics", json::object());
        if (m.contains("shift_mm") && m["shift_mm"].is_number() && m["shift_mm"].get<double>() != 0)
            details.push_back(format("Förskjutning %.2f mm", m["shift_mm"].get<double>()));
        if (m.contains("delta_e") && m["delta_e"].is_number() && d["kind"] == "Color")
            details.push_back(format("ΔE00 %.1f", m["delta_e"].get<double>()));
        std::string cat = d.value("category", "");
        if (CAT_SV.count(cat)) details.insert(details.begin(), "<b>" + CAT_SV.at(cat) + "</b>");
        if (d.contains("table")) {
            const json& ti = d["table"];
            if (ti.value("element", "") == "rule") {
                if (ti.contains("between_rows") && !ti["between_rows"].is_null())
                    details.push_back(format("Tabell %d: linjen mellan rad %d och %d", ti["table"].get<int>(),
                                             ti["between_rows"][0].get<int>() + 1, ti["between_rows"][1].get<int>() + 1));
                else
                    details.push_back(format("Tabell %d: %s", ti["table"].get<int>(), ti.value("orient", "") == "h" ? "yttre ram" : "lodrät linje"));
            } else if (ti.value("row_shift", false)) {
                details.push_back(format("Tabell %d: rad %d förskjuten (”", ti["table"].get<int>(), ti["row"].get<int>() + 1) +
                                  esc(ti.value("row_label", json(""))) + "”)");
            } else {
                std::string wh = format("rad %d", ti["row"].get<int>() + 1);
                if (ti.contains("col") && !ti["col"].is_null()) wh += format(", kolumn %d", ti["col"].get<int>() + 1);
                std::string lab = ti.contains("row_label") && ti["row_label"].is_string() ? esc(ti["row_label"]) : "";
                std::string cell = ti.contains("cell_text") && ti["cell_text"].is_string() ? esc(ti["cell_text"]) : "";
                bool col_truthy = ti.contains("col") && ti["col"].is_number() && ti["col"].get<int>() != 0;
                details.push_back(format("Tabell %d: ", ti["table"].get<int>()) + wh + (lab.empty() ? "" : " – " + lab) +
                                  (!cell.empty() && col_truthy ? " = ”" + cell + "” i originalet" : ""));
            }
        }
        if (d.contains("braille") && d["kind"] == "Braille") {
            const json& bi = d["braille"];
            if (d.value("cause", "") == "embossing_missing")
                details.push_back("Prägling saknas för hela raden (väntat ”" + esc(bi.value("expected_text", json(""))) + "”)");
            else if (bi.contains("cell")) {
                details.push_back(format("Cell %d: väntat ”", bi["cell"].get<int>() + 1) + esc(bi["expected"]) + "” " +
                                  bi["expected_unicode"].get<std::string>() + ", läst ”" + esc(bi["read"]) + "” " +
                                  bi["read_unicode"].get<std::string>());
                auto list = [](const json& a) {
                    std::vector<std::string> v;
                    for (auto& x : a) v.push_back(std::to_string(x.get<int>()));
                    return text::join(v, ", ");
                };
                if (!bi["missing_dots"].empty()) details.push_back("Saknade punkter: " + list(bi["missing_dots"]));
                if (!bi["extra_dots"].empty()) details.push_back("Extra punkter: " + list(bi["extra_dots"]));
            }
        }
        int ntc = 0;
        for (auto& tc : d.value("text_change", json::array())) {
            if (ntc++ >= 4) break;
            std::string k = tc.value("kind", "");
            if (k == "Style")
                details.push_back("Text ”" + esc(tc["from"]) + "”: " + esc(tc.value("font_from", json(""))) + " " +
                                  tc.value("size_from", json()).dump() + " pt → " + esc(tc.value("font_to", json(""))) + " " +
                                  tc.value("size_to", json()).dump() + " pt");
            else if (k == "Moved")
                details.push_back("Text ”" + esc(tc["from"]) + "” flyttad " + tc.value("shift_mm", json()).dump() + " mm");
            else {
                std::string ksv = k == "Changed" ? "ändrad" : k == "Missing" ? "borttagen" : k == "Added" ? "tillagd" : "";
                std::string df = tc.contains("diff") ? " <span class=muted>(" + esc(tc["diff"]) + ")</span>" : "";
                details.push_back("Text " + ksv + ": ”<del>" + esc(tc.value("from", json(""))) + "</del>” → ”<ins>" +
                                  esc(tc.value("to", json(""))) + "</ins>”" + df);
            }
        }
        if (d.value("text_only", false)) details.push_back("<i>Endast i textlagret – syns inte i tryck</i>");
        if (d.contains("barcode"))
            details.push_back("Streckkod: väntat " + esc(d["barcode"].value("expected", json())) + ", läst " +
                              (d["barcode"].value("read", json()).is_null() ? std::string("None") : esc(d["barcode"]["read"])));
        if (d.contains("cause") && d["cause"].is_string() && !d["cause"].get<std::string>().empty()) {
            std::string c = d["cause"];
            details.push_back(c == "fold" ? "Veck" : c == "registration" ? "Osäker registrering" : esc(c));
        }
        if (d.value("source", "") == "consensus") details.push_back("Hittad via jämförelse mellan instanser");
        json cert = d.value("certainty", json{{"level", "possible"}, {"score", 0}, {"reasons", json::array()}});
        std::string lvl = cert["level"], lc = LEVEL_COLOR.at(lvl);
        std::string why;
        for (auto& rr : cert["reasons"]) why += "<li>" + esc(rr) + "</li>";
        std::string cert_html = format("<div class=cert><div class=bar><i style=\"width:%d%%;background:%s\"></i></div><b>%d %%</b></div><ul class=why>",
                                       cert["score"].get<int>(), lc.c_str(), cert["score"].get<int>()) + why + "</ul>";
        rows_by_level[lvl].push_back("<tr><td class=num>" + std::to_string(d["id"].get<int>()) + "</td><td>" + cert_html +
                                     "</td><td><span class=\"sev " + sv(SEV_CLASS, d["severity"]) + "\">" + d["severity"].get<std::string>() +
                                     "</span></td><td><span class=kind style=\"--k: " + rgb_css(kind_rgb(d["kind"])) + "\">" +
                                     sv(KIND_SV, d["kind"]) + "</span></td><td>" + insts_txt + "</td><td>" + where + "</td><td>" +
                                     (details.empty() ? std::string("–") : text::join(details, "<br>")) + "</td><td>" + thumbs + "</td></tr>");
    }
    std::string gallery;
    for (auto& io : result["instances"]) {
        int i = io["index"];
        std::string img;
        if (inst_pos.count(i)) {
            cv::Mat ov = r.aligned[inst_pos[i]].clone();
            for (auto& d : result["differences"]) {
                if (!d.contains("bbox_px") || d["bbox_px"].is_null()) continue;
                if (std::none_of(d["instances"].begin(), d["instances"].end(), [&](const json& x) { return x == i; })) continue;
                int x = d["bbox_px"][0], y = d["bbox_px"][1], ww = d["bbox_px"][2], hh = d["bbox_px"][3];
                cv::rectangle(ov, cv::Point(x - 3, y - 3), cv::Point(x + ww + 3, y + hh + 3), kind_rgb(d["kind"]), 2);
            }
            img = "<img src=\"" + image::png_data_uri(thumb(ov, 360, 400)) + "\">";
        }
        std::string st = io["status"];
        std::string rms = io["registration_rms_px"].is_null() ? "–" : format("%.2f px", io["registration_rms_px"].get<double>());
        std::string mde = io["mean_delta_e"].is_null() ? "–" : io["mean_delta_e"].dump();
        std::string braille;
        if (!io["braille"].is_null()) {
            std::vector<std::string> v;
            for (auto& x : io["braille"]["read"]) v.push_back(x.is_null() ? "–" : x.get<std::string>());
            braille = " · punktskrift: " + esc(text::join(v, " / "));
        }
        gallery += "<figure class=\"inst " + st + "\">" + img + format("<figcaption><b>Instans %d</b> · rad %d, kol %d", i,
                   io["row"].get<int>() + 1, io["col"].get<int>() + 1) + " · <span class=\"st " + st + "\">" + STATUS_SV.at(st) +
                   "</span><br><span class=muted>registrering " + rms + " · ΔE medel " + mde + (io["folds"].empty() ? "" : " · veck") +
                   braille + "</span></figcaption></figure>";
    }
    const json& sm = result["summary"];
    const json& sh = result["sheet"];
    auto fname = [](const json& p) {
        auto u = std::filesystem::u8path(p.get<std::string>()).filename().u8string();
        return html_escape(std::string(u.begin(), u.end()));
    };
    std::string levels_nav, sections;
    for (auto& l : LEVELS) {
        auto& rows = rows_by_level[l];
        levels_nav += "<a class=lv href=\"#lv-" + l + "\" style=\"--c:" + LEVEL_COLOR.at(l) + "\"><b>" + std::to_string(rows.size()) + "</b>" +
                      LEVEL_SV.at(l).first + "</a>";
        if (rows.empty()) continue;
        sections += "<h3 id=\"lv-" + l + "\" style=\"--c:" + LEVEL_COLOR.at(l) + "\">" + LEVEL_SV.at(l).first + " (" + std::to_string(rows.size()) +
                    ")</h3><p class=\"muted lvdesc\">" + LEVEL_SV.at(l).second +
                    "</p><table><thead><tr><th>#</th><th>Säkerhet</th><th>Allvar</th><th>Typ</th><th>Instanser</th><th>Position</th>"
                    "<th>Detaljer</th><th>Utsnitt</th></tr></thead><tbody>" + text::join(rows, "") + "</tbody></table>";
    }
    std::string timing;
    for (auto& [k, v] : result["timing_ms"].items()) timing += (timing.empty() ? "" : ", ") + k + " " + v.dump();
    double rot = std::fmod(std::fmod(sh["rotation_deg"].get<double>() + 90, 180) + 180, 180) - 90;
    std::string doc = std::string("<!doctype html>\n<html lang=\"sv\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                                  "<title>Jämförelserapport</title>\n<style>") + LABEL_CSS + "</style></head><body><main>\n<h1>Jämförelserapport</h1>\n"
        "<div class=muted>Referens: " + fname(result["reference"]["path"]) + " · Prov: " + fname(result["sample"]["path"]) + " · printcmp " +
        result["engine"]["printcmp"].get<std::string>() + " (native)</div>\n<div class=cards>\n" +
        format("<div class=card><b>%d%s</b>instanser hittade</div>", sh["instances_found"].get<int>(),
               sh["instances_expected"].is_null() ? "" : (" / " + std::to_string(sh["instances_expected"].get<int>())).c_str()) +
        format("<div class=card><b style=\"color:var(--major)\">%d</b>underkända</div>", sm["instances_fail"].get<int>()) +
        format("<div class=card><b style=\"color:var(--minor)\">%d</b>med varning</div>", sm["instances_warn"].get<int>()) +
        format("<div class=card><b style=\"color:var(--pass)\">%d</b>godkända</div>", sm["instances_pass"].get<int>()) +
        format("<div class=card><b>%d</b>skillnader (%d systematiska)</div>", sm["differences"].get<int>(), sm["systematic"].get<int>()) +
        format("<div class=card><b>%+.2f°</b>arkets snedställning</div>", rot) + "</div>\n" + contents(result) +
        "\n<h2>Ark</h2><img src=\"" + image::png_data_uri(sheet) + "\" alt=\"Arköversikt\">\n<h2>Skillnader, säkrast först</h2>\n<div class=levels>" +
        levels_nav + "</div>\n" + (sections.empty() ? std::string("<p>Inga skillnader hittades.</p>") : sections) +
        "\n<h2>Instanser</h2><div class=gallery>" + gallery + "</div>\n" + data_check_html(result.value("data_check", json())) +
        "\n<p class=muted>Tider (ms): " + timing + "</p>\n</main></body></html>";
    write_file(path, doc);
}

}  // namespace pc::report
