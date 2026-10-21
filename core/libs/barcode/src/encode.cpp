#include "printcmp/barcode/encode.hpp"

#include <ZXing/BitMatrix.h>
#include <ZXing/MultiFormatWriter.h>

namespace pc::barcode {

namespace {

Modules to_modules(const ZXing::BitMatrix& m) {
    Modules out{m.width(), m.height(), std::vector<uint8_t>(size_t(m.width()) * m.height())};
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) out.dark[size_t(y) * m.width() + x] = m.get(x, y) ? 1 : 0;
    return out;
}

}  // namespace

Modules encode_ean13(const std::string& digits) {
    return to_modules(ZXing::MultiFormatWriter(ZXing::BarcodeFormat::EAN13).setMargin(0).encode(digits.substr(0, 12), 95, 1));
}

Modules encode_qr(const std::string& data, char ecc) {
    // zxing-cpp: nivå 0–8, (nivå - 1) / 2 -> L, M, Q, H
    int level = ecc == 'L' ? 1 : ecc == 'Q' ? 5 : ecc == 'H' ? 7 : 3;
    return to_modules(ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setEccLevel(level).setMargin(0).encode(data, 0, 0));
}

}  // namespace pc::barcode
