// Innehållsströmmar med qpdf: dekorfärger (stanslinje, lack, punktskrift) och PDF-lager (OCG).
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include "printcmp/core/regex.hpp"
#include "printcmp/pdf/document.hpp"
#include "printcmp/pdf/spots.hpp"
#include "printcmp/pdf/layers.hpp"
#include "qpdfio.hpp"

namespace pc::pdf {

namespace {

struct Instr {
    std::vector<QPDFObjectHandle> operands;
    std::string op;
    QPDFObjectHandle op_obj;
};

class Collector : public QPDFObjectHandle::ParserCallbacks {
public:
    std::vector<Instr> out;
    void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
        if (obj.isOperator()) {
            pending_.op = obj.getOperatorValue();
            pending_.op_obj = obj;
            out.push_back(std::move(pending_));
            pending_ = Instr{};
        } else {
            pending_.operands.push_back(obj);
        }
    }
    void handleEOF() override {}

private:
    Instr pending_;
};

std::vector<Instr> parse_page(QPDFPageObjectHelper& page) {
    Collector c;
    page.parseContents(&c);
    return std::move(c.out);
}

std::vector<Instr> parse_stream(QPDFObjectHandle stream) {
    Collector c;
    QPDFObjectHandle::parseContentStream(stream, &c);
    return std::move(c.out);
}

std::string unparse(const std::vector<Instr>& ins) {
    std::string s;
    for (const auto& i : ins) {
        for (const auto& o : i.operands) {
            s += o.unparse();
            s += ' ';
        }
        s += i.op;
        s += '\n';
    }
    return s;
}

Bytes save(QPDF& q) {
    QPDFWriter w(q);
    w.setOutputMemory();
    w.write();
    auto buf = w.getBufferSharedPointer();
    return Bytes(buf->getBuffer(), buf->getBuffer() + buf->getSize());
}

std::shared_ptr<QPDF> load(const Bytes& data) {
    auto q = std::make_shared<QPDF>();
    q->setSuppressWarnings(true);
    q->processMemoryFile("input.pdf", reinterpret_cast<const char*>(data.data()), data.size());
    return q;
}

QPDFPageObjectHelper page_of(QPDF& q, int index) {
    auto pages = QPDFPageDocumentHelper(q).getAllPages();
    if (index < 0 || index >= (int)pages.size()) throw Error(ErrorCode::InvalidArgument, "sidan finns inte");
    return pages[index];
}

double num(const QPDFObjectHandle& o) { return o.isNumber() ? o.getNumericValue() : 0.0; }

using M6 = std::array<double, 6>;
const M6 IDENTITY{1, 0, 0, 1, 0, 0};

M6 mul(const M6& a, const M6& b) {
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
}

Pt apply(const M6& m, double x, double y) { return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]}; }

std::vector<Pt> bezier(Pt p0, Pt p1, Pt p2, Pt p3, int n = 12) {
    std::vector<Pt> pts;
    for (int i = 1; i <= n; ++i) {
        double t = double(i) / n, mt = 1 - t;
        pts.push_back({mt * mt * mt * p0.x + 3 * mt * mt * t * p1.x + 3 * mt * t * t * p2.x + t * t * t * p3.x,
                       mt * mt * mt * p0.y + 3 * mt * mt * t * p1.y + 3 * mt * t * t * p2.y + t * t * t * p3.y});
    }
    return pts;
}

std::string strip_slash(const std::string& s) { return !s.empty() && s[0] == '/' ? s.substr(1) : s; }

std::map<std::string, std::string> colorspace_names(QPDFObjectHandle res) {
    std::map<std::string, std::string> out;
    if (!res.isDictionary() || !res.hasKey("/ColorSpace")) return out;
    auto cs = res.getKey("/ColorSpace");
    if (!cs.isDictionary()) return out;
    for (const auto& key : cs.getKeys()) {
        auto v = cs.getKey(key);
        if (!v.isArray() || v.getArrayNItems() < 2) continue;
        auto k0 = v.getArrayItem(0);
        if (!k0.isName()) continue;
        if (k0.getName() == "/Separation" && v.getArrayItem(1).isName()) {
            out[key] = strip_slash(v.getArrayItem(1).getName());
        } else if (k0.getName() == "/DeviceN" && v.getArrayItem(1).isArray()) {
            std::string names;
            auto arr = v.getArrayItem(1);
            for (int i = 0; i < arr.getArrayNItems(); ++i) {
                if (i) names += "+";
                if (arr.getArrayItem(i).isName()) names += strip_slash(arr.getArrayItem(i).getName());
            }
            out[key] = names;
        }
    }
    return out;
}

}  // namespace

namespace detail {

std::shared_ptr<QPDF> qpdf_load(const Bytes& data) { return load(data); }
Bytes qpdf_save(QPDF& q) { return save(q); }

}  // namespace detail

SpotRules default_spot_rules() {
    // ordningen spelar roll: första matchande roll gäller
    return {{"braille", "(?i)braille|punktskrift|blindskrift|emboss|prägl"},
            {"dieline", "(?i)cut|die|stans|kiss|crease|perf"},
            {"varnish", "(?i)varnish|lack|foil|folie|hot ?stamp"}};
}

std::vector<std::vector<Pt>> SpotAnalysis::polygons(const std::string& role) const {
    std::vector<std::vector<Pt>> out;
    for (const auto& p : paths)
        if (p.role == role) out.insert(out.end(), p.polygons.begin(), p.polygons.end());
    return out;
}

double polygon_area(const std::vector<Pt>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const Pt& q = p[i];
        const Pt& r = p[(i + 1) % p.size()];
        a += q.x * r.y - r.x * q.y;
    }
    return std::abs(a) / 2;
}

SpotAnalysis analyze_spots(const Bytes& data, int page_index, const SpotRules& rules) {
    std::vector<std::pair<std::string, Regex>> compiled;
    for (const auto& [role, rx] : rules) compiled.emplace_back(role, Regex(rx));
    auto role_of = [&](const std::optional<std::string>& spot) -> std::string {
        if (!spot) return "";
        for (const auto& [role, rx] : compiled)
            if (rx.search(*spot)) return role;
        return "";
    };

    auto q = load(data);
    auto page = page_of(*q, page_index);
    auto cs_map = colorspace_names(page.getAttribute("/Resources", false));
    SpotAnalysis result;
    std::set<std::string> names;
    for (const auto& [k, v] : cs_map)
        if (!v.empty()) names.insert(v);
    result.spot_names.assign(names.begin(), names.end());

    auto ops = parse_page(page);
    std::vector<Instr> new_ops;
    M6 ctm = IDENTITY;
    struct Saved { M6 ctm; std::optional<std::string> s, f; };
    std::vector<Saved> stack;
    std::optional<std::string> stroke_spot, fill_spot;
    std::vector<std::vector<Pt>> subpaths;
    std::vector<Pt> cur;
    bool have_start = false;
    auto close_cur = [&] {
        if (!cur.empty()) subpaths.push_back(cur);
        cur.clear();
    };
    auto lookup = [&](const QPDFObjectHandle& o) -> std::optional<std::string> {
        if (!o.isName()) return std::nullopt;
        auto it = cs_map.find(o.getName());
        if (it == cs_map.end()) return std::nullopt;
        return it->second;
    };
    static const std::set<std::string> paint_ops{"S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n"};

    for (auto& ins : ops) {
        const std::string& o = ins.op;
        const auto& a = ins.operands;
        bool keep = true;
        if (o == "q") stack.push_back({ctm, stroke_spot, fill_spot});
        else if (o == "Q") {
            if (!stack.empty()) {
                ctm = stack.back().ctm;
                stroke_spot = stack.back().s;
                fill_spot = stack.back().f;
                stack.pop_back();
            }
        } else if (o == "cm" && a.size() == 6) {
            M6 m{num(a[0]), num(a[1]), num(a[2]), num(a[3]), num(a[4]), num(a[5])};
            ctm = mul(m, ctm);
        } else if (o == "CS" && !a.empty()) stroke_spot = lookup(a[0]);
        else if (o == "cs" && !a.empty()) fill_spot = lookup(a[0]);
        else if (o == "G" || o == "RG" || o == "K") stroke_spot.reset();
        else if (o == "g" || o == "rg" || o == "k") fill_spot.reset();
        else if (o == "m" && a.size() >= 2) {
            close_cur();
            cur = {apply(ctm, num(a[0]), num(a[1]))};
            have_start = true;
        } else if (o == "l" && a.size() >= 2) cur.push_back(apply(ctm, num(a[0]), num(a[1])));
        else if ((o == "c" && a.size() >= 6) || ((o == "v" || o == "y") && a.size() >= 4)) {
            Pt p0 = cur.empty() ? Pt{0, 0} : cur.back(), p1, p2, p3;
            if (o == "c") {
                p1 = apply(ctm, num(a[0]), num(a[1]));
                p2 = apply(ctm, num(a[2]), num(a[3]));
                p3 = apply(ctm, num(a[4]), num(a[5]));
            } else if (o == "v") {
                p1 = p0;
                p2 = apply(ctm, num(a[0]), num(a[1]));
                p3 = apply(ctm, num(a[2]), num(a[3]));
            } else {
                p1 = apply(ctm, num(a[0]), num(a[1]));
                p2 = apply(ctm, num(a[2]), num(a[3]));
                p3 = p2;
            }
            auto b = bezier(p0, p1, p2, p3);
            cur.insert(cur.end(), b.begin(), b.end());
        } else if (o == "re" && a.size() >= 4) {
            close_cur();
            double x = num(a[0]), y = num(a[1]), w = num(a[2]), h = num(a[3]);
            subpaths.push_back({apply(ctm, x, y), apply(ctm, x + w, y), apply(ctm, x + w, y + h), apply(ctm, x, y + h)});
        } else if (o == "h") {
            if (!cur.empty() && have_start) cur.push_back(cur.front());
        } else if (paint_ops.count(o)) {
            close_cur();
            bool strokes = o == "S" || o == "s" || o == "B" || o == "B*" || o == "b" || o == "b*";
            bool fills = o == "f" || o == "F" || o == "f*" || o == "B" || o == "B*" || o == "b" || o == "b*";
            std::string s_role = strokes ? role_of(stroke_spot) : "";
            std::string f_role = fills ? role_of(fill_spot) : "";
            std::vector<std::vector<Pt>> polys;
            for (auto& p : subpaths)
                if (p.size() >= 2) polys.push_back(p);
            if (!s_role.empty() && !polys.empty()) result.paths.push_back({s_role, *stroke_spot, "stroke", polys});
            if (!f_role.empty() && !polys.empty()) result.paths.push_back({f_role, *fill_spot, "fill", polys});
            // sanering: ta bort målningen i icke-tryckande färger
            if (o != "n" && (!s_role.empty() || !f_role.empty())) {
                bool stroke_keep = strokes && s_role.empty();
                bool fill_keep = fills && f_role.empty();
                bool even_odd = o == "f*" || o == "B*" || o == "b*";
                std::string repl = (stroke_keep && fill_keep) ? o : fill_keep ? (even_odd ? "f*" : "f")
                                                                 : stroke_keep ? "S" : "n";
                Instr r;
                r.op = repl;
                new_ops.push_back(r);
                keep = false;
            }
            subpaths.clear();
            have_start = false;
        }
        if ((o == "Tj" || o == "TJ" || o == "'" || o == "\"") && !role_of(fill_spot).empty())
            keep = false;   // text i en icke-tryckande/utesluten dekorfärg
        if (keep) new_ops.push_back(ins);
    }
    std::string content = unparse(new_ops);
    page.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(q.get(), content));
    result.sanitized_pdf = save(*q);
    return result;
}

// ---------------------------------------------------------------- lager

namespace {

std::string ocg_name(QPDFObjectHandle o) {
    if (!o.isDictionary() || !o.hasKey("/Name")) return "";
    auto n = o.getKey("/Name");
    return n.isString() ? n.getUTF8Value() : n.isName() ? strip_slash(n.getName()) : "";
}

std::vector<QPDFObjectHandle> ocgs_of(QPDFObjectHandle obj) {
    if (!obj.isDictionary()) return {};
    std::string t = obj.hasKey("/Type") && obj.getKey("/Type").isName() ? obj.getKey("/Type").getName() : "";
    if (t == "/OCG") return {obj};
    if (t == "/OCMD") {
        if (!obj.hasKey("/OCGs")) return {};
        auto g = obj.getKey("/OCGs");
        if (g.isArray()) return g.getArrayAsVector();
        return {g};
    }
    return {};
}

std::vector<Regex> compile_rules(const std::vector<std::string>& rules) {
    static const std::string special = ".*+?[]()|^$\\";
    std::vector<Regex> out;
    for (const auto& r : rules) {
        bool is_rx = r.find_first_of(special) != std::string::npos;
        out.emplace_back(is_rx ? r : "^" + Regex::escape(r) + "$", true);
    }
    return out;
}

struct Visibility {
    std::vector<Regex> inc, exc;
    bool operator()(const std::string& name) const {
        auto any = [&](const std::vector<Regex>& v) {
            for (const auto& r : v)
                if (r.search(name)) return true;
            return false;
        };
        if (!inc.empty() && !any(inc)) return false;
        return !(!exc.empty() && any(exc));
    }
};

bool oc_visible(QPDFObjectHandle oc, const Visibility& visible) {
    auto groups = ocgs_of(oc);
    if (groups.empty()) return true;
    std::vector<bool> states;
    for (auto& g : groups) states.push_back(visible(ocg_name(g)));
    std::string policy = "/AnyOn";
    if (oc.isDictionary() && oc.hasKey("/Type") && oc.getKey("/Type").isName() && oc.getKey("/Type").getName() == "/OCMD" &&
        oc.hasKey("/P") && oc.getKey("/P").isName())
        policy = oc.getKey("/P").getName();
    bool all = std::all_of(states.begin(), states.end(), [](bool b) { return b; });
    bool any = std::any_of(states.begin(), states.end(), [](bool b) { return b; });
    if (policy == "/AllOn") return all;
    if (policy == "/AnyOff") return !all;
    if (policy == "/AllOff") return !any;
    return any;
}

std::vector<Instr> filter_stream(const std::vector<Instr>& ops, QPDFObjectHandle res, const Visibility& visible,
                                 std::set<QPDFObjGen>& seen_forms) {
    QPDFObjectHandle props = res.isDictionary() && res.hasKey("/Properties") ? res.getKey("/Properties") : QPDFObjectHandle::newNull();
    QPDFObjectHandle xobjs = res.isDictionary() && res.hasKey("/XObject") ? res.getKey("/XObject") : QPDFObjectHandle::newNull();
    std::vector<Instr> out;
    std::vector<bool> stack;
    int hidden_depth = 0;
    for (const auto& ins : ops) {
        const std::string& o = ins.op;
        if (o == "BDC" || o == "BMC") {
            bool hide = false;
            if (o == "BDC" && ins.operands.size() >= 2 && ins.operands[0].isName() && ins.operands[0].getName() == "/OC") {
                auto ref = ins.operands[1];
                QPDFObjectHandle oc = ref;
                if (ref.isName()) oc = props.isDictionary() && props.hasKey(ref.getName()) ? props.getKey(ref.getName())
                                                                                          : QPDFObjectHandle::newNull();
                if (!oc.isNull() && !oc_visible(oc, visible)) hide = true;
            }
            stack.push_back(hide);
            if (hide || hidden_depth) {
                ++hidden_depth;
                continue;
            }
        } else if (o == "EMC") {
            if (!stack.empty()) stack.pop_back();
            if (hidden_depth) {
                --hidden_depth;
                continue;
            }
        } else if (hidden_depth) {
            continue;
        } else if (o == "Do" && !ins.operands.empty() && ins.operands[0].isName() && xobjs.isDictionary() &&
                   xobjs.hasKey(ins.operands[0].getName())) {
            auto xo = xobjs.getKey(ins.operands[0].getName());
            auto dict = xo.isStream() ? xo.getDict() : xo;
            if (dict.hasKey("/OC") && !oc_visible(dict.getKey("/OC"), visible)) continue;
            if (dict.hasKey("/Subtype") && dict.getKey("/Subtype").isName() && dict.getKey("/Subtype").getName() == "/Form" &&
                !seen_forms.count(xo.getObjGen())) {
                seen_forms.insert(xo.getObjGen());
                auto sub = filter_stream(parse_stream(xo), dict.hasKey("/Resources") ? dict.getKey("/Resources")
                                                                                      : QPDFObjectHandle::newNull(),
                                         visible, seen_forms);
                xo.replaceStreamData(unparse(sub), QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
            }
        }
        out.push_back(ins);
    }
    return out;
}

}  // namespace

namespace {

LayerInfo layers_of(QPDF& q, int page_index);

}  // namespace

LayerInfo list_layers(const Bytes& data, int page_index) {
    auto q = load(data);
    return layers_of(*q, page_index);
}

std::vector<LayerInfo> list_layers(const Bytes& data) {
    auto q = load(data);
    int n = int(QPDFPageDocumentHelper(*q).getAllPages().size());
    std::vector<LayerInfo> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.push_back(layers_of(*q, i));
    return out;
}

namespace {

LayerInfo layers_of(QPDF& qpdf, int page_index) {
    QPDF* q = &qpdf;
    LayerInfo info;
    auto root = q->getRoot();
    auto page = page_of(*q, page_index);
    auto res = page.getAttribute("/Resources", false);
    if (root.hasKey("/OCProperties")) {
        auto ocp = root.getKey("/OCProperties");
        std::set<QPDFObjGen> off, used;
        if (ocp.hasKey("/D") && ocp.getKey("/D").hasKey("/OFF")) {
            for (auto& o : ocp.getKey("/D").getKey("/OFF").getArrayAsVector()) off.insert(o.getObjGen());
        }
        if (res.isDictionary() && res.hasKey("/Properties")) {
            auto props = res.getKey("/Properties");
            for (const auto& k : props.getKeys())
                for (auto& g : ocgs_of(props.getKey(k))) used.insert(g.getObjGen());
        }
        if (res.isDictionary() && res.hasKey("/XObject")) {
            auto xs = res.getKey("/XObject");
            for (const auto& k : xs.getKeys()) {
                auto xo = xs.getKey(k);
                auto d = xo.isStream() ? xo.getDict() : xo;
                if (d.hasKey("/OC"))
                    for (auto& g : ocgs_of(d.getKey("/OC"))) used.insert(g.getObjGen());
            }
        }
        if (ocp.hasKey("/OCGs"))
            for (auto& g : ocp.getKey("/OCGs").getArrayAsVector())
                info.ocgs.push_back({ocg_name(g), !off.count(g.getObjGen()), used.count(g.getObjGen()) > 0});
    }
    std::set<std::string> spots;
    if (res.isDictionary() && res.hasKey("/ColorSpace")) {
        auto cs = res.getKey("/ColorSpace");
        for (const auto& k : cs.getKeys()) {
            auto v = cs.getKey(k);
            if (!v.isArray() || v.getArrayNItems() < 2 || !v.getArrayItem(0).isName()) continue;
            if (v.getArrayItem(0).getName() == "/Separation" && v.getArrayItem(1).isName())
                spots.insert(strip_slash(v.getArrayItem(1).getName()));
            else if (v.getArrayItem(0).getName() == "/DeviceN" && v.getArrayItem(1).isArray())
                for (auto& n : v.getArrayItem(1).getArrayAsVector())
                    if (n.isName()) spots.insert(strip_slash(n.getName()));
        }
    }
    spots.erase("All");
    spots.erase("None");
    info.spots.assign(spots.begin(), spots.end());
    return info;
}

}  // namespace

Bytes filter_layers(const Bytes& data, int page_index, const std::vector<std::string>& include,
                    const std::vector<std::string>& exclude, std::map<std::string, bool>* decision) {
    if (include.empty() && exclude.empty()) return data;
    Visibility visible{compile_rules(include), compile_rules(exclude)};
    auto q = load(data);
    auto root = q->getRoot();
    if (root.hasKey("/OCProperties")) {
        auto ocp = root.getKey("/OCProperties");
        auto on = QPDFObjectHandle::newArray(), off = QPDFObjectHandle::newArray();
        if (ocp.hasKey("/OCGs"))
            for (auto& g : ocp.getKey("/OCGs").getArrayAsVector()) {
                bool v = visible(ocg_name(g));
                if (decision) (*decision)[ocg_name(g)] = v;
                (v ? on : off).appendItem(g);
            }
        if (ocp.hasKey("/D")) {
            auto d = ocp.getKey("/D");
            d.replaceKey("/ON", on);
            d.replaceKey("/OFF", off);
            d.replaceKey("/BaseState", QPDFObjectHandle::newName("/ON"));
            if (d.hasKey("/AS")) d.removeKey("/AS");
        }
    }
    auto page = page_of(*q, page_index);
    std::set<QPDFObjGen> seen;
    auto ops = filter_stream(parse_page(page), page.getAttribute("/Resources", false), visible, seen);
    page.getObjectHandle().replaceKey("/Contents", QPDFObjectHandle::newStream(q.get(), unparse(ops)));
    return save(*q);
}

SpotRules excluded_spot_rule(const std::vector<std::string>& exclude_spots) {
    if (exclude_spots.empty()) return {};
    static const std::string special = ".*+?[]()|^$\\";
    std::string rx = "(?i)";
    for (size_t i = 0; i < exclude_spots.size(); ++i) {
        const auto& r = exclude_spots[i];
        std::string p = r.find_first_of(special) != std::string::npos ? r : "^" + Regex::escape(r) + "$";
        rx += (i ? "|" : "") + std::string("(?:") + p + ")";
    }
    return {{"excluded", rx}};
}

}  // namespace pc::pdf
