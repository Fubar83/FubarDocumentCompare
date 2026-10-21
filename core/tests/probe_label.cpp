// Felsökning: rendera etikettmallarna med testgeneratorn och läs dem med kärnan.
#include <cstdio>

#include "printcmp/compare/reference.hpp"
#include "printcmp/doc/doctext.hpp"
#include "printcmp/testgen/label.hpp"

using namespace pc;

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    for (auto& name : testgen::template_names()) {
        auto pdf = testgen::render_label_pdf(testgen::label_template(name));
        write_file(out + "/" + name + ".pdf", std::string(pdf.begin(), pdf.end()));
        compare::ReferenceOptions ro;
        auto m = compare::analyse_reference(pdf, name + ".pdf", ro);
        std::printf("%s: %zu byte, %.1f x %.1f mm, dekorfärger:", name.c_str(), pdf.size(), m->ref.geom.width_mm(), m->ref.geom.height_mm());
        for (auto& s : m->spots.spot_names) std::printf(" [%s]", s.c_str());
        std::printf("\n  koder:");
        for (auto& c : m->codes) std::printf(" %s=%s", c.symbology.c_str(), c.data.c_str());
        std::printf("\n  tabeller %zu, punktskrift:", m->tables.size());
        for (auto& b : m->braille) std::printf(" '%s'", b.text().c_str());
        std::printf("\n  objekt:");
        std::map<std::string, int> kinds;
        for (auto& o : m->objs) kinds[o.kind]++;
        for (auto& [k, n] : kinds) std::printf(" %s=%d", k.c_str(), n);
        doc::LoadOptions lo;
        lo.furniture = false;
        auto d = doc::load_file(out + "/" + name + ".pdf", lo);
        std::printf("\n  text:");
        for (auto& w : d.words) std::printf(" %s", w.text.c_str());
        std::printf("\n");
    }
}
