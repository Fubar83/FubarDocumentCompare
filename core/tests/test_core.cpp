// Kärnans hjälpfunktioner mot referensvärden ur Python och numpy (avrundning, difflib, statistik).
#include "printcmp/core/core.hpp"
#include "printcmp/core/difflib.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/core/text.hpp"
#include "test.hpp"

using namespace pc;

TEST(difflib_matches_python) {
    std::vector<char> a{'q', 'a', 'b', 'x', 'c', 'd'}, b{'a', 'b', 'y', 'c', 'd', 'f'};
    difflib::SequenceMatcher<char> sm(a, b);
    CHECK_NEAR(sm.ratio(), 0.6666666666666666, 1e-12);
    // [('delete',0,1,0,0), ('equal',1,3,0,2), ('replace',3,4,2,3), ('equal',4,6,3,5), ('insert',6,6,5,6)]
    auto ops = sm.get_opcodes();
    CHECK_EQ(ops.size(), size_t(5));
    CHECK(ops[0].tag == difflib::Opcode::Delete && ops[0].i1 == 0 && ops[0].i2 == 1);
    CHECK(ops[2].tag == difflib::Opcode::Replace && ops[2].i1 == 3 && ops[2].j1 == 2 && ops[2].j2 == 3);
    CHECK(ops[4].tag == difflib::Opcode::Insert && ops[4].j1 == 5 && ops[4].j2 == 6);
    CHECK_NEAR(difflib::char_ratio("Förvaras kylt", "Forvaras kalt"), 0.8461538461538461, 1e-12);
}

TEST(rounding_is_bankers_like_python) {
    CHECK_EQ(pyround(0.5), 0L);
    CHECK_EQ(pyround(1.5), 2L);
    CHECK_EQ(pyround(2.5), 2L);
    CHECK_EQ(pyround(-0.5), 0L);
    CHECK_EQ(round_to(2.675, 2), 2.67);
    CHECK_EQ(round_to(0.125, 2), 0.12);
    CHECK_EQ(round_to(1.005, 2), 1.0);
    CHECK_EQ(round_to(-1.235, 2), -1.24);
    CHECK_EQ(round_to(2.5, 0), 2.0);
}

TEST(statistics_like_numpy) {
    CHECK_NEAR(percentile({1, 2, 3, 4, 10}, 25), 2.0, 1e-12);
    CHECK_NEAR(median({3, 1, 2, 10}), 2.5, 1e-12);
}

TEST(unicode_text) {
    CHECK_EQ(text::casefold("Straße"), std::string("strasse"));
    CHECK_EQ(text::nfkc("\xEF\xAC\x81"), std::string("fi"));   // ligaturen ﬁ
    CHECK_EQ(text::nfc("o\xCC\x88"), std::string("\xC3\xB6"));  // o + kombinerande trema → ö
    CHECK_EQ(text::length("Ρόφημα"), size_t(6));
    CHECK(text::is_digit_str("0123"));
    CHECK(!text::is_digit_str("12a"));
    CHECK_EQ(text::split_ws("  a  b\tc ").size(), size_t(3));
}

TEST(regex_is_unicode_aware) {
    Regex r(R"(LOT\s(\d{4})-(\d{4}))");
    Regex::Match m;
    CHECK(r.search("Batch: LOT 2024-0042 bäst före", m));
    CHECK_EQ(m.groups.at(1), std::string("2024"));
    CHECK_EQ(Regex(R"(\w+)").find_all("Овсяный напиток").size(), size_t(2));
    CHECK_EQ(Regex("a(b)").replace("xabyab", "[$1]"), std::string("x[b]y[b]"));
    CHECK(Regex("havre", true).search("HAVREDRYCK"));
    CHECK_EQ(Regex::escape("1.5 (g)"), std::string(R"(1\.5\ \(g\))"));   // som re.escape
}

TEST(hashing_and_encoding) {
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(base64({'M', 'a', 'n'}), std::string("TWFu"));
    CHECK_EQ(base64({'M', 'a'}), std::string("TWE="));
}

TEST(geometry) {
    Mat3 t;   // identitet
    t(0, 2) = 10;
    t(1, 2) = -5;
    auto p = t.apply({1, 2});
    CHECK_NEAR(p.x, 11, 1e-12);
    CHECK_NEAR(p.y, -3, 1e-12);
    auto q = t.inverse().apply(p);
    CHECK_NEAR(q.x, 1, 1e-12);
    CHECK_NEAR(q.y, 2, 1e-12);
}

TEST(cancel_token) {
    CancelToken c;
    c.check();
    c.cancel();
    bool threw = false;
    try {
        c.check();
    } catch (const Error& e) {
        threw = e.code() == ErrorCode::Cancelled;
    }
    CHECK(threw);
}
