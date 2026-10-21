// PDFiums fontuppslag (FPDF_SYSFONTINFO): tillhandahållna fontfiler efter token, annars
// operativsystemets uppslag oförändrat.
#include <fpdf_sysfontinfo.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "fontreg.hpp"

namespace pc::pdf::detail {

namespace {

constexpr size_t TOKEN_HEX = 20;

std::mutex g_mutex;
std::unordered_map<std::string, std::weak_ptr<const FontFile>> g_fonts;   // token-hex -> fontfil
std::atomic<uint64_t> g_loads{0};

struct Handle {
    std::shared_ptr<const FontFile> font;
};
std::unordered_set<void*> g_handles;   // våra handtag (resten tillhör systemuppslaget)

FPDF_SYSFONTINFO* g_system = nullptr;

bool ours(void* h) {
    std::lock_guard<std::mutex> l(g_mutex);
    return g_handles.count(h) > 0;
}

// "…FDCF<hex>_…" -> fontfilen, om tokenet finns någonstans i namnet (PDFium kan ha ändrat resten).
std::shared_ptr<const FontFile> lookup(const char* face) {
    if (!face) return nullptr;
    std::string f = face;
    for (auto& c : f) c = (char)std::toupper((unsigned char)c);
    size_t at = f.find(SUPPLIED_PREFIX);
    if (at == std::string::npos || f.size() < at + 4 + TOKEN_HEX) return nullptr;
    std::string hex = f.substr(at + 4, TOKEN_HEX);
    for (auto& c : hex) c = (char)std::tolower((unsigned char)c);
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_fonts.find(hex);
    return it == g_fonts.end() ? nullptr : it->second.lock();
}

bool builtin_only(const char* face) {
    if (!face) return false;
    std::string f = face;
    for (auto& c : f) c = (char)std::toupper((unsigned char)c);
    return f.find("FDCN") != std::string::npos;
}

// Tabellen tag ur sfnt-filen (0 = hela filen).
std::pair<const unsigned char*, size_t> table_of(const Bytes& d, unsigned int tag) {
    if (tag == 0) return {d.data(), d.size()};
    if (d.size() < 12) return {nullptr, 0};
    auto u16 = [&](size_t o) { return size_t(d[o] << 8 | d[o + 1]); };
    auto u32 = [&](size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; };
    size_t n = u16(4);
    for (size_t i = 0; i < n && 12 + i * 16 + 16 <= d.size(); ++i) {
        size_t rec = 12 + i * 16;
        if (u32(rec) != tag) continue;
        size_t off = u32(rec + 8), len = u32(rec + 12);
        if (off > d.size() || len > d.size() - off) return {nullptr, 0};
        return {d.data() + off, len};
    }
    return {nullptr, 0};
}

void Release(FPDF_SYSFONTINFO*) {}

void EnumFonts(FPDF_SYSFONTINFO*, void* mapper) {
    if (g_system && g_system->EnumFonts) g_system->EnumFonts(g_system, mapper);
}

void* MapFont(FPDF_SYSFONTINFO*, int weight, FPDF_BOOL italic, int charset, int pitch, const char* face, FPDF_BOOL* exact) {
    if (auto f = lookup(face)) {
        auto h = new Handle{f};
        std::lock_guard<std::mutex> l(g_mutex);
        g_handles.insert(h);
        if (exact) *exact = 1;
        return h;
    }
    if (builtin_only(face)) return nullptr;   // PDFium tar sina inbyggda ersättningar
    return g_system && g_system->MapFont ? g_system->MapFont(g_system, weight, italic, charset, pitch, face, exact) : nullptr;
}

void* GetFont(FPDF_SYSFONTINFO*, const char* face) {
    if (lookup(face)) return MapFont(nullptr, 400, 0, FXFONT_DEFAULT_CHARSET, 0, face, nullptr);
    if (builtin_only(face)) return nullptr;
    return g_system && g_system->GetFont ? g_system->GetFont(g_system, face) : nullptr;
}

unsigned long GetFontData(FPDF_SYSFONTINFO*, void* h, unsigned int table, unsigned char* buf, unsigned long size) {
    if (ours(h)) {
        auto* fh = static_cast<Handle*>(h);
        auto [p, n] = table_of(*fh->font->data, table);
        if (!p) return 0;
        if (table == 0) ++g_loads;
        if (buf && size >= n) std::memcpy(buf, p, n);
        return (unsigned long)n;
    }
    return g_system && g_system->GetFontData ? g_system->GetFontData(g_system, h, table, buf, size) : 0;
}

unsigned long GetFaceName(FPDF_SYSFONTINFO*, void* h, char* buf, unsigned long size) {
    if (ours(h)) {
        // unikt per fil: PDFium cachar laddade typsnitt efter namnet (och vikt/kursiv), och två filer
        // med samma familj (t.ex. två kunders "Acme Sans") får inte dela post
        std::string name = std::string(SUPPLIED_PREFIX) + static_cast<Handle*>(h)->font->sha256.substr(0, TOKEN_HEX) + "_" +
                           static_cast<Handle*>(h)->font->family;
        unsigned long n = (unsigned long)name.size() + 1;
        if (buf && size >= n) std::memcpy(buf, name.c_str(), n);
        return n;
    }
    return g_system && g_system->GetFaceName ? g_system->GetFaceName(g_system, h, buf, size) : 0;
}

int GetFontCharset(FPDF_SYSFONTINFO*, void* h) {
    if (ours(h)) return FXFONT_ANSI_CHARSET;
    return g_system && g_system->GetFontCharset ? g_system->GetFontCharset(g_system, h) : FXFONT_ANSI_CHARSET;
}

void DeleteFont(FPDF_SYSFONTINFO*, void* h) {
    {
        std::lock_guard<std::mutex> l(g_mutex);
        if (g_handles.erase(h)) {
            delete static_cast<Handle*>(h);
            return;
        }
    }
    if (g_system && g_system->DeleteFont) g_system->DeleteFont(g_system, h);
}

FPDF_SYSFONTINFO g_info{1, Release, EnumFonts, MapFont, GetFont, GetFontData, GetFaceName, GetFontCharset, DeleteFont};

}  // namespace

std::string register_font(const std::shared_ptr<const FontFile>& font) {
    std::string hex = font->sha256.substr(0, TOKEN_HEX);
    std::lock_guard<std::mutex> l(g_mutex);
    auto& slot = g_fonts[hex];
    if (slot.expired()) slot = font;
    // städa bort fontfiler som ingen uppsättning håller längre
    if (g_fonts.size() > 256)
        for (auto it = g_fonts.begin(); it != g_fonts.end();) it = it->second.expired() ? g_fonts.erase(it) : std::next(it);
    return std::string(SUPPLIED_PREFIX) + hex;
}

void install_font_info() {
    g_system = FPDF_GetDefaultSystemFontInfo();
    FPDF_SetSystemFontInfo(&g_info);
}

}  // namespace pc::pdf::detail

namespace pc::pdf {

uint64_t supplied_font_loads() { return detail::g_loads.load(); }

}  // namespace pc::pdf
