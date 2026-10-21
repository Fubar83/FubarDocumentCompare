#include "printcmp/pdfwrite/truetype.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

#include "printcmp/core/core.hpp"

namespace pc::pdfwrite {

namespace {

uint16_t u16(const unsigned char* p) { return uint16_t(p[0] << 8 | p[1]); }
int16_t i16(const unsigned char* p) { return int16_t(u16(p)); }
uint32_t u32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void put16(std::vector<unsigned char>& v, size_t at, uint16_t x) {
    v[at] = x >> 8;
    v[at + 1] = x & 0xff;
}
void put32(std::vector<unsigned char>& v, size_t at, uint32_t x) {
    for (int k = 0; k < 4; ++k) v[at + k] = (x >> (24 - 8 * k)) & 0xff;
}
void app16(std::vector<unsigned char>& v, uint16_t x) {
    v.push_back(x >> 8);
    v.push_back(x & 0xff);
}
void app32(std::vector<unsigned char>& v, uint32_t x) {
    for (int k = 0; k < 4; ++k) v.push_back((x >> (24 - 8 * k)) & 0xff);
}

uint32_t checksum(const unsigned char* p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (int k = 0; k < 4; ++k) w = w << 8 | (i + k < n ? p[i + k] : 0);
        s += w;
    }
    return s;
}

}  // namespace

std::shared_ptr<const TrueType> TrueType::load(const std::string& path, const std::string& name) {
    auto t = std::make_shared<TrueType>();
    t->name_ = name;
    auto bytes = read_file_bytes(path);
    t->data_.assign(bytes.begin(), bytes.end());
    const auto* d = t->data_.data();
    if (t->data_.size() < 12) throw Error(ErrorCode::FileFormat, "ingen TrueType-fil: " + path);
    int n = u16(d + 4);
    for (int i = 0; i < n; ++i) {
        const auto* r = d + 12 + 16 * i;
        t->tables_[std::string(reinterpret_cast<const char*>(r), 4)] = {u32(r + 8), u32(r + 12)};
    }
    for (const char* need : {"head", "hhea", "hmtx", "maxp", "loca", "glyf", "cmap"})
        if (!t->tables_.count(need)) throw Error(ErrorCode::FileFormat, std::string("tabellen ") + need + " saknas: " + path);
    const auto* head = t->tab("head");
    t->upem_ = u16(head + 18);
    t->bbox_ = {i16(head + 36), i16(head + 38), i16(head + 40), i16(head + 42)};
    t->loca_long_ = i16(head + 50);
    const auto* hhea = t->tab("hhea");
    t->ascent_ = i16(hhea + 4);
    t->descent_ = i16(hhea + 6);
    t->num_hmetrics_ = u16(hhea + 34);
    t->num_glyphs_ = u16(t->tab("maxp") + 4);
    uint32_t os2len = 0;
    if (const auto* os2 = t->tab("OS/2", &os2len)) {
        t->weight_ = u16(os2 + 4);
        if (os2len >= 72) {   // typografiska mått, som reportlab
            t->ascent_ = i16(os2 + 68);
            t->descent_ = i16(os2 + 70);
        }
        if (u16(os2) >= 2 && os2len >= 90) t->cap_height_ = i16(os2 + 88);
    }
    if (!t->cap_height_) t->cap_height_ = int(t->ascent_ * 0.7);
    if (const auto* post = t->tab("post")) t->italic_angle_ = int32_t(u32(post + 4)) / 65536.0;

    // cmap: helst (3,10) format 12, annars (3,1)/(0,x) format 4
    const auto* cmap = t->tab("cmap");
    const unsigned char* best = nullptr;
    int best_rank = 0;
    for (int i = 0, nt = u16(cmap + 2); i < nt; ++i) {
        const auto* r = cmap + 4 + 8 * i;
        const auto* sub = cmap + u32(r + 4);
        int pid = u16(r), eid = u16(r + 2), fmt = u16(sub);
        int rank = (fmt == 12 && (pid == 3 || pid == 0)) ? 3 : (fmt == 4 && (pid == 3 && eid == 1)) ? 2 : (fmt == 4 && pid == 0) ? 1 : 0;
        if (rank > best_rank) best = sub, best_rank = rank;
    }
    if (!best) throw Error(ErrorCode::FileFormat, "ingen Unicode-cmap: " + path);
    if (u16(best) == 12) {
        uint32_t ng = u32(best + 12);
        for (uint32_t g = 0; g < ng; ++g) {
            const auto* r = best + 16 + 12 * g;
            uint32_t a = u32(r), b = u32(r + 4), s = u32(r + 8);
            for (uint32_t c = a; c <= b && c < 0x110000; ++c) t->cmap_[char32_t(c)] = uint16_t(s + (c - a));
        }
    } else {
        int seg = u16(best + 6) / 2;
        const auto* ends = best + 14;
        const auto* starts = ends + 2 * seg + 2;
        const auto* deltas = starts + 2 * seg;
        const auto* ranges = deltas + 2 * seg;
        for (int s = 0; s < seg; ++s) {
            uint16_t a = u16(starts + 2 * s), b = u16(ends + 2 * s), ro = u16(ranges + 2 * s);
            int16_t delta = i16(deltas + 2 * s);
            for (uint32_t c = a; c <= b && c != 0xFFFF; ++c) {
                uint16_t g;
                if (ro == 0) g = uint16_t(c + delta);
                else {
                    const auto* gp = ranges + 2 * s + ro + 2 * (c - a);
                    g = u16(gp);
                    if (g) g = uint16_t(g + delta);
                }
                if (g) t->cmap_[char32_t(c)] = g;
            }
        }
    }
    return t;
}

const unsigned char* TrueType::tab(const char* tag, uint32_t* len) const {
    auto it = tables_.find(tag);
    if (it == tables_.end()) return nullptr;
    if (len) *len = it->second.len;
    return data_.data() + it->second.off;
}

uint16_t TrueType::glyph(char32_t cp) const {
    auto it = cmap_.find(cp);
    return it == cmap_.end() ? 0 : it->second;
}

int TrueType::advance(uint16_t gid) const {
    const auto* hmtx = tab("hmtx");
    int i = std::min<int>(gid, num_hmetrics_ - 1);
    return u16(hmtx + 4 * i);
}

double TrueType::width(const std::u32string& s, double size_pt) const {
    long w = 0;
    for (char32_t c : s) w += advance(glyph(c));
    return w * size_pt / upem_;
}

std::pair<uint32_t, uint32_t> TrueType::glyph_range(uint16_t gid) const {
    const auto* loca = tab("loca");
    if (gid >= num_glyphs_) return {0, 0};
    if (loca_long_) return {u32(loca + 4 * gid), u32(loca + 4 * gid + 4)};
    return {uint32_t(u16(loca + 2 * gid)) * 2, uint32_t(u16(loca + 2 * gid + 2)) * 2};
}

// Sammansatta glyfer: flaggor för varje komponent
enum : uint16_t { ARGS_WORDS = 1, ARGS_XY = 2, HAVE_SCALE = 8, MORE = 0x20, XY_SCALE = 0x40, TWO_BY_TWO = 0x80 };

void TrueType::outline_into(uint16_t gid, const double m[6], std::vector<PathOp>& out, int depth) const {
    auto [a, b] = glyph_range(gid);
    if (b <= a || depth > 8) return;
    const auto* g = tab("glyf") + a;
    int nc = i16(g);
    auto T = [&](double x, double y, double* o) {
        o[0] = m[0] * x + m[2] * y + m[4];
        o[1] = m[1] * x + m[3] * y + m[5];
    };
    if (nc < 0) {
        const auto* p = g + 10;
        uint16_t flags;
        do {
            flags = u16(p);
            uint16_t cg = u16(p + 2);
            p += 4;
            double dx, dy;
            if (flags & ARGS_WORDS) {
                dx = i16(p), dy = i16(p + 2);
                p += 4;
            } else {
                dx = int8_t(p[0]), dy = int8_t(p[1]);
                p += 2;
            }
            if (!(flags & ARGS_XY)) dx = dy = 0;   // punktmatchning: används inte av DejaVu
            double c[4] = {1, 0, 0, 1};
            if (flags & HAVE_SCALE) {
                c[0] = c[3] = i16(p) / 16384.0;
                p += 2;
            } else if (flags & XY_SCALE) {
                c[0] = i16(p) / 16384.0, c[3] = i16(p + 2) / 16384.0;
                p += 4;
            } else if (flags & TWO_BY_TWO) {
                c[0] = i16(p) / 16384.0, c[1] = i16(p + 2) / 16384.0, c[2] = i16(p + 4) / 16384.0, c[3] = i16(p + 6) / 16384.0;
                p += 8;
            }
            double mm[6] = {m[0] * c[0] + m[2] * c[1], m[1] * c[0] + m[3] * c[1],
                            m[0] * c[2] + m[2] * c[3], m[1] * c[2] + m[3] * c[3],
                            m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5]};
            outline_into(cg, mm, out, depth + 1);
        } while (flags & MORE);
        return;
    }
    std::vector<int> ends(nc);
    for (int i = 0; i < nc; ++i) ends[i] = u16(g + 10 + 2 * i);
    int npts = nc ? ends.back() + 1 : 0;
    const auto* p = g + 10 + 2 * nc;
    p += 2 + u16(p);   // instruktioner
    std::vector<uint8_t> fl(npts);
    for (int i = 0; i < npts;) {
        uint8_t f = *p++;
        fl[i++] = f;
        if (f & 8)
            for (int r = *p++; r > 0 && i < npts; --r) fl[i++] = f;
    }
    std::vector<double> xs(npts), ys(npts);
    int v = 0;
    for (int i = 0; i < npts; ++i) {
        if (fl[i] & 2) v += (fl[i] & 16) ? *p : -int(*p), ++p;
        else if (!(fl[i] & 16)) v += i16(p), p += 2;
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; ++i) {
        if (fl[i] & 4) v += (fl[i] & 32) ? *p : -int(*p), ++p;
        else if (!(fl[i] & 32)) v += i16(p), p += 2;
        ys[i] = v;
    }
    int s0 = 0;
    for (int c = 0; c < nc; ++c) {
        int e = ends[c], n = e - s0 + 1;
        if (n <= 0) continue;
        auto on = [&](int k) { return (fl[s0 + (k % n + n) % n] & 1) != 0; };
        auto P = [&](int k) { int i = s0 + (k % n + n) % n; return std::array<double, 2>{xs[i], ys[i]}; };
        // startpunkt på kurvan
        int first = -1;
        for (int k = 0; k < n; ++k)
            if (on(k)) { first = k; break; }
        std::array<double, 2> start;
        if (first < 0) {
            auto a0 = P(0), a1 = P(1);
            start = {(a0[0] + a1[0]) / 2, (a0[1] + a1[1]) / 2};
            first = 0;   // börjar mellan punkt 0 och 1; slingan nedan går från punkt 1
        } else start = P(first);
        PathOp mv{'m'};
        T(start[0], start[1], mv.p);
        out.push_back(mv);
        auto cur = start;
        auto quad = [&](std::array<double, 2> q, std::array<double, 2> to) {
            PathOp op{'c'};
            double c1x = cur[0] + 2.0 / 3 * (q[0] - cur[0]), c1y = cur[1] + 2.0 / 3 * (q[1] - cur[1]);
            double c2x = to[0] + 2.0 / 3 * (q[0] - to[0]), c2y = to[1] + 2.0 / 3 * (q[1] - to[1]);
            T(c1x, c1y, op.p);
            T(c2x, c2y, op.p + 2);
            T(to[0], to[1], op.p + 4);
            out.push_back(op);
            cur = to;
        };
        bool all_off = !on(first);
        int k0 = all_off ? 1 : first + 1, steps = n;
        for (int s = 0; s < steps; ++s) {
            int k = k0 + s;
            bool last = s == steps - 1;
            if (on(k)) {
                auto to = (last && !all_off) ? start : P(k);
                PathOp op{'l'};
                T(to[0], to[1], op.p);
                out.push_back(op);
                cur = to;
            } else {
                auto q = P(k);
                if (on(k + 1)) {
                    quad(q, P(k + 1));
                    ++s;   // nästa punkt redan använd
                } else {
                    auto r = P(k + 1);
                    std::array<double, 2> mid{(q[0] + r[0]) / 2, (q[1] + r[1]) / 2};
                    if (all_off && last) mid = start;
                    quad(q, mid);
                }
            }
        }
        out.push_back(PathOp{'h'});
        s0 = e + 1;
    }
}

std::vector<PathOp> TrueType::outline(uint16_t gid) const {
    std::vector<PathOp> out;
    const double id[6] = {1, 0, 0, 1, 0, 0};
    outline_into(gid, id, out, 0);
    return out;
}

void TrueType::components(uint16_t gid, std::set<uint16_t>& out, int depth) const {
    if (!out.insert(gid).second || depth > 8) return;
    auto [a, b] = glyph_range(gid);
    if (b <= a) return;
    const auto* g = tab("glyf") + a;
    if (i16(g) >= 0) return;
    const auto* p = g + 10;
    uint16_t flags;
    do {
        flags = u16(p);
        components(u16(p + 2), out, depth + 1);
        p += 4 + ((flags & ARGS_WORDS) ? 4 : 2);
        p += (flags & HAVE_SCALE) ? 2 : (flags & XY_SCALE) ? 4 : (flags & TWO_BY_TWO) ? 8 : 0;
    } while (flags & MORE);
}

std::vector<unsigned char> TrueType::subset(const std::set<uint16_t>& gids) const {
    std::set<uint16_t> keep{0};
    for (auto g : gids) components(g, keep, 0);
    // glyf och loca (långt format), glyf-id oförändrade
    std::vector<unsigned char> glyf, loca;
    const auto* src = tab("glyf");
    for (int g = 0; g < num_glyphs_; ++g) {
        app32(loca, uint32_t(glyf.size()));
        if (!keep.count(uint16_t(g))) continue;
        auto [a, b] = glyph_range(uint16_t(g));
        glyf.insert(glyf.end(), src + a, src + b);
        while (glyf.size() % 4) glyf.push_back(0);
    }
    app32(loca, uint32_t(glyf.size()));
    std::map<std::string, std::vector<unsigned char>> out;
    auto copy = [&](const char* tag) {
        uint32_t n = 0;
        if (const auto* p = tab(tag, &n)) out[tag].assign(p, p + n);
    };
    for (const char* t : {"head", "hhea", "hmtx", "maxp", "cvt ", "fpgm", "prep", "OS/2"}) copy(t);
    out["glyf"] = std::move(glyf);
    out["loca"] = std::move(loca);
    put16(out["head"], 50, 1);   // långt loca-format
    put32(out["head"], 8, 0);    // checkSumAdjustment räknas om nedan
    // post format 3 (inga glyfnamn)
    std::vector<unsigned char> post(32, 0);
    put32(post, 0, 0x00030000);
    if (const auto* p = tab("post")) std::memcpy(post.data() + 4, p + 4, 12);
    out["post"] = post;

    std::vector<unsigned char> f;
    uint16_t n = uint16_t(out.size());
    uint16_t es = 0;
    while ((2u << es) <= n) ++es;
    app32(f, 0x00010000);
    app16(f, n);
    app16(f, uint16_t(16u << es));
    app16(f, es);
    app16(f, uint16_t(n * 16 - (16u << es)));
    size_t dir = f.size();
    f.resize(dir + 16 * n);
    size_t head_off = 0, i = 0;
    for (auto& [tag, body] : out) {   // std::map: sorterat efter tagg, som formatet kräver
        size_t off = f.size();
        if (tag == "head") head_off = off;
        std::memcpy(f.data() + dir + 16 * i, tag.data(), 4);
        put32(f, dir + 16 * i + 4, checksum(body.data(), body.size()));
        put32(f, dir + 16 * i + 8, uint32_t(off));
        put32(f, dir + 16 * i + 12, uint32_t(body.size()));
        f.insert(f.end(), body.begin(), body.end());
        while (f.size() % 4) f.push_back(0);
        ++i;
    }
    put32(f, head_off + 8, 0xB1B0AFBA - checksum(f.data(), f.size()));
    return f;
}

std::shared_ptr<const TrueType> font(const std::string& name) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const TrueType>> cache;
    std::lock_guard<std::mutex> l(m);
    if (auto it = cache.find(name); it != cache.end()) return it->second;
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v ? v : ""); };
    if (!env("PRINTCMP_FONT_DIR").empty()) dirs.push_back(fs::u8path(env("PRINTCMP_FONT_DIR")));
    dirs.push_back("/usr/share/fonts/truetype/dejavu");
    dirs.push_back("/usr/share/fonts/TTF");
    dirs.push_back(fs::u8path(env("WINDIR").empty() ? "C:\\Windows" : env("WINDIR")) / "Fonts");
    if (!env("LOCALAPPDATA").empty()) dirs.push_back(fs::u8path(env("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts");
    dirs.push_back("/Library/Fonts");
    if (!env("HOME").empty()) dirs.push_back(fs::u8path(env("HOME")) / "Library" / "Fonts");
    for (auto& d : dirs) {
        auto p = d / (name + ".ttf");
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            auto f = TrueType::load(p.string(), name);
            cache[name] = f;
            return f;
        }
    }
    throw Error(ErrorCode::FileNotFound, name + ".ttf hittades inte; installera DejaVu eller sätt PRINTCMP_FONT_DIR");
}

}  // namespace pc::pdfwrite
