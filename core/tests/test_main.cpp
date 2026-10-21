#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    int run = 0;
    for (auto& c : pctest::registry()) {
        if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
        int before = pctest::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            pctest::fail(c.name, 0, std::string("undantag: ") + e.what());
        }
        std::printf("%s %s\n", pctest::failures() == before ? "ok  " : "FEL ", c.name);
        ++run;
    }
    std::printf("%d test, %d fel\n", run, pctest::failures());
    return pctest::failures() ? 1 : 0;
}
