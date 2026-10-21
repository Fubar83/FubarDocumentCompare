// Toleransprofiler. Startvärden; kalibreras mot testmängden.
#include "printcmp/compare/profiles.hpp"

namespace pc::compare {

const char* VERSION = "0.2.0";

static const json& profiles() {
    static const json P = json::parse(R"({
  "label_standard": {
    "min_blob_mm2": 0.04, "struct_dl": 20.0, "struct_dl_strong": 35.0, "struct_dl_text": 15.0,
    "min_text_blob_mm2": 0.015, "text_small_mm": 2.6, "ssim": 0.35, "edge_radius_mm": 0.15,
    "delta_e": 8.0, "min_color_blob_mm2": 2.0, "color_window_mm": 1.5, "local_cell_mm": 6.0,
    "max_local_shift_mm": 0.6, "fold_band_mm": 1.6, "dieline_margin_mm": 0.6, "merge_mm": 0.5,
    "descreen": true, "normalize": true, "illumination_flatten": false, "local_registration": true,
    "fold_detection": true
  },
  "label_strict": {"extends": "label_standard",
    "min_blob_mm2": 0.02, "struct_dl": 15.0, "delta_e": 4.0, "edge_radius_mm": 0.10},
  "leaflet": {"extends": "label_standard",
    "min_blob_mm2": 0.10, "struct_dl": 20.0, "delta_e": 6.0, "edge_radius_mm": 0.20, "max_local_shift_mm": 1.0},
  "pdf_vs_pdf": {"extends": "label_standard",
    "min_blob_mm2": 0.005, "min_text_blob_mm2": 0.005, "struct_dl": 8.0, "struct_dl_text": 8.0, "struct_dl_strong": 20.0,
    "ssim": 0.9, "edge_radius_mm": 0.03, "delta_e": 1.5, "min_color_blob_mm2": 0.05, "color_window_mm": 0.6,
    "descreen": false, "normalize": false, "local_registration": false, "fold_detection": false}
})");
    return P;
}

std::vector<std::string> profile_names() {
    std::vector<std::string> v;
    for (auto& [k, _] : profiles().items()) v.push_back(k);
    return v;
}

json resolve_profile(const std::string& name, const json& overrides) {
    if (!profiles().contains(name)) throw Error(ErrorCode::InvalidArgument, "okänd toleransprofil: " + name);
    std::vector<json> chain;
    json p = profiles()[name];
    while (p.contains("extends")) {
        std::string parent = p["extends"];
        p.erase("extends");
        chain.push_back(p);
        p = profiles()[parent];
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) p.update(*it);
    if (overrides.is_object())
        for (auto& [k, v] : overrides.items()) {
            if (!p.contains(k)) throw Error(ErrorCode::InvalidArgument, "okänd toleransparameter: " + k);
            p[k] = v;
        }
    return p;
}

}  // namespace pc::compare
