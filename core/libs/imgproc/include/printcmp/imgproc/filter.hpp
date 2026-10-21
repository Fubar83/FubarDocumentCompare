// Filter: en operation bild -> bild med samma storlek. Nya filter läggs till genom att ärva
// Filter (eller med custom()); inget annat behöver ändras.
//
// Kanaler: filtren arbetar per kanal om inget annat sägs. Djup: utjämning, morfologi och skärpning
// behåller bildens djup (mättat); derivator (sobel, scharr, laplace, gradient) ger CV_32F med tecken.
// Kanter: OpenCV:s BORDER_REFLECT_101 vid bildens kant, som när hela bilden filtreras.
#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace pc::imgproc {

class Filter {
public:
    virtual ~Filter() = default;
    virtual cv::Mat apply(const cv::Mat& src) const = 0;
    // Hur många pixlar runt ett område filtret läser. Ett område filtreras med så mycket
    // omgivning, så att resultatet blir detsamma som när hela bilden filtreras.
    virtual int margin() const = 0;
    virtual std::string name() const = 0;
};

using FilterPtr = std::shared_ptr<const Filter>;

// ---------------------------------------------------------------- matriser
// Egen matris (korrelation som cv::filter2D, ankare i mitten). normalize: dela med summan.
// out_depth: -1 = samma som bilden, CV_32F för filter med negativa vikter.
FilterPtr kernel(const cv::Mat& k, bool normalize = false, double delta = 0, int out_depth = -1);
FilterPtr kernel(std::initializer_list<std::initializer_list<float>> rows, bool normalize = false, double delta = 0,
                 int out_depth = -1);
// Separerbar matris (rad och kolumn), snabbare för stora utjämningar.
FilterPtr separable(const std::vector<float>& row, const std::vector<float>& col, int out_depth = -1);

// ---------------------------------------------------------------- utjämning
FilterPtr gaussian(double sigma_x, double sigma_y = 0);
FilterPtr box(int size);
FilterPtr median(int size);                                   // udda storlek
FilterPtr bilateral(int diameter, double sigma_color, double sigma_space);

// ---------------------------------------------------------------- kanter och derivator (CV_32F)
FilterPtr sobel(int dx, int dy, int size = 3);
FilterPtr scharr(int dx, int dy);
FilterPtr laplacian(int size = 3);
FilterPtr gradient_magnitude(int size = 3);                   // sqrt(gx² + gy²)
FilterPtr canny(double low, double high, int aperture = 3);   // CV_8U 0/255, enkanalig

// ---------------------------------------------------------------- skärpa och ton
FilterPtr unsharp(double amount, double sigma);               // src + amount·(src − gauss(src))
FilterPtr invert();
FilterPtr gray();                                             // enkanalig
FilterPtr gamma(double g);
FilterPtr normalize_range(double lo = 0, double hi = 255);    // min–max-sträckning per kanal
FilterPtr clahe(double clip_limit = 2.0, int tiles = 8);      // lokal histogramutjämning (8-bit)

// ---------------------------------------------------------------- morfologi
enum class Morph { Erode, Dilate, Open, Close, Gradient, TopHat, BlackHat };
enum class Shape { Rect, Ellipse, Cross };
FilterPtr morph(Morph op, int radius, Shape shape = Shape::Ellipse, int iterations = 1);

// ---------------------------------------------------------------- tröskling (enkanalig CV_8U 0/255)
FilterPtr threshold(double t, bool inverse = false);
FilterPtr otsu(bool inverse = false);
FilterPtr adaptive_threshold(int block_size, double c, bool inverse = false);

// ---------------------------------------------------------------- sammansättning
// Filtren i tur och ordning; marginalen är summan.
FilterPtr chain(std::vector<FilterPtr> filters);
// Egen funktion. margin måste täcka hur långt funktionen läser runt en pixel.
FilterPtr custom(std::function<cv::Mat(const cv::Mat&)> fn, int margin, std::string name = "custom");

}  // namespace pc::imgproc
