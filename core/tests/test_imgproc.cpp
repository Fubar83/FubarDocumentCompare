// pc_imgproc: filter på områden, FFT, mönstersökning och mätning.
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "printcmp/imgproc/imgproc.hpp"
#include "test.hpp"

using namespace pc;
using namespace pc::imgproc;

namespace {

cv::Mat noise_rgb(int w, int h, uint64 seed) {
    cv::Mat m(h, w, CV_8UC3);
    cv::RNG(seed).fill(m, cv::RNG::UNIFORM, 0, 256);
    return m;
}

cv::Mat texture(int w, int h, uint64 seed) {
    cv::Mat m(h, w, CV_8U);
    cv::RNG(seed).fill(m, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(m, m, cv::Size(0, 0), 1.2);
    cv::normalize(m, m, 0, 255, cv::NORM_MINMAX);
    return m;
}

// största absoluta skillnaden inom masken
double max_diff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask) {
    cv::Mat fa, fb, d;
    a.convertTo(fa, CV_64F);
    b.convertTo(fb, CV_64F);
    cv::absdiff(fa, fb, d);
    std::vector<cv::Mat> ch;
    cv::split(d, ch);
    cv::Mat per_pixel = ch[0];
    for (size_t i = 1; i < ch.size(); ++i) per_pixel = cv::max(per_pixel, ch[i]);
    double mx = 0;
    cv::minMaxLoc(per_pixel, nullptr, &mx, nullptr, nullptr, mask.empty() ? cv::noArray() : mask);
    return mx;
}

}  // namespace

TEST(imgproc_region_filter_equals_whole_image) {
    RegionSet regions = {Region::box({0, 0, 60, 40}), Region::circle({120, 90}, 35),
                         Region::polygon({{180, 120}, {239, 150}, {200, 179}, {170, 175}}), Region::box({100, 0, 140, 20})};
    cv::Mat u8 = noise_rgb(240, 180, 1), f32;
    u8.convertTo(f32, CV_32F, 1.0 / 255);
    for (const cv::Mat& src : {u8, f32}) {
        bool is_u8 = src.depth() == CV_8U;
        std::vector<FilterPtr> filters = {gaussian(2.5),
                                          kernel({{0, -1, 0}, {-1, 5, -1}, {0, -1, 0}}),
                                          kernel(cv::Mat::ones(7, 5, CV_32F), true),
                                          median(5),
                                          morph(Morph::Close, 3),
                                          unsharp(1.5, 2.0),
                                          sobel(1, 0),
                                          gradient_magnitude()};
        // i 8 bitar förstärker laplace avrundningen efter gauss; kedjan provas på flyttal
        if (!is_u8) filters.push_back(chain({gaussian(1.0), laplacian(3)}));
        // OpenCV kan välja en annan heltalsväg för små utsnitt: 8-bitarsresultat får skilja 1 nivå
        double tol = is_u8 ? 1.0 : 1e-4;
        for (auto& f : filters) {
            cv::Mat full = f->apply(src);
            cv::Mat out = apply_regions(*f, src, regions);
            cv::Mat inside = mask_of(regions, src.size());
            double d = max_diff(out, full, inside);
            if (d > tol) std::fprintf(stderr, "  %s (%s): skillnad %.6f\n", f->name().c_str(), is_u8 ? "8 bit" : "flyttal", d);
            CHECK(d <= tol);
            // utanför områdena: originalet (i filtrets typ)
            cv::Mat outside = inside == 0;
            CHECK(max_diff(out, fit_type(src, out.type()), outside) == 0);
        }
    }
}

TEST(imgproc_mask_limits_written_pixels) {
    cv::Mat src = noise_rgb(100, 100, 2), dst = src.clone();
    Region c = Region::circle({50, 50}, 20);
    apply_regions(*invert(), src, {c}, dst);
    CHECK(dst.at<cv::Vec3b>(31, 31) == src.at<cv::Vec3b>(31, 31));   // i rektangeln, utanför cirkeln
    cv::Vec3b in = src.at<cv::Vec3b>(50, 50);
    CHECK(dst.at<cv::Vec3b>(50, 50) == cv::Vec3b(255 - in[0], 255 - in[1], 255 - in[2]));
}

TEST(imgproc_results_into_other_image) {
    cv::Mat src = noise_rgb(120, 80, 3);
    cv::Mat dst = cv::Mat::zeros(300, 300, CV_8UC3);
    Region r = Region::box({10, 10, 40, 30});
    apply_regions(*gaussian(1.0), src, {r}, dst, Placement{{100, 100}, 2.0});
    // källrutan (10,10,40,30) · 2 + (100,100) = (120,120,80,60)
    CHECK(cv::countNonZero(dst(cv::Rect(120, 120, 80, 60)).reshape(1)) > 0);
    CHECK(cv::countNonZero(dst(cv::Rect(0, 0, 110, 110)).reshape(1)) == 0);
    cv::Scalar m_src = cv::mean(filter_region(*gaussian(1.0), src, r)), m_dst = cv::mean(dst(cv::Rect(120, 120, 80, 60)));
    CHECK_NEAR(m_src[0], m_dst[0], 1.5);

    // från områden till andra områden
    cv::Mat dst2 = cv::Mat::zeros(200, 200, CV_8UC3);
    apply_to_targets(*gray(), src, {r, Region::box({60, 40, 20, 20})}, dst2,
                     {Region::box({0, 0, 80, 60}), Region::circle({150, 150}, 30)});
    CHECK(cv::countNonZero(dst2(cv::Rect(0, 0, 80, 60)).reshape(1)) > 0);
    CHECK(dst2.at<cv::Vec3b>(121, 121) == cv::Vec3b(0, 0, 0));   // hörnet utanför cirkeln
    cv::Vec3b g = dst2.at<cv::Vec3b>(150, 150);
    CHECK(g[0] == g[1] && g[1] == g[2]);                           // gråskala tillbaka som RGB
}

TEST(imgproc_regions_from_mask_and_grid) {
    cv::Mat m = cv::Mat::zeros(100, 100, CV_8U);
    cv::circle(m, {20, 20}, 8, 255, cv::FILLED);
    cv::rectangle(m, cv::Rect(50, 50, 30, 10), 255, cv::FILLED);
    m.at<uchar>(90, 90) = 255;
    auto rs = regions_from_mask(m, 5);
    CHECK_EQ(rs.size(), size_t(2));
    CHECK_EQ(rs[1].area(), 300);
    CHECK_EQ(grid({0, 0, 100, 50}, 2, 4, 4).size(), size_t(8));
    CHECK(Region::mm(10, 10, 5, 5, 254).rect == cv::Rect(100, 100, 50, 50));
}

// ------------------------------------------------------------------ FFT

TEST(fft_roundtrip) {
    cv::Mat img = texture(97, 61, 4), back;
    auto s = fft::forward(img);
    fft::inverse(s).convertTo(back, CV_8U);
    CHECK(max_diff(img, back, {}) <= 1);
}

TEST(fft_convolution_equals_filter2d) {
    cv::Mat img(90, 120, CV_32F), k(11, 15, CV_32F), ref;
    cv::RNG(5).fill(img, cv::RNG::UNIFORM, 0, 1);
    cv::RNG(6).fill(k, cv::RNG::UNIFORM, -1, 1);
    cv::filter2D(img, ref, CV_32F, k);
    CHECK(max_diff(fft::convolve(img, k), ref, {}) < 1e-3);
}

TEST(fft_lowpass_removes_fine_pattern) {
    cv::Mat img(128, 128, CV_32F);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
            img.at<float>(y, x) = float(100 + 50 * std::sin(2 * CV_PI * x / 64.0) + 30 * std::sin(2 * CV_PI * x / 4.0));
    fft::FrequencySpec lp;
    lp.cutoff = 0.08;
    cv::Mat out = fft::filter_frequency(img, lp);
    // den fina vågen (period 4) borta, den grova (period 64) kvar
    double fine = 0, coarse = 0;
    for (int x = 0; x < 128; ++x) {
        fine += out.at<float>(64, x) * std::sin(2 * CV_PI * x / 4.0);
        coarse += out.at<float>(64, x) * std::sin(2 * CV_PI * x / 64.0);
    }
    CHECK(std::abs(fine / 64) < 1.5);
    CHECK(coarse / 64 > 40);
}

TEST(fft_finds_and_removes_screen) {
    // raster med period 6 px i 30°, ovanpå en mjuk gradient
    cv::Mat img(160, 160, CV_32F);
    double a = 30 * CV_PI / 180;
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 160; ++x)
            img.at<float>(y, x) = float(80 + 0.4 * x + 40 * std::cos(2 * CV_PI * (x * std::cos(a) + y * std::sin(a)) / 6.0));
    double p = fft::dominant_period(img);
    CHECK_NEAR(p, 6.0, 0.3);
    auto peaks = fft::spectral_peaks(img, 1);
    CHECK(!peaks.empty());
    if (peaks.empty()) return;
    CHECK(std::abs(std::abs(peaks[0].angle_deg) - 30) < 4 || std::abs(std::abs(peaks[0].angle_deg) - 150) < 4);
    cv::Mat clean = fft::notch(img, peaks, 0.015);
    cv::Mat smooth(img.size(), CV_32F);
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 160; ++x) smooth.at<float>(y, x) = float(80 + 0.4 * x);
    cv::Scalar m, before, after;
    cv::meanStdDev(img(cv::Rect(20, 20, 120, 120)) - smooth(cv::Rect(20, 20, 120, 120)), m, before);
    cv::meanStdDev(clean(cv::Rect(20, 20, 120, 120)) - smooth(cv::Rect(20, 20, 120, 120)), m, after);
    CHECK(after[0] < 0.1 * before[0]);
}

TEST(fft_phase_correlation_recovers_shift) {
    cv::Mat a = texture(200, 160, 7), b;
    cv::warpAffine(a, b, cv::Matx23f(1, 0, 7.3f, 0, 1, -4.6f), a.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT);
    auto s = fft::phase_correlate(a, b);
    CHECK_NEAR(s.offset.x, 7.3, 0.2);
    CHECK_NEAR(s.offset.y, -4.6, 0.2);
    CHECK(s.response > 0.3);
}

// ------------------------------------------------------------------ mönstersökning

TEST(match_finds_all_instances) {
    cv::Mat img = texture(300, 200, 8) / 4 + 100, t = texture(24, 24, 9);
    std::vector<cv::Point> at = {{20, 30}, {150, 40}, {240, 150}};
    for (auto& p : at) t.copyTo(img(cv::Rect(p, t.size())));
    auto ms = find_template(img, t);
    CHECK_EQ(ms.size(), size_t(3));
    for (auto& p : at) {
        bool found = false;
        for (auto& m : ms) found |= std::abs(m.box.x - p.x) <= 1 && std::abs(m.box.y - p.y) <= 1 && m.score > 0.95;
        CHECK(found);
    }
    // bara inom ett område
    MatchOptions o;
    o.search_in = {Region::box({100, 0, 120, 100})};
    auto inside = find_template(img, t, o);
    CHECK_EQ(inside.size(), size_t(1));
    if (!inside.empty()) CHECK(std::abs(inside[0].box.x - 150) <= 1);
}

TEST(match_pyramid_equals_direct_search) {
    cv::Mat img = texture(900, 700, 21) / 4 + 100, t = texture(160, 120, 22);
    std::vector<cv::Point> at = {{40, 60}, {500, 90}, {300, 480}, {690, 520}};
    for (auto& p : at) t.copyTo(img(cv::Rect(p, t.size())));
    MatchOptions direct;
    direct.pyramid = 1;
    MatchOptions fast;   // automatiskt: faktor 4 för en mall på 120 px
    auto a = find_template(img, t, direct), b = find_template(img, t, fast);
    CHECK_EQ(a.size(), at.size());
    CHECK_EQ(b.size(), at.size());
    for (auto& p : at) {
        bool found = false;
        for (auto& m : b) found |= m.box.tl() == p && m.score > 0.99;
        CHECK(found);
    }
}

TEST(match_rotation_and_scale) {
    cv::Mat img = texture(260, 260, 10) / 4 + 100, t = texture(40, 30, 11);
    // mallen roterad 20° och skalad 1,2, med mitten i (130, 120)
    cv::Mat big;
    cv::resize(t, big, cv::Size(), 1.2, 1.2, cv::INTER_LINEAR);
    cv::Mat R = cv::getRotationMatrix2D(cv::Point2f(big.cols / 2.0f, big.rows / 2.0f), 20, 1.0);
    R.at<double>(0, 2) += 130 - big.cols / 2.0;
    R.at<double>(1, 2) += 120 - big.rows / 2.0;
    cv::Mat mask = cv::Mat::zeros(img.size(), CV_8U), warped;
    cv::warpAffine(big, warped, R, img.size(), cv::INTER_LINEAR);
    cv::warpAffine(cv::Mat(big.size(), CV_8U, cv::Scalar(255)), mask, R, img.size(), cv::INTER_NEAREST);
    warped.copyTo(img, mask);
    MatchOptions o;
    o.angles = {-20, -10, 0, 10, 20, 30};
    o.scales = {1.0, 1.2};
    o.threshold = 0.6;
    auto ms = find_template(img, t, o);
    CHECK(!ms.empty());
    if (ms.empty()) return;
    CHECK_NEAR(ms[0].angle, 20, 1e-9);
    CHECK_NEAR(ms[0].scale, 1.2, 1e-9);
    CHECK_NEAR(ms[0].center.x, 130, 2.0);
    CHECK_NEAR(ms[0].center.y, 120, 2.0);
    CHECK_EQ(ms[0].corners.size(), size_t(4));
}

TEST(features_find_rotated_scaled_pattern) {
    // tydligt mönster: text och former
    cv::Mat t(120, 200, CV_8U, cv::Scalar(255));
    cv::putText(t, "PRINTCMP", {8, 50}, cv::FONT_HERSHEY_DUPLEX, 1.2, 0, 2);
    cv::circle(t, {40, 90}, 18, 0, 3);
    cv::rectangle(t, cv::Rect(90, 70, 50, 35), 0, cv::FILLED);
    cv::line(t, {150, 65}, {195, 115}, 0, 3);
    cv::Mat img(400, 500, CV_8U, cv::Scalar(230));
    cv::Mat A = cv::getRotationMatrix2D(cv::Point2f(100, 60), 35, 0.8);
    A.at<double>(0, 2) += 250 - 100;
    A.at<double>(1, 2) += 200 - 60;
    cv::Mat warped, m;
    cv::warpAffine(t, warped, A, img.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(255));
    cv::warpAffine(cv::Mat(t.size(), CV_8U, cv::Scalar(255)), m, A, img.size(), cv::INTER_NEAREST);
    warped.copyTo(img, m);
    auto fs = find_features(img, t);
    CHECK_EQ(fs.size(), size_t(1));
    if (fs.empty()) return;
    // mallens mitt (100, 60) ska hamna i (250, 200)
    cv::Point2d c = 0.25 * (cv::Point2d(fs[0].corners[0]) + cv::Point2d(fs[0].corners[1]) + cv::Point2d(fs[0].corners[2]) +
                            cv::Point2d(fs[0].corners[3]));
    CHECK_NEAR(c.x, 250, 3.0);
    CHECK_NEAR(c.y, 200, 3.0);
}

TEST(measure_blobs_after_threshold) {
    cv::Mat img(200, 300, CV_8UC3, cv::Scalar(20, 20, 20));
    cv::circle(img, {50, 50}, 15, cv::Scalar(240, 240, 240), cv::FILLED);
    cv::circle(img, {150, 100}, 25, cv::Scalar(240, 240, 240), cv::FILLED);
    cv::rectangle(img, cv::Rect(220, 140, 40, 30), cv::Scalar(240, 240, 240), cv::FILLED);
    auto bin = otsu()->apply(img);
    auto rs = blobs(bin, 20);
    CHECK_EQ(rs.size(), size_t(3));
    auto st = measure(img, rs);
    for (auto& s : st) CHECK_NEAR(s.mean[0], 240, 1.0);
    CHECK_NEAR(st[1].area, CV_PI * 25 * 25, 60);
    CHECK_NEAR(st[1].centroid.x, 150, 0.5);
    cv::Mat vis = img.clone();
    draw_regions(vis, rs);
    CHECK(cv::norm(vis, img) > 0);
}
