// Port av Pythons difflib.SequenceMatcher (Ratcliff/Obershelp), autojunk=False.
// Ger samma opkoder som Pythons difflib, som textjämförelsens regler bygger på.
#pragma once

#include <algorithm>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace pc::difflib {

struct Match {
    size_t a, b, size;
};

struct Opcode {
    enum Tag { Equal, Replace, Delete, Insert } tag;
    size_t i1, i2, j1, j2;
};

template <class T, class Hash = std::hash<T>>
class SequenceMatcher {
public:
    SequenceMatcher(const std::vector<T>& a, const std::vector<T>& b) : a_(a), b_(b) {
        for (size_t j = 0; j < b_.size(); ++j) b2j_[b_[j]].push_back(j);
    }

    Match find_longest_match(size_t alo, size_t ahi, size_t blo, size_t bhi) const {
        size_t besti = alo, bestj = blo, bestsize = 0;
        std::unordered_map<size_t, size_t> j2len;
        for (size_t i = alo; i < ahi; ++i) {
            std::unordered_map<size_t, size_t> newj2len;
            auto it = b2j_.find(a_[i]);
            if (it != b2j_.end()) {
                for (size_t j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    size_t k = 1;
                    if (j > 0) {
                        auto p = j2len.find(j - 1);
                        if (p != j2len.end()) k = p->second + 1;
                    }
                    newj2len[j] = k;
                    if (k > bestsize) {
                        besti = i - k + 1;
                        bestj = j - k + 1;
                        bestsize = k;
                    }
                }
            }
            j2len.swap(newj2len);
        }
        // utan skräpelement: inga utvidgningar behövs (autojunk=False, isjunk=None)
        while (besti > alo && bestj > blo && a_[besti - 1] == b_[bestj - 1]) {
            --besti;
            --bestj;
            ++bestsize;
        }
        while (besti + bestsize < ahi && bestj + bestsize < bhi && a_[besti + bestsize] == b_[bestj + bestsize])
            ++bestsize;
        return {besti, bestj, bestsize};
    }

    const std::vector<Match>& get_matching_blocks() {
        if (have_blocks_) return blocks_;
        std::vector<std::tuple<size_t, size_t, size_t, size_t>> queue{{0, a_.size(), 0, b_.size()}};
        std::vector<Match> mb;
        while (!queue.empty()) {
            auto [alo, ahi, blo, bhi] = queue.back();
            queue.pop_back();
            Match x = find_longest_match(alo, ahi, blo, bhi);
            if (x.size) {
                mb.push_back(x);
                if (alo < x.a && blo < x.b) queue.emplace_back(alo, x.a, blo, x.b);
                if (x.a + x.size < ahi && x.b + x.size < bhi) queue.emplace_back(x.a + x.size, ahi, x.b + x.size, bhi);
            }
        }
        std::sort(mb.begin(), mb.end(), [](const Match& p, const Match& q) {
            return std::tie(p.a, p.b, p.size) < std::tie(q.a, q.b, q.size);
        });
        size_t i1 = 0, j1 = 0, k1 = 0;
        blocks_.clear();
        for (const auto& m : mb) {
            if (i1 + k1 == m.a && j1 + k1 == m.b) {
                k1 += m.size;
            } else {
                if (k1) blocks_.push_back({i1, j1, k1});
                i1 = m.a;
                j1 = m.b;
                k1 = m.size;
            }
        }
        if (k1) blocks_.push_back({i1, j1, k1});
        blocks_.push_back({a_.size(), b_.size(), 0});
        have_blocks_ = true;
        return blocks_;
    }

    std::vector<Opcode> get_opcodes() {
        size_t i = 0, j = 0;
        std::vector<Opcode> ops;
        for (const auto& m : get_matching_blocks()) {
            Opcode::Tag tag;
            bool emit = true;
            if (i < m.a && j < m.b) tag = Opcode::Replace;
            else if (i < m.a) tag = Opcode::Delete;
            else if (j < m.b) tag = Opcode::Insert;
            else emit = false;
            if (emit) ops.push_back({tag, i, m.a, j, m.b});
            i = m.a + m.size;
            j = m.b + m.size;
            if (m.size) ops.push_back({Opcode::Equal, m.a, i, m.b, j});
        }
        return ops;
    }

    double ratio() {
        size_t matches = 0;
        for (const auto& m : get_matching_blocks()) matches += m.size;
        size_t la = a_.size() + b_.size();
        return la ? 2.0 * matches / la : 1.0;
    }

    double quick_ratio() const {
        std::unordered_map<T, long, Hash> avail;
        for (const auto& x : b_) avail[x]++;
        size_t matches = 0;
        for (const auto& x : a_) {
            auto it = avail.find(x);
            if (it != avail.end() && it->second > 0) {
                --it->second;
                ++matches;
            }
        }
        size_t la = a_.size() + b_.size();
        return la ? 2.0 * matches / la : 1.0;
    }

    double real_quick_ratio() const {
        size_t la = a_.size(), lb = b_.size();
        return (la + lb) ? 2.0 * std::min(la, lb) / (la + lb) : 1.0;
    }

private:
    const std::vector<T>& a_;
    const std::vector<T>& b_;
    std::unordered_map<T, std::vector<size_t>, Hash> b2j_;
    std::vector<Match> blocks_;
    bool have_blocks_ = false;
};

// Teckensträngar jämförs per Unicode-kodpunkt, som i Python.
std::vector<char32_t> codepoints(const std::string& utf8);
std::string from_codepoints(const std::vector<char32_t>& cps, size_t i, size_t j);

// Kompakt teckenvis skillnad: '1[0→8] %'.
std::string char_diff(const std::string& a, const std::string& b);
double char_ratio(const std::string& a, const std::string& b);

}  // namespace pc::difflib
