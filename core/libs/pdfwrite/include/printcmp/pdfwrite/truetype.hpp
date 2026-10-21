// TrueType: mått, glyfkonturer och delmängd (för inbäddning i PDF). Bara det testgeneratorn
// behöver: cmap 4/12, hmtx, glyf/loca (enkla och sammansatta glyfer).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pc::pdfwrite {

struct PathOp {
    char op;          // 'm' moveto, 'l' lineto, 'c' curveto (3 punkter), 'h' close
    double p[6];
};

class TrueType {
public:
    static std::shared_ptr<const TrueType> load(const std::string& path, const std::string& name = "");

    const std::string& name() const { return name_; }   // fontens namn i registret (t.ex. DejaVuSans-Bold)
    int units_per_em() const { return upem_; }
    int ascent() const { return ascent_; }      // OS/2 sTypoAscender (annars hhea), fontenheter
    int descent() const { return descent_; }    // negativ
    int cap_height() const { return cap_height_; }
    int weight() const { return weight_; }
    double italic_angle() const { return italic_angle_; }
    std::array<int, 4> bbox() const { return bbox_; }

    uint16_t glyph(char32_t cp) const;                   // 0 = saknas
    int advance(uint16_t gid) const;                     // fontenheter
    double width(const std::u32string& s, double size_pt) const;   // punkter
    std::vector<PathOp> outline(uint16_t gid) const;     // fontenheter, y uppåt

    // Fontfil med bara de använda glyferna (glyf-id behålls, övriga töms).
    std::vector<unsigned char> subset(const std::set<uint16_t>& gids) const;

private:
    struct Table {
        uint32_t off = 0, len = 0;
    };
    const unsigned char* tab(const char* tag, uint32_t* len = nullptr) const;
    std::pair<uint32_t, uint32_t> glyph_range(uint16_t gid) const;
    void outline_into(uint16_t gid, const double m[6], std::vector<PathOp>& out, int depth) const;
    void components(uint16_t gid, std::set<uint16_t>& out, int depth) const;

    std::string name_;
    std::vector<unsigned char> data_;
    std::map<std::string, Table> tables_;
    int upem_ = 1000, ascent_ = 0, descent_ = 0, cap_height_ = 0, weight_ = 400;
    int num_glyphs_ = 0, num_hmetrics_ = 0, loca_long_ = 0;
    double italic_angle_ = 0;
    std::array<int, 4> bbox_{};
    std::map<char32_t, uint16_t> cmap_;
};

// Typsnitt efter namn ("DejaVuSans-Bold"), sökt i PRINTCMP_FONT_DIR och plattformens fontkataloger.
std::shared_ptr<const TrueType> font(const std::string& name);

}  // namespace pc::pdfwrite
