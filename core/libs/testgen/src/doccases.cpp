// Testmaterial för PDF mot PDF: manus mot satt bok, etikettversioner och datakontroll.
//
// Bokfall (book_*): samma strukturerade text sätts två gånger,
//  * manus: A4, 12 pt, rak vänstermarginal, "Sida N" i sidfoten och ett utkasthuvud,
//  * bok: A5, smal 10 pt, marginaljusterad, avstavad, typografiska citattecken och ligaturer,
//    löpande huvud och sidnummer i ytterhörnet – eller en tvåspaltig tidningssida (A4).
// I bokversionen görs kända ändringar (facit i truth.json). Allt annat (sidbrytning, avstavning,
// typsnitt, löpande huvud, citattecken) ska inte ge skillnader.
//
// Etikettfall (label_*): två versioner av samma etikett (fast layout) plus en fil med förväntade
// uppgifter (expected.txt eller expected.csv med flera varianter) där vissa värden avsiktligt inte stämmer.
#include <functional>
#include <map>
#include <set>

#include "printcmp/core/difflib.hpp"
#include "printcmp/core/regex.hpp"
#include "printcmp/testgen/cases.hpp"
#include "util.hpp"

namespace pc::testgen {

namespace fs = std::filesystem;

namespace {

const char* GENERATOR_VERSION = "doccases-cpp/1";

// ---------------------------------------------------------------- text

const std::vector<std::string>& WORDS() {
    static const std::vector<std::string> w = text::split_ws(R"(och att det som en på är av för med till den har inte om ett men var sig så vid
han hon de vi ni kan när från ut eller efter skulle hade mot alla under även här mycket
nu bara vara kommer också hur sedan första andra genom över samma redan mellan många utan
dag år tid hus väg vatten skog sjö stad land arbete bok brev rum fönster dörr bord stol
morgon kväll vinter sommar regn vind ljus mörker röst hand ögon ansikte minne fråga svar
gammal ny stor liten lång kort varm kall tyst stilla klar mörk ljus röd grön blå grå vit
gick kom såg tog stod satt låg hörde visste trodde ville kunde fick gjorde sade tänkte
långsamt försiktigt plötsligt nästan alltid aldrig ibland kanske verkligen tillsammans
vägen huset staden sjön skogen fönstret dörren bordet rummet brevet boken vattnet ljuset
fjällstugan järnvägsstationen sommarnatten vinterkvällen ångbåtsbryggan skärgårdsön
kustlandskapet morgonsolen eftermiddagen sjukhuskorridoren förlagsredaktionen
handelsträdgården kyrkogårdsmuren fiskeläget fönsterbrädan trädgårdsgången
återkomsten förklaringen berättelsen anteckningarna fotografierna bokhandeln)");
    return w;
}
const std::vector<std::string> NAMES = {"Elin", "Johan", "Maria", "Anders", "Sofia", "Erik", "Karin", "Nils", "Ingrid", "Oskar"};
const std::vector<std::string> FI_WORDS = {"fika", "fiskeläget", "fingrarna", "fint", "filten", "officiellt", "fiolen"};

struct Tok {
    std::string t;
    bool i = false, b = false;
    int s = 0, id = 0;
    std::optional<std::array<double, 3>> color;
};
struct Para {
    std::vector<Tok> toks;
    std::string font;   // "" | "PARA_FONT"
};
struct Chapter {
    std::string title;
    std::optional<double> heading_size;
    std::vector<Para> paras;
};
struct Doc {
    std::string title, author;
    std::vector<Chapter> chapters;
};

std::string capitalize_first(const std::string& s) {   // s[:1].upper() + s[1:]
    auto u = u32(s);
    if (u.empty()) return s;
    return text::upper(u8(u[0])) + u8(u.substr(1));
}
std::string capitalize(const std::string& s) {         // str.capitalize()
    auto u = u32(s);
    if (u.empty()) return s;
    return text::upper(u8(u[0])) + text::lower(u8(u.substr(1)));
}
size_t ulen(const std::string& s) { return u32(s).size(); }

std::vector<Tok> sentence(Rng& rng, int n_min = 6, int n_max = 15) {
    int n = int(rng.integers(n_min, n_max + 1));
    std::vector<Tok> toks;
    for (int k = 0; k < n; ++k) {
        double r = rng.random();
        Tok t;
        t.t = r < 0.06 ? rng.choice(NAMES) : r < 0.10 ? rng.choice(FI_WORDS) : rng.choice(WORDS());
        toks.push_back(t);
    }
    if (rng.random() < 0.25) {
        int k = int(rng.integers(1, n));
        // alla tre alternativen dras innan ett väljs (stabil slumpföljd)
        std::vector<std::string> alt;
        int a = int(rng.integers(2, 99)), b = int(rng.integers(1, 9));
        alt.push_back(std::to_string(a) + "," + std::to_string(b));
        alt.push_back(std::to_string(rng.integers(1890, 2024)));
        alt.push_back(std::to_string(rng.integers(2, 40)));
        toks.insert(toks.begin() + k, Tok{rng.choice(alt)});
        if (rng.random() < 0.5)
            toks.insert(toks.begin() + k + 1, Tok{rng.choice(std::vector<std::string>{"mg", "kilometer", "procent", "kronor", "år"})});
    }
    if (rng.random() < 0.3 && toks.size() > 4) {
        int k = int(rng.integers(1, int64_t(toks.size()) - 2));
        int m = int(rng.integers(1, 4));
        for (int j = k; j < std::min<int>(k + m, int(toks.size())); ++j) toks[j].i = true;
    } else if (rng.random() < 0.15) {
        int k = int(rng.integers(1, int64_t(toks.size())));
        toks[k].b = true;
    }
    toks[0].t = capitalize_first(toks[0].t);
    if (rng.random() < 0.12 && toks.size() > 5) {
        int k = int(rng.integers(2, int64_t(toks.size()) - 2));
        toks[k].t += ",";
    }
    std::string end = rng.random() < 0.85 ? "." : rng.choice(std::vector<std::string>{"?", "!"});
    toks.back().t += end;
    if (rng.random() < 0.1) {   // replik
        toks[0].t = "\"" + toks[0].t;
        toks.back().t += "\"";
    }
    return toks;
}

Doc make_text(Rng& rng, int chapters, std::array<int, 2> paras) {
    Doc d;
    d.title = "Ljuset över " + capitalize(rng.choice(std::vector<std::string>{"sjön", "staden", "skärgården", "fjället"}));
    d.author = rng.choice(NAMES);
    d.author += " " + rng.choice(std::vector<std::string>{"Lindqvist", "Berg", "Holm", "Ek", "Sandberg"});
    std::vector<std::string> mid(WORDS().begin() + 60, WORDS().begin() + 140);
    int tid = 0;
    for (int c = 0; c < chapters; ++c) {
        Chapter ch;
        std::string w1 = rng.choice(mid), w2 = rng.choice(mid);
        ch.title = std::to_string(c + 1) + ". " + capitalize(w1 + " " + w2);
        int np = int(rng.integers(paras[0], paras[1]));
        for (int p = 0; p < np; ++p) {
            Para pa;
            int ns = int(rng.integers(3, 7));
            for (int s = 0; s < ns; ++s)
                for (auto t : sentence(rng)) {
                    t.s = s;
                    t.id = tid++;
                    pa.toks.push_back(t);
                }
            ch.paras.push_back(pa);
        }
        d.chapters.push_back(ch);
    }
    return d;
}

// ---------------------------------------------------------------- sättning

struct Profile {
    double page_w, page_h;
    double m_top, m_bottom, m_inner, m_outer;
    int cols;
    double gutter;
    std::string body;
    double body_size, body_lead;
    std::string italic, bold, heading;
    double heading_size;
    std::string title;
    double title_size;
    bool justify, hyphenate;
    double indent, para_space;
    bool typo;
    std::string header, footer;
};

const Profile& profile(const std::string& name) {
    static const std::map<std::string, Profile> P = {
        {"manuscript", {210, 297, 25, 25, 25, 25, 1, 0, "DejaVuSerif", 12, 21, "DejaVuSerif-Italic", "DejaVuSerif-Bold",
                        "DejaVuSerif-Bold", 16, "DejaVuSerif-Bold", 22, false, false, 0, 10, false, "manus", "sida"}},
        {"book", {148, 210, 17, 18, 15, 13, 1, 0, "DejaVuSerifCondensed", 10, 13, "DejaVuSerifCondensed-Italic",
                  "DejaVuSerifCondensed-Bold", "DejaVuSerifCondensed-Bold", 13, "DejaVuSerifCondensed-Bold", 18, true, true, 4, 0,
                  true, "running", "outer"}},
        {"magazine", {210, 297, 20, 22, 16, 16, 2, 8, "DejaVuSans", 9, 12, "DejaVuSans-Oblique", "DejaVuSans-Bold",
                      "DejaVuSans-Bold", 12, "DejaVuSans-Bold", 20, true, true, 3, 0, true, "running", "center"}},
    };
    return P.at(name);
}

const std::u32string VOWELS = U"aeiouyåäöé";

// Bokens typografi: ”…”, tankstreck, fi-ligatur.
std::string typo(std::string t) {
    t = text::replace_all(t, "\"", "”");
    t = text::replace_all(t, " - ", " – ");
    return text::replace_all(t, "fi", "ﬁ");
}

std::vector<size_t> hyph_points(const std::u32string& word) {
    auto core = word;
    const std::u32string strip = U".,;:!?”\"";
    while (!core.empty() && strip.find(core.back()) != std::u32string::npos) core.pop_back();
    if (core.size() < 7 || !text::is_alpha_str(u8(core))) return {};
    auto low = [](char32_t c) { return u32(text::lower(u8(c)))[0]; };
    std::u32string consonants = VOWELS + U"lnrstkmg";
    std::vector<size_t> pts;
    for (size_t k = 3; k + 2 < core.size(); ++k)
        if (VOWELS.find(low(core[k])) == std::u32string::npos && VOWELS.find(low(core[k + 1])) != std::u32string::npos &&
            consonants.find(low(core[k - 1])) != std::u32string::npos)
            pts.push_back(k);
    return pts;
}

double width_mm(const std::string& font_name, double size, const std::string& s) { return text_width_mm(font_name, size, s); }

class Typesetter {
public:
    Typesetter(const Profile& p, std::string title) : p_(p), title_(std::move(title)) {}

    bool title_page = false;
    std::string chapter;
    std::optional<std::pair<int, int>> page_error;   // (från sida, tillägg)
    double y = 0;
    int page = 0;

    void new_page(bool chapter_start = false) {
        if (page) furniture();
        page++;
        cv_ = &doc_.new_page(p_.page_w * MM, p_.page_h * MM);
        col_ = 0;
        chapter_first_ = chapter_start;
        y = p_.m_top;
    }

    int paragraph(const std::vector<Tok>& toks, std::string font = "", double size = 0, double lead = 0, double indent = 0,
                  std::optional<bool> justify_opt = std::nullopt, int keep = 0) {
        if (font.empty()) font = p_.body;
        if (!size) size = p_.body_size;
        if (!lead) lead = p_.body_lead;
        bool justify = justify_opt.value_or(p_.justify);
        double lead_mm = lead * 25.4 / 72;
        struct Item {
            std::string txt, font;
            double size;
            std::optional<std::array<double, 3>> color;
            double w = 0;
        };
        std::vector<Item> items;
        for (auto& t : toks) {
            std::string f = t.b ? p_.bold : t.i ? p_.italic : font;
            items.push_back({p_.typo ? typo(t.t) : t.t, f, size, t.color});
        }
        std::vector<std::pair<std::vector<Item>, double>> lines;
        std::vector<Item> cur;
        double cur_w = 0;
        auto [x0, cw, top, bottom] = col_box();
        size_t k = 0;
        while (k < items.size()) {
            auto it = items[k];
            double ind = lines.empty() ? indent : 0.0;
            double avail = cw - ind;
            double w = width_mm(it.font, it.size, it.txt), sp = width_mm(it.font, it.size, " ");
            double need = w + (cur.empty() ? 0 : sp);
            if (cur_w + need <= avail) {
                it.w = w;
                cur.push_back(it);
                cur_w += need;
                ++k;
                continue;
            }
            bool placed = false;
            if (p_.hyphenate) {
                auto u = u32(it.txt);
                auto pts = hyph_points(u);
                for (auto r = pts.rbegin(); r != pts.rend(); ++r) {
                    std::string head = u8(u.substr(0, *r)) + "-";
                    double hw = width_mm(it.font, it.size, head);
                    if (cur_w + (cur.empty() ? 0 : sp) + hw <= avail) {
                        auto h = it;
                        h.txt = head;
                        h.w = hw;
                        cur.push_back(h);
                        items[k].txt = u8(u.substr(*r));
                        placed = true;
                        break;
                    }
                }
            }
            if (cur.empty() && !placed) {   // ett ord längre än raden
                it.w = w;
                cur.push_back(it);
                ++k;
            }
            lines.push_back({cur, ind});
            cur.clear();
            cur_w = 0;
        }
        if (!cur.empty()) lines.push_back({cur, lines.empty() ? indent : 0.0});
        for (size_t li = 0; li < lines.size(); ++li) {
            auto& [ln, ind] = lines[li];
            advance(lead_mm * (1 + (li == 0 ? keep : 0)));
            auto [bx0, bcw, btop, bbottom] = col_box();
            y += lead_mm;
            double nat = 0;
            for (size_t j = 0; j < ln.size(); ++j) {
                nat += ln[j].w;
                if (j + 1 < ln.size()) nat += width_mm(ln[j].font, ln[j].size, " ");
            }
            double extra = 0;
            if (justify && li + 1 < lines.size() && ln.size() > 1) {
                extra = (bcw - ind - nat) / (ln.size() - 1);
                if (extra > 6) extra = 0;
            }
            double x = bx0 + ind;
            for (size_t wi = 0; wi < ln.size(); ++wi) {
                auto& w = ln[wi];
                cv_->fill_color(w.color ? Color::rgb((*w.color)[0], (*w.color)[1], (*w.color)[2]) : Color::rgb(0, 0, 0));
                bool last = wi + 1 == ln.size();
                cv_->text(w.font, w.size, x * MM, (p_.page_h - y) * MM, w.txt + (last ? "" : " "));
                x += w.w + width_mm(w.font, w.size, " ") + extra;
            }
        }
        return int(lines.size());
    }

    void space(double pt) { y += pt * 25.4 / 72; }

    std::pair<Bytes, int> finish() {
        furniture();
        return {doc_.save(), doc_.page_count()};
    }

private:
    std::array<double, 4> col_box() const {
        double left = (page % 2 == 1) ? p_.m_inner : p_.m_outer;   // sida 1 = höger (recto)
        double width = p_.page_w - p_.m_inner - p_.m_outer;
        double cw = (width - (p_.cols - 1) * p_.gutter) / p_.cols;
        return {left + col_ * (cw + p_.gutter), cw, p_.m_top, p_.page_h - p_.m_bottom};
    }

    int printed_number() const {
        int n = page;
        if (page_error && page >= page_error->first) n += page_error->second;
        return n;
    }

    void furniture() {
        if (page == 1 && title_page) return;
        auto& c = *cv_;
        double W = p_.page_w, H = p_.page_h;
        c.fill_color(Color::rgb(0, 0, 0));
        std::string n = std::to_string(printed_number());
        if (p_.header == "manus") {
            c.text_right("DejaVuSans", 9, (W - 25) * MM, (H - 12) * MM, "Manus · " + title_ + " · utkast 3");
        } else if (p_.header == "running" && !chapter_first_) {
            if (page % 2 == 0) c.text(p_.italic, 8, p_.m_outer * MM, (H - 10) * MM, title_);
            else c.text_right(p_.italic, 8, (W - p_.m_outer) * MM, (H - 10) * MM, chapter);
        }
        if (p_.footer == "sida") c.text_centred("DejaVuSans", 9, W / 2 * MM, 12 * MM, "Sida " + n);
        else if (p_.footer == "outer") {
            if (chapter_first_) c.text_centred(p_.body, 9, W / 2 * MM, 9 * MM, n);
            else if (page % 2 == 0) c.text(p_.body, 9, p_.m_outer * MM, 9 * MM, n);
            else c.text_right(p_.body, 9, (W - p_.m_outer) * MM, 9 * MM, n);
        } else c.text_centred(p_.body, 8, W / 2 * MM, 9 * MM, n);
    }

    void advance(double need_mm) {
        auto [x0, cw, top, bottom] = col_box();
        if (y + need_mm > bottom) {
            if (col_ + 1 < p_.cols) {
                ++col_;
                y = top;
            } else new_page();
        }
    }

    const Profile& p_;
    std::string title_;
    PdfDocument doc_;
    Canvas* cv_ = nullptr;
    int col_ = 0;
    bool chapter_first_ = false;
};

std::vector<Tok> words_of(const std::string& s) {
    std::vector<Tok> out;
    for (auto& w : text::split_ws(s)) out.push_back(Tok{w});
    return out;
}

std::pair<Bytes, int> typeset(const Doc& doc, const std::string& prof, std::optional<std::pair<int, int>> page_error = std::nullopt) {
    const auto& p = profile(prof);
    Typesetter ts(p, doc.title);
    ts.title_page = true;
    ts.new_page(true);
    ts.y = p.page_h * 0.28;
    ts.paragraph(words_of(doc.title), p.title, p.title_size, p.title_size * 1.3, 0, false);
    ts.space(14);
    ts.paragraph(words_of(doc.author), p.italic, p.body_size + 1, 0, 0, false);
    ts.page_error = page_error;
    for (auto& ch : doc.chapters) {
        ts.chapter = ch.title;
        if (prof == "magazine" && ts.page > 1) ts.space(10);
        else {
            ts.new_page(true);
            if (prof != "magazine") ts.y += 20;
        }
        double hs = ch.heading_size.value_or(p.heading_size);
        ts.paragraph(words_of(ch.title), p.heading, hs, hs * 1.35, 0, false, 3);
        ts.space(10);
        for (size_t pi = 0; pi < ch.paras.size(); ++pi) {
            const auto& para = ch.paras[pi];
            std::string pf;
            if (para.font == "PARA_FONT") pf = p.body.find("Sans") != std::string::npos ? "DejaVuSerif" : "DejaVuSans";
            double ind = (pi == 0 || !p.indent) ? 0 : p.indent;
            ts.paragraph(para.toks, pf, 0, 0, ind);
            if (p.para_space) ts.space(p.para_space);
        }
    }
    return ts.finish();
}

// ---------------------------------------------------------------- ändringar i boken

std::string tok_text(const std::vector<Tok>& toks) {
    std::vector<std::string> w;
    for (auto& t : toks) w.push_back(t.t);
    return text::join(w, " ");
}

const Regex& lower_word(int min) {
    static Regex r4(R"(^[a-zåäö]{4,}$)"), r5(R"(^[a-zåäö]{5,}$)");
    return min == 4 ? r4 : r5;
}
bool has_digit(const std::string& s) {
    static Regex d(R"(\d)");
    return d.search(s);
}

struct BookMutation {
    Doc doc;
    json truth;
    bool page_pending = false;
};

BookMutation mutate_book(const Doc& doc, Rng& rng, int n) {
    BookMutation r{doc, json::array()};
    Doc& d = r.doc;
    std::set<std::pair<int, int>> used;
    std::vector<std::string> kinds = {"typo", "number", "word_delete", "word_insert", "sentence_delete", "punct", "para_move",
                                      "italic_lost", "bold_lost", "heading_size", "para_font", "color", "page_number"};
    rng.shuffle(kinds);
    kinds.resize(std::min<size_t>(kinds.size(), size_t(std::max(n, 0))));

    struct Pick {
        int ci, pi;
        Para* pa;
    };
    auto pick = [&](size_t min_tokens = 20, std::function<bool(const Para&)> need = nullptr) -> std::optional<Pick> {
        std::vector<std::pair<int, int>> cands;
        for (int ci = 0; ci < int(d.chapters.size()); ++ci)
            for (int pi = 0; pi < int(d.chapters[ci].paras.size()); ++pi) {
                const auto& pa = d.chapters[ci].paras[pi];
                if (!used.count({ci, pi}) && pa.toks.size() >= min_tokens && (!need || need(pa))) cands.push_back({ci, pi});
            }
        if (cands.empty()) return std::nullopt;
        auto [ci, pi] = cands[rng.index(cands.size())];
        used.insert({ci, pi});
        return Pick{ci, pi, &d.chapters[ci].paras[pi]};
    };
    // första index i en slumpad ordning som uppfyller villkoret (som next(k for k in rng.permutation(...)))
    auto first_in = [&](std::vector<int> order, std::function<bool(int)> ok) {
        rng.shuffle(order);
        for (int k : order)
            if (ok(k)) return k;
        throw Error(ErrorCode::NotFound, "ingen kandidat för ändringen");
    };
    auto range = [](int a, int b) {
        std::vector<int> v;
        for (int k = a; k < b; ++k) v.push_back(k);
        return v;
    };

    for (const auto& kind : kinds) {
        json rec = {{"id", "m" + std::to_string(r.truth.size() + 1)}, {"mutation", kind}};
        if (kind == "typo") {
            auto p = pick();
            if (!p) continue;
            auto& toks = p->pa->toks;
            int k = first_in(range(0, int(toks.size())), [&](int k) { return lower_word(5).full_match(toks[k].t); });
            auto w = u32(toks[k].t);
            int j = int(rng.integers(1, int64_t(w.size()) - 2));
            auto nw = w.substr(0, j) + w[j + 1] + w[j] + w.substr(j + 2);
            if (nw == w) nw = w.substr(0, j) + (w[j] != U'a' ? U'a' : U'e') + w.substr(j + 1);
            toks[k].t = u8(nw);
            rec.update({{"kind", "Changed"}, {"ref_text", u8(w)}, {"smp_text", u8(nw)}});
        } else if (kind == "number") {
            auto p = pick(20, [](const Para& pa) { return std::any_of(pa.toks.begin(), pa.toks.end(), [](const Tok& t) { return has_digit(t.t); }); });
            if (!p) continue;
            auto& toks = p->pa->toks;
            auto it = std::find_if(toks.begin(), toks.end(), [](const Tok& t) { return has_digit(t.t); });
            std::string w = it->t, nw = w;
            size_t j = w.find_last_of("0123456789");
            nw[j] = char('0' + ((w[j] - '0') + 3) % 10);
            it->t = nw;
            rec.update({{"kind", "Changed"}, {"ref_text", w}, {"smp_text", nw}, {"category", "Number"}});
        } else if (kind == "word_delete") {
            auto p = pick();
            if (!p) continue;
            auto& toks = p->pa->toks;
            int k = first_in(range(2, int(toks.size()) - 2), [&](int k) { return lower_word(4).full_match(toks[k].t); });
            rec.update({{"kind", "Missing"}, {"ref_text", toks[k].t}, {"smp_text", ""}});
            toks.erase(toks.begin() + k);
        } else if (kind == "word_insert") {
            auto p = pick();
            if (!p) continue;
            auto& toks = p->pa->toks;
            int k = int(rng.integers(2, int64_t(toks.size()) - 2));
            std::string w = rng.choice(std::vector<std::string>{"verkligen", "faktiskt", "egentligen", "tydligen"});
            toks.insert(toks.begin() + k, Tok{w});
            rec.update({{"kind", "Added"}, {"ref_text", ""}, {"smp_text", w}});
        } else if (kind == "sentence_delete") {
            auto p = pick(20, [](const Para& pa) {
                std::set<int> s;
                for (auto& t : pa.toks) s.insert(t.s);
                return s.size() >= 3;
            });
            if (!p) continue;
            auto& toks = p->pa->toks;
            std::set<int> ss;
            for (auto& t : toks) ss.insert(t.s);
            int s = *std::next(ss.begin());
            std::vector<Tok> gone, keep;
            for (auto& t : toks) (t.s == s ? gone : keep).push_back(t);
            toks = keep;
            rec.update({{"kind", "Missing"}, {"ref_text", tok_text(gone)}, {"smp_text", ""}});
        } else if (kind == "punct") {
            auto p = pick(20, [](const Para& pa) {
                for (size_t k = 0; k + 1 < pa.toks.size(); ++k)
                    if (text::ends_with(pa.toks[k].t, ".")) return true;
                return false;
            });
            if (!p) continue;
            auto& toks = p->pa->toks;
            size_t k = 0;
            bool found = false;
            for (; k + 1 < toks.size(); ++k)
                if (text::ends_with(toks[k].t, ".") && !text::ends_with(toks[k].t, ".\"")) {
                    found = true;
                    break;
                }
            if (!found) continue;
            std::string w = toks[k].t, nw = w.substr(0, w.size() - 1) + ";";
            toks[k].t = nw;
            rec.update({{"kind", "Changed"}, {"ref_text", w}, {"smp_text", nw}, {"category", "Punctuation"}});
        } else if (kind == "para_move") {
            auto p = pick(30);
            if (!p) continue;
            int ci = p->ci, pi = p->pi;
            if (d.chapters.size() < 2) continue;
            int nch = int(d.chapters.size());
            int dst = (ci + 1 + int(rng.integers(nch - 1))) % nch;
            Para pa = d.chapters[ci].paras[pi];
            d.chapters[ci].paras.erase(d.chapters[ci].paras.begin() + pi);
            // håll index i used giltiga: blockera hela kapitlen
            for (int k = 0; k < 40; ++k) used.insert({ci, k}), used.insert({dst, k});
            auto& dp = d.chapters[dst].paras;
            dp.insert(dp.begin() + rng.integers(1, int64_t(dp.size())), pa);
            rec.update({{"kind", "Moved"}, {"ref_text", tok_text(pa.toks)}, {"smp_text", tok_text(pa.toks)}});
        } else if (kind == "italic_lost") {
            auto p = pick(20, [](const Para& pa) { return std::any_of(pa.toks.begin(), pa.toks.end(), [](const Tok& t) { return t.i; }); });
            if (!p) continue;
            std::vector<Tok*> run;
            for (auto& t : p->pa->toks)
                if (t.i) run.push_back(&t);
            int s0 = run[0]->s;
            std::vector<Tok> same;
            for (auto* t : run)
                if (t->s == s0) {
                    t->i = false;
                    same.push_back(*t);
                }
            rec.update({{"kind", "Style"}, {"ref_text", tok_text(same)}, {"smp_text", tok_text(same)}, {"attr", "italic"}});
        } else if (kind == "bold_lost") {
            auto p = pick(20, [](const Para& pa) { return std::any_of(pa.toks.begin(), pa.toks.end(), [](const Tok& t) { return t.b; }); });
            if (!p) continue;
            auto it = std::find_if(p->pa->toks.begin(), p->pa->toks.end(), [](const Tok& t) { return t.b; });
            it->b = false;
            rec.update({{"kind", "Style"}, {"ref_text", it->t}, {"smp_text", it->t}, {"attr", "bold"}});
        } else if (kind == "heading_size") {
            int ci = int(rng.integers(int64_t(d.chapters.size())));
            d.chapters[ci].heading_size = 10.5;
            rec.update({{"kind", "Style"}, {"ref_text", d.chapters[ci].title}, {"smp_text", d.chapters[ci].title}, {"attr", "size"}});
        } else if (kind == "para_font") {
            auto p = pick(15, [](const Para& pa) { return !std::any_of(pa.toks.begin(), pa.toks.end(), [](const Tok& t) { return t.i || t.b; }); });
            if (!p) continue;
            p->pa->font = "PARA_FONT";   // ett annat typsnitt än bokens brödtext (sätts i typeset)
            rec.update({{"kind", "Style"}, {"ref_text", tok_text(p->pa->toks)}, {"smp_text", tok_text(p->pa->toks)}, {"attr", "family"}});
        } else if (kind == "color") {
            auto p = pick();
            if (!p) continue;
            auto& toks = p->pa->toks;
            int k = first_in(range(1, int(toks.size()) - 1), [&](int k) { return lower_word(4).full_match(toks[k].t) && !toks[k].i; });
            toks[k].color = std::array<double, 3>{0.8, 0.05, 0.05};
            rec.update({{"kind", "Style"}, {"ref_text", toks[k].t}, {"smp_text", toks[k].t}, {"attr", "color"}});
        } else if (kind == "page_number") {
            r.page_pending = true;   // sidan väljs när sidantalet är känt
            rec["kind"] = "PageNumber";
        }
        r.truth.push_back(rec);
    }
    return r;
}

void write_bytes(const fs::path& p, const Bytes& b) { write_file(p.string(), std::string(b.begin(), b.end())); }

// ---------------------------------------------------------------- etiketter

std::vector<json*> text_elements(json& spec) {
    std::vector<json*> out;
    for (auto& e : spec["elements"])
        if (e["type"] == "text" || e["type"] == "variable") out.push_back(&e);
    return out;
}

bool is_white(const json& e) {
    if (!e.contains("color")) return false;
    auto v = e["color"].value("cmyk", std::vector<double>{0, 0, 0, 1});
    return v == std::vector<double>{0, 0, 0, 0};
}

std::pair<json, json> mutate_doc_label(const json& spec, Rng& rng, int n) {
    json s = spec, truth = json::array();
    std::set<std::string> used;
    std::vector<std::string> kinds = {"text", "size", "font", "bold", "color", "move", "word_delete", "word_add", "swap"};
    rng.shuffle(kinds);
    for (const auto& kind : kinds) {
        if (int(truth.size()) >= n) break;
        std::vector<json*> els;
        for (auto* e : text_elements(s)) {
            size_t nw = text::split_ws((*e)["text"]).size();
            if (!used.count((*e)["id"]) && (*e)["type"] == "text" && nw >= size_t(kind == "word_delete" || kind == "swap" ? 2 : 1) &&
                !is_white(*e))
                els.push_back(e);
        }
        if (els.empty()) continue;
        json& e = *els[rng.index(els.size())];
        json rec = {{"id", "m" + std::to_string(truth.size() + 1)}, {"mutation", kind}, {"element", e["id"]}};
        if (kind == "text") {
            auto w = u32(e["text"].get<std::string>());
            std::vector<size_t> letters;
            for (size_t k = 0; k < w.size(); ++k)
                if (text::is_alnum(w[k])) letters.push_back(k);
            size_t j = letters[rng.index(letters.size())];
            std::u32string rep;
            if (text::is_digit(w[j])) rep = u32(std::to_string(((w[j] - U'0') + 2) % 10));
            else rep = u32(text::lower(u8(w[j]))) != U"e" ? U"e" : U"a";
            auto nw = w.substr(0, j) + rep + w.substr(j + 1);
            e["text"] = u8(nw);
            // ordet som innehåller tecknet (första förekomsten, som str.find)
            auto ws = text::split_ws(u8(w));
            std::string word;
            size_t wi = 0;
            for (size_t q = 0; q < ws.size(); ++q) {
                auto pos = w.find(u32(ws[q]));
                if (pos != std::u32string::npos && pos <= j && j < pos + u32(ws[q]).size()) {
                    word = ws[q];
                    break;
                }
            }
            wi = size_t(std::find(ws.begin(), ws.end(), word) - ws.begin());
            rec.update({{"kind", "Changed"}, {"ref_text", word}, {"smp_text", text::split_ws(u8(nw))[wi]}});
        } else if (kind == "size") {
            e["size_pt"] = round_to(e["size_pt"].get<double>() + 1.0, 1);
            rec.update({{"kind", "Style"}, {"ref_text", e["text"]}, {"attr", "size"}});
        } else if (kind == "font") {
            e["font"] = e["font"].get<std::string>().find("Sans") != std::string::npos ? "DejaVuSerif" : "DejaVuSans";
            rec.update({{"kind", "Style"}, {"ref_text", e["text"]}, {"attr", "family"}});
        } else if (kind == "bold") {
            std::string f = e["font"];
            if (f.find("Bold") != std::string::npos) continue;
            e["font"] = text::replace_all(f, "Condensed", "") + "-Bold";
            rec.update({{"kind", "Style"}, {"ref_text", e["text"]}, {"attr", "bold"}});
        } else if (kind == "color") {
            e["color"] = {{"cmyk", {0.0, 0.9, 0.9, 0.0}}};
            rec.update({{"kind", "Style"}, {"ref_text", e["text"]}, {"attr", "color"}});
        } else if (kind == "move") {
            double dx = rng.sign() * rng.uniform(1.0, 2.5);
            double dy = rng.sign() * rng.uniform(0.6, 1.5);
            e["pos_mm"] = {e["pos_mm"][0].get<double>() + dx, e["pos_mm"][1].get<double>() + dy};
            rec.update({{"kind", "Moved"}, {"ref_text", e["text"]}, {"shift_mm", {round_to(dx, 2), round_to(dy, 2)}}});
        } else if (kind == "word_delete") {
            auto ws = text::split_ws(e["text"]);
            int k = int(rng.integers(1, int64_t(ws.size())));
            rec.update({{"kind", "Missing"}, {"ref_text", ws[k]}});
            ws.erase(ws.begin() + k);
            e["text"] = text::join(ws, " ");
        } else if (kind == "word_add") {
            std::string w = rng.choice(std::vector<std::string>{"ekologisk", "extra", "ny", "svensk"});
            auto ws = text::split_ws(e["text"]);
            int k = int(rng.integers(1, int64_t(ws.size()) + 1));
            ws.insert(ws.begin() + k, w);
            e["text"] = text::join(ws, " ");
            rec.update({{"kind", "Added"}, {"smp_text", w}});
        } else if (kind == "swap") {
            json* other = nullptr;
            for (auto* o : text_elements(s))
                if (!used.count((*o)["id"]) && (*o)["id"] != e["id"] && (*o)["type"] == "text" &&
                    std::fabs((*o)["size_pt"].get<double>() - e["size_pt"].get<double>()) < 0.6 && (*o)["font"] == e["font"] &&
                    std::fabs((*o)["pos_mm"][0].get<double>() - e["pos_mm"][0].get<double>()) < 1) {
                    other = o;
                    break;
                }
            if (!other) continue;
            std::swap(e["pos_mm"], (*other)["pos_mm"]);
            used.insert((*other)["id"]);
            rec.update({{"kind", "Moved"}, {"ref_text", e["text"]}, {"other", (*other)["id"]}, {"other_text", (*other)["text"]}});
        }
        used.insert(e["id"]);
        truth.push_back(rec);
    }
    return {s, truth};
}

// Facit: finns en ordföljd (lika många ord ±1) i etikettens text som liknar värdet?
std::string near_or_missing(const std::string& val, const std::vector<std::string>& lines, double thr = 0.75) {
    int n = int(text::split_ws(val).size());
    double best = 0;
    for (auto& ln : lines) {
        auto ws = text::split_ws(ln);
        for (int s = 0; s < int(ws.size()); ++s)
            for (int k : {n - 1, n, n + 1})
                if (k >= 1 && s + k <= int(ws.size()))
                    best = std::max(best, difflib::char_ratio(val, text::join(std::vector<std::string>(ws.begin() + s, ws.begin() + s + k), " ")));
    }
    return best >= thr ? "near" : "missing";
}

// Läsbar text under en EAN-13: '7 310865 001234' (med kontrollsiffra).
std::string ean13_hri(const std::string& data12) {
    int sum = 0;
    for (int k = 0; k < 12; ++k) sum += (data12[k] - '0') * (k % 2 ? 3 : 1);
    std::string s = data12.substr(0, 12) + char('0' + (10 - sum % 10) % 10);
    return s.substr(0, 1) + " " + s.substr(1, 6) + " " + s.substr(7);
}

struct Value {
    std::string field, value, element;
};

// (fält, värde, element) som en verklig datafil skulle innehålla
std::vector<Value> label_values(const json& spec) {
    std::vector<Value> vals;
    static Regex digit(R"(\d)");
    for (auto& e : spec["elements"]) {
        std::string t = e["type"];
        if ((t == "text" || t == "variable") && !is_white(e)) {
            if (t == "variable") vals.push_back({"Batch", "/" + digit.replace(Regex::escape(e["text"]), "\\d") + "/", e["id"]});
            else {
                std::string txt = text::strip(e["text"]);
                // förväntade värden är fraser, inte alltid hela raden
                if (ulen(txt) > 28 && txt.find(',') != std::string::npos) txt = txt.substr(0, txt.find(','));
                vals.push_back({capitalize(e["id"]), txt, e["id"]});
            }
        } else if (t == "ean13") vals.push_back({"EAN", e["data"].get<std::string>().substr(0, 12), e["id"]});
        else if (t == "table")
            for (size_t r = 1; r < 3 && r < e["rows"].size(); ++r) vals.push_back({e["rows"][r][0], e["rows"][r][1], e["id"]});
    }
    return vals;
}

std::string csv_field(const std::string& v) {
    if (v.find_first_of(";\"\r\n") == std::string::npos) return v;
    return "\"" + text::replace_all(v, "\"", "\"\"") + "\"";
}

std::string jobspec_doc(const std::string& ref, const std::string& smp, const std::string& layout, const std::string& data) {
    json j = {{"schema", "printcmp.jobspec/1"}, {"reference", {{"path", ref}}}, {"sample", {{"path", smp}}},
              {"compare", {{"mode", "document"}, {"layout", layout}}}};
    if (!data.empty()) j["data_check"] = {{"path", data}};
    j["outputs"] = {{"dir", "out"}};
    return j.dump(2);
}

}  // namespace

fs::path make_book_case(uint64_t seed, const fs::path& out, const std::string& prof, int n_mut) {
    Rng rng(seed);
    int nch = int(rng.integers(4, 7));
    Doc doc = make_text(rng, nch, {6, 11});
    auto m = mutate_book(doc, rng, n_mut);
    char name[32];
    std::snprintf(name, sizeof name, "book_%05llu", (unsigned long long)seed);
    fs::path c = out / name;
    fs::create_directories(c);
    write_bytes(c / "manuscript.pdf", typeset(doc, "manuscript").first);
    std::optional<std::pair<int, int>> page_error;
    if (m.page_pending) {
        int n_pages = typeset(m.doc, prof).second;
        if (n_pages >= 4) {
            page_error = std::make_pair(int(rng.integers(3, n_pages + 1)), 1);   // från denna sida: ett nummer för högt
            for (auto& t : m.truth)
                if (t["kind"] == "PageNumber") t["page"] = page_error->first;
        } else {
            json keep = json::array();
            for (auto& t : m.truth)
                if (t["kind"] != "PageNumber") keep.push_back(t);
            m.truth = keep;
        }
    }
    write_bytes(c / (prof + ".pdf"), typeset(m.doc, prof, page_error).first);
    write_file((c / "jobspec.json").string(), jobspec_doc("manuscript.pdf", prof + ".pdf", "reflow", ""));
    int words = 0;
    for (auto& ch : doc.chapters)
        for (auto& p : ch.paras) words += int(p.toks.size());
    json truth = {{"schema", "printcmp.doctruth/1"}, {"generator", GENERATOR_VERSION}, {"seed", seed}, {"type", "book"},
                  {"profile", prof}, {"layout", "reflow"}, {"mutations", m.truth}, {"words", words}};
    write_file((c / "truth.json").string(), truth.dump(2));
    return c;
}

fs::path make_doc_label_case(uint64_t seed, const fs::path& out, const std::string& tpl, int n_mut) {
    Rng rng(seed);
    json ref = label_template(tpl);
    auto [smp, truth] = mutate_doc_label(ref, rng, n_mut);
    char name[32];
    std::snprintf(name, sizeof name, "label_%05llu", (unsigned long long)seed);
    fs::path c = out / name;
    fs::create_directories(c);
    write_bytes(c / "ref.pdf", render_label_pdf(ref));
    // Datafil: värden ur referensen; några görs avsiktligt fel (nära, saknas, fel streckkod)
    auto vals = label_values(ref);
    std::vector<std::string> parts, smp_lines;
    for (auto& e : smp["elements"]) {
        parts.push_back(e.value("text", ""));
        if (e["type"] == "text" || e["type"] == "variable") smp_lines.push_back(e["text"]);
    }
    for (auto& e : smp["elements"])
        if (e["type"] == "table")
            for (auto& r : e["rows"]) {
                std::vector<std::string> cells;
                for (auto& x : r) {
                    parts.push_back(x);
                    cells.push_back(x);
                }
                smp_lines.push_back(text::join(cells, " "));
            }
    std::string smp_text = text::join(parts, " ");
    struct Item {
        std::string field, expected, status;
    };
    std::vector<Item> data;
    for (auto& v : vals) {
        std::string st = (text::starts_with(v.value, "/") || v.field == "EAN") ? "found"
                         : smp_text.find(v.value) != std::string::npos   ? "found"
                                                                         : near_or_missing(v.value, smp_lines);
        data.push_back({v.field, v.value, st});
    }
    // avsiktligt felaktiga förväntningar
    std::vector<size_t> fields;
    for (size_t i = 0; i < data.size(); ++i)
        if (data[i].status == "found" && !text::starts_with(data[i].expected, "/") && data[i].field != "EAN") fields.push_back(i);
    if (!fields.empty()) {
        auto& d = data[fields[rng.index(fields.size())]];
        auto v = u32(d.expected);
        size_t k = v.size() / 2;
        for (size_t q = 0; q < v.size(); ++q)
            if (text::is_digit(v[q])) {
                k = q;
                break;
            }
        std::u32string rep = text::is_digit(v[k]) ? u32(std::to_string(((v[k] - U'0') + 1) % 10)) : (v[k] != U'o' ? U"o" : U"u");
        d.expected = u8(v.substr(0, k) + rep + v.substr(k + 1));
        d.status = near_or_missing(d.expected, smp_lines);
    }
    data.push_back({"Allergener", "Kan innehålla spår av nötter", "missing"});
    Item* ean = nullptr;
    for (auto& d : data)
        if (d.field == "EAN") ean = &d;
    if (ean && rng.random() < 0.5) {
        for (auto& e : smp["elements"])
            if (e["type"] == "ean13") {
                std::string digits = e["data"].get<std::string>().substr(0, 12);
                std::string old_hri = ean13_hri(digits);
                digits[7] = char('0' + ((digits[7] - '0') + 4) % 10);
                e["data"] = digits;
                ean->status = "mismatch";
                // siffrorna under streckkoden är text: den sista gruppen ändras (och kontrollsiffran)
                auto last = [](const std::string& h) { return h.substr(h.rfind(' ') + 1); };
                truth.push_back({{"id", "m" + std::to_string(truth.size() + 1)}, {"mutation", "ean"}, {"element", e["id"]},
                                 {"kind", "Changed"}, {"ref_text", last(old_hri)}, {"smp_text", last(ean13_hri(digits))},
                                 {"category", "Number"}});
                break;
            }
    }
    write_bytes(c / "smp.pdf", render_label_pdf(smp));
    bool use_csv = rng.random() < 0.5;
    int variant = 0;
    std::string data_file;
    if (use_csv) {
        // flera varianter i samma fil: den rätta + två lockbeten
        static Regex digit(R"(\d)");
        std::vector<std::vector<std::string>> rows;
        for (int v = 0; v < 3; ++v) {
            std::vector<std::string> row;
            for (auto& d : data) {
                std::string val = d.expected;
                if (v != 0 && !text::starts_with(val, "/")) {
                    if (digit.search(val)) {
                        std::string o;
                        for (char ch : val) o += (ch >= '0' && ch <= '9') ? char('0' + ((ch - '0') + v) % 10) : ch;
                        val = o;
                    } else val += v == 1 ? " special" : " light";
                }
                row.push_back(val);
            }
            rows.push_back(row);
        }
        auto order = rng.permutation(3);
        std::string csv;
        for (size_t i = 0; i < data.size(); ++i) csv += (i ? ";" : "") + csv_field(data[i].field);
        csv += "\r\n";
        for (int k : order) {
            for (size_t i = 0; i < rows[k].size(); ++i) csv += (i ? ";" : "") + csv_field(rows[k][i]);
            csv += "\r\n";
        }
        write_file((c / "expected.csv").string(), csv);
        variant = int(std::find(order.begin(), order.end(), 0) - order.begin());
        data_file = "expected.csv";
    } else {
        std::string txt = "# förväntade uppgifter (fält: värde)\n";
        for (auto& d : data) txt += d.field + ": " + d.expected + "\n";
        write_file((c / "expected.txt").string(), txt);
        data_file = "expected.txt";
    }
    write_file((c / "jobspec.json").string(), jobspec_doc("ref.pdf", "smp.pdf", "fixed", data_file));
    json jl = {{"schema", "printcmp.jobspec/1"}, {"reference", {{"path", "ref.pdf"}}}, {"sample", {{"path", "smp.pdf"}}},
               {"work_dpi", 400}, {"data_check", {{"path", data_file}}}, {"outputs", {{"dir", "out_label"}}}};
    write_file((c / "jobspec_label.json").string(), jl.dump(2));
    json items = json::object();
    for (auto& d : data) items[d.field] = d.status;
    json t = {{"schema", "printcmp.doctruth/1"}, {"generator", GENERATOR_VERSION}, {"seed", seed}, {"type", "label"},
              {"template", tpl}, {"layout", "fixed"}, {"mutations", truth},
              {"data_check", {{"file", data_file}, {"variant", variant}, {"items", items}}}};
    write_file((c / "truth.json").string(), t.dump(2));
    return c;
}

}  // namespace pc::testgen
