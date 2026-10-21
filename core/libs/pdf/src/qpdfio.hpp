// Privat för pc_pdf: läsa och skriva PDF med qpdf (för filerna som skriver om PDF:er).
#pragma once

#include <qpdf/QPDF.hh>

#include <memory>

#include "printcmp/pdf/document.hpp"

namespace pc::pdf::detail {

std::shared_ptr<QPDF> qpdf_load(const Bytes& data);
Bytes qpdf_save(QPDF& q);

}  // namespace pc::pdf::detail
