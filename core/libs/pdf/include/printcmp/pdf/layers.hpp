// PDF-lager (Optional Content): lista och filtrera.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "printcmp/pdf/document.hpp"
#include "printcmp/pdf/spots.hpp"

namespace pc::pdf {

struct Ocg {
    std::string name;
    bool default_on = true;
    bool used_on_page = false;
    // ISO 19593-1 bearbetningssteg (GTS_Metadata): t.ex. Structural/Cutting. Tomma om lagret är vanligt
    // innehåll. Bearbetningsstegen trycks inte; de beskriver stansning, mått, positioner, punktskrift …
    std::string step_group, step_type;
};
struct LayerInfo {
    std::vector<Ocg> ocgs;
    std::vector<std::string> spots;
};
LayerInfo list_layers(const Bytes& pdf, int page);
// Alla sidor på en gång (PDF:en tolkas en gång).
std::vector<LayerInfo> list_layers(const Bytes& pdf);
// Tar bort uteslutna lager ur innehållsströmmarna. Returnerar ny PDF och {lager: med?}.
Bytes filter_layers(const Bytes& pdf, int page, const std::vector<std::string>& include,
                    const std::vector<std::string>& exclude, std::map<std::string, bool>* decision);
// Extra regel för pdfspots: uteslutna dekorfärger tas bort ur renderingen.
SpotRules excluded_spot_rule(const std::vector<std::string>& exclude_spots);

}  // namespace pc::pdf
