// Felsökning: avkoda streckkoder ur en PDF med kärnans pc_barcode (zxing-cpp).
#include <cstdio>

#include "printcmp/compare/datacheck.hpp"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    for (double dpi : {300.0, 400.0, 600.0}) {
        auto codes = pc::compare::codes_from_pdf(argv[1], 1, dpi);
        std::printf("dpi %.0f: %zu koder\n", dpi, codes.size());
        for (auto& c : codes) std::printf("  %s %s\n", c["symbology"].get<std::string>().c_str(), c["data"].get<std::string>().c_str());
    }
    return 0;
}
