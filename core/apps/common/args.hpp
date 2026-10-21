// Gemensamt för kommandoradsprogrammen: argument som UTF-8 (även i Windows) och enkla flaggor.
#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace pc::app {

inline std::string u8(const std::filesystem::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct Args {
    std::vector<std::string> v;   // v[0] = programmet

    // Läser kommandoraden som UTF-8 och sätter konsolen till UTF-8.
    static Args from_main(int argc, char** argv) {
        Args a;
#ifdef _WIN32
        (void)argc;
        (void)argv;
        SetConsoleOutputCP(CP_UTF8);
        int n = 0;
        LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 0; i < n; ++i) a.v.push_back(u8(std::filesystem::path(w[i])));
        LocalFree(w);
#else
        a.v.assign(argv, argv + argc);
#endif
        return a;
    }

    size_t size() const { return v.size(); }
    const std::string& operator[](size_t i) const { return v.at(i); }
    std::string opt(const std::string& name, const std::string& def = "") const {
        for (size_t i = 0; i + 1 < v.size(); ++i)
            if (v[i] == name) return v[i + 1];
        return def;
    }
    bool has(const std::string& name) const { return std::find(v.begin(), v.end(), name) != v.end(); }
    // alla värden för en flagga som får upprepas (--region a --region b)
    std::vector<std::string> all(const std::string& name) const {
        std::vector<std::string> out;
        for (size_t i = 0; i + 1 < v.size(); ++i)
            if (v[i] == name) out.push_back(v[i + 1]);
        return out;
    }
    std::vector<std::string> list(const std::string& name, char sep = ',') const {
        std::vector<std::string> out;
        std::string s = opt(name);
        for (size_t p = 0, q; p <= s.size(); p = q + 1) {
            q = s.find(sep, p);
            if (q == std::string::npos) q = s.size();
            if (q > p) out.push_back(s.substr(p, q - p));
        }
        return out;
    }
};

}  // namespace pc::app
