// printcmp-img – bildbehandling på områden från kommandoraden (tunt skal runt pc_imgproc).
//
//   printcmp-img filter IN UT --filter SPEC [--region R]... [--into BILD] [--offset x,y] [--scale s] [--target R]...
//   printcmp-img fft IN UT [--peaks n]                 spektrum (log, centrerat) + toppar som JSON
//   printcmp-img descreen IN UT [--peaks n] [--radius r] [--min-freq f]   ta bort rastret (notch på toppar över f)
//   printcmp-img find IN MALL UT [--threshold t] [--angles a:b:steg] [--scales s1,s2] [--max n] [--region R]... [--features]
//   printcmp-img blobs IN UT [--min-area a] [--max-area a] [--inverse]
//   printcmp-img crop IN UT --region R               (utsnitt, t.ex. en mall att söka efter)
//
// SPEC: filter efter varandra med '+', t.ex. "gaussian:1.5+sobel:1,0" eller "kernel:0,-1,0;-1,5,-1;0,-1,0".
//   gaussian:s  box:n  median:n  bilateral:d,sc,ss  sobel:dx,dy  scharr:dx,dy  laplacian:n  gradient
//   canny:lo,hi  unsharp:amount,sigma  invert  gray  gamma:g  normalize  clahe:clip
//   erode:r  dilate:r  open:r  close:r  tophat:r  blackhat:r  threshold:t  otsu  adaptive:block,c
//   kernel:rad;rad;…  lowpass:f  highpass:f  bandpass:f1,f2  bandstop:f1,f2   (f i cykler/px)
// R: box:x,y,w,h  circle:x,y,r  poly:x1,y1,x2,y2,…  mm:x,y,w,h,dpi
// Resultat (träffar, toppar, områden) skrivs som JSON på stdout.
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "../common/args.hpp"
#include "printcmp/core/core.hpp"
#include "printcmp/imgproc/imgproc.hpp"

using namespace pc;
using namespace pc::imgproc;
namespace fs = std::filesystem;

namespace {

std::vector<double> nums(const std::string& s, char sep = ',') {
    std::vector<double> out;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(std::stod(s.substr(a, b - a)));
        a = b + 1;
    }
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

cv::Mat load(const std::string& path) {
    auto bytes = read_file_bytes(path);
    cv::Mat img = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    if (img.empty()) throw Error(ErrorCode::FileFormat, "kan inte läsa " + path);
    if (img.channels() == 3) cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
    else if (img.channels() == 4) cv::cvtColor(img, img, cv::COLOR_BGRA2RGBA);
    return img;
}

void save(const std::string& path, const cv::Mat& rgb) {
    cv::Mat out = rgb;
    if (out.depth() != CV_8U && out.depth() != CV_16U) out = visualize(out);
    if (out.channels() == 3) cv::cvtColor(out, out, cv::COLOR_RGB2BGR);
    else if (out.channels() == 4) cv::cvtColor(out, out, cv::COLOR_RGBA2BGRA);
    std::vector<uchar> buf;
    std::string ext = fs::u8path(path).extension().string();
    if (!cv::imencode(ext.empty() ? ".png" : ext, out, buf)) throw Error(ErrorCode::InvalidArgument, "kan inte skriva " + path);
    write_file(path, std::string(buf.begin(), buf.end()));
}

Region parse_region(const std::string& spec) {
    auto colon = spec.find(':');
    std::string kind = spec.substr(0, colon);
    auto v = nums(spec.substr(colon + 1));
    if (kind == "box" && v.size() == 4) return Region::box(cv::Rect(int(v[0]), int(v[1]), int(v[2]), int(v[3])));
    if (kind == "circle" && v.size() == 3) return Region::circle(cv::Point(int(v[0]), int(v[1])), int(v[2]));
    if (kind == "mm" && v.size() == 5) return Region::mm(v[0], v[1], v[2], v[3], v[4]);
    if (kind == "poly" && v.size() >= 6 && v.size() % 2 == 0) {
        std::vector<cv::Point> pts;
        for (size_t i = 0; i < v.size(); i += 2) pts.push_back(cv::Point(int(v[i]), int(v[i + 1])));
        return Region::polygon(pts);
    }
    throw Error(ErrorCode::InvalidArgument, "okänt område: " + spec);
}

RegionSet parse_regions(const app::Args& a, const std::string& flag) {
    RegionSet out;
    for (auto& s : a.all(flag)) out.push_back(parse_region(s));
    return out;
}

FilterPtr parse_one(const std::string& spec) {
    auto colon = spec.find(':');
    std::string name = spec.substr(0, colon), arg = colon == std::string::npos ? "" : spec.substr(colon + 1);
    auto v = arg.empty() || name == "kernel" ? std::vector<double>{} : nums(arg);
    auto at = [&](size_t i, double def) { return i < v.size() ? v[i] : def; };
    if (name == "gaussian") return gaussian(at(0, 1.0), at(1, 0));
    if (name == "box") return box(int(at(0, 3)));
    if (name == "median") return median(int(at(0, 3)));
    if (name == "bilateral") return bilateral(int(at(0, 9)), at(1, 50), at(2, 5));
    if (name == "sobel") return sobel(int(at(0, 1)), int(at(1, 0)), int(at(2, 3)));
    if (name == "scharr") return scharr(int(at(0, 1)), int(at(1, 0)));
    if (name == "laplacian") return laplacian(int(at(0, 3)));
    if (name == "gradient") return gradient_magnitude(int(at(0, 3)));
    if (name == "canny") return canny(at(0, 50), at(1, 150));
    if (name == "unsharp") return unsharp(at(0, 1.0), at(1, 2.0));
    if (name == "invert") return invert();
    if (name == "gray") return gray();
    if (name == "gamma") return gamma(at(0, 1.0));
    if (name == "normalize") return normalize_range();
    if (name == "clahe") return clahe(at(0, 2.0));
    if (name == "erode") return morph(Morph::Erode, int(at(0, 1)));
    if (name == "dilate") return morph(Morph::Dilate, int(at(0, 1)));
    if (name == "open") return morph(Morph::Open, int(at(0, 1)));
    if (name == "close") return morph(Morph::Close, int(at(0, 1)));
    if (name == "tophat") return morph(Morph::TopHat, int(at(0, 5)));
    if (name == "blackhat") return morph(Morph::BlackHat, int(at(0, 5)));
    if (name == "threshold") return threshold(at(0, 128));
    if (name == "otsu") return otsu();
    if (name == "adaptive") return adaptive_threshold(int(at(0, 31)), at(1, 5));
    if (name == "kernel") {
        auto rows = split(arg, ';');
        std::vector<std::vector<double>> m;
        for (auto& r : rows) m.push_back(nums(r));
        cv::Mat k(int(m.size()), int(m[0].size()), CV_32F);
        for (int y = 0; y < k.rows; ++y)
            for (int x = 0; x < k.cols; ++x) k.at<float>(y, x) = float(m[y].at(x));
        double s = cv::sum(k)[0];
        return kernel(k, false, 0, std::abs(s) < 1e-9 ? CV_32F : -1);   // nollsumma (kanter) -> flyttal
    }
    fft::FrequencySpec f;
    if (name == "lowpass" || name == "highpass") {
        f.pass = name == "lowpass" ? fft::Pass::Low : fft::Pass::High;
        f.cutoff = at(0, 0.1);
        return fft::frequency_filter(f);
    }
    if (name == "bandpass" || name == "bandstop") {
        f.pass = name == "bandpass" ? fft::Pass::Band : fft::Pass::BandStop;
        f.cutoff = at(0, 0.05);
        f.cutoff2 = at(1, 0.2);
        return fft::frequency_filter(f);
    }
    throw Error(ErrorCode::InvalidArgument, "okänt filter: " + spec);
}

FilterPtr parse_filter(const std::string& spec) {
    std::vector<FilterPtr> fs;
    for (auto& p : split(spec, '+')) fs.push_back(parse_one(p));
    return fs.size() == 1 ? fs[0] : chain(fs);
}

json region_json(const Region& r) {
    return {{"x", r.rect.x}, {"y", r.rect.y}, {"w", r.rect.width}, {"h", r.rect.height}, {"label", r.label}, {"score", r.score}};
}

int cmd_filter(const app::Args& a) {
    cv::Mat src = load(a[2]);
    auto f = parse_filter(a.opt("--filter", "gaussian:1"));
    RegionSet regions = parse_regions(a, "--region");
    if (regions.empty()) regions.push_back(Region::box(cv::Rect(0, 0, src.cols, src.rows)));
    cv::Mat dst = a.has("--into") ? load(a.opt("--into")) : cv::Mat();
    RegionSet targets = parse_regions(a, "--target");
    if (!targets.empty()) {
        if (dst.empty()) dst = src.clone();
        apply_to_targets(*f, src, regions, dst, targets);
    } else {
        Placement p;
        if (a.has("--offset")) {
            auto o = nums(a.opt("--offset"));
            p.offset = cv::Point(int(o.at(0)), int(o.at(1)));
        }
        p.scale = std::stod(a.opt("--scale", "1"));
        apply_regions(*f, src, regions, dst, p);
    }
    save(a[3], dst);
    std::printf("%s\n", json{{"filter", f->name()}, {"margin", f->margin()}, {"regions", regions.size()}, {"out", a[3]}}.dump().c_str());
    return 0;
}

int cmd_fft(const app::Args& a) {
    cv::Mat src = load(a[2]);
    save(a[3], visualize(fft::magnitude(fft::forward(src))));
    json peaks = json::array();
    for (auto& p : fft::spectral_peaks(src, std::stoi(a.opt("--peaks", "6"))))
        peaks.push_back({{"fx", p.freq.x}, {"fy", p.freq.y}, {"period_px", p.period_px}, {"angle_deg", p.angle_deg}, {"magnitude", p.magnitude}});
    std::printf("%s\n", json{{"dominant_period_px", fft::dominant_period(src)}, {"peaks", peaks}}.dump(2).c_str());
    return 0;
}

int cmd_descreen(const app::Args& a) {
    cv::Mat src = load(a[2]);
    int n = std::stoi(a.opt("--peaks", "4"));
    save(a[3], fft::descreen(src, n, std::stod(a.opt("--min-freq", "0.12")), std::stod(a.opt("--radius", "0.01"))));
    std::printf("%s\n", json{{"peaks_per_channel", n}, {"out", a[3]}}.dump().c_str());
    return 0;
}

int cmd_find(const app::Args& a) {
    cv::Mat img = load(a[2]), templ = load(a[3]), vis = img.channels() == 1 ? visualize(img) : img.clone();
    json out = json::array();
    if (a.has("--features")) {
        FeatureOptions o;
        o.max_instances = std::stoi(a.opt("--max", "1"));
        o.search_in = parse_regions(a, "--region");
        for (auto& m : find_features(img, templ, o)) {
            std::vector<cv::Point> pts;
            json corners = json::array();
            for (auto& c : m.corners) {
                pts.push_back(cv::Point(int(c.x), int(c.y)));
                corners.push_back({c.x, c.y});
            }
            cv::polylines(vis, pts, true, cv::Scalar(0, 160, 0), 2, cv::LINE_AA);
            out.push_back({{"corners", corners}, {"inliers", m.inliers}, {"score", m.score}});
        }
    } else {
        MatchOptions o;
        o.threshold = std::stod(a.opt("--threshold", "0.8"));
        o.max_matches = std::stoi(a.opt("--max", "100"));
        o.search_in = parse_regions(a, "--region");
        if (a.has("--angles")) {
            auto r = nums(a.opt("--angles"), ':');
            o.angles.clear();
            for (double x = r.at(0); x <= r.at(1) + 1e-9; x += r.at(2)) o.angles.push_back(x);
        }
        if (a.has("--scales")) o.scales = nums(a.opt("--scales"));
        auto ms = find_template(img, templ, o);
        draw_matches(vis, ms);
        for (auto& m : ms)
            out.push_back({{"x", m.box.x}, {"y", m.box.y}, {"w", m.box.width}, {"h", m.box.height}, {"cx", m.center.x},
                           {"cy", m.center.y}, {"score", m.score}, {"angle", m.angle}, {"scale", m.scale}});
    }
    save(a[4], vis);
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

int cmd_blobs(const app::Args& a) {
    cv::Mat src = load(a[2]);
    cv::Mat bin = otsu(a.has("--inverse"))->apply(src);
    auto rs = blobs(bin, std::stoi(a.opt("--min-area", "10")), std::stoi(a.opt("--max-area", "0")));
    auto st = measure(src, rs);
    cv::Mat vis = src.channels() == 1 ? visualize(src) : src.clone();
    draw_regions(vis, rs);
    save(a[3], vis);
    json out = json::array();
    for (size_t i = 0; i < rs.size(); ++i) {
        json r = region_json(rs[i]);
        r["area"] = st[i].area;
        r["mean"] = st[i].mean[0];
        r["cx"] = st[i].centroid.x;
        r["cy"] = st[i].centroid.y;
        out.push_back(r);
    }
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    auto a = app::Args::from_main(argc, argv);
    if (a.size() < 2) {
        std::fprintf(stderr, "användning: printcmp-img filter|fft|descreen|find|blobs ...\n");
        return 1;
    }
    try {
        const auto& cmd = a[1];
        if (cmd == "filter" && a.size() >= 4) return cmd_filter(a);
        if (cmd == "fft" && a.size() >= 4) return cmd_fft(a);
        if (cmd == "descreen" && a.size() >= 4) return cmd_descreen(a);
        if (cmd == "find" && a.size() >= 5) return cmd_find(a);
        if (cmd == "blobs" && a.size() >= 4) return cmd_blobs(a);
        if (cmd == "crop" && a.size() >= 4) {
            cv::Mat src = load(a[2]);
            Region r = parse_region(a.opt("--region")).clipped(src.size());
            save(a[3], src(r.rect).clone());
            return 0;
        }
        std::fprintf(stderr, "okänt kommando eller för få argument\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fel: %s\n", e.what());
        return 3;
    }
}
