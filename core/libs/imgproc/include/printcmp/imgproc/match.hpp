// Mönstersökning: mallmatchning (normaliserad korrelation, OpenCV räknar stora mallar via DFT),
// med flera träffar, vinklar och skalor, sökning inom områden, och särdragsmatchning (ORB +
// homografi) för mönster som kan vara roterade, skalade eller perspektivförvrängda.
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/imgproc/region.hpp"

namespace pc::imgproc {

enum class MatchMethod { CorrCoeffNormed, CorrNormed, SqDiffNormed };

struct MatchOptions {
    MatchMethod method = MatchMethod::CorrCoeffNormed;
    double threshold = 0.8;               // poäng 0..1 (för SqDiffNormed: 1 − avstånd)
    int max_matches = 100;
    double max_overlap = 0.3;             // träffar som överlappar mer (IoU) slås ihop; den bästa behålls
    std::vector<double> angles{0.0};      // grader moturs
    std::vector<double> scales{1.0};
    cv::Mat template_mask;                // CV_8U, mallens storlek; bara satta pixlar jämförs
    RegionSet search_in;                  // tom = hela bilden; annars bara inom områdena
    bool gray = true;                     // jämför i gråskala (annars alla kanaler)
    // Grov-till-fin: sök först i en nedskalad bild (faktor), förfina sedan varje kandidat i full
    // upplösning. 0 = automatiskt (mallens kortaste sida ≥ 24 px i grovsteget), 1 = av.
    int pyramid = 0;
};

struct Match {
    cv::Rect box;                         // omslutande ruta i bilden
    cv::Point2d center;
    double score = 0;
    double angle = 0, scale = 1;
    std::vector<cv::Point2f> corners;     // mallens hörn i bilden (roterad/skalad)
    Region region() const;                // som område (polygonen), för vidare filtrering
};

// Alla träffar över tröskeln, bäst först.
std::vector<Match> find_template(const cv::Mat& image, const cv::Mat& templ, const MatchOptions& opt = {});
// Responskartan (poäng per läge, mallens övre vänstra hörn). CV_32F.
cv::Mat match_map(const cv::Mat& image, const cv::Mat& templ, MatchMethod method = MatchMethod::CorrCoeffNormed,
                  const cv::Mat& mask = {});
// Lokala maxima ≥ threshold, minst min_distance px isär, bäst först.
std::vector<cv::Point> peaks(const cv::Mat& response, double threshold, int min_distance, int max_n = 1000);
RegionSet to_regions(const std::vector<Match>& matches);

// ---------------------------------------------------------------- särdrag (skala/rotation/perspektiv)
struct FeatureOptions {
    int max_features = 4000;
    double ratio = 0.75;                  // Lowes kvottest
    int min_inliers = 12;
    double ransac_px = 4.0;
    int max_instances = 1;                // fler: varje hittad förekomst maskas bort och sökningen görs om
    RegionSet search_in;
};
struct FeatureMatch {
    cv::Matx33d H;                        // mall -> bild
    std::vector<cv::Point2f> corners;     // mallens hörn i bilden
    int inliers = 0, matches = 0;
    double score = 0;                     // inliers / matches
    Region region() const;
};
std::vector<FeatureMatch> find_features(const cv::Mat& image, const cv::Mat& templ, const FeatureOptions& opt = {});

}  // namespace pc::imgproc
