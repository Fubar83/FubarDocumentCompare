// Originalet analyserat en gång: rendering, dekorfärger, lager, objekt, tabeller, punktskrift, masker.
#pragma once

#include <map>
#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "printcmp/artwork/braille.hpp"
#include "printcmp/artwork/masks.hpp"
#include "printcmp/artwork/objmap.hpp"
#include "printcmp/artwork/structure.hpp"
#include "printcmp/barcode/barcode.hpp"
#include "printcmp/pdf/fonts.hpp"
#include "printcmp/pdf/layers.hpp"
#include "printcmp/pdf/spots.hpp"

namespace pc::compare {

struct ReferenceOptions {
    int page = 1;                         // 1-baserad
    std::string box = "trim";
    double dpi = 600;
    std::string profile = "label_standard";
    json tolerance_overrides = json::object();
    std::vector<std::string> include_layers, exclude_layers, exclude_spots;
    pdf::SpotRules extra_spot_rules;      // utöver standardreglerna (stanslinje, lack, punktskrift)
    std::vector<artwork::UserMask> masks;
    std::shared_ptr<const pdf::FontSet> fonts;   // typsnitt som inte är inbäddade (original och PDF-prov); null = systemets
};

struct ReferenceModel {
    std::string path;
    pdf::Bytes original, filtered;
    ReferenceOptions opt;
    json tol;
    pdf::SpotRules rules;
    pdf::SpotAnalysis spots;
    pdf::Rendered ref;
    double dpi = 0, ppm = 0;
    std::vector<barcode::Code> codes;
    std::vector<artwork::Obj> objs;
    std::vector<artwork::Table> tables;
    std::vector<artwork::BrailleLine> braille;
    artwork::Masks masks;
    cv::Mat text_mask;
    pdf::LayerInfo layers;
    std::map<std::string, bool> layer_decision;
    json info() const;     // "reference"-delen av result.json
};

std::shared_ptr<const ReferenceModel> analyse_reference(const pdf::Bytes& pdf, const std::string& path,
                                                        const ReferenceOptions& opt, const JobContext& ctx = {});

}  // namespace pc::compare
