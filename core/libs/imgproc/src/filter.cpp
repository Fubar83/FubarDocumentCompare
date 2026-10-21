#include "printcmp/imgproc/filter.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pc::imgproc {

namespace {

// Ett filter ur en funktion, en marginal och ett namn.
class FnFilter final : public Filter {
public:
    FnFilter(std::function<cv::Mat(const cv::Mat&)> fn, int margin, std::string name)
        : fn_(std::move(fn)), margin_(margin), name_(std::move(name)) {}
    cv::Mat apply(const cv::Mat& src) const override { return fn_(src); }
    int margin() const override { return margin_; }
    std::string name() const override { return name_; }

private:
    std::function<cv::Mat(const cv::Mat&)> fn_;
    int margin_;
    std::string name_;
};

FilterPtr make(std::function<cv::Mat(const cv::Mat&)> fn, int margin, std::string name) {
    return std::make_shared<FnFilter>(std::move(fn), margin, std::move(name));
}

// OpenCV:s kärnstorlek för en given sigma är högst ≈ 8σ+1 (flyttal); marginalen täcker den.
int gaussian_margin(double sigma) { return int(std::ceil(4 * sigma)) + 1; }

cv::Mat to_gray(const cv::Mat& src) {
    if (src.channels() == 1) return src;
    cv::Mat g;
    cv::cvtColor(src, g, src.channels() == 4 ? cv::COLOR_RGBA2GRAY : cv::COLOR_RGB2GRAY);
    return g;
}

cv::Mat to_u8(const cv::Mat& src) {
    if (src.depth() == CV_8U) return src;
    cv::Mat o;
    double scale = src.depth() == CV_16U ? 1.0 / 257 : (src.depth() == CV_32F || src.depth() == CV_64F) ? 255.0 : 1.0;
    // flyttal antas vara 0..1 om största värdet är ≤ 1, annars 0..255
    if (src.depth() == CV_32F || src.depth() == CV_64F) {
        double mx;
        cv::minMaxLoc(src.reshape(1), nullptr, &mx);
        if (mx > 1.0 + 1e-6) scale = 1.0;
    }
    src.convertTo(o, CV_8U, scale);
    return o;
}

double type_max(int depth) {
    switch (depth) {
        case CV_8U: return 255;
        case CV_16U: return 65535;
        case CV_32F:
        case CV_64F: return 1.0;
        default: return 255;
    }
}

int cv_shape(Shape s) { return s == Shape::Rect ? cv::MORPH_RECT : s == Shape::Cross ? cv::MORPH_CROSS : cv::MORPH_ELLIPSE; }

int cv_morph(Morph m) {
    switch (m) {
        case Morph::Erode: return cv::MORPH_ERODE;
        case Morph::Dilate: return cv::MORPH_DILATE;
        case Morph::Open: return cv::MORPH_OPEN;
        case Morph::Close: return cv::MORPH_CLOSE;
        case Morph::Gradient: return cv::MORPH_GRADIENT;
        case Morph::TopHat: return cv::MORPH_TOPHAT;
        default: return cv::MORPH_BLACKHAT;
    }
}

}  // namespace

// ------------------------------------------------------------------ matriser

FilterPtr kernel(const cv::Mat& k_in, bool normalize, double delta, int out_depth) {
    CV_Assert(!k_in.empty() && k_in.channels() == 1);
    cv::Mat k;
    k_in.convertTo(k, CV_32F);
    if (normalize) {
        double s = cv::sum(k)[0];
        if (std::abs(s) > 1e-12) k /= s;
    }
    int margin = std::max(k.rows, k.cols) / 2 + 1;
    return make(
        [k, delta, out_depth](const cv::Mat& src) {
            cv::Mat dst;
            cv::filter2D(src, dst, out_depth, k, cv::Point(-1, -1), delta, cv::BORDER_DEFAULT);
            return dst;
        },
        margin, "kernel " + std::to_string(k.cols) + "x" + std::to_string(k.rows));
}

FilterPtr kernel(std::initializer_list<std::initializer_list<float>> rows, bool normalize, double delta, int out_depth) {
    int h = int(rows.size()), w = h ? int(rows.begin()->size()) : 0;
    cv::Mat k(h, w, CV_32F);
    int r = 0;
    for (auto& row : rows) {
        CV_Assert(int(row.size()) == w);
        int c = 0;
        for (float v : row) k.at<float>(r, c++) = v;
        ++r;
    }
    return kernel(k, normalize, delta, out_depth);
}

FilterPtr separable(const std::vector<float>& row, const std::vector<float>& col, int out_depth) {
    cv::Mat kx(row, true), ky(col, true);
    int margin = int(std::max(row.size(), col.size())) / 2 + 1;
    return make(
        [kx, ky, out_depth](const cv::Mat& src) {
            cv::Mat dst;
            cv::sepFilter2D(src, dst, out_depth, kx, ky);
            return dst;
        },
        margin, "separable");
}

// ------------------------------------------------------------------ utjämning

FilterPtr gaussian(double sx, double sy) {
    if (sy <= 0) sy = sx;
    return make(
        [sx, sy](const cv::Mat& src) {
            cv::Mat dst;
            cv::GaussianBlur(src, dst, cv::Size(0, 0), sx, sy);
            return dst;
        },
        gaussian_margin(std::max(sx, sy)), "gaussian");
}

FilterPtr box(int size) {
    return make(
        [size](const cv::Mat& src) {
            cv::Mat dst;
            cv::blur(src, dst, cv::Size(size, size));
            return dst;
        },
        size / 2 + 1, "box");
}

FilterPtr median(int size) {
    if (size % 2 == 0) ++size;
    return make(
        [size](const cv::Mat& src) {
            cv::Mat dst;
            cv::medianBlur(src, dst, size);
            return dst;
        },
        size / 2 + 1, "median");
}

FilterPtr bilateral(int d, double sc, double ss) {
    int margin = (d > 0 ? d / 2 : int(std::lround(1.5 * ss))) + 1;
    return make(
        [d, sc, ss](const cv::Mat& src) {
            cv::Mat dst;
            cv::bilateralFilter(src, dst, d, sc, ss);
            return dst;
        },
        margin, "bilateral");
}

// ------------------------------------------------------------------ kanter

FilterPtr sobel(int dx, int dy, int size) {
    return make(
        [dx, dy, size](const cv::Mat& src) {
            cv::Mat dst;
            cv::Sobel(src, dst, CV_32F, dx, dy, size);
            return dst;
        },
        size / 2 + 1, "sobel");
}

FilterPtr scharr(int dx, int dy) {
    return make(
        [dx, dy](const cv::Mat& src) {
            cv::Mat dst;
            cv::Scharr(src, dst, CV_32F, dx, dy);
            return dst;
        },
        2, "scharr");
}

FilterPtr laplacian(int size) {
    return make(
        [size](const cv::Mat& src) {
            cv::Mat dst;
            cv::Laplacian(src, dst, CV_32F, size);
            return dst;
        },
        std::max(1, size / 2) + 1, "laplacian");
}

FilterPtr gradient_magnitude(int size) {
    return make(
        [size](const cv::Mat& src) {
            cv::Mat gx, gy;
            cv::Sobel(src, gx, CV_32F, 1, 0, size);
            cv::Sobel(src, gy, CV_32F, 0, 1, size);
            std::vector<cv::Mat> cx, cy, out;
            cv::split(gx, cx);
            cv::split(gy, cy);
            for (size_t i = 0; i < cx.size(); ++i) {
                cv::Mat m;
                cv::magnitude(cx[i], cy[i], m);
                out.push_back(m);
            }
            cv::Mat dst;
            cv::merge(out, dst);
            return dst;
        },
        size / 2 + 1, "gradient");
}

FilterPtr canny(double low, double high, int aperture) {
    // Hysteresen följer kanter över hela området; vid områdets kant kan den skilja något mot hela bilden.
    return make(
        [low, high, aperture](const cv::Mat& src) {
            cv::Mat dst;
            cv::Canny(to_u8(to_gray(src)), dst, low, high, aperture);
            return dst;
        },
        aperture / 2 + 2, "canny");
}

// ------------------------------------------------------------------ skärpa och ton

FilterPtr unsharp(double amount, double sigma) {
    return make(
        [amount, sigma](const cv::Mat& src) {
            cv::Mat f, blur, dst;
            src.convertTo(f, CV_32F);
            cv::GaussianBlur(f, blur, cv::Size(0, 0), sigma);
            cv::Mat sharp = f * (1 + amount) - blur * amount;
            sharp.convertTo(dst, src.depth());
            return dst;
        },
        gaussian_margin(sigma), "unsharp");
}

FilterPtr invert() {
    return make(
        [](const cv::Mat& src) {
            cv::Mat dst;
            cv::subtract(cv::Scalar::all(type_max(src.depth())), src, dst);
            return dst;
        },
        0, "invert");
}

FilterPtr gray() {
    return make([](const cv::Mat& src) { return to_gray(src).clone(); }, 0, "gray");
}

FilterPtr gamma(double g) {
    return make(
        [g](const cv::Mat& src) {
            if (src.depth() == CV_8U) {
                cv::Mat lut(1, 256, CV_8U);
                for (int i = 0; i < 256; ++i) lut.at<uchar>(i) = cv::saturate_cast<uchar>(255.0 * std::pow(i / 255.0, g));
                cv::Mat dst;
                cv::LUT(src, lut, dst);
                return dst;
            }
            cv::Mat f, dst;
            double mx = type_max(src.depth());
            src.convertTo(f, CV_32F, 1.0 / mx);
            cv::pow(cv::max(f, 0), g, f);
            f.convertTo(dst, src.depth(), mx);
            return dst;
        },
        0, "gamma");
}

FilterPtr normalize_range(double lo, double hi) {
    // per område: sträcker områdets eget min–max
    return make(
        [lo, hi](const cv::Mat& src) {
            std::vector<cv::Mat> ch;
            cv::split(src, ch);
            for (auto& c : ch) cv::normalize(c, c, lo, hi, cv::NORM_MINMAX);
            cv::Mat dst;
            cv::merge(ch, dst);
            return dst;
        },
        0, "normalize");
}

FilterPtr clahe(double clip_limit, int tiles) {
    // per område: rutnätet läggs över området
    return make(
        [clip_limit, tiles](const cv::Mat& src) {
            auto c = cv::createCLAHE(clip_limit, cv::Size(tiles, tiles));
            cv::Mat u8 = to_u8(src);
            if (u8.channels() == 1) {
                cv::Mat dst;
                c->apply(u8, dst);
                return dst;
            }
            cv::Mat lab, dst;   // ljusheten i Lab, färgen orörd
            cv::cvtColor(u8, lab, cv::COLOR_RGB2Lab);
            std::vector<cv::Mat> ch;
            cv::split(lab, ch);
            c->apply(ch[0], ch[0]);
            cv::merge(ch, lab);
            cv::cvtColor(lab, dst, cv::COLOR_Lab2RGB);
            return dst;
        },
        0, "clahe");
}

// ------------------------------------------------------------------ morfologi

FilterPtr morph(Morph op, int radius, Shape shape, int iterations) {
    cv::Mat k = cv::getStructuringElement(cv_shape(shape), cv::Size(2 * radius + 1, 2 * radius + 1));
    bool twice = op == Morph::Open || op == Morph::Close || op == Morph::TopHat || op == Morph::BlackHat || op == Morph::Gradient;
    int margin = radius * iterations * (twice ? 2 : 1) + 1;
    return make(
        [op, k, iterations](const cv::Mat& src) {
            cv::Mat dst;
            cv::morphologyEx(src, dst, cv_morph(op), k, cv::Point(-1, -1), iterations);
            return dst;
        },
        margin, "morph");
}

// ------------------------------------------------------------------ tröskling

FilterPtr threshold(double t, bool inverse) {
    return make(
        [t, inverse](const cv::Mat& src) {
            cv::Mat dst;
            cv::threshold(to_gray(src), dst, t, 255, inverse ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY);
            dst.convertTo(dst, CV_8U);
            return dst;
        },
        0, "threshold");
}

FilterPtr otsu(bool inverse) {
    // per område: tröskeln räknas på områdets egen histogram
    return make(
        [inverse](const cv::Mat& src) {
            cv::Mat dst;
            cv::threshold(to_u8(to_gray(src)), dst, 0, 255, (inverse ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY) | cv::THRESH_OTSU);
            return dst;
        },
        0, "otsu");
}

FilterPtr adaptive_threshold(int block, double c, bool inverse) {
    if (block % 2 == 0) ++block;
    return make(
        [block, c, inverse](const cv::Mat& src) {
            cv::Mat dst;
            cv::adaptiveThreshold(to_u8(to_gray(src)), dst, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                                  inverse ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY, block, c);
            return dst;
        },
        block / 2 + 1, "adaptive_threshold");
}

// ------------------------------------------------------------------ sammansättning

FilterPtr chain(std::vector<FilterPtr> filters) {
    int margin = 0;
    std::string name;
    for (auto& f : filters) {
        margin += f->margin();
        name += (name.empty() ? "" : " > ") + f->name();
    }
    return make(
        [filters](const cv::Mat& src) {
            cv::Mat cur = src;
            for (auto& f : filters) cur = f->apply(cur);
            return cur;
        },
        margin, name);
}

FilterPtr custom(std::function<cv::Mat(const cv::Mat&)> fn, int margin, std::string name) {
    return make(std::move(fn), margin, std::move(name));
}

}  // namespace pc::imgproc
