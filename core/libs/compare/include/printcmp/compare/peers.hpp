// Etiketterna på ett scannat ark jämförda med varandra, utan original: hitta etiketterna (även snedda
// och upp och ned), välj den mest typiska som förlaga, passa in de andra mot den och jämför var och en
// med medianen av alla (konsensus). Ett fel som bara finns på en etikett syns; ett fel som finns på
// alla (systematiskt) kan inte upptäckas utan original.
#pragma once

#include <array>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"

namespace pc::compare {

struct PeerOptions {
    std::optional<int> expected_count;   // antal etiketter om det är känt (styr detekteringen)
    double min_label_mm2 = 300;          // mindre föremål är inte etiketter
    std::string profile = "label_standard";
    json tolerance_overrides = json::object();
    bool local_registration = true;      // lokal passning (veck, buktning)
    int seed = 1;
};

struct PeerFinding {
    int label = 0;                        // etikettens index
    std::string kind;                     // Missing (ljusare), Added (mörkare), Shape, Color (ΔE00), Uncertain (veckzon)
    Box bbox_px;                          // i förlagans px (etikettens koordinater)
    double x_mm = 0, y_mm = 0, w_mm = 0, h_mm = 0, area_mm2 = 0;
    double delta_l = 0;                   // största ljushetsskillnad mot konsensus
    std::string detail;
};

struct PeerLabel {
    int index = 0, row = 0, col = 0;
    double angle_deg = 0;                 // vridning på arket, moturs 0–360 (180 = upp och ned)
    std::array<Pt, 4> corners;            // på arket: övre vänster, övre höger, nedre höger, nedre vänster (efter passning)
    double similarity = 0;                // korrelation med konsensus, 0..1
    double registration_rms_px = 0;
    bool master = false;                  // förlagan som de andra passas in mot
    std::vector<PeerFinding> findings;
};

struct PeerResult {
    double dpi = 0;
    double sheet_angle_deg = 0;           // −45..45°
    cv::Size label_size;                  // förlagans storlek i px
    std::vector<PeerLabel> labels;        // i läsordning (rad för rad)
    cv::Mat consensus_rgb;                // medianen av alla inpassade etiketter
    std::vector<cv::Mat> aligned_rgb;     // varje etikett inpassad mot förlagan (samma ordning som labels)
    std::vector<std::string> notes;       // t.ex. "bara två etiketter: skillnaderna kan inte tillskrivas en av dem"
    json to_json() const;
};

// Bara detekteringen: etiketternas hörn (övre vänster först) och vridning modulo 90°, utan inbördes ordning.
struct DetectedLabel {
    std::array<Pt, 4> corners;
    double angle_deg = 0;
};
std::vector<DetectedLabel> detect_labels(const cv::Mat& sheet_rgb, double dpi, const PeerOptions& opt = {});

PeerResult compare_peers(const cv::Mat& sheet_rgb, double dpi, const PeerOptions& opt = {});

}  // namespace pc::compare
