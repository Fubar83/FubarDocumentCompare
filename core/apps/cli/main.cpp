// printcmp-cli – kör ett jobb från en JobSpec, jämför två dokument eller kontrollerar uppgifter.
//
//   printcmp-cli run jobspec.json [--out DIR] [--quiet]
//   printcmp-cli doc referens.pdf prov.pdf [--layout reflow|fixed] [--data förväntat.txt] [--out DIR]
//   printcmp-cli data dokument.pdf förväntat.txt [--row N|kolumn=värde] [--out fil.json]
#include "../common/args.hpp"
#include "printcmp/core/text.hpp"
#include "printcmp/jobspec/runner.hpp"
#include "printcmp/report/report.hpp"

using namespace pc;
namespace fs = std::filesystem;

namespace {

JobContext console_ctx(bool quiet) {
    JobContext ctx;
    if (!quiet)
        ctx.sink = [](const Event& e) {
            if (e.type == Event::Type::Progress) std::fprintf(stderr, "\r%-10s %5.1f %%", stage_name(e.stage), e.fraction * 100);
            else if (e.type == Event::Type::Warning) std::fprintf(stderr, "\nvarning: %s\n", e.message.c_str());
        };
    return ctx;
}

int cmd_run(const app::Args& a) {
    bool quiet = a.has("--quiet");
    auto r = jobspec::run_file(a[2], a.opt("--out"), console_ctx(quiet));
    if (quiet) return 0;
    const auto& res = r.result;
    json s = {{"differences", res["summary"]["differences"]}, {"by_kind", res["summary"]["by_kind"]}};
    if (res.contains("sheet")) {
        s["instances"] = res["sheet"]["instances_found"];
        s["fail"] = res["summary"]["instances_fail"];
        s["seconds"] = round_to(r.seconds, 1);
    }
    s["out"] = r.out_dir;
    std::fprintf(stderr, "\n%s\n", s.dump().c_str());
    return 0;
}

int cmd_doc(const app::Args& a) {
    compare::DocumentOptions o;
    o.layout = a.opt("--layout", "reflow");
    o.furniture_ignore = a.opt("--furniture", "ignore") == "ignore";
    if (a.has("--data")) {
        compare::DataCheckOptions d;
        d.path = a.opt("--data");
        o.data_check = d;
    }
    fs::path out = fs::absolute(fs::u8path(a.opt("--out", "out")));
    fs::create_directories(out);
    bool quiet = a.has("--quiet");
    json res = compare::run_document_job(a[2], a[3], o, console_ctx(quiet));
    report::write_json(app::u8(out / "result.json"), res);
    report::write_document_html(app::u8(out / "report.html"), res);
    if (!quiet)
        std::fprintf(stderr, "\n%s\n",
                     json{{"differences", res["summary"]["differences"]}, {"by_kind", res["summary"]["by_kind"]}, {"out", app::u8(out)}}
                         .dump()
                         .c_str());
    return 0;
}

int cmd_data(const app::Args& a) {
    doc::LoadOptions lo;
    lo.furniture = false;
    auto d = doc::load_file(a[2], lo);
    compare::DataCheckOptions dco;
    dco.path = a[3];
    std::string row = a.opt("--row");
    if (!row.empty()) {
        if (text::is_digit_str(row)) dco.row = std::stoi(row);
        else dco.row_match = row;
    }
    dco.ignore_case = a.has("--ignore-case");
    json res = compare::run_data_check(dco, &d, {});
    for (auto& it : res["items"]) {
        std::string st = it["status"], loc;
        if (!it["locations"].empty()) loc = "  (s. " + it["locations"][0]["page"].dump() + ")";
        std::string extra = st == "near" ? " -> '" + it.value("found_text", "") + "' " + it.value("diff", "") : "";
        std::printf("%-8s %s: %s%s%s\n", st.c_str(), it["field"].get<std::string>().c_str(), it["expected"].get<std::string>().c_str(),
                    loc.c_str(), extra.c_str());
    }
    if (a.has("--out")) report::write_json(a.opt("--out"), res);
    return res["ok"].get<bool>() ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    auto a = app::Args::from_main(argc, argv);
    if (a.size() < 2) {
        std::fprintf(stderr, "användning: printcmp-cli run JOBSPEC | doc REF PROV | data PDF FÖRVÄNTAT\n");
        return 1;
    }
    try {
        const auto& cmd = a[1];
        if (cmd == "run" && a.size() >= 3) return cmd_run(a);
        if (cmd == "doc" && a.size() >= 4) return cmd_doc(a);
        if (cmd == "data" && a.size() >= 4) return cmd_data(a);
        std::fprintf(stderr, "okänt kommando eller för få argument\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nfel: %s\n", e.what());
        return 3;
    }
}
