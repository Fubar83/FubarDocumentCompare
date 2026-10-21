// Frekvensplanet (DFT via OpenCV): spektrum, frekvensfilter, notchfilter för periodiska mönster
// (raster, moiré), faltning med stora matriser, faskorrelation och rasterperiod.
//
// Frekvenser anges i cykler per pixel, 0 … 0,5 (Nyquist). En period på p px är frekvensen 1/p.
#pragma once

#include <opencv2/core.hpp>
#include <vector>

#include "printcmp/imgproc/filter.hpp"

namespace pc::fft {

enum class Padding { Zero, Reflect };

struct Spectrum {
    cv::Mat complex;          // CV_32FC2, storlek = optimal DFT-storlek ≥ bilden
    cv::Size image_size;      // bildens storlek före utfyllnad
};

// Bilden (enkanalig; flerkanalig görs grå) till spektrum. Reflect minskar kantartefakter vid filtrering.
Spectrum forward(const cv::Mat& image, Padding pad = Padding::Reflect);
// Tillbaka till en reell bild (CV_32F) i bildens ursprungliga storlek.
cv::Mat inverse(const Spectrum& s);
// |F| (log(1+|F|) om log), med nollfrekvensen i mitten om centered. CV_32F.
cv::Mat magnitude(const Spectrum& s, bool log = true, bool centered = true);
cv::Mat phase(const Spectrum& s, bool centered = true);
void shift(cv::Mat& m);   // byt kvadranter (fftshift; sin egen invers för jämna storlekar)

// ---------------------------------------------------------------- frekvensfilter
enum class Pass { Low, High, Band, BandStop };
enum class Shape { Ideal, Gaussian, Butterworth };
struct FrequencySpec {
    Pass pass = Pass::Low;
    Shape shape = Shape::Gaussian;
    double cutoff = 0.1;      // cykler/px; för Band/BandStop den undre gränsen
    double cutoff2 = 0.25;    // övre gränsen för Band/BandStop
    int order = 2;            // Butterworth
};
// Filtrets förstärkning 0..1 över spektrets storlek (nollfrekvensen i hörnet, som DFT:n).
cv::Mat frequency_mask(cv::Size spectrum_size, const FrequencySpec& spec);
// Filtrera en bild i frekvensplanet (per kanal, behåller typen).
cv::Mat filter_frequency(const cv::Mat& image, const FrequencySpec& spec);
// Som Filter (för områden och kedjor). margin: hur mycket omgivning som tas med runt ett område.
imgproc::FilterPtr frequency_filter(const FrequencySpec& spec, int margin = 16);

// ---------------------------------------------------------------- periodiska mönster
struct Peak {
    cv::Point2d freq;         // (fx, fy) i cykler/px
    double magnitude = 0;
    double period_px = 0;     // 1 / |freq|
    double angle_deg = 0;     // riktning på vågen (0 = vågfronter lodräta)
};
// De starkaste topparna i spektrumet utanför min_freq (bara en av varje symmetriskt par).
std::vector<Peak> spectral_peaks(const cv::Mat& image, int n = 4, double min_freq = 0.02);
// Dämpa topparna (och deras speglingar) med gaussiska notchar med radien radius (cykler/px).
cv::Mat notch(const cv::Mat& image, const std::vector<Peak>& peaks, double radius = 0.01);
// Rasterperiod i px (starkaste toppen), 0 om ingen tydlig topp.
double dominant_period(const cv::Mat& image, double min_freq = 0.02);
// Ta bort tryckrastret: per kanal (färgplåtarna har olika rastervinklar) notchas de n starkaste
// topparna över min_freq. Rastret ligger högt i frekvens (3–7 px vid 600 dpi); lägre toppar är
// motivet eller moiré mellan plåtarna och lämnas kvar.
cv::Mat descreen(const cv::Mat& image, int n = 4, double min_freq = 0.12, double radius = 0.01);

// ---------------------------------------------------------------- faltning och korrelation
// Samma resultat som cv::filter2D (korrelation, ankare i mitten, BORDER_REFLECT_101), men via DFT:
// snabbare för stora matriser. Per kanal; resultatet är CV_32F.
cv::Mat convolve(const cv::Mat& image, const cv::Mat& kernel);

struct Shift {
    cv::Point2d offset;       // b är a förskjuten med offset (px, delpixel)
    double response = 0;      // toppens styrka 0..1
};
// Förskjutning mellan två bilder av samma motiv (faskorrelation, Hanning-fönster).
Shift phase_correlate(const cv::Mat& a, const cv::Mat& b, bool window = true);

}  // namespace pc::fft
