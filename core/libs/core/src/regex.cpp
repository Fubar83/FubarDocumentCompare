#define PCRE2_CODE_UNIT_WIDTH 8
#include "printcmp/core/regex.hpp"

#include <pcre2.h>

#include "printcmp/core/core.hpp"

namespace pc {

struct Regex::Impl {
    pcre2_code* code = nullptr;
    pcre2_code* anchored = nullptr;
    ~Impl() {
        if (code) pcre2_code_free(code);
        if (anchored) pcre2_code_free(anchored);
    }
};

static pcre2_code* compile(const std::string& pattern, uint32_t extra) {
    int err = 0;
    PCRE2_SIZE off = 0;
    pcre2_code* c = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), pattern.size(),
                                  PCRE2_UTF | PCRE2_UCP | extra, &err, &off, nullptr);
    if (!c) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(err, buf, sizeof buf);
        throw Error(ErrorCode::InvalidArgument,
                    "ogiltigt reguljärt uttryck '" + pattern + "': " + reinterpret_cast<char*>(buf));
    }
    return c;
}

Regex::Regex(const std::string& pattern, bool ignore_case) : impl_(std::make_unique<Impl>()) {
    uint32_t f = ignore_case ? PCRE2_CASELESS : 0;
    impl_->code = compile(pattern, f);
    impl_->anchored = compile("(?:" + pattern + ")\\z", f | PCRE2_ANCHORED);
}

Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

static bool run(pcre2_code* code, const std::string& s, size_t from, Regex::Match* m) {
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), from, 0, md, nullptr);
    bool ok = rc > 0;
    if (ok && m) {
        PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        m->start = ov[0];
        m->end = ov[1];
        m->groups.clear();
        for (int i = 0; i < rc; ++i) {
            if (ov[2 * i] == PCRE2_UNSET) m->groups.emplace_back();
            else m->groups.push_back(s.substr(ov[2 * i], ov[2 * i + 1] - ov[2 * i]));
        }
    }
    pcre2_match_data_free(md);
    return ok;
}

bool Regex::search(const std::string& s) const { return run(impl_->code, s, 0, nullptr); }
bool Regex::search(const std::string& s, Match& m, size_t from) const { return run(impl_->code, s, from, &m); }
bool Regex::full_match(const std::string& s, Match* m) const { return run(impl_->anchored, s, 0, m); }

std::vector<Regex::Match> Regex::find_all(const std::string& s) const {
    std::vector<Match> out;
    size_t pos = 0;
    Match m;
    while (pos <= s.size() && run(impl_->code, s, pos, &m)) {
        out.push_back(m);
        if (m.end == m.start) {
            // tom träff: hoppa fram ett helt UTF-8-tecken
            pos = m.end + 1;
            while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) ++pos;
        } else {
            pos = m.end;
        }
    }
    return out;
}

std::string Regex::replace(const std::string& s, const std::string& repl) const {
    std::string out;
    size_t last = 0;
    for (const auto& m : find_all(s)) {
        out += s.substr(last, m.start - last);
        std::string r;
        for (size_t i = 0; i < repl.size(); ++i) {
            if (repl[i] == '$' && i + 1 < repl.size() && isdigit(static_cast<unsigned char>(repl[i + 1]))) {
                size_t g = repl[i + 1] - '0';
                if (g < m.groups.size()) r += m.groups[g];
                ++i;
            } else {
                r += repl[i];
            }
        }
        out += r;
        last = m.end;
    }
    out += s.substr(last);
    return out;
}

std::string Regex::escape(const std::string& s) {
    static const std::string special = "\\^$.|?*+()[]{}-/ #";
    std::string o;
    for (char c : s) {
        if (special.find(c) != std::string::npos) o += '\\';
        o += c;
    }
    return o;
}

}  // namespace pc
