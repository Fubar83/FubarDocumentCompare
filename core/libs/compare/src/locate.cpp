// Etiketterna på ett ark: samma lokalisering som jämförelsen använder, utan registrering och diff.
#include "printcmp/compare/locate.hpp"

#include <opencv2/imgproc.hpp>

#include "printcmp/imgproc/transform.hpp"
#include "printcmp/scan/locate.hpp"

namespace pc::compare {

namespace {

// Grovlokaliseringen ger vinkeln på någon grad när. Räta upp etiketten med de grova hörnen, mät
// snedheten som är kvar (kanter och textrader) och vrid hörnen med den kring mitten.
void refine(LocatedLabel& l, const cv::Mat& sheet, cv::Size size) {
    if (size.width < 32 || size.height < 32) return;
    std::array<cv::Point2d, 4> q;
    for (int k = 0; k < 4; ++k) q[k] = {l.corners[k].x, l.corners[k].y};
    cv::Mat upright = imgproc::warp_quad(sheet, q, size);
    auto s = imgproc::estimate_skew(upright, 3.0);
    if (s.confidence < 0.3 || std::abs(s.angle_deg) < 0.01) return;
    l.angle_deg = std::fmod(l.angle_deg + s.angle_deg + 360.0, 360.0);
    cv::Mat m = cv::getRotationMatrix2D(cv::Point2d(l.center.x, l.center.y), s.angle_deg, 1.0);
    for (auto& c : l.corners) {
        double x = m.at<double>(0, 0) * c.x + m.at<double>(0, 1) * c.y + m.at<double>(0, 2);
        double y = m.at<double>(1, 0) * c.x + m.at<double>(1, 1) * c.y + m.at<double>(1, 2);
        c = {x, y};
    }
}

}  // namespace

LocatedSheet locate_labels(const ReferenceModel& R, const cv::Mat& sheet_rgb, double sheet_dpi, const LocateOptions& opt) {
    if (sheet_rgb.empty() || sheet_rgb.type() != CV_8UC3) throw Error(ErrorCode::InvalidArgument, "arket ska vara en RGB-bild");
    if (!(sheet_dpi > 0)) throw Error(ErrorCode::InvalidArgument, "arkets upplösning saknas");
    const cv::Mat& ref = R.ref.rgb;
    auto [insts, info] = scan::locate(ref, R.masks.inside, R.dpi, sheet_rgb, sheet_dpi, opt.rotations, opt.max_skew_deg, opt.expected_count);
    double angle = info.value("sheet_angle_deg", 0.0);
    double row_tol = 0.4 * std::min(ref.rows, ref.cols) * sheet_dpi / R.dpi;
    insts = scan::assign_grid(insts, angle, row_tol);
    LocatedSheet out;
    std::vector<double> skews;
    for (auto& in : insts) {
        LocatedLabel l;
        l.index = in.index;
        l.row = in.row;
        l.col = in.col;
        l.angle_deg = in.angle_deg;
        l.score = in.score;
        l.center = in.center;
        Pt cs[4] = {{0, 0}, {(double)ref.cols, 0}, {(double)ref.cols, (double)ref.rows}, {0, (double)ref.rows}};
        for (int k = 0; k < 4; ++k) l.corners[k] = in.H0.apply(cs[k]);
        if (opt.refine) refine(l, sheet_rgb, cv::Size((int)std::lround(ref.cols * sheet_dpi / R.dpi), (int)std::lround(ref.rows * sheet_dpi / R.dpi)));
        double s = std::fmod(l.angle_deg + 3600.0 + 45.0, 90.0) - 45.0;   // snedheten utan 90°-steg
        skews.push_back(s);
        out.labels.push_back(l);
    }
    if (!skews.empty()) {
        std::nth_element(skews.begin(), skews.begin() + skews.size() / 2, skews.end());
        out.sheet_angle_deg = skews[skews.size() / 2];
    } else {
        out.sheet_angle_deg = std::fmod(angle + 3600.0 + 45.0, 90.0) - 45.0;
    }
    return out;
}

}  // namespace pc::compare
