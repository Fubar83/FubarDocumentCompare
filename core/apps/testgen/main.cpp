// printcmp-testgen – testmaterial med facit.
//
//   printcmp-testgen labels --out cases --n 6 --seed 300 [--difficulty easy|normal|hard] [--template NAMN]
//                    [--rows 4 --cols 2] [--mutations a,b] [--n-artwork N] [--scanner legacy|cis|ccd]
//                    [--fold-lift MM] [--fold-p P] [--rotation GRADER] [--scale S] [--dpi DPI]
//   printcmp-testgen docs --out doccases [--books 3] [--labels 8] [--clean 0] [--seed 700]
//   printcmp-testgen pdf TEMPLATE ut.pdf                 (en etikettmall som PDF)
#include "../common/args.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/testgen/cases.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

int labels(const app::Args& a) {
    fs::path out = fs::u8path(a.opt("--out", "cases"));
    int n = std::stoi(a.opt("--n", "4"));
    uint64_t seed0 = std::stoull(a.opt("--seed", "100"));
    auto names = testgen::template_names();
    for (int i = 0; i < n; ++i) {
        testgen::CaseOptions o;
        o.seed = seed0 + i;
        o.template_name = a.opt("--template", names[i % names.size()]);
        o.difficulty = a.opt("--difficulty", "normal");
        o.rows = std::stoi(a.opt("--rows", "4"));
        o.cols = std::stoi(a.opt("--cols", "2"));
        if (a.has("--mutations")) o.mutations = a.list("--mutations");
        if (a.has("--n-artwork")) o.n_artwork = std::stoi(a.opt("--n-artwork"));
        if (a.has("--n-print")) o.n_print = std::stoi(a.opt("--n-print"));
        o.scanner = a.opt("--scanner", "legacy");
        if (a.has("--fold-lift")) o.fold_lift = std::stod(a.opt("--fold-lift"));
        if (a.has("--fold-p")) o.fold_p = std::stod(a.opt("--fold-p"));
        if (a.has("--rotation")) o.rotation_deg = std::stod(a.opt("--rotation"));
        if (a.has("--scale")) o.scale = std::stod(a.opt("--scale"));
        if (a.has("--dpi")) o.dpi = std::stoi(a.opt("--dpi"));
        char name[32];
        std::snprintf(name, sizeof name, "case_%05llu", (unsigned long long)o.seed);
        auto t = testgen::make_case(o, out / name);
        int sys = 0;
        for (auto& d : t["defects"]) sys += d["systematic"].get<bool>();
        std::string extra;
        for (auto& f : t["sheet"]["folds"])
            if (f.contains("lift_mm")) extra += format(", lyft %g mm %s", f["lift_mm"].get<double>(), f["kind"].get<std::string>().c_str());
        std::printf("%s: %s, %zu fel (%d systematiska), veck %zu, rotation %+.2f°, scanner %s%s\n", name, o.template_name.c_str(),
                    t["defects"].size(), sys, t["sheet"]["folds"].size(), t["sheet"]["rotation_deg"].get<double>(),
                    o.scanner.c_str(), extra.c_str());
        std::fflush(stdout);
    }
    return 0;
}

int docs(const app::Args& a) {
    fs::path out = fs::u8path(a.opt("--out", "doccases"));
    int books = std::stoi(a.opt("--books", "3")), nlabels = std::stoi(a.opt("--labels", "8")), clean = std::stoi(a.opt("--clean", "0"));
    uint64_t seed = std::stoull(a.opt("--seed", "700"));
    const std::vector<std::string> profiles = {"book", "book", "magazine"};
    for (int k = 0; k < books; ++k) {
        auto c = testgen::make_book_case(seed + k, out, profiles[k % profiles.size()]);
        std::printf("%s %s\n", c.filename().string().c_str(), profiles[k % profiles.size()].c_str());
        std::fflush(stdout);
    }
    for (int k = 0; k < clean; ++k) {
        auto c = testgen::make_book_case(seed + 50 + k, out, profiles[k % profiles.size()], 0);
        std::printf("%s %s utan ändringar\n", c.filename().string().c_str(), profiles[k % profiles.size()].c_str());
        std::fflush(stdout);
    }
    auto names = testgen::template_names();
    for (int k = 0; k < nlabels; ++k) {
        auto c = testgen::make_doc_label_case(seed + 100 + k, out, names[k % names.size()]);
        std::printf("%s %s\n", c.filename().string().c_str(), names[k % names.size()].c_str());
        std::fflush(stdout);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    auto a = app::Args::from_main(argc, argv);
    if (a.size() < 2) {
        std::fprintf(stderr, "användning: printcmp-testgen labels|docs|pdf ...\n");
        return 1;
    }
    try {
        const auto& cmd = a[1];
        if (cmd == "labels") return labels(a);
        if (cmd == "docs") return docs(a);
        if (cmd == "pdf" && a.size() >= 4) {
            auto pdf = testgen::render_label_pdf(testgen::label_template(a[2]));
            write_file(a[3], std::string(pdf.begin(), pdf.end()));
            return 0;
        }
        std::fprintf(stderr, "okänt kommando\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fel: %s\n", e.what());
        return 3;
    }
}
