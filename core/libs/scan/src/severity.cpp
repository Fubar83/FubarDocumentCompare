#include "printcmp/scan/severity.hpp"

#include <algorithm>

namespace pc::scan {

const std::vector<std::string> SEV = {"Info", "Minor", "Major", "Critical"};
int sev_index(const std::string& s) {
    auto it = std::find(SEV.begin(), SEV.end(), s);
    return it == SEV.end() ? 0 : (int)(it - SEV.begin());
}

}  // namespace pc::scan
