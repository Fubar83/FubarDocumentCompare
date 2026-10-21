// Flatbäddsscanner och fysiska veck (simulator v2).
//
// CIS: linsrad direkt under glaset, kort skärpedjup och snabbt ljusfall när papperet lyfter,
// ingen perspektivverkan, RGB-dioderna blinkar i tur och ordning (kanalförskjutning).
// CCD: lång optisk väg, långt skärpedjup och svagt ljusfall men centralprojektion tvärs vagnen.
// Bildkoordinater: x tvärs vagnen (sensorraden), y i matningsriktningen.
//
// Veck är en höjdkarta h(x, y) i mm över glaset. Av höjden följer oskärpa, ljusfall,
// sluttningsskuggning, förkortning och (CCD) perspektiv. Ett bergveck kan spräcka färgen.
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "printcmp/testgen/sim.hpp"

namespace pc::testgen::sim {

json ScannerModel::to_json() const {
    return {{"kind", kind},           {"blur_px_per_mm", blur_px_per_mm}, {"light_h0_mm", light_h0_mm},
            {"slope_gain", slope_gain}, {"light_sign", light_sign},     {"persp_L_mm", persp_L_mm},
            {"chan_shift_px", chan_shift_px}, {"jitter_px", jitter_px}, {"y_scale", y_scale},
            {"vignette", vignette},   {"shot", shot},                    {"gamma", gamma},
            {"emboss_gain", emboss_gain}};
}

ScannerModel make_model(const std::string& kind, Rng& rng) {
    ScannerModel m;
    m.kind = kind;
    if (kind == "cis") {
        double d = rng.uniform(0.25, 0.4);
        m.blur_px_per_mm = rng.uniform(3.0, 5.0);
        m.light_h0_mm = rng.uniform(1.2, 1.8);
        m.slope_gain = rng.uniform(0.4, 0.7);
        m.light_sign = rng.random() < 0.5 ? 1.0 : -1.0;
        m.persp_L_mm = 0;
        m.chan_shift_px = {-d, 0.0, d};
        m.jitter_px = rng.uniform(0.05, 0.15);
        m.y_scale = 1 + rng.uniform(-0.002, 0.002);
        m.vignette = rng.uniform(0.0, 0.015);
        m.shot = rng.uniform(0.02, 0.05);
        m.gamma = rng.uniform(0.93, 1.07);
        m.emboss_gain = rng.uniform(0.7, 0.9);
        return m;
    }
    if (kind == "ccd") {
        m.blur_px_per_mm = rng.uniform(0.3, 0.7);
        m.light_h0_mm = rng.uniform(6.0, 10.0);
        m.slope_gain = rng.uniform(0.7, 1.1);
        m.light_sign = rng.random() < 0.5 ? 1.0 : -1.0;
        m.persp_L_mm = rng.uniform(250, 400);
        for (auto& c : m.chan_shift_px) c = rng.uniform(-0.15, 0.15);
        m.jitter_px = rng.uniform(0.15, 0.35);
        m.y_scale = 1 + rng.uniform(-0.003, 0.003);
        m.vignette = rng.uniform(0.015, 0.04);
        m.shot = rng.uniform(0.01, 0.03);
        m.gamma = rng.uniform(0.93, 1.07);
        m.emboss_gain = rng.uniform(1.1, 1.4);
        return m;
    }
    throw Error(ErrorCode::InvalidArgument, "okänd scanner: " + kind);
}

// ------------------------------------------------------------------ veck

cv::Point2d Fold::normal() const {
    double dx = p1.x - p0.x, dy = p1.y - p0.y, n = std::hypot(dx, dy);
    return {-dy / n, dx / n};
}

double Fold::signed_dist(double x, double y) const {
    auto n = normal();
    return (x - p0.x) * n.x + (y - p0.y) * n.y;
}

void Fold::profile(double s, double& h, double& dh) const {
    double a = std::fabs(s) / width_mm, sg = s > 0 ? 1 : s < 0 ? -1 : 0;
    if (kind == "mountain") {
        h = lift_mm * std::exp(-a);
        dh = -sg * h / width_mm;
    } else {
        // dalveck: vecklinjen ligger mot glaset, vingarna reser sig och pressas ned igen
        h = lift_mm * a * std::exp(1 - a);
        dh = sg * lift_mm * (1 - a) * std::exp(1 - a) / width_mm;
    }
}

double Fold::arc_excess(double s) const {
    // hur mycket längre papperet är än projektionen från linjen ut till s (tabell, interpolerad)
    if (ex_t_.empty()) {
        const int N = 1024;
        ex_t_.resize(N);
        ex_v_.assign(N, 0.0);
        for (int i = 0; i < N; ++i) ex_t_[i] = 8 * width_mm * i / (N - 1);
        for (int i = 1; i < N; ++i) {
            double h, dh;
            profile(ex_t_[i], h, dh);
            ex_v_[i] = ex_v_[i - 1] + (std::sqrt(1 + dh * dh) - 1) * (ex_t_[i] - ex_t_[i - 1]);
        }
    }
    double a = std::fabs(s), sg = s > 0 ? 1 : s < 0 ? -1 : 0, v;
    if (a >= ex_t_.back()) v = ex_v_.back();
    else {
        size_t i = size_t(std::upper_bound(ex_t_.begin(), ex_t_.end(), a) - ex_t_.begin());
        double t = (a - ex_t_[i - 1]) / (ex_t_[i] - ex_t_[i - 1]);
        v = ex_v_[i - 1] + t * (ex_v_[i] - ex_v_[i - 1]);
    }
    return sg * v;
}

double Fold::band_mm(double h_min) const {
    if (lift_mm <= h_min) return 0.5;
    if (kind == "mountain") return width_mm * std::log(lift_mm / h_min);
    double best = 0;
    for (int i = 0; i < 4000; ++i) {
        double a = 20.0 * i / 3999;
        if (lift_mm * a * std::exp(1 - a) > h_min) best = a;
    }
    return best * width_mm;
}

Fold random_fold(Rng& rng, int SW, int SH, double ppm, std::array<double, 2> lift_range, double crack_p) {
    (void)ppm;
    bool vertical = rng.random() < 0.5;
    double a = rng.uniform(-3, 3) * M_PI / 180;
    Fold f;
    if (vertical) {
        double x = rng.uniform(0.3, 0.7) * SW;
        f.p0 = {x - std::tan(a) * SH / 2, 0.0};
        f.p1 = {x + std::tan(a) * SH / 2, double(SH)};
    } else {
        double y = rng.uniform(0.3, 0.7) * SH;
        f.p0 = {0.0, y - std::tan(a) * SW / 2};
        f.p1 = {double(SW), y + std::tan(a) * SW / 2};
    }
    f.kind = rng.random() < 0.7 ? "mountain" : "valley";
    f.lift_mm = rng.uniform(lift_range[0], lift_range[1]);
    f.width_mm = rng.uniform(1.5, 5.0);
    f.crack = f.kind == "mountain" && rng.random() < crack_p;
    f.crack_w_mm = rng.uniform(0.05, 0.15);
    return f;
}

void crack_ink(cv::Mat& sheet, const cv::Matx23d& G, const Fold& f, double ppm, Rng& rng) {
    cv::Matx23d Gi;
    cv::invertAffineTransform(G, Gi);
    auto P = [&](cv::Point2d p) { return cv::Point2d(Gi(0, 0) * p.x + Gi(0, 1) * p.y + Gi(0, 2), Gi(1, 0) * p.x + Gi(1, 1) * p.y + Gi(1, 2)); };
    auto p0 = P(f.p0), p1 = P(f.p1);
    int n = int(std::hypot(p1.x - p0.x, p1.y - p0.y) / (0.5 * ppm)) + 2;
    // sprickan är bruten: slumpvisa partier
    cv::Mat r(1, n / 6 + 2, CV_32F);
    for (int i = 0; i < r.cols; ++i) r.at<float>(0, i) = float(rng.random());
    cv::Mat on;
    cv::resize(r, on, cv::Size(n, 1), 0, 0, cv::INTER_LINEAR);
    int th = std::max(1, int(std::lround(f.crack_w_mm * ppm)));
    cv::Mat m = cv::Mat::zeros(sheet.size(), CV_32F);
    for (int i = 0; i < n - 1; ++i) {
        if (!(on.at<float>(0, i) > 0.45f)) continue;
        auto a = p0 + (p1 - p0) * (double(i) / (n - 1)), b = p0 + (p1 - p0) * (double(i + 1) / (n - 1));
        cv::line(m, cv::Point(int(a.x * 16), int(a.y * 16)), cv::Point(int(b.x * 16), int(b.y * 16)), 1.0, th, cv::LINE_AA, 4);
    }
    cv::GaussianBlur(m, m, cv::Size(0, 0), 0.5);
    double pm = (PAPER[0] + PAPER[1] + PAPER[2]) / 3;
    for (int y = 0; y < sheet.rows; ++y) {
        auto* s = sheet.ptr<cv::Vec3f>(y);
        const float* mr = m.ptr<float>(y);
        for (int x = 0; x < sheet.cols; ++x) {
            if (mr[x] <= 0) continue;
            // bara där det finns färg (mörkt) syns sprickan
            double ink = std::clamp((pm - (s[x][0] + s[x][1] + s[x][2]) / 3) / 0.5, 0.0, 1.0);
            float a = float(0.85 * mr[x] * ink);
            s[x] = s[x] * (1 - a) + PAPER * a;
        }
    }
}

// ------------------------------------------------------------------ geometri

Geometry::Geometry(const cv::Matx23d& G, std::vector<Fold> folds, const ScannerModel& m, double ppm, cv::Size size, Rng& rng)
    : folds_(std::move(folds)), m_(m), ppm_(ppm), size_(size) {
    cv::invertAffineTransform(G, Gi_);
    for (auto& f : folds_) f.arc_excess(0);   // bygg tabellen nu: to_sheet anropas sedan från flera trådar
    // stegmotor: summa av några sinusar (perioder 3–40 mm)
    const int k = 5;
    for (int i = 0; i < k; ++i) {
        double per = rng.uniform(3, 40) * ppm, ph = rng.uniform(0, 2 * M_PI), amp = m.jitter_px * rng.uniform(0.3, 1.0) / std::sqrt(k);
        jit_.push_back({per, ph, amp});
    }
}

void Geometry::height(double x, double y, double& H, double& dHy) const {
    H = dHy = 0;
    for (auto& f : folds_) {
        double h, dh;
        f.profile(f.signed_dist(x, y) / ppm_, h, dh);
        H += h;
        dHy += dh * f.normal().y;
    }
}

cv::Point2d Geometry::to_sheet(double x, double y) const {
    double H = 0, dHy = 0;
    if (!folds_.empty()) height(x, y, H, dHy);
    double yy = y / m_.y_scale;
    for (auto& [per, ph, amp] : jit_) yy -= amp * std::sin(2 * M_PI * y / per + ph);
    double xx = x;
    // CCD: lyft innehåll syns förskjutet från optiska axeln -> dra tillbaka
    if (m_.persp_L_mm > 0) xx -= H * (x - size_.width / 2.0) / m_.persp_L_mm;
    // förkortning: punkten vid projicerat avstånd s ligger på papperet vid s + överskott
    for (auto& f : folds_) {
        double ex = f.arc_excess(f.signed_dist(x, y) / ppm_) * ppm_;
        auto n = f.normal();
        xx += ex * n.x;
        yy += ex * n.y;
    }
    return {Gi_(0, 0) * xx + Gi_(0, 1) * yy + Gi_(0, 2), Gi_(1, 0) * xx + Gi_(1, 1) * yy + Gi_(1, 2)};
}

cv::Point2d Geometry::to_scan(double px, double py, const cv::Matx23d& G, int iters) const {
    double qx = G(0, 0) * px + G(0, 1) * py + G(0, 2), qy = G(1, 0) * px + G(1, 1) * py + G(1, 2);
    for (int i = 0; i < iters; ++i) {
        auto e = to_sheet(qx, qy);
        double rx = px - e.x, ry = py - e.y;
        qx += G(0, 0) * rx + G(0, 1) * ry;
        qy += G(1, 0) * rx + G(1, 1) * ry;
    }
    return {qx, qy};
}

// ------------------------------------------------------------------ scanning

namespace {

// rumsligt varierande gaussisk oskärpa genom interpolation mellan nivåer
cv::Mat varying_blur(const cv::Mat& img, const cv::Mat& sigma) {
    const std::vector<double> levels = {0.0, 0.7, 1.4, 2.8, 5.6, 11.2};
    double smax;
    cv::minMaxLoc(sigma, nullptr, &smax);
    std::vector<double> lv;
    for (double l : levels)
        if (l <= smax * 1.01) lv.push_back(l);
    for (double l : levels)
        if (l > smax) {
            lv.push_back(l);
            break;
        }
    if (lv.size() < 2) return img.clone();
    std::vector<cv::Mat> stack;
    for (double l : lv) {
        if (l == 0) stack.push_back(img);
        else {
            cv::Mat b;
            cv::GaussianBlur(img, b, cv::Size(0, 0), l);
            stack.push_back(b);
        }
    }
    cv::Mat out = cv::Mat::zeros(img.size(), img.type());
    double top = lv.back();
    for (int y = 0; y < img.rows; ++y) {
        const float* sr = sigma.ptr<float>(y);
        auto* o = out.ptr<cv::Vec3f>(y);
        for (int x = 0; x < img.cols; ++x) {
            double s = std::clamp(double(sr[x]), 0.0, top);
            for (size_t i = 0; i + 1 < lv.size(); ++i) {
                double a = lv[i], b = lv[i + 1];
                bool sel = i == 0 ? (s >= a && s <= b) : (s > a && s <= b);
                if (!sel) continue;
                float t = float((s - a) / (b - a));
                o[x] = stack[i].at<cv::Vec3f>(y, x) * (1 - t) + stack[i + 1].at<cv::Vec3f>(y, x) * t;
                break;
            }
        }
    }
    return out;
}

}  // namespace

ScanResult scan(const cv::Mat& sheet, const cv::Matx23d& G, const std::vector<Fold>& folds, const ScannerModel& m, Rng& rng,
                double ppm, std::array<double, 2> mtf, double read_noise, cv::Vec3f lid, double color_jitter) {
    int SH = sheet.rows, SW = sheet.cols;
    ScanResult res;
    res.geo = std::make_unique<Geometry>(G, folds, m, ppm, cv::Size(SW, SH), rng);
    const Geometry& geo = *res.geo;
    cv::Matx23d Gi;
    cv::invertAffineTransform(G, Gi);
    std::vector<cv::Mat> chans;
    cv::split(sheet, chans);
    std::vector<cv::Mat> outc(3);
    for (auto& c : outc) c.create(SH, SW, CV_32F);
    cv::Mat Hmap, shade;
    if (!folds.empty()) {
        Hmap.create(SH, SW, CV_32F);
        shade.create(SH, SW, CV_32F);
    }
    const int step = 256;
    for (int y0 = 0; y0 < SH; y0 += step) {
        int y1 = std::min(SH, y0 + step), n = y1 - y0;
        cv::Mat px(n, SW, CV_64F), py(n, SW, CV_64F);
        cv::parallel_for_(cv::Range(0, n), [&](const cv::Range& r) {
            for (int j = r.start; j < r.end; ++j)
                for (int x = 0; x < SW; ++x) {
                    int y = y0 + j;
                    auto p = geo.to_sheet(x, y);
                    px.at<double>(j, x) = p.x;
                    py.at<double>(j, x) = p.y;
                    if (!folds.empty()) {
                        double H, dHy;
                        geo.height(x, y, H, dHy);
                        Hmap.at<float>(y, x) = float(H);
                        double fall = 1.0 / (1.0 + (H / m.light_h0_mm) * (H / m.light_h0_mm));
                        double sl = 1.0 + m.slope_gain * m.light_sign * dHy;
                        shade.at<float>(y, x) = float(std::clamp(fall * sl, 0.05, 1.6));
                    }
                }
        });
        for (int c = 0; c < 3; ++c) {
            // kanalförskjutning i matningsriktningen (RGB exponeras vid olika vagnläge)
            double dy = m.chan_shift_px[c];
            cv::Mat mx(n, SW, CV_32F), my(n, SW, CV_32F);
            for (int j = 0; j < n; ++j)
                for (int x = 0; x < SW; ++x) {
                    mx.at<float>(j, x) = float(px.at<double>(j, x) + dy * Gi(0, 1));
                    my.at<float>(j, x) = float(py.at<double>(j, x) + dy * Gi(1, 1));
                }
            cv::Mat dst = outc[c].rowRange(y0, y1);
            cv::remap(chans[c], dst, mx, my, cv::INTER_CUBIC, cv::BORDER_CONSTANT, cv::Scalar(lid[c]));
        }
    }
    chans.clear();
    cv::Mat out;
    cv::merge(outc, out);
    outc.clear();
    if (!folds.empty()) {
        // oskärpa efter höjd, bara i området där papperet lyfter
        cv::Mat sig = Hmap * m.blur_px_per_mm;
        cv::Mat nz;
        cv::findNonZero(sig > 0.15, nz);
        if (!nz.empty()) {
            double smax;
            cv::minMaxLoc(sig, nullptr, &smax);
            auto bb = cv::boundingRect(nz);
            int pad = int(4 * smax) + 4;
            int a0 = std::max(0, bb.y - pad), a1 = std::min(SH, bb.y + bb.height - 1 + pad + 1);
            int b0 = std::max(0, bb.x - pad), b1 = std::min(SW, bb.x + bb.width - 1 + pad + 1);
            cv::Rect roi(b0, a0, b1 - b0, a1 - a0);
            varying_blur(out(roi), sig(roi)).copyTo(out(roi));
        }
        std::vector<cv::Mat> ch;
        cv::split(out, ch);
        for (auto& c : ch) c = c.mul(shade);
        cv::merge(ch, out);
    }
    // damm på glaset (i fokus, mörkt)
    int n_d = rng.poisson(1.5);
    for (int i = 0; i < n_d; ++i) {
        double cx = rng.uniform(0.05, 0.95) * SW, cy = rng.uniform(0.05, 0.95) * SH;
        double r = rng.uniform(0.03, 0.12) * ppm;
        int sz = int(4 * r) + 3;
        cv::Mat dm = cv::Mat::zeros(sz, sz, CV_32F);
        cv::Size axes(int(r * 16), int(r * rng.uniform(0.5, 1.0) * 16));
        double ang = rng.uniform(0, 180);
        cv::ellipse(dm, cv::Point((sz / 2) * 16, (sz / 2) * 16), axes, ang, 0, 360, 1.0, -1, cv::LINE_AA, 4);
        int x0 = int(cx) - sz / 2, y0 = int(cy) - sz / 2;
        cv::Rect roi = cv::Rect(x0, y0, sz, sz) & cv::Rect(0, 0, SW, SH);
        for (int y = roi.y; y < roi.y + roi.height; ++y)
            for (int x = roi.x; x < roi.x + roi.width; ++x) {
                float a = dm.at<float>(y - y0, x - x0) * 0.8f;
                auto& v = out.at<cv::Vec3f>(y, x);
                v = v * (1 - a) + cv::Vec3f(0.15f, 0.15f, 0.15f) * a;
            }
        res.dust.push_back({x0, y0, sz, sz});
    }
    // optikens grundskärpa (kan skilja i x och y)
    cv::GaussianBlur(out, out, cv::Size(0, 0), mtf[0], mtf[1]);
    // vinjettering tvärs sensorraden
    if (m.vignette > 0)
        for (int y = 0; y < SH; ++y) {
            auto* row = out.ptr<cv::Vec3f>(y);
            for (int x = 0; x < SW; ++x) {
                double t = (x - SW / 2.0) / (SW / 2.0);
                row[x] *= float(1 - m.vignette * t * t);
            }
        }
    // färgprofil och tonkurva
    cv::Matx33f M = cv::Matx33f::eye();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) M(i, j) += float(rng.normal(0, color_jitter));
    cv::transform(out, out, M);
    // brus: läsbrus + signalberoende (fotonbrus)
    cv::RNG cvr(rng.next());
    res.rgb.create(SH, SW, CV_8UC3);
    cv::Mat noise(1, SW * 3, CV_32F);
    for (int y = 0; y < SH; ++y) {
        cvr.fill(noise, cv::RNG::NORMAL, 0.0, 1.0);
        auto* row = out.ptr<cv::Vec3f>(y);
        auto* o = res.rgb.ptr<cv::Vec3b>(y);
        const float* nz = noise.ptr<float>(0);
        for (int x = 0; x < SW; ++x)
            for (int c = 0; c < 3; ++c) {
                double v = std::pow(std::clamp(double(row[x][c]), 0.0, 1.0), m.gamma) * 255.0;
                double sd = std::sqrt(read_noise * read_noise + m.shot * v);
                v += nz[3 * x + c] * sd;
                o[x][c] = uchar(std::clamp(v + 0.5, 0.0, 255.0));
            }
    }
    return res;
}

}  // namespace pc::testgen::sim
