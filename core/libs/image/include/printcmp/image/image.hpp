// pc_image: bildinläsning, färgrymder och bildmått. Bilder är cv::Mat (RGB, inte BGR).
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace pc::image {

// Scan: RGB uint8 och upplösning. 16-bit skalas till 8-bit. dpi_override > 0 ersätter filens.
struct Scan {
    cv::Mat rgb;
    double dpi = 0;
};
Scan read_scan(const std::string& path, double dpi_override = 0);
Scan from_buffer(const unsigned char* data, int width, int height, int stride, int channels, double dpi);

void write_png(const std::string& path, const cv::Mat& rgb);
// Okomprimerad RGB-TIFF med upplösning (som tifffile.imwrite(..., resolution=(dpi, dpi)))
void write_tiff(const std::string& path, const cv::Mat& rgb, double dpi);
std::string png_data_uri(const cv::Mat& rgb);
// Kodar i minnet. Bilden är RGB, RGBA eller grå. quality: JPEG 1–100 (-1 = 85); PNG komprimeringsnivå 0–9 (-1 = 3).
enum class Format { Png, Jpeg };
std::vector<unsigned char> encode(const cv::Mat& img, Format format = Format::Png, int quality = -1);
cv::Mat resize_to_dpi(const cv::Mat& rgb, double src_dpi, double dst_dpi);

cv::Mat to_float(const cv::Mat& rgb_u8);                 // CV_32FC3, 0..1
cv::Mat gray_f32(const cv::Mat& rgb);                    // 0..1, som cv2.COLOR_RGB2GRAY
cv::Mat luma(const cv::Mat& rgb_f);                      // 0.299 R + 0.587 G + 0.114 B

// skimage.color.rgb2lab (sRGB, D65), float in 0..1 -> CV_32FC3 L*a*b*.
cv::Mat rgb2lab(const cv::Mat& rgb_f);
// skimage.color.deltaE_ciede2000 per pixel.
cv::Mat delta_e_2000(const cv::Mat& lab1, const cv::Mat& lab2);
double delta_e_2000(const cv::Vec3f& a, const cv::Vec3f& b);
// skimage.metrics.structural_similarity(full=True), gaussian_weights, sigma 1.5, data_range.
cv::Mat ssim_map(const cv::Mat& a, const cv::Mat& b, double data_range);

// numpy-liknande hjälp
std::vector<float> masked_values(const cv::Mat& img_f32, const cv::Mat& mask_u8);
double masked_median(const cv::Mat& img_f32, const cv::Mat& mask_u8);
double masked_mean(const cv::Mat& img_f32, const cv::Mat& mask_u8);
cv::Mat ellipse_kernel(int radius);

}  // namespace pc::image
