// Allvarsskalan för fynd: Info < Minor < Major < Critical.
#pragma once

#include <string>
#include <vector>

namespace pc::scan {

extern const std::vector<std::string> SEV;   // Info, Minor, Major, Critical
int sev_index(const std::string& s);         // okänd = 0 (Info)

}  // namespace pc::scan
