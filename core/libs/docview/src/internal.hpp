// Privat för pc_docview: dokument och varianter ur cachen.
#pragma once

#include <memory>

#include "printcmp/docview/docview.hpp"
#include "printcmp/pdf/spots.hpp"

namespace pc::docview::detail {

// Tolkat dokument (nyckel: SHA-256).
std::shared_ptr<const pdf::Document> document(Cache& cache, const Pdf& pdf);
// Varianten för en sida: lager och dekorfärger borttagna ur sidans innehåll. Tom variant = originalet.
struct VariantPdf {
    std::shared_ptr<const Bytes> bytes;
    std::shared_ptr<const pdf::Document> doc;
};
VariantPdf variant(Cache& cache, const Pdf& pdf, int page_index, const Variant& v);
// Kontrollerar 1-baserad sida och ger 0-baserat index.
int page_index(const pdf::Document& doc, int page);
// Dekorfärgens roll enligt standardreglerna (dieline, varnish, braille) eller tom.
std::string spot_role(const std::string& name);

}  // namespace pc::docview::detail
