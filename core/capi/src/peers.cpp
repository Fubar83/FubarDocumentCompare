// C-API: etiketterna på ett ark jämförda med varandra, utan original.
#include <opencv2/imgproc.hpp>

#include "fdc/fdc.h"
#include "internal.hpp"
#include "printcmp/compare/peers.hpp"

using namespace pc;
using namespace pc::capi;

struct fdc_peers {
    json j;
    cv::Mat consensus;
    std::vector<cv::Mat> aligned;
    double dpi = 0;
};

namespace {

compare::PeerOptions peer_options(const char* options_json) {
    compare::PeerOptions opt;
    if (!options_json || !*options_json) return opt;
    auto o = json::parse(options_json);
    if (o.contains("expectedCount") && o["expectedCount"].is_number_integer() && o["expectedCount"].get<int>() > 0)
        opt.expected_count = o["expectedCount"].get<int>();
    if (o.contains("minLabelMm2") && o["minLabelMm2"].is_number()) opt.min_label_mm2 = o["minLabelMm2"].get<double>();
    if (o.contains("profile") && o["profile"].is_string()) opt.profile = o["profile"].get<std::string>();
    if (o.contains("profile") && o["profile"].is_number_integer()) opt.profile = profile_name((fdc_profile)o["profile"].get<int>());
    if (o.contains("toleranceOverrides") && o["toleranceOverrides"].is_object()) opt.tolerance_overrides = o["toleranceOverrides"];
    if (o.contains("localRegistration") && o["localRegistration"].is_boolean()) opt.local_registration = o["localRegistration"].get<bool>();
    if (o.contains("seed") && o["seed"].is_number_integer()) opt.seed = o["seed"].get<int>();
    return opt;
}

cv::Mat sheet_rgb(const fdc_image* sheet) {
    cv::Mat rgb = sheet->raster.pixels;
    if (rgb.channels() == 1) cv::cvtColor(rgb, rgb, cv::COLOR_GRAY2RGB);
    else if (rgb.channels() == 4) cv::cvtColor(rgb, rgb, cv::COLOR_RGBA2RGB);
    return rgb;
}

double r3(double v) { return std::round(v * 1000) / 1000; }

json corners_json(const std::array<Pt, 4>& cs) {
    json out = json::array();
    for (auto& c : cs) out.push_back({{"x", r3(c.x)}, {"y", r3(c.y)}});
    return out;
}

json rect_json(double x, double y, double w, double h) { return {{"x", r3(x)}, {"y", r3(y)}, {"width", r3(w)}, {"height", r3(h)}}; }

json peers_json(const compare::PeerResult& r) {
    json labels = json::array();
    int master = -1;
    for (auto& l : r.labels) {
        if (l.master) master = l.index;
        json fs = json::array();
        for (auto& f : l.findings)
            fs.push_back({{"label", f.label}, {"kind", f.kind}, {"bboxPx", rect_json(f.bbox_px.x0, f.bbox_px.y0, f.bbox_px.w(), f.bbox_px.h())},
                          {"bboxMm", rect_json(f.x_mm, f.y_mm, f.w_mm, f.h_mm)}, {"areaMm2", f.area_mm2}, {"deltaL", f.delta_l},
                          {"detail", f.detail}});
        labels.push_back({{"index", l.index}, {"row", l.row}, {"col", l.col}, {"angleDeg", l.angle_deg}, {"corners", corners_json(l.corners)},
                          {"similarity", l.similarity}, {"registrationRmsPx", l.registration_rms_px}, {"master", l.master}, {"findings", fs}});
    }
    return {{"dpi", r.dpi}, {"sheetAngleDeg", r.sheet_angle_deg}, {"labelWidth", r.label_size.width}, {"labelHeight", r.label_size.height},
            {"masterIndex", master}, {"labels", labels}, {"notes", r.notes}};
}

fdc_image* image_of(const cv::Mat& m, double dpi) {
    pdf::Raster r;
    r.pixels = m;
    r.geom.dpi = dpi;
    r.full_width = m.cols;
    r.full_height = m.rows;
    return new fdc_image{std::move(r)};
}

fdc_buffer* buffer_of(const json& j) {
    auto s = j.dump();
    return new fdc_buffer{std::vector<unsigned char>(s.begin(), s.end())};
}

}  // namespace

extern "C" {

FDC_API fdc_status fdc_sheet_detect_labels(const fdc_image* sheet, const char* options_json, fdc_buffer** json_out) {
    return guard([&] {
        if (!sheet || !json_out) return set_error(FDC_ERR_INVALID_ARGUMENT, "ark eller out saknas");
        double dpi = sheet->raster.geom.dpi;
        if (!(dpi > 0)) return set_error(FDC_ERR_INVALID_ARGUMENT, "arket saknar upplösning (dpi)");
        auto found = compare::detect_labels(sheet_rgb(sheet), dpi, peer_options(options_json));
        json labels = json::array();
        for (auto& d : found) labels.push_back({{"angleDeg", r3(d.angle_deg)}, {"corners", corners_json(d.corners)}});
        *json_out = buffer_of({{"dpi", dpi}, {"labels", labels}});
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_sheet_compare_peers(fdc_engine* e, const fdc_image* sheet, const char* options_json, fdc_peers** out) {
    return guard([&] {
        if (!e || !sheet || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, ark eller out saknas");
        double dpi = sheet->raster.geom.dpi;
        if (!(dpi > 0)) return set_error(FDC_ERR_INVALID_ARGUMENT, "arket saknar upplösning (dpi)");
        auto r = compare::compare_peers(sheet_rgb(sheet), dpi, peer_options(options_json));
        auto* p = new fdc_peers;
        p->j = peers_json(r);
        p->consensus = r.consensus_rgb;
        p->aligned = std::move(r.aligned_rgb);
        p->dpi = dpi;
        *out = p;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_peers_json(const fdc_peers* p, fdc_buffer** out) {
    return guard([&] {
        if (!p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller out saknas");
        *out = buffer_of(p->j);
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_peers_image(const fdc_peers* p, int label, fdc_image** out) {
    return guard([&] {
        if (!p || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "resultat eller out saknas");
        if (label < -1 || label >= (int)p->aligned.size()) return set_error(FDC_ERR_INVALID_ARGUMENT, "etiketten finns inte");
        const cv::Mat& m = label < 0 ? p->consensus : p->aligned[label];
        if (m.empty()) return set_error(FDC_ERR_INVALID_ARGUMENT, "ingen bild");
        *out = image_of(m.clone(), p->dpi);
        return FDC_OK;
    });
}

FDC_API void fdc_peers_free(fdc_peers* p) { delete p; }

}  // extern "C"
