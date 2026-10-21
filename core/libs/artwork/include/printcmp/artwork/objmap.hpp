// Objektkarta ur originalet: text, konturerad text, linjer, bilder, streckkoder, vektorer.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "printcmp/core/core.hpp"
#include "printcmp/pdf/document.hpp"

namespace pc::artwork {

struct Obj {
    int id = 0;
    std::string kind;          // Text | Image | Vector | Rule | Barcode | OutlinedText
    Box bbox;                  // ref px
    std::string text;
    std::vector<Box> glyphs;   // OutlinedText
    double baseline_px = 0;
};

std::vector<Obj> build_objects(const pdf::Bytes& pdf, int page, const pdf::Geometry& geom,
                               const std::vector<Box>& exclude_boxes);
std::vector<const Obj*> overlapping(const std::vector<Obj>& objs, const Box& box,
                                    const std::set<std::string>& kinds = {}, double min_frac = 0.05);

}  // namespace pc::artwork
