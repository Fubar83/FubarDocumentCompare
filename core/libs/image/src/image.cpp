#include "printcmp/image/image.hpp"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "printcmp/core/core.hpp"
#include "printcmp/core/text.hpp"

namespace pc::image {

static double tiff_dpi(const std::string& path) {
    TIFFSetWarningHandler(nullptr);
#ifdef _WIN32
    std::wstring w = std::filesystem::u8path(path).wstring();
    TIFF* t = TIFFOpenW(w.c_str(), "r");
#else
    TIFF* t = TIFFOpen(path.c_str(), "r");
#endif
    if (!t) return 0;
    float xres = 0;
    uint16_t unit = RESUNIT_INCH;
    double dpi = 0;
    if (TIFFGetField(t, TIFFTAG_XRESOLUTION, &xres) && xres > 0) {
        TIFFGetFieldDefaulted(t, TIFFTAG_RESOLUTIONUNIT, &unit);
        dpi = unit == RESUNIT_CENTIMETER ? xres * 2.54 : xres;
    }
    TIFFClose(t);
    return dpi;
}

void write_tiff(const std::string& path, const cv::Mat& rgb, double dpi) {
    CV_Assert(rgb.type() == CV_8UC3);
#ifdef _WIN32
    std::wstring w = std::filesystem::u8path(path).wstring();
    TIFF* t = TIFFOpenW(w.c_str(), "w");
#else
    TIFF* t = TIFFOpen(path.c_str(), "w");
#endif
    if (!t) throw Error(ErrorCode::FileNotFound, "kan inte skriva " + path);
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, uint32_t(rgb.cols));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, uint32_t(rgb.rows));
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, uint32_t(std::max(1, 65536 / (rgb.cols * 3))));
    TIFFSetField(t, TIFFTAG_XRESOLUTION, float(dpi));
    TIFFSetField(t, TIFFTAG_YRESOLUTION, float(dpi));
    TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    for (int y = 0; y < rgb.rows; ++y)
        if (TIFFWriteScanline(t, const_cast<uchar*>(rgb.ptr<uchar>(y)), uint32_t(y), 0) < 0) {
            TIFFClose(t);
            throw Error(ErrorCode::Internal, "TIFF-skrivning misslyckades: " + path);
        }
    TIFFClose(t);
}

static cv::Mat to_rgb8(cv::Mat img) {
    if (img.channels() == 1) cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
    if (img.channels() == 4) cv::cvtColor(img, img, cv::COLOR_BGRA2BGR);
    cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
    if (img.depth() == CV_16U) {
        cv::Mat o;
        img.convertTo(o, CV_8U, 1.0 / 257.0, 0.5 - 1e-9);
        img = o;
    } else if (img.depth() != CV_8U) {
        cv::Mat o;
        img.convertTo(o, CV_8U);
        img = o;
    }
    return img;
}

Scan read_scan(const std::string& path, double dpi_override) {
    std::string ext = text::lower(std::filesystem::u8path(path).extension().string());
    auto bytes = read_file_bytes(path);
    cv::Mat img = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    if (img.empty()) throw Error(ErrorCode::FileFormat, "kan inte läsa " + path);
    Scan s;
    s.rgb = to_rgb8(img);
    if (ext == ".tif" || ext == ".tiff") s.dpi = tiff_dpi(path);
    if (dpi_override > 0) s.dpi = dpi_override;
    if (s.dpi <= 0)
        throw Error(ErrorCode::InvalidArgument, path + ": upplösning saknas i filen – ange provets dpi");
    return s;
}

Scan from_buffer(const unsigned char* data, int width, int height, int stride, int channels, double dpi) {
    if (!data || width <= 0 || height <= 0 || (channels != 1 && channels != 3 && channels != 4))
        throw Error(ErrorCode::InvalidArgument, "ogiltig bildbuffert");
    if (dpi <= 0) throw Error(ErrorCode::InvalidArgument, "bildbufferten saknar upplösning");
    int type = channels == 1 ? CV_8UC1 : channels == 3 ? CV_8UC3 : CV_8UC4;
    cv::Mat view(height, width, type, const_cast<unsigned char*>(data), static_cast<size_t>(stride));
    cv::Mat rgb;
    if (channels == 1) cv::cvtColor(view, rgb, cv::COLOR_GRAY2RGB);
    else if (channels == 4) cv::cvtColor(view, rgb, cv::COLOR_RGBA2RGB);
    else rgb = view.clone();
    return {rgb, dpi};
}

void write_png(const std::string& path, const cv::Mat& rgb) {
    cv::Mat bgr;
    if (rgb.channels() == 3) cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    else bgr = rgb;
    std::vector<unsigned char> buf;
    cv::imencode(".png", bgr, buf);
    write_file(path, std::string(buf.begin(), buf.end()));
}

std::string png_data_uri(const cv::Mat& rgb) {
    cv::Mat bgr;
    if (rgb.channels() == 3) cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    else bgr = rgb;
    std::vector<unsigned char> buf;
    if (!cv::imencode(".png", bgr, buf)) throw Error(ErrorCode::Internal, "PNG-kodning misslyckades");
    return "data:image/png;base64," + base64(buf);
}

std::vector<unsigned char> encode(const cv::Mat& img, Format format, int quality) {
    cv::Mat bgr;
    if (img.channels() == 3) cv::cvtColor(img, bgr, cv::COLOR_RGB2BGR);
    else if (img.channels() == 4 && format == Format::Jpeg) cv::cvtColor(img, bgr, cv::COLOR_RGBA2BGR);
    else if (img.channels() == 4) cv::cvtColor(img, bgr, cv::COLOR_RGBA2BGRA);
    else bgr = img;
    std::vector<unsigned char> buf;
    bool ok = format == Format::Jpeg
                  ? cv::imencode(".jpg", bgr, buf, {cv::IMWRITE_JPEG_QUALITY, quality < 0 ? 85 : std::clamp(quality, 1, 100)})
                  : cv::imencode(".png", bgr, buf, {cv::IMWRITE_PNG_COMPRESSION, quality >= 0 ? std::clamp(quality, 0, 9) : 3});
    if (!ok) throw Error(ErrorCode::Internal, "bildkodning misslyckades");
    return buf;
}


cv::Mat resize_to_dpi(const cv::Mat& rgb, double src_dpi, double dst_dpi) {
    if (std::abs(src_dpi - dst_dpi) < 1e-6) return rgb;
    double s = dst_dpi / src_dpi;
    cv::Mat out;
    cv::resize(rgb, out, cv::Size(std::max(1, (int)std::lround(rgb.cols * s)), std::max(1, (int)std::lround(rgb.rows * s))),
               0, 0, s < 1 ? cv::INTER_AREA : cv::INTER_CUBIC);
    return out;
}

cv::Mat to_float(const cv::Mat& rgb_u8) {
    cv::Mat f;
    rgb_u8.convertTo(f, CV_32F, 1.0 / 255.0);
    return f;
}

cv::Mat gray_f32(const cv::Mat& rgb) {
    cv::Mat g = rgb;
    if (rgb.channels() == 3) cv::cvtColor(rgb, g, cv::COLOR_RGB2GRAY);
    cv::Mat f;
    g.convertTo(f, CV_32F, g.depth() == CV_8U ? 1.0 / 255.0 : 1.0);
    return f;
}

cv::Mat luma(const cv::Mat& rgb_f) {
    cv::Mat out(rgb_f.size(), CV_32F);
    rgb_f.forEach<cv::Vec3f>([&](const cv::Vec3f& p, const int* pos) {
        out.at<float>(pos[0], pos[1]) = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    });
    return out;
}

static inline double srgb_lin(double c) { return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92; }
static inline double lab_f(double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; }

cv::Mat rgb2lab(const cv::Mat& rgb_f) {
    CV_Assert(rgb_f.type() == CV_32FC3);
    cv::Mat out(rgb_f.size(), CV_32FC3);
    // sRGB-tabell för 8-bitarsvärden snabbar upp det vanliga fallet; annars exakt formel
    rgb_f.forEach<cv::Vec3f>([&](const cv::Vec3f& p, const int* pos) {
        double r = srgb_lin(p[0]), g = srgb_lin(p[1]), b = srgb_lin(p[2]);
        double X = (0.412453 * r + 0.357580 * g + 0.180423 * b) / 0.95047;
        double Y = (0.212671 * r + 0.715160 * g + 0.072169 * b);
        double Z = (0.019334 * r + 0.119193 * g + 0.950227 * b) / 1.08883;
        double fx = lab_f(X), fy = lab_f(Y), fz = lab_f(Z);
        double L = Y > 0.008856 ? 116.0 * fy - 16.0 : 903.3 * Y;
        out.at<cv::Vec3f>(pos[0], pos[1]) = cv::Vec3f((float)L, (float)(500.0 * (fx - fy)), (float)(200.0 * (fy - fz)));
    });
    return out;
}

double delta_e_2000(const cv::Vec3f& p1, const cv::Vec3f& p2) {
    const double kL = 1, kC = 1, kH = 1;
    double L1 = p1[0], a1 = p1[1], b1 = p1[2], L2 = p2[0], a2 = p2[1], b2 = p2[2];
    double Cbar = 0.5 * (std::hypot(a1, b1) + std::hypot(a2, b2));
    double c7 = std::pow(Cbar, 7);
    double G = 0.5 * (1 - std::sqrt(c7 / (c7 + std::pow(25.0, 7))));
    double a1p = a1 * (1 + G), a2p = a2 * (1 + G);
    double C1 = std::hypot(a1p, b1), C2 = std::hypot(a2p, b2);
    double h1 = std::atan2(b1, a1p), h2 = std::atan2(b2, a2p);
    if (h1 < 0) h1 += 2 * CV_PI;
    if (h2 < 0) h2 += 2 * CV_PI;
    double dL = L2 - L1, dC = C2 - C1;
    double dh = h2 - h1;
    if (C1 * C2 == 0) dh = 0;
    else if (dh > CV_PI) dh -= 2 * CV_PI;
    else if (dh < -CV_PI) dh += 2 * CV_PI;
    double dH = 2 * std::sqrt(C1 * C2) * std::sin(dh / 2);
    double Lbar = 0.5 * (L1 + L2), Cb = 0.5 * (C1 + C2);
    double hbar = h1 + h2;
    if (C1 * C2 != 0) {
        if (std::abs(h1 - h2) > CV_PI) hbar += (hbar < 2 * CV_PI) ? 2 * CV_PI : -2 * CV_PI;
        hbar *= 0.5;
    }
    double T = 1 - 0.17 * std::cos(hbar - CV_PI / 6) + 0.24 * std::cos(2 * hbar) + 0.32 * std::cos(3 * hbar + CV_PI / 30) -
               0.20 * std::cos(4 * hbar - 63 * CV_PI / 180);
    double dTheta = 30 * CV_PI / 180 * std::exp(-std::pow((hbar * 180 / CV_PI - 275) / 25, 2));
    double cb7 = std::pow(Cb, 7);
    double Rc = 2 * std::sqrt(cb7 / (cb7 + std::pow(25.0, 7)));
    double l50 = (Lbar - 50) * (Lbar - 50);
    double SL = 1 + 0.015 * l50 / std::sqrt(20 + l50);
    double SC = 1 + 0.045 * Cb;
    double SH = 1 + 0.015 * Cb * T;
    double RT = -std::sin(2 * dTheta) * Rc;
    double tl = dL / (kL * SL), tc = dC / (kC * SC), th = dH / (kH * SH);
    return std::sqrt(tl * tl + tc * tc + th * th + RT * tc * th);
}

cv::Mat delta_e_2000(const cv::Mat& lab1, const cv::Mat& lab2) {
    cv::Mat out(lab1.size(), CV_32F);
    lab1.forEach<cv::Vec3f>([&](const cv::Vec3f& p, const int* pos) {
        out.at<float>(pos[0], pos[1]) = (float)delta_e_2000(p, lab2.at<cv::Vec3f>(pos[0], pos[1]));
    });
    return out;
}

cv::Mat ssim_map(const cv::Mat& a, const cv::Mat& b, double R) {
    cv::Mat X, Y;
    a.convertTo(X, CV_64F);
    b.convertTo(Y, CV_64F);
    auto filt = [](const cv::Mat& m) {
        cv::Mat o;
        // scipy.ndimage.gaussian_filter, sigma 1.5, truncate 3.5 (radie 5), mode 'reflect'
        cv::GaussianBlur(m, o, cv::Size(11, 11), 1.5, 1.5, cv::BORDER_REFLECT);
        return o;
    };
    cv::Mat ux = filt(X), uy = filt(Y);
    cv::Mat uxx = filt(X.mul(X)), uyy = filt(Y.mul(Y)), uxy = filt(X.mul(Y));
    cv::Mat vx = uxx - ux.mul(ux), vy = uyy - uy.mul(uy), vxy = uxy - ux.mul(uy);
    double C1 = (0.01 * R) * (0.01 * R), C2 = (0.03 * R) * (0.03 * R);
    cv::Mat A1 = 2 * ux.mul(uy) + C1, A2 = 2 * vxy + C2;
    cv::Mat B1 = ux.mul(ux) + uy.mul(uy) + C1, B2 = vx + vy + C2;
    cv::Mat S = A1.mul(A2) / B1.mul(B2);
    cv::Mat out;
    S.convertTo(out, CV_32F);
    return out;
}

std::vector<float> masked_values(const cv::Mat& img, const cv::Mat& mask) {
    std::vector<float> v;
    for (int y = 0; y < img.rows; ++y) {
        const float* p = img.ptr<float>(y);
        const uchar* m = mask.ptr<uchar>(y);
        for (int x = 0; x < img.cols; ++x)
            if (m[x]) v.push_back(p[x]);
    }
    return v;
}

double masked_median(const cv::Mat& img, const cv::Mat& mask) {
    auto v = masked_values(img, mask);
    return median(std::vector<double>(v.begin(), v.end()));
}

double masked_mean(const cv::Mat& img, const cv::Mat& mask) {
    auto v = masked_values(img, mask);
    if (v.empty()) return std::nan("");
    double s = 0;
    for (float x : v) s += x;
    return s / v.size();
}

cv::Mat ellipse_kernel(int r) { return cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * r + 1, 2 * r + 1)); }

}  // namespace pc::image
