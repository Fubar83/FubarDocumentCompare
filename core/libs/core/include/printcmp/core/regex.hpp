// Reguljära uttryck (PCRE2, UTF + Unicode-egenskaper), så att \d, \w och (?i) beter sig som i Python.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pc {

class Regex {
public:
    struct Match {
        size_t start = 0, end = 0;     // byteindex i UTF-8-strängen
        std::vector<std::string> groups;  // grupp 0 = hela träffen
    };

    explicit Regex(const std::string& pattern, bool ignore_case = false);
    ~Regex();
    Regex(Regex&&) noexcept;
    Regex& operator=(Regex&&) noexcept;
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;

    bool search(const std::string& s) const;
    bool search(const std::string& s, Match& m, size_t from = 0) const;
    bool full_match(const std::string& s, Match* m = nullptr) const;
    std::vector<Match> find_all(const std::string& s) const;
    std::string replace(const std::string& s, const std::string& repl) const;   // global, $1-syntax

    static std::string escape(const std::string& s);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pc
