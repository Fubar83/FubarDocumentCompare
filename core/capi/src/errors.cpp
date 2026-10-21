// Fel, versioner och översättningen mellan kärnans typer och C-API:ts uppräkningar.
#include "internal.hpp"

namespace pc::capi {

thread_local std::string g_last_error;



fdc_status set_error(fdc_status s, const std::string& msg) {
    g_last_error = msg;
    return s;
}

fdc_status from_code(ErrorCode c) {
    switch (c) {
        case ErrorCode::InvalidArgument: return FDC_ERR_INVALID_ARGUMENT;
        case ErrorCode::FileNotFound: return FDC_ERR_FILE_NOT_FOUND;
        case ErrorCode::FileFormat: return FDC_ERR_FILE_FORMAT;
        case ErrorCode::PdfError: return FDC_ERR_PDF;
        case ErrorCode::Cancelled: return FDC_ERR_CANCELLED;
        case ErrorCode::NotFound: return FDC_ERR_NOT_FOUND;
        default: return FDC_ERR_INTERNAL;
    }
}


std::vector<std::string> split_list(const char* s) {
    std::vector<std::string> out;
    if (!s) return out;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == ';') {
            if (!text::strip(cur).empty()) out.push_back(text::strip(cur));
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!text::strip(cur).empty()) out.push_back(text::strip(cur));
    return out;
}

const char* profile_name(fdc_profile p) {
    switch (p) {
        case FDC_PROFILE_LABEL_STRICT: return "label_strict";
        case FDC_PROFILE_LEAFLET: return "leaflet";
        case FDC_PROFILE_PDF: return "pdf_vs_pdf";
        default: return "label_standard";
    }
}

fdc_kind kind_of(const std::string& k) {
    static const std::map<std::string, fdc_kind> M = {
        {"Missing", FDC_KIND_MISSING}, {"Added", FDC_KIND_ADDED}, {"Changed", FDC_KIND_CHANGED}, {"Moved", FDC_KIND_MOVED},
        {"Color", FDC_KIND_COLOR}, {"Shape", FDC_KIND_SHAPE}, {"Barcode", FDC_KIND_BARCODE}, {"Braille", FDC_KIND_BRAILLE},
        {"Uncertain", FDC_KIND_UNCERTAIN}, {"Style", FDC_KIND_STYLE}, {"StyleSystematic", FDC_KIND_STYLE_SYSTEMATIC},
        {"PageNumber", FDC_KIND_PAGE_NUMBER}};
    auto it = M.find(k);
    return it == M.end() ? FDC_KIND_OTHER : it->second;
}

fdc_severity sev_of(const std::string& s) {
    return s == "Critical" ? FDC_SEV_CRITICAL : s == "Major" ? FDC_SEV_MAJOR : s == "Minor" ? FDC_SEV_MINOR : FDC_SEV_INFO;
}
fdc_certainty cert_of(const std::string& s) {
    return s == "confirmed" ? FDC_CERT_CONFIRMED : s == "high" ? FDC_CERT_HIGH : s == "probable" ? FDC_CERT_PROBABLE : FDC_CERT_POSSIBLE;
}

std::string str_of(const json& v) { return v.is_string() ? v.get<std::string>() : v.is_null() ? "" : v.dump(); }


}  // namespace pc::capi

extern "C" {

FDC_API uint32_t fdc_api_version(void) { return FDC_API_VERSION; }
FDC_API const char* fdc_version_string(void) { return compare::VERSION; }
FDC_API const char* fdc_last_error(void) { return g_last_error.c_str(); }


}  // extern "C"
