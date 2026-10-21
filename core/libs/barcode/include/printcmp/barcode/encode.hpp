// Kodning av streckkoder till moduler (för testmaterial och förhandsvisning). zxing-cpp är en
// implementationsdetalj i pc_barcode; användarna ser bara modulmatrisen.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc::barcode {

struct Modules {
    int width = 0, height = 0;
    std::vector<uint8_t> dark;   // radvis, 1 = mörk modul
    bool get(int x, int y) const { return dark[size_t(y) * width + x] != 0; }
};

// EAN-13 utan tyst zon: 95 moduler på en rad. 12 siffror (kontrollsiffran räknas ut) eller 13.
Modules encode_ean13(const std::string& digits);
// QR utan tyst zon. ecc: 'L', 'M', 'Q' eller 'H'.
Modules encode_qr(const std::string& data, char ecc = 'M');

}  // namespace pc::barcode
