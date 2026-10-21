// Geometriska transformer av hela bilder (utdata kan få en annan storlek) och automatisk snedhet.
//
// Vinklar i grader, positiva moturs som bilden ser ut (OpenCV:s konvention med y nedåt).
// Fyllnad utanför bilden: vit (papper) om inget annat anges.
#pragma once

#include <array>
#include <opencv2/core.hpp>

namespace pc::imgproc {

enum class Interpolation { Auto, Nearest, Linear, Cubic, Area };

// Vrid kring mitten. expand: bilden växer så att inget skärs bort.
cv::Mat rotate(const cv::Mat& src, double deg, bool expand = true, const cv::Scalar& fill = cv::Scalar::all(255),
               Interpolation interp = Interpolation::Auto);
// Vrid i steg om 90° moturs (exakt, utan interpolation).
cv::Mat rotate90(const cv::Mat& src, int quarter_turns);
cv::Mat flip(const cv::Mat& src, bool horizontal, bool vertical);
// Skala med faktorer (Auto: Area vid förminskning, Cubic vid förstoring), eller till en storlek.
cv::Mat scale(const cv::Mat& src, double fx, double fy, Interpolation interp = Interpolation::Auto);
cv::Mat resize_to(const cv::Mat& src, cv::Size size, Interpolation interp = Interpolation::Auto);
// Utsnitt (begränsas till bilden).
cv::Mat crop(const cv::Mat& src, const cv::Rect& rect);
// Affin avbildning src -> utdata (2×3), till storleken size.
cv::Mat warp_affine(const cv::Mat& src, const cv::Matx23d& m, cv::Size size, const cv::Scalar& fill = cv::Scalar::all(255),
                    Interpolation interp = Interpolation::Auto);
// Fyrhörning (övre vänster, övre höger, nedre höger, nedre vänster) till en upprätt rektangel (perspektiv).
cv::Mat warp_quad(const cv::Mat& src, const std::array<cv::Point2d, 4>& corners, cv::Size size,
                  Interpolation interp = Interpolation::Auto);

// Snedhet ur kanternas riktningar (text, linjer, etikett- och arkkanter), modulo 90°: vinkeln som
// innehållet är vridet (moturs positiv). deskew vrider tillbaka. confidence 0..1: hur tydlig toppen är.
struct Skew {
    double angle_deg = 0;
    double confidence = 0;
};
Skew estimate_skew(const cv::Mat& src, double max_deg = 15);
cv::Mat deskew(const cv::Mat& src, double max_deg = 15, Skew* found = nullptr, bool expand = true,
               const cv::Scalar& fill = cv::Scalar::all(255));

}  // namespace pc::imgproc
