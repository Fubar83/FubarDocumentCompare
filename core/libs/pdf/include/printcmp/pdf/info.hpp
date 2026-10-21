// Dokument- och sidfakta via PDFium: informationsordlistan, sidrutor, rotation och typsnitt.
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "printcmp/pdf/document.hpp"

namespace pc::pdf {

using Rect4 = std::array<double, 4>;   // (left, bottom, right, top) i PDF-punkter

struct PageBoxes {
    Rect4 media{}, crop{}, trim{}, bleed{}, art{};
    bool has_crop = false, has_trim = false, has_bleed = false, has_art = false;   // annars ärvd/standard
};

// Ett typsnitt som sidans text använder.
struct FontUse {
    std::string name;          // utan delmängdsprefix ("ABCDEF+")
    std::string base_name;     // som i PDF:en (/BaseFont)
    std::string family;
    bool embedded = false;     // annars ersätter PDFium det vid rendering
    bool subset = false;
    int flags = 0;             // FontDescriptor /Flags
    int weight = 0;            // 0 = okänd
    int text_objects = 0;      // antal textobjekt på sidan med typsnittet
};

struct PageFacts {
    int index = 0;             // 0-baserad
    double width_pt = 0, height_pt = 0;
    int rotation_deg = 0;      // /Rotate
    PageBoxes boxes;
    std::vector<FontUse> fonts;
};

struct DocumentFacts {
    int page_count = 0;
    std::vector<std::pair<std::string, std::string>> info;   // Title, Author, Creator, Producer … (UTF-8)
    int file_version = 0;      // 17 = PDF 1.7
};

DocumentFacts document_facts(const Document& doc);
PageFacts page_facts(const Document& doc, int page_index);

}  // namespace pc::pdf
