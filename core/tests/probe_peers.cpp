// Felsökning: etiketterna på ett ark jämförda med varandra. Skriver arket med de hittade etiketterna och
// fynden (peers-sheet.png), konsensus och varje inpassad etikett till utkatalogen.
//   probe_peers <ark.tif> [utkatalog] [antal]
#include <cstdio>
#include <opencv2/imgproc.hpp>

#include "printcmp/compare/peers.hpp"
#include "printcmp/image/image.hpp"

using namespace pc;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "användning: probe_peers <ark> [utkatalog] [antal]\n");
        return 2;
    }
    std::string out = argc > 2 ? argv[2] : ".";
    auto scan = image::read_scan(argv[1]);
    compare::PeerOptions opt;
    if (argc > 3) opt.expected_count = std::atoi(argv[3]);
    auto found = compare::detect_labels(scan.rgb, scan.dpi, opt);
    std::printf("detektering: %zu etiketter\n", found.size());
    for (auto& d : found)
        std::printf("  %.2f°  (%.0f, %.0f) (%.0f, %.0f) (%.0f, %.0f) (%.0f, %.0f)\n", d.angle_deg, d.corners[0].x, d.corners[0].y, d.corners[1].x,
                    d.corners[1].y, d.corners[2].x, d.corners[2].y, d.corners[3].x, d.corners[3].y);
    auto r = compare::compare_peers(scan.rgb, scan.dpi, opt);
    std::printf("%s\n", r.to_json().dump(1).substr(0, 3000).c_str());
    double s = std::min(1.0, 1600.0 / std::max(scan.rgb.cols, scan.rgb.rows));
    cv::Mat small;
    cv::resize(scan.rgb, small, cv::Size(), s, s, cv::INTER_AREA);
    for (auto& d : found)
        for (int k = 0; k < 4; ++k)
            cv::line(small, cv::Point((int)(d.corners[k].x * s), (int)(d.corners[k].y * s)),
                     cv::Point((int)(d.corners[(k + 1) % 4].x * s), (int)(d.corners[(k + 1) % 4].y * s)), cv::Scalar(255, 0, 0), 2);
    for (auto& l : r.labels)
        for (int k = 0; k < 4; ++k)
            cv::line(small, cv::Point((int)(l.corners[k].x * s), (int)(l.corners[k].y * s)),
                     cv::Point((int)(l.corners[(k + 1) % 4].x * s), (int)(l.corners[(k + 1) % 4].y * s)),
                     l.master ? cv::Scalar(0, 80, 255) : cv::Scalar(0, 170, 0), 1);
    image::write_png(out + "/peers-sheet.png", small);
    if (!r.consensus_rgb.empty()) image::write_png(out + "/peers-consensus.png", r.consensus_rgb);
    for (size_t i = 0; i < r.aligned_rgb.size(); ++i) image::write_png(out + "/peers-label-" + std::to_string(i) + ".png", r.aligned_rgb[i]);
    return 0;
}
