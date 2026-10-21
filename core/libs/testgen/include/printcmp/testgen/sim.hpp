// Tryck- och scansimulering. Bilder är float RGB 0..1 (CV_32FC3).
#pragma once

#include <array>
#include <optional>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/testgen/label.hpp"
#include "printcmp/testgen/rng.hpp"

namespace pc::testgen::sim {

extern const cv::Vec3f PAPER;

// ---------------------------------------------------------------- tryck

struct PrintParams {
    bool halftone = true;
    double lpcm = 70;
    std::array<double, 4> dot_gain{}, density{1, 1, 1, 1};   // C, M, Y, K
    std::array<std::array<double, 2>, 4> misreg_mm{};
    json to_json() const;
};

// Simulera tryck av en etikett renderad i dpi_hi (RGB, CV_8UC3). Returnerar float RGB i dpi_out.
cv::Mat print_label(const cv::Mat& rgb_hi_u8, double dpi_hi, double dpi_out, const PrintParams& p, Rng& rng);

struct PrintDefect {
    Rect box;
    std::string polarity;
    double strength;
};
std::optional<PrintDefect> add_print_defect(cv::Mat& img, double ppm, const std::string& kind, Rng& rng,
                                            const std::array<double, 4>& region_mm, const std::vector<Rect>& avoid_mm);

cv::Mat rounded_rect_mask(int w, int h, double r);   // CV_8U 0/1
// v1: veck som förskjutning vinkelrätt mot linjen + skuggband
cv::Mat fold_v1(const cv::Mat& img, cv::Point2d p0, cv::Point2d p1, double w_px, double strength, double amp_px);
// v1: enkel scanner (MTF, färgmatris, brus) -> uint8 RGB
cv::Mat scanner_v1(const cv::Mat& img, Rng& rng, std::array<double, 2> mtf_sigma, double noise_sigma,
                   double color_jitter = 0.012);
// prägling: kupolformade punkter belysta från en sida; ändrar img på plats
void emboss(cv::Mat& img, const std::vector<cv::Point2d>& dots_px, double r_px, double strength, cv::Point2d light);

// ---------------------------------------------------------------- flatbäddsscanner

struct ScannerModel {
    std::string kind;          // cis | ccd
    double blur_px_per_mm = 0, light_h0_mm = 0, slope_gain = 0, light_sign = 1, persp_L_mm = 0;
    std::array<double, 3> chan_shift_px{};
    double jitter_px = 0, y_scale = 1, vignette = 0, shot = 0, gamma = 1, emboss_gain = 1;
    json to_json() const;
};
ScannerModel make_model(const std::string& kind, Rng& rng);

struct Fold {
    cv::Point2d p0, p1;        // px i scannerbilden
    std::string kind;          // mountain | valley
    double lift_mm = 0, width_mm = 0;
    bool crack = false;
    double crack_w_mm = 0.08;

    cv::Point2d normal() const;
    double signed_dist(double x, double y) const;
    void profile(double s_mm, double& h, double& dh) const;
    double arc_excess(double s_mm) const;
    double band_mm(double h_min = 0.05) const;

private:
    mutable std::vector<double> ex_t_, ex_v_;
};
Fold random_fold(Rng& rng, int SW, int SH, double ppm, std::array<double, 2> lift_range, double crack_p = 0.4);
// spräckt färg längs ett bergveck (arkets koordinater, före scanning)
void crack_ink(cv::Mat& sheet, const cv::Matx23d& G, const Fold& f, double ppm, Rng& rng);

// Avbildning scannerbild -> ark, och höjd över glaset
class Geometry {
public:
    Geometry(const cv::Matx23d& G, std::vector<Fold> folds, const ScannerModel& m, double ppm, cv::Size size, Rng& rng);
    void height(double x, double y, double& H, double& dHy) const;
    cv::Point2d to_sheet(double x, double y) const;
    cv::Point2d to_scan(double px, double py, const cv::Matx23d& G, int iters = 6) const;

private:
    cv::Matx23d Gi_;
    std::vector<Fold> folds_;
    ScannerModel m_;
    double ppm_;
    cv::Size size_;
    std::vector<std::array<double, 3>> jit_;   // period, fas, amplitud
};

struct ScanResult {
    cv::Mat rgb;                                 // CV_8UC3
    std::unique_ptr<Geometry> geo;
    std::vector<std::array<int, 4>> dust;        // x, y, w, h i scan-px
};
ScanResult scan(const cv::Mat& sheet, const cv::Matx23d& G, const std::vector<Fold>& folds, const ScannerModel& m, Rng& rng,
                double ppm, std::array<double, 2> mtf_sigma, double read_noise, cv::Vec3f lid, double color_jitter = 0.012);

}  // namespace pc::testgen::sim
