// Särdragsmatchning: ORB-punkter, Lowes kvottest och homografi med RANSAC. Hittar ett mönster
// oberoende av skala, rotation och (måttligt) perspektiv; flera förekomster en i taget.
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include "printcmp/imgproc/match.hpp"

namespace pc::imgproc {

namespace {

cv::Mat gray_u8(const cv::Mat& m) {
    cv::Mat g = m;
    if (m.channels() == 3) cv::cvtColor(m, g, cv::COLOR_RGB2GRAY);
    else if (m.channels() == 4) cv::cvtColor(m, g, cv::COLOR_RGBA2GRAY);
    if (g.depth() != CV_8U) {
        cv::Mat u;
        double mx;
        cv::minMaxLoc(g, nullptr, &mx);
        g.convertTo(u, CV_8U, mx <= 1.0 ? 255.0 : 1.0);
        g = u;
    }
    return g;
}

}  // namespace

Region FeatureMatch::region() const {
    std::vector<cv::Point> pts;
    for (auto& p : corners) pts.push_back(cv::Point(int(std::lround(p.x)), int(std::lround(p.y))));
    Region r = Region::polygon(pts);
    r.score = score;
    return r;
}

std::vector<FeatureMatch> find_features(const cv::Mat& image, const cv::Mat& templ, const FeatureOptions& opt) {
    cv::Mat img = gray_u8(image), tpl = gray_u8(templ);
    auto orb = cv::ORB::create(opt.max_features);
    std::vector<cv::KeyPoint> kt, ki;
    cv::Mat dt, di;
    orb->detectAndCompute(tpl, cv::noArray(), kt, dt);
    cv::Mat search_mask = opt.search_in.empty() ? cv::Mat() : mask_of(opt.search_in, img.size());
    orb->detectAndCompute(img, search_mask, ki, di);
    std::vector<FeatureMatch> out;
    if (dt.empty() || di.empty()) return out;
    cv::BFMatcher matcher(cv::NORM_HAMMING);
    std::vector<bool> used(ki.size(), false);
    const std::vector<cv::Point2f> tcorners = {{0, 0}, {float(tpl.cols), 0}, {float(tpl.cols), float(tpl.rows)}, {0, float(tpl.rows)}};
    for (int inst = 0; inst < opt.max_instances; ++inst) {
        // bildpunkter som inte hör till en redan hittad förekomst
        std::vector<int> idx;
        for (size_t i = 0; i < ki.size(); ++i)
            if (!used[i]) idx.push_back(int(i));
        if (int(idx.size()) < opt.min_inliers) break;
        cv::Mat d(int(idx.size()), di.cols, di.type());
        for (size_t r = 0; r < idx.size(); ++r) di.row(idx[r]).copyTo(d.row(int(r)));
        std::vector<std::vector<cv::DMatch>> knn;
        matcher.knnMatch(dt, d, knn, 2);
        std::vector<cv::Point2f> pt, pi;
        for (auto& m : knn)
            if (m.size() == 2 && m[0].distance < opt.ratio * m[1].distance) {
                pt.push_back(kt[m[0].queryIdx].pt);
                pi.push_back(ki[idx[m[0].trainIdx]].pt);
            }
        if (int(pt.size()) < opt.min_inliers) break;
        cv::Mat inl;
        cv::Mat H = cv::findHomography(pt, pi, cv::RANSAC, opt.ransac_px, inl);
        if (H.empty()) break;
        int n_in = cv::countNonZero(inl);
        if (n_in < opt.min_inliers) break;
        FeatureMatch fm;
        fm.H = cv::Matx33d(H);
        cv::perspectiveTransform(tcorners, fm.corners, H);
        fm.inliers = n_in;
        fm.matches = int(pt.size());
        fm.score = double(n_in) / pt.size();
        out.push_back(fm);
        // ta bort förekomstens punkter och leta efter nästa
        for (int i : idx)
            if (cv::pointPolygonTest(fm.corners, ki[i].pt, false) >= 0) used[i] = true;
    }
    return out;
}

}  // namespace pc::imgproc
