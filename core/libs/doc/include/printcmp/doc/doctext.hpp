// pc_doc: textlagret över alla sidor – ord med sida, läge och stil, läsordning, sidmöbler, avstavning.
// Allt i mm med origo i sidans övre vänstra hörn.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/pdf/document.hpp"

namespace pc::doc {

struct Style {
    std::string family;
    double size = 0;       // pt, effektiv
    bool bold = false, italic = false;
    std::string color;     // "#rrggbb"
    using Key = std::tuple<std::string, double, bool, bool, std::string>;
    Key key(double step = 0.25) const;
    std::string describe() const;
    bool operator==(const Style& o) const { return key() == o.key(); }
};

struct Word {
    std::string text;
    int page = 0;              // 0-baserad
    Box bbox;                  // mm
    Style style;
    int line = -1;
    std::string norm;
    std::string role = "body";
    std::vector<std::pair<int, Box>> parts;
    bool hyphenated = false;
    double base = 0;           // baslinje, mm från överkant
    std::vector<std::pair<int, Box>> boxes() const {
        return parts.empty() ? std::vector<std::pair<int, Box>>{{page, bbox}} : parts;
    }
};

struct Document {
    std::vector<Word> words;          // brödtext i läsordning
    std::vector<Word> furniture;      // sidhuvud, sidfot, sidnummer
    std::vector<std::pair<double, double>> pages;   // (bredd, höjd) mm
    std::vector<std::optional<std::string>> page_labels;
    std::string path;
};

struct LoadOptions {
    int first_page = 0, last_page = 0;   // 1-baserat, inklusive; 0 = alla
    bool typography = true;
    bool ignore_case = false;
    bool furniture = true;
};

Document load(const pdf::Bytes& pdf, const LoadOptions& opt = {}, const std::string& path = "");
Document load_file(const std::string& path, const LoadOptions& opt = {});

std::string normalize_text(const std::string& s, bool typography = true, bool ignore_case = false);
std::vector<Word> dehyphenate(const std::vector<Word>& words, const std::set<std::string>& vocab);
std::set<std::string> vocabulary(const std::vector<const Document*>& docs);

struct PageIssue {
    int page;
    std::string printed, expected;
};
std::vector<PageIssue> page_number_issues(const Document& d);

}  // namespace pc::doc
