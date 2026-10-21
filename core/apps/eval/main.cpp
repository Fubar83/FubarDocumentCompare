// printcmp-eval – utvärdering mot facit och fast benchmark (tunt skal runt pc_bench).
//
//   printcmp-eval scan CASES [--run] [--result-dir out] [--json metrics.json] [--verbose] [--jobs N]
//   printcmp-eval doc CASES [--run] [--result-dir out] [--json docmetrics.json] [--verbose] [--jobs N]
//   printcmp-eval bench [--out bench] [--sets a,b] [--jobs 4] [--regen] [--gen-only] [--rerun]
//
// --run kör analysen (kärnan i samma process) innan utvärderingen; annars läses befintlig result.json.
#include <mutex>

#include "../common/args.hpp"
#include "printcmp/bench/bench.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

void print(const std::string& line) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

bench::SuiteOptions suite_options(const app::Args& a, int default_jobs) {
    return {a.has("--run"), a.opt("--result-dir", "out"), std::stoi(a.opt("--jobs", std::to_string(default_jobs))), a.has("--verbose")};
}

}  // namespace

int main(int argc, char** argv) {
    auto a = app::Args::from_main(argc, argv);
    if (a.size() < 2) {
        std::fprintf(stderr, "användning: printcmp-eval scan|doc|bench ...\n");
        return 1;
    }
    try {
        const auto& cmd = a[1];
        if ((cmd == "scan" || cmd == "doc") && a.size() >= 3) {
            fs::path dir = fs::u8path(a[2]);
            json r = cmd == "scan" ? bench::scan_suite(dir, suite_options(a, 1), print) : bench::doc_suite(dir, suite_options(a, 4), print);
            if (a.has("--json")) write_file(a.opt("--json"), r.dump(2));
            return 0;
        }
        if (cmd == "bench") {
            bench::BenchOptions o;
            o.out = fs::u8path(a.opt("--out", "bench"));
            o.sets = a.list("--sets");
            o.jobs = std::stoi(a.opt("--jobs", "4"));
            o.regen = a.has("--regen");
            o.gen_only = a.has("--gen-only");
            o.rerun = a.has("--rerun");
            bench::run_bench(o, print);
            return 0;
        }
        std::fprintf(stderr, "okänt kommando eller för få argument\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fel: %s\n", e.what());
        return 3;
    }
}
