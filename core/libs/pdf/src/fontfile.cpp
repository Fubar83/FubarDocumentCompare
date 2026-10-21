// Fontfiler (TrueType och OpenType/CFF): namn, mått och teckenkarta ur sfnt-tabellerna, och
// uppsättningar av fontfiler med namnuppslag.
#include <algorithm>
#include <cctype>
#include <map>

#include "printcmp/core/text.hpp"
#include "printcmp/pdf/fonts.hpp"

namespace pc::pdf {

namespace {

struct Reader {
    const unsigned char* p;
    size_t n;
    void need(size_t off, size_t len) const {
        if (off > n || len > n - off) throw Error(ErrorCode::FileFormat, "fontfilen är trasig (tabell utanför filen)");
    }
    uint16_t u16(size_t off) const {
        need(off, 2);
        return uint16_t(p[off] << 8 | p[off + 1]);
    }
    int16_t s16(size_t off) const { return int16_t(u16(off)); }
    uint32_t u32(size_t off) const {
        need(off, 4);
        return uint32_t(p[off]) << 24 | uint32_t(p[off + 1]) << 16 | uint32_t(p[off + 2]) << 8 | p[off + 3];
    }
};

struct Table {
    uint32_t off = 0, len = 0;
};

std::string utf16be(const Reader& r, size_t off, size_t len) {
    std::vector<char32_t> cps;
    for (size_t i = 0; i + 1 < len; i += 2) {
        char32_t c = r.u16(off + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
            char32_t lo = r.u16(off + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        cps.push_back(c);
    }
    return text::encode(cps);
}

void read_names(const Reader& r, const Table& t, FontFile& f) {
    uint16_t count = r.u16(t.off + 2), strings = r.u16(t.off + 4);
    // bästa post per nameID: Windows/Unicode engelska först, sedan annan Windows, sedan Mac Roman
    std::map<int, std::pair<int, std::string>> best;
    for (uint16_t i = 0; i < count; ++i) {
        size_t rec = t.off + 6 + size_t(i) * 12;
        uint16_t platform = r.u16(rec), encoding = r.u16(rec + 2), language = r.u16(rec + 4);
        uint16_t id = r.u16(rec + 6), len = r.u16(rec + 8), off = r.u16(rec + 10);
        if (id != 1 && id != 2 && id != 4 && id != 6 && id != 16) continue;
        size_t at = size_t(t.off) + strings + off;
        r.need(at, len);
        int score;
        std::string s;
        if (platform == 3 && (encoding == 1 || encoding == 10)) {
            score = language == 0x409 ? 3 : 2;
            s = utf16be(r, at, len);
        } else if (platform == 0) {
            score = 2;
            s = utf16be(r, at, len);
        } else if (platform == 1 && encoding == 0) {
            score = 1;
            s.assign(reinterpret_cast<const char*>(r.p + at), len);
        } else {
            continue;
        }
        auto it = best.find(id);
        if (it == best.end() || it->second.first < score) best[id] = {score, s};
    }
    auto get = [&](int id) { return best.count(id) ? best[id].second : std::string(); };
    f.postscript_name = get(6);
    f.family = !get(16).empty() ? get(16) : get(1);
    f.subfamily = get(2);
    f.full_name = get(4);
}

void read_cmap(const Reader& r, const Table& t, FontFile& f) {
    uint16_t n = r.u16(t.off + 2);
    size_t chosen = 0;
    int chosen_score = -1;
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = t.off + 4 + size_t(i) * 8;
        uint16_t platform = r.u16(rec), encoding = r.u16(rec + 2);
        size_t sub = t.off + r.u32(rec + 4);
        uint16_t format = r.u16(sub);
        int score = -1;
        if (format == 12 && (platform == 3 || platform == 0)) score = 3;
        else if (format == 4 && ((platform == 3 && encoding == 1) || platform == 0)) score = 2;
        else if (format == 4 && platform == 3 && encoding == 0) score = 1;   // symbolfont
        if (score > chosen_score) chosen_score = score, chosen = sub;
    }
    if (chosen_score < 0) return;
    uint16_t format = r.u16(chosen);
    if (format == 4) {
        uint16_t segs = r.u16(chosen + 6) / 2;
        size_t ends = chosen + 14, starts = ends + segs * 2 + 2, deltas = starts + segs * 2, ranges = deltas + segs * 2;
        for (uint16_t s = 0; s < segs; ++s) {
            uint16_t end = r.u16(ends + s * 2), start = r.u16(starts + s * 2);
            int16_t delta = r.s16(deltas + s * 2);
            uint16_t range = r.u16(ranges + s * 2);
            for (uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
                uint16_t g;
                if (range == 0) {
                    g = uint16_t(c + delta);
                } else {
                    size_t at = ranges + s * 2 + range + (c - start) * 2;
                    g = r.u16(at);
                    if (g) g = uint16_t(g + delta);
                }
                if (g) f.cmap[c] = g;
            }
        }
    } else if (format == 12) {
        uint32_t groups = r.u32(chosen + 12);
        for (uint32_t i = 0; i < groups; ++i) {
            size_t at = chosen + 16 + size_t(i) * 12;
            uint32_t start = r.u32(at), end = r.u32(at + 4), g = r.u32(at + 8);
            if (end < start || end - start > 0x10FFFF) continue;
            for (uint32_t c = start; c <= end; ++c)
                if (g + (c - start) <= 0xFFFF) f.cmap[c] = uint16_t(g + (c - start));
        }
    }
}

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return o;
}

// Namnet utan skiljetecken och versaler: "Acme Sans-Regular" och "AcmeSans,Regular" blir lika.
std::string loose(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c) || c >= 0x80) o += (char)std::tolower(c);
    return o;
}

std::string strip_subset(const std::string& n) {
    if (n.size() > 7 && n[6] == '+') {
        bool tag = true;
        for (int i = 0; i < 6; ++i) tag = tag && n[i] >= 'A' && n[i] <= 'Z';
        if (tag) return n.substr(7);
    }
    return n;
}

}  // namespace

uint16_t FontFile::glyph(char32_t cp) const {
    auto it = cmap.find(cp);
    return it == cmap.end() ? 0 : it->second;
}

int FontFile::advance(uint16_t gid) const {
    if (advances.empty()) return 0;
    return gid < advances.size() ? advances[gid] : advances.back();
}

std::shared_ptr<const FontFile> parse_font(Bytes data, std::vector<std::string> aliases) {
    Reader r{data.data(), data.size()};
    if (data.size() < 12) throw Error(ErrorCode::FileFormat, "inte en fontfil");
    uint32_t version = r.u32(0);
    if (version == 0x74746366) throw Error(ErrorCode::FileFormat, "fontsamlingar (TTC) stöds inte; dela upp dem i TTF/OTF");
    if (version != 0x00010000 && version != 0x4F54544F && version != 0x74727565)
        throw Error(ErrorCode::FileFormat, "inte en TrueType- eller OpenType-fil");
    std::map<std::string, Table> tables;
    uint16_t n = r.u16(4);
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = 12 + size_t(i) * 16;
        r.need(rec, 16);
        std::string tag(reinterpret_cast<const char*>(data.data() + rec), 4);
        Table t{r.u32(rec + 8), r.u32(rec + 12)};
        r.need(t.off, t.len);
        tables[tag] = t;
    }
    for (const char* req : {"head", "hhea", "hmtx", "cmap", "maxp"})
        if (!tables.count(req)) throw Error(ErrorCode::FileFormat, std::string("fontfilen saknar tabellen ") + req);

    auto f = std::make_shared<FontFile>();
    const auto& head = tables["head"];
    f->units_per_em = std::max<int>(16, r.u16(head.off + 18));
    f->italic = (r.u16(head.off + 44) & 2) != 0;
    if (tables.count("OS/2")) {
        const auto& os2 = tables["OS/2"];
        f->weight = r.u16(os2.off + 4);
        f->fs_type = r.u16(os2.off + 8);
    }
    if (tables.count("name")) read_names(r, tables["name"], *f);
    int glyphs = r.u16(tables["maxp"].off + 4);
    int metrics = std::min<int>(r.u16(tables["hhea"].off + 34), glyphs);
    f->advances.reserve(metrics);
    for (int i = 0; i < metrics; ++i) f->advances.push_back(r.u16(tables["hmtx"].off + size_t(i) * 4));
    read_cmap(r, tables["cmap"], *f);
    if (f->postscript_name.empty()) f->postscript_name = f->full_name.empty() ? f->family : f->full_name;
    if (f->postscript_name.empty()) throw Error(ErrorCode::FileFormat, "fontfilen har inget namn");
    f->aliases = std::move(aliases);
    f->sha256 = sha256_hex(data.data(), data.size());
    f->data = std::make_shared<const Bytes>(std::move(data));
    return f;
}

FontSet::FontSet(std::vector<std::shared_ptr<const FontFile>> fonts, FontFallback fallback)
    : fonts_(std::move(fonts)), fallback_(fallback) {
    std::vector<std::string> keys;
    for (auto& f : fonts_) {
        std::string k = f->sha256;
        for (auto& a : f->aliases) k += "|" + a;
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    std::string all = fallback_ == FontFallback::System ? "system" : "builtin";
    for (auto& k : keys) all += "\n" + k;
    hash_ = sha256_hex(all);
    // senare filer vinner inte över tidigare: första träffen gäller (ordningen är anroparens prioritet)
    for (auto& f : fonts_) {
        std::vector<std::string> names = f->aliases;
        names.push_back(f->postscript_name);
        names.push_back(f->full_name);
        for (auto& n : names) {
            if (n.empty()) continue;
            exact_.emplace(lower(n), f);
            loose_.emplace(loose(n), f);
        }
    }
}

std::shared_ptr<const FontFile> FontSet::match(const std::string& pdf_font_name) const {
    std::string n = strip_subset(pdf_font_name);
    if (auto it = exact_.find(lower(n)); it != exact_.end()) return it->second;
    if (auto it = loose_.find(loose(n)); it != loose_.end()) return it->second;
    return nullptr;
}

}  // namespace pc::pdf
