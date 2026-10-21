// Bildbehandling i C-API:t: en kedja av operationer som JSON (filter – även på utvalda områden –
// och geometriska transformer), snedhet, bilder ur egna pixlar och etiketterna på ett ark.
//
// Operationerna: [{"op": "gaussian", "sigma": 2, "regions": [{"rect": [x, y, w, h]}]}, {"op": "deskew"}, …]
// Filter: gray, invert, gamma, normalize, clahe, gaussian, box, median, bilateral, sharpen, sobel,
//   laplacian, gradient, edges, threshold, otsu, adaptiveThreshold, morph, kernel, linear, lowpass,
//   highpass, bandpass, bandstop, descreen. "regions" (rect, polygon, circle) begränsar ett filter till
//   områdena; resten av bilden lämnas orörd.
// Geometri (hela bilden): rotate, rotate90, flip, scale, resize, crop, deskew, warpQuad, warpAffine.
#include <opencv2/imgproc.hpp>

#include "internal.hpp"
#include "printcmp/compare/locate.hpp"
#include "printcmp/imgproc/imgproc.hpp"

namespace {

namespace ip = pc::imgproc;

double num(const json& o, const char* k, double d) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : d; }
int inum(const json& o, const char* k, int d) { return (int)std::lround(num(o, k, d)); }
bool flag(const json& o, const char* k, bool d) { return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : d; }
std::string str(const json& o, const char* k, const std::string& d) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : d; }

[[noreturn]] void bad(const std::string& msg) { throw Error(ErrorCode::InvalidArgument, msg); }

cv::Scalar fill_of(const json& o) {
    if (!o.contains("fill")) return cv::Scalar::all(255);
    auto f = o["fill"];
    if (!f.is_array() || f.size() < 3) bad("fill ska vara [r, g, b] eller [r, g, b, a]");
    return cv::Scalar(f[0].get<double>(), f[1].get<double>(), f[2].get<double>(), f.size() > 3 ? f[3].get<double>() : 255);
}

ip::RegionSet regions_of(const json& rs) {
    ip::RegionSet out;
    for (auto& r : rs) {
        if (r.contains("rect")) {
            auto a = r["rect"];
            out.push_back(ip::Region::box(cv::Rect(a[0].get<int>(), a[1].get<int>(), a[2].get<int>(), a[3].get<int>())));
        } else if (r.contains("polygon")) {
            std::vector<cv::Point> pts;
            for (auto& p : r["polygon"]) pts.emplace_back((int)std::lround(p[0].get<double>()), (int)std::lround(p[1].get<double>()));
            out.push_back(ip::Region::polygon(pts));
        } else if (r.contains("circle")) {
            auto c = r["circle"];
            out.push_back(ip::Region::circle(cv::Point(c[0].get<int>(), c[1].get<int>()), c[2].get<int>()));
        } else {
            bad("ett område ska ha rect, polygon eller circle");
        }
    }
    return out;
}

pc::fft::FrequencySpec freq(const json& o, pc::fft::Pass pass) {
    pc::fft::FrequencySpec s;
    s.pass = pass;
    auto shape = str(o, "shape", "gaussian");
    s.shape = shape == "ideal" ? pc::fft::Shape::Ideal : shape == "butterworth" ? pc::fft::Shape::Butterworth : pc::fft::Shape::Gaussian;
    s.cutoff = num(o, pass == pc::fft::Pass::Band || pass == pc::fft::Pass::BandStop ? "low" : "cutoff", 0.1);
    s.cutoff2 = num(o, "high", 0.25);
    s.order = inum(o, "order", 2);
    return s;
}

// Filtret för en operation, eller null om operationen är geometrisk.
ip::FilterPtr filter_of(const std::string& op, const json& o) {
    if (op == "gray") return ip::gray();
    if (op == "invert") return ip::invert();
    if (op == "gamma") return ip::gamma(num(o, "gamma", 1.0));
    if (op == "normalize") return ip::normalize_range(num(o, "low", 0), num(o, "high", 255));
    if (op == "clahe") return ip::clahe(num(o, "clip", 2.0), inum(o, "tiles", 8));
    if (op == "gaussian") return ip::gaussian(num(o, "sigma", 1.0), num(o, "sigmaY", 0));
    if (op == "box") return ip::box(inum(o, "size", 3));
    if (op == "median") return ip::median(inum(o, "size", 3) | 1);
    if (op == "bilateral") return ip::bilateral(inum(o, "diameter", 9), num(o, "sigmaColor", 50), num(o, "sigmaSpace", 50));
    if (op == "sharpen") return ip::unsharp(num(o, "amount", 1.0), num(o, "sigma", 1.5));
    if (op == "sobel") return ip::sobel(inum(o, "dx", 1), inum(o, "dy", 0), inum(o, "size", 3));
    if (op == "laplacian") return ip::laplacian(inum(o, "size", 3));
    if (op == "gradient") return ip::gradient_magnitude(inum(o, "size", 3));
    if (op == "edges") return ip::canny(num(o, "low", 50), num(o, "high", 150));
    if (op == "threshold") return ip::threshold(num(o, "value", 128), flag(o, "inverse", false));
    if (op == "otsu") return ip::otsu(flag(o, "inverse", false));
    if (op == "adaptiveThreshold") return ip::adaptive_threshold(inum(o, "blockSize", 31) | 1, num(o, "c", 10), flag(o, "inverse", false));
    if (op == "morph") {
        static const std::map<std::string, ip::Morph> ops{{"erode", ip::Morph::Erode},     {"dilate", ip::Morph::Dilate},
                                                          {"open", ip::Morph::Open},       {"close", ip::Morph::Close},
                                                          {"gradient", ip::Morph::Gradient}, {"tophat", ip::Morph::TopHat},
                                                          {"blackhat", ip::Morph::BlackHat}};
        auto it = ops.find(str(o, "operation", "dilate"));
        if (it == ops.end()) bad("okänd morfologi: " + str(o, "operation", ""));
        auto shape = str(o, "shape", "ellipse");
        return ip::morph(it->second, inum(o, "radius", 1),
                         shape == "rect" ? ip::Shape::Rect : shape == "cross" ? ip::Shape::Cross : ip::Shape::Ellipse, inum(o, "iterations", 1));
    }
    if (op == "kernel") {
        if (!o.contains("matrix") || !o["matrix"].is_array() || o["matrix"].empty()) bad("kernel kräver matrix: [[…], …]");
        int rows = (int)o["matrix"].size(), cols = (int)o["matrix"][0].size();
        cv::Mat k(rows, cols, CV_32F);
        for (int r = 0; r < rows; ++r) {
            if ((int)o["matrix"][r].size() != cols) bad("matrisens rader ska vara lika långa");
            for (int c = 0; c < cols; ++c) k.at<float>(r, c) = o["matrix"][r][c].get<float>();
        }
        return ip::kernel(k, flag(o, "normalize", false), num(o, "delta", 0));
    }
    if (op == "linear") {   // ut = in · gain + offset (kontrast och ljushet)
        double gain = num(o, "gain", 1.0), offset = num(o, "offset", 0.0);
        return ip::custom([gain, offset](const cv::Mat& s) {
            cv::Mat d;
            s.convertTo(d, s.depth(), gain, offset);
            return d;
        }, 0, "linear");
    }
    if (op == "lowpass") return pc::fft::frequency_filter(freq(o, pc::fft::Pass::Low), inum(o, "margin", 16));
    if (op == "highpass") return pc::fft::frequency_filter(freq(o, pc::fft::Pass::High), inum(o, "margin", 16));
    if (op == "bandpass") return pc::fft::frequency_filter(freq(o, pc::fft::Pass::Band), inum(o, "margin", 16));
    if (op == "bandstop") return pc::fft::frequency_filter(freq(o, pc::fft::Pass::BandStop), inum(o, "margin", 16));
    if (op == "descreen") {
        int n = inum(o, "peaks", 4);
        double mf = num(o, "minFreq", 0.12), rad = num(o, "radius", 0.01);
        return ip::custom([n, mf, rad](const cv::Mat& s) { return pc::fft::descreen(s, n, mf, rad); }, inum(o, "margin", 24), "descreen");
    }
    return nullptr;
}

cv::Mat geometry(const std::string& op, const json& o, const cv::Mat& m, json& info) {
    if (op == "rotate") return ip::rotate(m, num(o, "degrees", 0), flag(o, "expand", true), fill_of(o));
    if (op == "rotate90") return ip::rotate90(m, inum(o, "turns", 1));
    if (op == "flip") return ip::flip(m, flag(o, "horizontal", true), flag(o, "vertical", false));
    if (op == "scale") {
        double f = num(o, "factor", 0);
        return ip::scale(m, f > 0 ? f : num(o, "x", 1), f > 0 ? f : num(o, "y", num(o, "x", 1)));
    }
    if (op == "resize") return ip::resize_to(m, cv::Size(inum(o, "width", m.cols), inum(o, "height", m.rows)));
    if (op == "crop") return ip::crop(m, cv::Rect(inum(o, "x", 0), inum(o, "y", 0), inum(o, "width", m.cols), inum(o, "height", m.rows)));
    if (op == "deskew") {
        ip::Skew s;
        auto out = ip::deskew(m, num(o, "maxDegrees", 15), &s, flag(o, "expand", true), fill_of(o));
        info.push_back({{"op", "deskew"}, {"angleDeg", std::round(s.angle_deg * 1000) / 1000}, {"confidence", std::round(s.confidence * 1000) / 1000}});
        return out;
    }
    if (op == "warpQuad") {
        if (!o.contains("corners") || o["corners"].size() != 4) bad("warpQuad kräver corners: fyra [x, y]");
        std::array<cv::Point2d, 4> q;
        for (int i = 0; i < 4; ++i) q[i] = {o["corners"][i][0].get<double>(), o["corners"][i][1].get<double>()};
        int w = inum(o, "width", 0), h = inum(o, "height", 0);
        if (w <= 0 || h <= 0) {   // standard: sidornas längd
            w = (int)std::lround(std::max(cv::norm(q[1] - q[0]), cv::norm(q[2] - q[3])));
            h = (int)std::lround(std::max(cv::norm(q[3] - q[0]), cv::norm(q[2] - q[1])));
        }
        return ip::warp_quad(m, q, cv::Size(w, h));
    }
    if (op == "warpAffine") {
        if (!o.contains("matrix") || o["matrix"].size() != 6) bad("warpAffine kräver matrix: [a, b, c, d, e, f]");
        auto a = o["matrix"];
        cv::Matx23d mx(a[0].get<double>(), a[1].get<double>(), a[2].get<double>(), a[3].get<double>(), a[4].get<double>(), a[5].get<double>());
        return ip::warp_affine(m, mx, cv::Size(inum(o, "width", m.cols), inum(o, "height", m.rows)), fill_of(o));
    }
    bad("okänd operation: " + op);
}

// Tillbaka till 8 bit (1, 3 eller 4 kanaler): derivator och flyttal blir absolutbelopp, mättat.
cv::Mat to_u8(cv::Mat m) {
    if (m.depth() != CV_8U) {
        cv::Mat a = cv::abs(m), u;
        a.convertTo(u, CV_8U);
        m = u;
    }
    if (m.channels() == 2) cv::extractChannel(m, m, 0);
    return m;
}

cv::Mat run(cv::Mat m, const json& ops, json& info) {
    if (!ops.is_array()) bad("operationerna ska vara en lista");
    for (auto& o : ops) {
        auto op = str(o, "op", "");
        if (op.empty()) bad("en operation saknar \"op\"");
        if (auto f = filter_of(op, o)) {
            if (o.contains("regions") && !o["regions"].empty()) {
                auto regions = ip::clip(regions_of(o["regions"]), m.size());
                m = ip::fit_type(ip::apply_regions(*f, m, regions), m.type());   // utanför områdena: orörd
            } else {
                m = to_u8(f->apply(m));
            }
        } else {
            m = to_u8(geometry(op, o, m, info));
        }
    }
    return m;
}

}  // namespace

extern "C" {

FDC_API fdc_status fdc_image_create(int width, int height, int channels, const void* data, int stride, double dpi, fdc_image** out) {
    return guard([&] {
        if (!out || !data || width <= 0 || height <= 0 || (channels != 1 && channels != 3 && channels != 4))
            return set_error(FDC_ERR_INVALID_ARGUMENT, "ogiltig bild (bredd, höjd, kanaler 1/3/4 och data krävs)");
        if (stride <= 0) stride = width * channels;
        cv::Mat view(height, width, CV_8UC(channels), const_cast<void*>(data), (size_t)stride);
        pdf::Raster r;
        r.pixels = view.clone();
        r.geom.dpi = dpi;
        r.full_width = width;
        r.full_height = height;
        *out = new fdc_image{std::move(r)};
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_image_process(const fdc_image* img, const char* ops_json, fdc_image** out, fdc_buffer** info_out) {
    return guard([&] {
        if (!img || !ops_json || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "bild, operationer eller out saknas");
        json info = json::array();
        cv::Mat result;
        try {
            result = run(img->raster.pixels, json::parse(ops_json), info);
        } catch (const json::exception& e) {
            return set_error(FDC_ERR_INVALID_ARGUMENT, std::string("operationerna: ") + e.what());
        } catch (const cv::Exception& e) {
            return set_error(FDC_ERR_INVALID_ARGUMENT, std::string("bildbehandlingen: ") + e.what());
        } catch (const std::invalid_argument& e) {
            return set_error(FDC_ERR_INVALID_ARGUMENT, e.what());
        }
        pdf::Raster r;
        r.pixels = result;
        r.geom.dpi = img->raster.geom.dpi;
        r.full_width = result.cols;
        r.full_height = result.rows;
        *out = new fdc_image{std::move(r)};
        if (info_out) {
            auto s = info.dump();
            *info_out = new fdc_buffer{std::vector<unsigned char>(s.begin(), s.end())};
        }
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_image_estimate_skew(const fdc_image* img, double max_degrees, double* angle_deg, double* confidence) {
    return guard([&] {
        if (!img || !angle_deg) return set_error(FDC_ERR_INVALID_ARGUMENT, "bild eller angle_deg saknas");
        auto s = ip::estimate_skew(img->raster.pixels, max_degrees > 0 ? max_degrees : 15);
        *angle_deg = s.angle_deg;
        if (confidence) *confidence = s.confidence;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_reference_locate(fdc_engine* e, const fdc_reference* ref, const fdc_image* sheet, const char* options_json,
                                        fdc_buffer** json_out) {
    return guard([&] {
        if (!e || !ref || !sheet || !json_out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, referens, ark eller out saknas");
        compare::LocateOptions opt;
        if (options_json && *options_json) {
            auto o = json::parse(options_json);
            if (o.contains("rotations")) opt.rotations = o["rotations"].get<std::vector<int>>();
            opt.max_skew_deg = num(o, "maxSkewDeg", opt.max_skew_deg);
            if (o.contains("expectedCount") && o["expectedCount"].is_number_integer() && o["expectedCount"].get<int>() > 0)
                opt.expected_count = o["expectedCount"].get<int>();
        }
        cv::Mat rgb = sheet->raster.pixels;
        if (rgb.channels() == 1) cv::cvtColor(rgb, rgb, cv::COLOR_GRAY2RGB);
        else if (rgb.channels() == 4) cv::cvtColor(rgb, rgb, cv::COLOR_RGBA2RGB);
        double dpi = sheet->raster.geom.dpi;
        if (!(dpi > 0)) return set_error(FDC_ERR_INVALID_ARGUMENT, "arket saknar upplösning (dpi)");
        auto located = compare::locate_labels(*ref->model, rgb, dpi, opt);
        auto r3 = [](double v) { return std::round(v * 1000) / 1000; };
        json labels = json::array();
        for (auto& l : located.labels) {
            json corners = json::array();
            for (auto& c : l.corners) corners.push_back({{"x", r3(c.x)}, {"y", r3(c.y)}});
            labels.push_back({{"index", l.index}, {"row", l.row}, {"col", l.col}, {"angleDeg", r3(l.angle_deg)}, {"score", r3(l.score)},
                              {"center", {{"x", r3(l.center.x)}, {"y", r3(l.center.y)}}}, {"corners", corners}});
        }
        json j = {{"sheetAngleDeg", r3(located.sheet_angle_deg)}, {"dpi", dpi}, {"labels", labels}};
        auto s = j.dump();
        *json_out = new fdc_buffer{std::vector<unsigned char>(s.begin(), s.end())};
        return FDC_OK;
    });
}

}  // extern "C"
