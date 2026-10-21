// PDF:er identifierade av innehållet, varianter och dokument i cachen.
#include <algorithm>

#include "internal.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/pdf/layers.hpp"

namespace pc::docview {

Pdf Pdf::from_bytes(Bytes data) {
    Pdf p;
    p.sha256 = sha256_hex(data.data(), data.size());
    p.bytes = std::make_shared<const Bytes>(std::move(data));
    return p;
}

Pdf remember(Cache& cache, Bytes data) {
    Pdf p = Pdf::from_bytes(std::move(data));
    p.bytes = cache.put<Bytes>("pdf:" + p.sha256, p.bytes, p.bytes->size());
    return p;
}

std::optional<Pdf> recall(Cache& cache, const std::string& sha256) {
    auto b = cache.peek<Bytes>("pdf:" + sha256);
    if (!b) return std::nullopt;
    return Pdf{sha256, b};
}

bool Variant::empty() const {
    return include_layers.empty() && exclude_layers.empty() && exclude_spots.empty() && !hide_nonprinting && !fonts;
}

std::string Variant::key() const {
    std::string k;
    for (auto* list : {&include_layers, &exclude_layers, &exclude_spots}) {
        for (auto& s : *list) k += s + '\x1f';
        k += '\x1e';
    }
    k += hide_nonprinting ? '1' : '0';
    if (fonts) k += std::string(1, '\x1e') + fonts->hash();
    return k;
}

namespace detail {

std::shared_ptr<const pdf::Document> document(Cache& cache, const Pdf& pdf) {
    if (!pdf.bytes) throw Error(ErrorCode::InvalidArgument, "PDF:en saknar innehåll");
    return cache.get<pdf::Document>("doc:" + pdf.sha256, [&] {
        auto d = std::make_shared<const pdf::Document>(pdf.bytes);
        return std::pair{std::shared_ptr<const pdf::Document>(d), pdf.bytes->size() * 2 + 65536};
    });
}

int page_index(const pdf::Document& doc, int page) {
    int n = doc.page_count();
    if (page < 1 || page > n)
        throw Error(ErrorCode::InvalidArgument, "sidan " + std::to_string(page) + " finns inte (1–" + std::to_string(n) + ")");
    return page - 1;
}

VariantPdf variant(Cache& cache, const Pdf& pdf, int idx, const Variant& v) {
    if (v.empty()) return {pdf.bytes, document(cache, pdf)};
    std::string key = "variant:" + pdf.sha256 + ":" + std::to_string(idx) + ":" + v.key();
    // Variantens bytes håller typsnittsuppsättningen vid liv: PDFium kan hämta fontfilerna när som
    // helst så länge dokumentet finns i cachen.
    struct Owned {
        Bytes bytes;
        std::shared_ptr<const pdf::FontSet> fonts;
    };
    auto owned = cache.get<Owned>(key, [&] {
        Bytes b = pdf::filter_layers(*pdf.bytes, idx, v.include_layers, v.exclude_layers, nullptr);
        pdf::SpotRules rules = v.hide_nonprinting ? pdf::default_spot_rules() : pdf::SpotRules{};
        for (auto& r : pdf::excluded_spot_rule(v.exclude_spots)) rules.push_back(r);
        if (!rules.empty()) b = pdf::analyze_spots(b, idx, rules).sanitized_pdf;
        if (v.fonts) b = pdf::apply_fonts(b, *v.fonts);
        auto p = std::make_shared<const Owned>(Owned{std::move(b), v.fonts});
        return std::pair{std::shared_ptr<const Owned>(p), p->bytes.size()};
    });
    std::shared_ptr<const Bytes> bytes(owned, &owned->bytes);
    auto doc = cache.get<pdf::Document>("doc:" + key, [&] {
        auto d = std::make_shared<const pdf::Document>(bytes);
        return std::pair{std::shared_ptr<const pdf::Document>(d), bytes->size() * 2 + 65536};
    });
    return {bytes, doc};
}

std::string spot_role(const std::string& name) {
    static const auto rules = [] {
        std::vector<std::pair<std::string, Regex>> r;
        for (auto& [role, rx] : pdf::default_spot_rules()) r.emplace_back(role, Regex(rx));
        return r;
    }();
    for (auto& [role, rx] : rules)
        if (rx.search(name)) return role;
    return "";
}

}  // namespace detail

}  // namespace pc::docview
