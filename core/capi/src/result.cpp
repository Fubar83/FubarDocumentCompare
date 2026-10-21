// Resultat: typade vyer (strängar som lever lika länge som resultatet), utsnitt och export.
#include "internal.hpp"

// ------------------------------------------------------------------ resultat: typade vyer

void fdc_result::prepare() {
    const json& F = findings();
    for (auto& f : F) {
        FindingStrings s;
        s.category = f.value("category", "");
        if (kind == FDC_RESULT_DOCUMENT) {
            if (f.contains("text")) {
                s.from = str_of(f["text"].value("from", json()));
                s.to = str_of(f["text"].value("to", json()));
                s.has_from = !s.from.empty();
                s.has_to = !s.to.empty();
            }
            std::string k = f["kind"];
            if (f.contains("style"))
                s.detail = f["style"].value("reference", "") + " → " + f["style"].value("sample", "");
            else if (f.contains("move") && f["move"].contains("distance_mm"))
                s.detail = format("flyttad %.1f mm", f["move"]["distance_mm"].get<double>());
            else if (f.contains("text") && f["text"].contains("diff"))
                s.detail = f["text"]["diff"];
            else
                s.detail = k;
        } else {
            if (f.contains("text_change") && !f["text_change"].empty()) {
                s.from = str_of(f["text_change"][0].value("from", json()));
                s.to = str_of(f["text_change"][0].value("to", json()));
            } else if (f.contains("barcode")) {
                s.from = str_of(f["barcode"].value("expected", json()));
                s.to = str_of(f["barcode"].value("read", json()));
            } else if (f.contains("braille") && f["braille"].contains("expected")) {
                s.from = str_of(f["braille"]["expected"]);
                s.to = str_of(f["braille"].value("read", json()));
            }
            s.has_from = !s.from.empty();
            s.has_to = !s.to.empty();
            std::vector<std::string> parts;
            for (auto& o : f.value("objects", json::array()))
                if (o.contains("text") && parts.size() < 6) parts.push_back(o["text"]);
            if (f.contains("table") && f["table"].contains("row") && !f["table"]["row"].is_null())
                s.detail = format("Tabell %d, rad %d", f["table"]["table"].get<int>(), f["table"]["row"].get<int>() + 1);
            else if (f.contains("metrics") && f["metrics"].value("shift_mm", json()).is_number())
                s.detail = format("Förskjutning %.2f mm", f["metrics"]["shift_mm"].get<double>());
            else if (!parts.empty())
                s.detail = "Text: " + text::join(parts, " ");
            else if (f.contains("cause"))
                s.detail = str_of(f["cause"]);
        }
        std::vector<std::string> rs;
        if (f.contains("certainty"))
            for (auto& r : f["certainty"].value("reasons", json::array())) rs.push_back(r);
        s.reasons = text::join(rs, "\n");
        // fingeravtryck: typ, kategori, läge (0,5 mm) och text – stabilt mellan körningar och versioner
        std::string pos;
        const json* bb = nullptr;
        if (kind == FDC_RESULT_DOCUMENT) {
            const json& l = !f["ref"].is_null() ? f["ref"] : f["sample"];
            if (!l.is_null() && !l["bbox_mm"].is_null()) bb = &l["bbox_mm"];
        } else if (f.contains("bbox_mm") && !f["bbox_mm"].is_null()) {
            bb = &f["bbox_mm"];
        }
        if (bb) pos = format("%.0f,%.0f", (*bb)[0].get<double>() * 2, (*bb)[1].get<double>() * 2);
        s.fingerprint = sha256_hex(f["kind"].get<std::string>() + "|" + s.category + "|" + pos + "|" + s.from + "|" + s.to).substr(0, 16);
        fs.push_back(std::move(s));
    }
    if (auto dc = data_check())
        for (auto& it : (*dc)["items"]) {
            DataStrings d;
            d.field = str_of(it["field"]);
            d.expected = str_of(it["expected"]);
            if (it["status"] == "near") d.found = it.value("found_text", "");
            else if (it["status"] == "mismatch" && it.contains("barcode")) d.found = str_of(it["barcode"]["data"]);
            d.diff = it.contains("diff") ? str_of(it["diff"]) : it.contains("barcode") ? str_of(it["barcode"].value("diff", json())) : "";
            ds.push_back(std::move(d));
        }
}


extern "C" {

FDC_API fdc_result_kind fdc_result_get_kind(const fdc_result* r) { return r ? r->kind : FDC_RESULT_LABEL; }

FDC_API fdc_status fdc_result_summary(const fdc_result* r, fdc_summary* out) {
    if (!r || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller out saknas");
    fdc_summary s{};
    s.size = sizeof s;
    s.differences = (int)r->findings().size();
    if (r->kind == FDC_RESULT_LABEL) {
        const json& sm = r->j["summary"];
        s.labels_found = r->j["sheet"]["instances_found"];
        s.labels_expected = r->j["sheet"]["instances_expected"].is_null() ? 0 : r->j["sheet"]["instances_expected"].get<int>();
        s.labels_pass = sm["instances_pass"];
        s.labels_warn = sm["instances_warn"];
        s.labels_fail = sm["instances_fail"];
    } else if (r->kind == FDC_RESULT_DOCUMENT) {
        s.match_ratio = r->j["stats"]["match_ratio"];
    }
    if (auto dc = r->data_check()) {
        s.data_found = (*dc)["summary"]["found"];
        s.data_near = (*dc)["summary"]["near"];
        s.data_missing = (*dc)["summary"]["missing"];
        s.data_mismatch = (*dc)["summary"]["mismatch"];
    }
    uint32_t n = std::min<uint32_t>(out->size ? out->size : (uint32_t)sizeof s, (uint32_t)sizeof s);
    std::memcpy(out, &s, n);
    out->size = n;
    return FDC_OK;
}

FDC_API int fdc_result_finding_count(const fdc_result* r) { return r ? (int)r->findings().size() : 0; }

FDC_API fdc_status fdc_result_finding(const fdc_result* r, int i, fdc_finding* out) {
    if (!r || !out || i < 0 || i >= (int)r->findings().size()) return set_error(FDC_ERR_INVALID_ARGUMENT, "ogiltigt index");
    const json& f = r->findings()[i];
    const FindingStrings& s = r->fs[i];
    fdc_finding o{};
    o.size = sizeof o;
    o.id = f.value("id", i + 1);
    o.kind = kind_of(f["kind"]);
    o.severity = sev_of(f["severity"]);
    o.certainty = f.contains("certainty") ? cert_of(f["certainty"]["level"]) : FDC_CERT_POSSIBLE;
    o.score = f.contains("certainty") ? f["certainty"]["score"].get<int>() : 0;
    o.systematic = f.value("systematic", false);
    const json* bb = nullptr;
    if (r->kind == FDC_RESULT_DOCUMENT) {
        const json& l = !f["ref"].is_null() ? f["ref"] : f["sample"];
        if (!l.is_null()) {
            o.page = l["page"];
            if (!l["bbox_mm"].is_null()) bb = &l["bbox_mm"];
        }
    } else {
        auto ins = f.value("instances", json::array());
        o.label = ins.empty() ? 0 : ins[0].get<int>();
        o.label_count = (int)ins.size();
        if (f.contains("bbox_mm") && !f["bbox_mm"].is_null()) bb = &f["bbox_mm"];
    }
    if (bb) {
        o.x_mm = (*bb)[0];
        o.y_mm = (*bb)[1];
        o.w_mm = (*bb)[2];
        o.h_mm = (*bb)[3];
    }
    o.category = s.category.c_str();
    o.text_from = s.has_from ? s.from.c_str() : nullptr;
    o.text_to = s.has_to ? s.to.c_str() : nullptr;
    o.detail = s.detail.c_str();
    o.reasons = s.reasons.c_str();
    o.fingerprint = s.fingerprint.c_str();
    uint32_t n = std::min<uint32_t>(out->size ? out->size : (uint32_t)sizeof o, (uint32_t)sizeof o);
    std::memcpy(out, &o, n);
    out->size = n;
    return FDC_OK;
}

FDC_API int fdc_result_label_count(const fdc_result* r) { return r && r->kind == FDC_RESULT_LABEL ? (int)r->j["instances"].size() : 0; }

FDC_API fdc_status fdc_result_label(const fdc_result* r, int i, fdc_label_info* out) {
    if (!r || !out || r->kind != FDC_RESULT_LABEL || i < 0 || i >= (int)r->j["instances"].size())
        return set_error(FDC_ERR_INVALID_ARGUMENT, "ogiltigt index");
    const json& io = r->j["instances"][i];
    fdc_label_info o{};
    o.size = sizeof o;
    o.index = io["index"];
    o.row = io["row"];
    o.col = io["col"];
    std::string st = io["status"];
    o.status = st == "fail" ? FDC_LABEL_FAIL : st == "warn" ? FDC_LABEL_WARN : FDC_LABEL_PASS;
    o.rotation_deg = io["angle_deg"];
    Mat3 H;
    for (int k = 0; k < 9; ++k) H.m[k] = io["transform"]["h"][k];
    double w = r->label.ref_rgb.cols, h = r->label.ref_rgb.rows;
    Pt cs[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    for (int k = 0; k < 4; ++k) {
        Pt p = H.apply(cs[k]);
        o.corners[k] = {p.x, p.y};
    }
    o.folds = (int)io["folds"].size();
    o.registration_rms_px = io["registration_rms_px"].is_null() ? -1.0 : io["registration_rms_px"].get<double>();
    uint32_t n = std::min<uint32_t>(out->size ? out->size : (uint32_t)sizeof o, (uint32_t)sizeof o);
    std::memcpy(out, &o, n);
    out->size = n;
    return FDC_OK;
}

FDC_API int fdc_result_data_item_count(const fdc_result* r) { return r ? (int)r->ds.size() : 0; }

FDC_API fdc_status fdc_result_data_item(const fdc_result* r, int i, fdc_data_item* out) {
    if (!r || !out || i < 0 || i >= (int)r->ds.size()) return set_error(FDC_ERR_INVALID_ARGUMENT, "ogiltigt index");
    const json& it = (*r->data_check())["items"][i];
    const DataStrings& s = r->ds[i];
    fdc_data_item o{};
    o.size = sizeof o;
    o.field = s.field.c_str();
    o.expected = s.expected.c_str();
    std::string st = it["status"];
    o.status = st == "found" ? FDC_DATA_FOUND : st == "near" ? FDC_DATA_NEAR : st == "mismatch" ? FDC_DATA_MISMATCH : FDC_DATA_MISSING;
    o.found_text = s.found.empty() ? nullptr : s.found.c_str();
    o.diff = s.diff.empty() ? nullptr : s.diff.c_str();
    o.count = it.value("count", 0);
    o.page = it.contains("locations") && !it["locations"].empty() ? it["locations"][0]["page"].get<int>() : 0;
    uint32_t n = std::min<uint32_t>(out->size ? out->size : (uint32_t)sizeof o, (uint32_t)sizeof o);
    std::memcpy(out, &o, n);
    out->size = n;
    return FDC_OK;
}

FDC_API fdc_status fdc_result_render_crop(const fdc_result* r, int i, fdc_side side, int max_width, unsigned char* buffer, size_t buffer_len,
                                       int* width, int* height) {
    return guard([&] {
        if (!r || i < 0 || i >= (int)r->findings().size()) return set_error(FDC_ERR_INVALID_ARGUMENT, "ogiltigt index");
        const json& f = r->findings()[i];
        cv::Mat crop;
        auto cut = [&](const cv::Mat& img, const json& bb, double dpi) {
            double ppm = dpi / MM_PER_INCH, pad = 3.0;
            double x = bb[0], y = bb[1], w = bb[2], h = bb[3];
            cv::Rect rc((int)((x - pad) * ppm), (int)((y - pad) * ppm), (int)((w + 2 * pad) * ppm), (int)((h + 2 * pad) * ppm));
            rc &= cv::Rect(0, 0, img.cols, img.rows);
            if (rc.empty()) return cv::Mat();
            cv::Mat c = img(rc).clone();
            cv::rectangle(c, cv::Point((int)(x * ppm) - rc.x - 2, (int)(y * ppm) - rc.y - 2),
                          cv::Point((int)((x + w) * ppm) - rc.x + 2, (int)((y + h) * ppm) - rc.y + 2), cv::Scalar(220, 38, 38), 2);
            return c;
        };
        if (r->kind == FDC_RESULT_LABEL) {
            if (!f.contains("bbox_mm") || f["bbox_mm"].is_null()) return set_error(FDC_ERR_NOT_FOUND, "fyndet har inget läge");
            if (side == FDC_SIDE_REFERENCE) crop = cut(r->label.ref_rgb, f["bbox_mm"], r->label.dpi);
            else {
                int inst = f["instances"].empty() ? 0 : f["instances"][0].get<int>();
                for (size_t k = 0; k < r->label.instances.size(); ++k)
                    if (r->label.instances[k].index == inst) crop = cut(r->label.aligned[k], f["bbox_mm"], r->label.dpi);
            }
        } else if (r->kind == FDC_RESULT_DOCUMENT) {
            const json& l = side == FDC_SIDE_REFERENCE ? f["ref"] : f["sample"];
            if (l.is_null() || l["bbox_mm"].is_null()) return set_error(FDC_ERR_NOT_FOUND, "fyndet saknas på den sidan");
            std::string path = side == FDC_SIDE_REFERENCE ? r->j["reference"]["path"] : r->j["sample"]["path"];
            auto d = pdf::Document::open_file(path);
            cv::Mat page = pdf::render_page(*d, l["page"].get<int>() - 1, 200);
            crop = cut(page, l["bbox_mm"], 200);
        }
        if (crop.empty()) return set_error(FDC_ERR_NOT_FOUND, "inget utsnitt");
        if (max_width > 0 && crop.cols > max_width) cv::resize(crop, crop, cv::Size(), (double)max_width / crop.cols, (double)max_width / crop.cols, cv::INTER_AREA);
        if (width) *width = crop.cols;
        if (height) *height = crop.rows;
        size_t need = (size_t)crop.cols * crop.rows * 3;
        if (!buffer) return FDC_OK;
        if (buffer_len < need) return set_error(FDC_ERR_BUFFER_TOO_SMALL, "bufferten är för liten");
        for (int y = 0; y < crop.rows; ++y) std::memcpy(buffer + (size_t)y * crop.cols * 3, crop.ptr(y), (size_t)crop.cols * 3);
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_result_write_report(const fdc_result* r, const char* path) {
    return guard([&] {
        if (!r || !path) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller sökväg saknas");
        if (r->kind == FDC_RESULT_LABEL) report::write_label_html(path, r->label);
        else if (r->kind == FDC_RESULT_DOCUMENT) report::write_document_html(path, r->j);
        else {
            std::string html = "<!doctype html><html lang=sv><head><meta charset=utf-8><title>Datakontroll</title></head><body>" +
                               report::data_check_html(r->j) + "</body></html>";
            write_file(path, html);
        }
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_result_write_json(const fdc_result* r, const char* path) {
    return guard([&] {
        if (!r || !path) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller sökväg saknas");
        report::write_json(path, r->j);
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_result_write_layers(const fdc_result* r, const char* dir, double dpi) {
    return guard([&] {
        if (!r || !dir || r->kind != FDC_RESULT_LABEL) return set_error(FDC_ERR_INVALID_ARGUMENT, "bara etikettresultat har bildlager");
        report::write_layers(dir, r->label, dpi > 0 ? dpi : 200);
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_result_json(const fdc_result* r, fdc_buffer** out) {
    return guard([&] {
        if (!r || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller out saknas");
        auto s = report::json_text(r->j);
        *out = new fdc_buffer{std::vector<unsigned char>(s.begin(), s.end())};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_result_report_html(const fdc_result* r, fdc_buffer** out) {
    return guard([&] {
        if (!r || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller out saknas");
        std::string html;
        if (r->kind == FDC_RESULT_LABEL) html = report::label_html(r->label);
        else if (r->kind == FDC_RESULT_DOCUMENT) html = report::document_html(r->j);
        else
            html = "<!doctype html><html lang=sv><head><meta charset=utf-8><title>Datakontroll</title></head><body>" +
                   report::data_check_html(r->j) + "</body></html>";
        *out = new fdc_buffer{std::vector<unsigned char>(html.begin(), html.end())};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_result_crop_png(const fdc_result* r, int finding_index, fdc_side side, int max_width, fdc_buffer** out) {
    if (!out) return set_error(FDC_ERR_INVALID_ARGUMENT, "out saknas");
    int w = 0, h = 0;
    auto s = fdc_result_render_crop(r, finding_index, side, max_width, nullptr, 0, &w, &h);
    if (s != FDC_OK) return s;
    cv::Mat rgb(h, w, CV_8UC3);
    s = fdc_result_render_crop(r, finding_index, side, max_width, rgb.data, rgb.total() * 3, &w, &h);
    if (s != FDC_OK) return s;
    return guard([&] {
        *out = new fdc_buffer{image::encode(rgb, image::Format::Png)};
        return FDC_OK;
    });
}

FDC_API void fdc_result_free(fdc_result* r) { delete r; }


}  // extern "C"
