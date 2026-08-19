#include "api/luals_provider.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace lime {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool startsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string_view word(std::string_view& s) {
    s = trim(s);
    const std::size_t sp = s.find_first_of(" \t");
    const std::string_view w = s.substr(0, sp);
    s = (sp == std::string_view::npos) ? std::string_view{} : trim(s.substr(sp));
    return w;
}

std::string mapType(std::string_view t, bool& optional) {
    optional = false;
    t = trim(t);
    if (!t.empty() && t.back() == '?') { optional = true; t.remove_suffix(1); }

    if (t.find('|') != std::string_view::npos) return "any";
    if (t.empty() || t == "nil") return "";
    if (t == "int" || t == "integer") return "integer";
    if (t == "float" || t == "double") return "number";
    if (startsWith(t, "fun(") || t == "function") return "function";
    if (startsWith(t, "table<") || startsWith(t, "{")) return "table";
    return std::string(t);
}

struct Param {
    std::string name, type;
    std::string raw;
    bool optional = false;
};

struct Pending {
    std::string doc;
    std::string className;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<Param> params;
    std::string returnType;
    std::vector<std::string> overloads;
    struct Op { std::string op, rhs, ret; };
    std::vector<Op> operators;
    void clear() { *this = Pending{}; }
};

bool guessPure(std::string_view owner, std::string_view fn, bool hasReturn) {
    if (!hasReturn) return false;
    if (fn == "new") return true;
    for (std::string_view p : {"get", "is", "has", "to", "find", "length",
                               "normalized", "dot", "cross", "distance"})
        if (startsWith(fn, p)) return true;
    if (owner == "Vec2" || owner == "Vec3" || owner == "Vec4") return true;
    if (startsWith(owner, "Lime.Math")) return true;
    return false;
}

std::string categoryFor(std::string_view owner) {
    if (owner == "Lime") return "Lime";
    if (startsWith(owner, "Lime.")) {
        std::string c(owner.substr(5));
        return c.empty() ? "Lime" : c;
    }
    if (owner == "Vec2" || owner == "Vec3" || owner == "Vec4")
        return "Math/Vector";
    return "Objects/" + std::string(owner);
}

bool isEventClassName(std::string_view n) {
    const std::size_t p = n.find("_on");
    return p != std::string_view::npos && p + 3 < n.size()
           && std::isupper(static_cast<unsigned char>(n[p + 3])) != 0;
}

const char* operatorSymbol(std::string_view op) {
    if (op == "add") return "+";
    if (op == "sub") return "-";
    if (op == "mul") return "*";
    if (op == "div") return "/";
    if (op == "mod") return "%";
    if (op == "pow") return "^";
    if (op == "unm") return "-";
    if (op == "eq")  return "==";
    if (op == "lt")  return "<";
    if (op == "le")  return "<=";
    if (op == "len") return "#";
    if (op == "concat") return "..";
    return nullptr;
}

}

std::string humanizeIdentifier(std::string_view ident) {
    if (ident.empty()) return {};

    std::vector<std::string> words;
    std::string cur;
    for (std::size_t i = 0; i < ident.size(); ++i) {
        const char c = ident[i];
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLower =
            i > 0 && std::islower(static_cast<unsigned char>(ident[i - 1])) != 0;
        const bool prevUpper =
            i > 0 && std::isupper(static_cast<unsigned char>(ident[i - 1])) != 0;
        const bool nextLower =
            i + 1 < ident.size()
            && std::islower(static_cast<unsigned char>(ident[i + 1])) != 0;

        bool boundary = false;
        if (upper && prevLower) boundary = true;
        else if (upper && prevUpper && nextLower && cur.size() >= 2)
            boundary = true;

        if (boundary && !cur.empty()) { words.push_back(cur); cur.clear(); }
        if (c == '_') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } continue; }
        cur.push_back(c);
    }
    if (!cur.empty()) words.push_back(cur);

    std::string out;
    for (std::string& w : words) {
        std::size_t caps = 0;
        for (char c : w)
            if (std::isupper(static_cast<unsigned char>(c))) ++caps;
        const bool acronym =
            caps >= 2 && caps >= w.size() - 1;

        if (!acronym)
            w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
        if (!out.empty()) out += ' ';
        out += w;
    }
    return out;
}

std::string cleanDoc(std::string_view doc) {
    std::string out;
    out.reserve(doc.size());
    for (std::size_t i = 0; i < doc.size(); ++i) {
        if (doc[i] == '*' || doc[i] == '`') continue;
        if (std::isspace(static_cast<unsigned char>(doc[i]))) {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        out += doc[i];
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();

    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i] != '.') continue;
        if (out[i + 1] != ' ') continue;
        if (i + 2 >= out.size()) break;
        if (std::isupper(static_cast<unsigned char>(out[i + 2])) == 0) continue;
        if (i >= 1 && std::isdigit(static_cast<unsigned char>(out[i - 1]))
            && std::isdigit(static_cast<unsigned char>(out[i + 2])))
            continue;
        if (i >= 2 && out[i - 2] == '.'
            && std::isalpha(static_cast<unsigned char>(out[i - 1])) != 0)
            continue;
        out.resize(i + 1);
        break;
    }
    return out;
}

std::string eventClassToPath(std::string_view className) {
    std::string out(className);
    std::replace(out.begin(), out.end(), '_', '.');
    return out;
}

std::vector<std::pair<std::string, std::string>>
parseFunSignature(std::string_view fun) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::size_t open = fun.find('(');
    if (open == std::string_view::npos) return out;

    int depth = 0;
    std::size_t close = std::string_view::npos;
    for (std::size_t i = open; i < fun.size(); ++i) {
        if (fun[i] == '(') ++depth;
        else if (fun[i] == ')') { if (--depth == 0) { close = i; break; } }
    }
    if (close == std::string_view::npos) return out;

    std::string_view args = fun.substr(open + 1, close - open - 1);
    depth = 0;
    std::size_t start = 0;
    auto flush = [&](std::string_view piece) {
        piece = trim(piece);
        if (piece.empty()) return;
        const std::size_t colon = piece.find(':');
        if (colon == std::string_view::npos) {
            out.emplace_back(std::string(piece), "any");
        } else {
            out.emplace_back(std::string(trim(piece.substr(0, colon))),
                             std::string(trim(piece.substr(colon + 1))));
        }
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == '(' || args[i] == '<' || args[i] == '{') ++depth;
        else if (args[i] == ')' || args[i] == '>' || args[i] == '}') --depth;
        else if (args[i] == ',' && depth == 0) {
            flush(args.substr(start, i - start));
            start = i + 1;
        }
    }
    flush(args.substr(start));
    return out;
}

LuaLSProvider::LuaLSProvider(std::string apiPath, std::string enumsPath, int priority)
    : apiPath_(std::move(apiPath)), enumsPath_(std::move(enumsPath)),
      priority_(priority) {}

void LuaLSProvider::collect(TypeRegistry& types, std::vector<NodeDesc>& out,
                            Diagnostics& diag) {
    std::ifstream f(apiPath_, std::ios::binary);
    if (!f) {
        diag.error("cannot open API stub " + apiPath_);
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    Pending pend;
    auto typeId = [&](const std::string& n) {
        return types.intern(TypeDesc{n, 0, {}, false, {}});
    };

    struct PendingEvent { std::size_t index; std::string receiver; };
    std::vector<PendingEvent> pendingEvents;
    std::set<std::string> constructible;

    auto addParams = [&](NodeDesc& d, const std::vector<Param>& ps) {
        for (const Param& p : ps) {
            if (p.type.empty()) continue;
            PinDesc pin;
            pin.name = p.name;
            pin.dir = PinDir::In;
            pin.kind = PinKind::Data;
            pin.optional = p.optional;
            pin.type = typeId(p.type);
            d.pins.push_back(std::move(pin));
        }
    };

    auto addExec = [&](NodeDesc& d) {
        PinDesc in;  in.name = "in";   in.dir = PinDir::In;   in.kind = PinKind::Exec;
        PinDesc o;   o.name = "out";   o.dir = PinDir::Out;   o.kind = PinKind::Exec;
        d.pins.insert(d.pins.begin(), std::move(in));
        d.pins.push_back(std::move(o));
    };

    const std::string_view all(text);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t nl = all.find('\n', pos);
        const std::string_view line = all.substr(
            pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? all.size() + 1 : nl + 1;

        std::string_view s = trim(line);
        if (s.empty()) continue;

        if (startsWith(s, "---")) {
            std::string_view a = trim(s.substr(3));
            if (a.empty() || a == "@meta") continue;

            if (a.front() != '@') {
                if (!pend.doc.empty()) pend.doc += ' ';
                pend.doc += std::string(a);
                continue;
            }
            a.remove_prefix(1);
            const std::string_view tag = word(a);

            if (tag == "class") {
                pend.className = std::string(word(a));
            } else if (tag == "field") {
                const std::string_view n = word(a);
                const std::string_view t = word(a);
                bool opt = false;
                pend.fields.emplace_back(std::string(n), mapType(t, opt));
            } else if (tag == "param") {
                const std::string_view n = word(a);
                std::string_view t = a;
                if (!startsWith(t, "fun(")) t = word(a);
                bool opt = false;
                Param p;
                p.name = std::string(n);
                p.raw = std::string(trim(t));
                p.type = mapType(t, opt);
                p.optional = opt;
                pend.params.push_back(std::move(p));
            } else if (tag == "return") {
                bool opt = false;
                pend.returnType = mapType(word(a), opt);
            } else if (tag == "overload") {
                pend.overloads.emplace_back(a);
            } else if (tag == "operator") {
                const std::size_t paren = a.find('(');
                const std::size_t colon = a.rfind(':');
                if (colon == std::string_view::npos) continue;
                Pending::Op op;
                op.ret = std::string(trim(a.substr(colon + 1)));
                if (paren != std::string_view::npos && paren < colon) {
                    op.op = std::string(trim(a.substr(0, paren)));
                    const std::size_t close = a.rfind(')', colon);
                    if (close != std::string_view::npos && close > paren)
                        op.rhs = std::string(
                            trim(a.substr(paren + 1, close - paren - 1)));
                } else {
                    op.op = std::string(trim(a.substr(0, colon)));
                }
                pend.operators.push_back(std::move(op));
            }
            continue;
        }

        if (startsWith(s, "function ")) {
            std::string_view rest = trim(s.substr(9));
            const std::size_t paren = rest.find('(');
            if (paren == std::string_view::npos) { pend.clear(); continue; }
            const std::string path(trim(rest.substr(0, paren)));

            const std::size_t colon = path.find(':');
            const bool isMethod = colon != std::string::npos;
            const std::string owner =
                isMethod ? path.substr(0, colon)
                         : path.substr(0, path.find_last_of('.') == std::string::npos
                                              ? 0 : path.find_last_of('.'));
            const std::string fname =
                isMethod ? path.substr(colon + 1)
                         : (path.find_last_of('.') == std::string::npos
                                ? path : path.substr(path.find_last_of('.') + 1));

            if (isEventClassName(owner)) {
                if (fname == "hook") {
                    NodeDesc d;
                    d.id = eventClassToPath(owner);
                    d.display =
                        humanizeIdentifier(d.id.substr(d.id.find_last_of('.') + 1));
                    d.emit = "struct:event";
                    d.isEvent = true;
                    d.doc = cleanDoc(pend.doc);

                    const std::string recv = d.id.substr(0, d.id.find_last_of('.'));
                    d.category = "Events/" + recv;
                    d.target = d.id;
                    pendingEvents.push_back({out.size(), recv});

                    PinDesc o;
                    o.name = "out"; o.dir = PinDir::Out; o.kind = PinKind::Exec;
                    d.pins.push_back(std::move(o));

                    for (const Param& p : pend.params) {
                        if (!startsWith(p.raw, "fun(")) continue;
                        for (const auto& [an, at] : parseFunSignature(p.raw)) {
                            bool opt = false;
                            const std::string mapped = mapType(at, opt);
                            if (mapped.empty()) continue;
                            PinDesc arg;
                            arg.name = an;
                            arg.dir = PinDir::Out;
                            arg.kind = PinKind::Data;
                            arg.type = typeId(mapped);
                            d.pins.push_back(std::move(arg));
                        }
                        break;
                    }

                    if (d.doc.empty())
                        d.doc = "Runs when " + d.id + " fires.";
                    out.push_back(std::move(d));
                }
                pend.clear();
                continue;
            }

            const bool hasReturn = !pend.returnType.empty();

            NodeDesc d;
            d.doc = cleanDoc(pend.doc);
            d.category = categoryFor(owner);
            d.pure = guessPure(owner, fname, hasReturn);

            if (isMethod) {
                d.id = path;
                d.display = humanizeIdentifier(fname);
                d.emit = "method";
                d.target = fname;
                PinDesc self;
                self.name = "self";
                self.dir = PinDir::In;
                self.kind = PinKind::Data;
                self.type = typeId(owner);
                d.pins.push_back(std::move(self));
            } else {
                d.id = path;
                d.display = (fname == "new")
                                ? ("Make " + humanizeIdentifier(owner))
                                : humanizeIdentifier(fname);
                d.emit = (fname == "new") ? "construct" : "call";
                d.target = path;
                if (fname == "new") constructible.insert(owner);
            }

            std::vector<Param> params = pend.params;
            if (params.empty() && !pend.overloads.empty()) {
                std::size_t best = 0, bestN = 0;
                for (std::size_t i = 0; i < pend.overloads.size(); ++i) {
                    const auto sig = parseFunSignature(pend.overloads[i]);
                    if (sig.size() > bestN) { bestN = sig.size(); best = i; }
                }
                for (const auto& [an, at] : parseFunSignature(pend.overloads[best])) {
                    if (an == "self") continue;
                    bool opt = false;
                    Param p;
                    p.name = an;
                    p.type = mapType(at, opt);
                    p.optional = opt;
                    params.push_back(std::move(p));
                }
            }
            addParams(d, params);

            if (hasReturn) {
                PinDesc r;
                r.name = "ret";
                r.dir = PinDir::Out;
                r.kind = PinKind::Data;
                r.type = typeId(pend.returnType);
                d.pins.push_back(std::move(r));
            }
            if (!d.pure) addExec(d);

            if (d.doc.empty()) {
                std::string sig = d.id + "(";
                bool first = true;
                for (const PinDesc& p : d.pins) {
                    if (p.dir != PinDir::In || p.kind != PinKind::Data) continue;
                    if (p.name == "self") continue;
                    if (!first) sig += ", ";
                    sig += p.name;
                    first = false;
                }
                sig += ")";
                d.doc = "Calls " + sig
                        + (hasReturn ? ", returning " + pend.returnType + "." : ".");
            }

            out.push_back(std::move(d));
            pend.clear();
            continue;
        }

        if (s.find("= {}") != std::string_view::npos && !pend.className.empty()) {
            const std::string cls = pend.className;
            typeId(cls);

            for (const auto& [fn, ft] : pend.fields) {
                if (ft.empty()) continue;
                if (isEventClassName(ft)) continue;
                NodeDesc d;
                d.id = cls + "." + fn;
                d.display = humanizeIdentifier(fn);
                d.category = categoryFor(cls) + "/Fields";
                d.doc = "Reads the " + humanizeIdentifier(fn) + " of this object.";
                d.emit = "index";
                d.target = fn;
                d.pure = true;
                PinDesc self;
                self.name = "self"; self.dir = PinDir::In; self.kind = PinKind::Data;
                self.type = typeId(cls);
                d.pins.push_back(std::move(self));
                PinDesc r;
                r.name = "ret"; r.dir = PinDir::Out; r.kind = PinKind::Data;
                r.type = typeId(ft);
                d.pins.push_back(std::move(r));
                out.push_back(std::move(d));
            }

            for (const auto& [op, rhs, ret] : pend.operators) {
                const char* sym = operatorSymbol(op);
                if (!sym) continue;
                const bool unary = (op == "unm" || op == "len") || rhs.empty();

                NodeDesc d;
                d.id = cls + ".op." + op + (unary ? "" : "." + rhs);
                d.display = unary ? (std::string(sym) + cls)
                                  : (cls + " " + sym + " " + rhs);
                d.category = categoryFor(cls);
                d.emit = unary ? "unop" : "binop";
                d.target = sym;
                d.pure = true;
                PinDesc a;
                a.name = "a"; a.dir = PinDir::In; a.kind = PinKind::Data;
                a.type = typeId(cls);
                d.pins.push_back(std::move(a));
                if (!unary) {
                    bool opt = false;
                    const std::string rt = mapType(rhs, opt);
                    PinDesc b;
                    b.name = "b"; b.dir = PinDir::In; b.kind = PinKind::Data;
                    b.type = typeId(rt.empty() ? "any" : rt);
                    d.pins.push_back(std::move(b));
                }
                PinDesc r;
                r.name = "ret"; r.dir = PinDir::Out; r.kind = PinKind::Data;
                r.type = typeId(ret.empty() ? cls : ret);
                d.pins.push_back(std::move(r));
                d.doc = unary ? ("Computes " + std::string(sym) + "a on " + cls + ".")
                              : ("Computes a " + std::string(sym) + " b, where a is "
                                 + cls + " and b is " + rhs + ".");
                out.push_back(std::move(d));
            }
            pend.clear();
            continue;
        }

        pend.clear();
    }

    for (const PendingEvent& pe : pendingEvents) {
        if (pe.index >= out.size()) continue;
        if (!constructible.count(pe.receiver)) continue;

        NodeDesc& d = out[pe.index];
        d.category = "Objects/" + pe.receiver + "/Events";
        d.target = d.id.substr(d.id.find_last_of('.') + 1);

        PinDesc self;
        self.name = "self";
        self.dir = PinDir::In;
        self.kind = PinKind::Data;
        self.type = typeId(pe.receiver);
        d.pins.insert(d.pins.begin(), std::move(self));
    }

    {
        struct Group {
            std::vector<std::size_t> indices;
            std::vector<std::string> owners;
        };
        std::map<std::string, Group> bySignature;

        for (std::size_t i = 0; i < out.size(); ++i) {
            const NodeDesc& d = out[i];
            const bool method = d.emit == "method";
            const bool field = d.emit == "index";
            if (!method && !field) continue;

            const std::size_t sep = d.id.find_last_of(method ? ':' : '.');
            if (sep == std::string::npos) continue;
            const std::string owner = d.id.substr(0, sep);

            std::string key = std::string(method ? "m|" : "f|") + d.target
                            + "|" + (d.pure ? "1" : "0");
            for (const PinDesc& p : d.pins) {
                if (p.name == "self") continue;
                key += "|" + p.name + ":"
                     + std::string(types.get(p.type).name)
                     + (p.optional ? "?" : "");
            }
            Group& g = bySignature[key];
            g.indices.push_back(i);
            if (std::find(g.owners.begin(), g.owners.end(), owner) == g.owners.end())
                g.owners.push_back(owner);
        }

        constexpr std::size_t kMinOwners = 3;
        std::map<std::string, std::vector<const Group*>> byOwnerSet;
        for (const auto& [key, g] : bySignature) {
            if (g.owners.size() < kMinOwners) continue;
            std::vector<std::string> sorted = g.owners;
            std::sort(sorted.begin(), sorted.end());
            std::string setKey;
            for (const std::string& o : sorted) setKey += o + ",";
            byOwnerSet[setKey].push_back(&g);
        }

        std::vector<std::pair<std::string, std::vector<const Group*>>> sets(
            byOwnerSet.begin(), byOwnerSet.end());
        std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) {
            if (a.second.size() != b.second.size())
                return a.second.size() > b.second.size();
            return a.first < b.first;
        });

        std::set<std::size_t> drop;
        int index = 0;
        for (const auto& [setKey, groups] : sets) {
            const std::size_t owners = groups.front()->owners.size();
            if (index > 0 && groups.size() < 2 && owners < 6) continue;

            std::vector<std::string> members;
            for (const Group* g : groups)
                members.push_back(out[g->indices.front()].target);
            std::sort(members.begin(), members.end());
            std::string sig;
            for (const std::string& m : members) sig += m + ",";

            static const std::map<std::string, std::string> kNamed = {
                {"backgroundColor,border,enabled,getAbsolutePosition,isHovered,"
                 "moveToBack,moveToFront,position,", "Object2D"},
                {"setAlignment,setFont,text,wordWrap,", "TextElement"},
                {"hasParent,parentTo,",                "Parentable"},
                {"x,y,",                               "Vector"},
                {"destroy,",                           "Destroyable"},
                {"getReferenceCount,",                 "RefCounted"},
                {"visible,",                           "Hideable"},
                {"position,",                          "Positionable"},
                {"rotation,",                          "Rotatable"},
                {"debug,",                             "Debuggable"},
            };

            std::string base;
            if (index == 0) {
                base = "SceneObject";
            } else if (const auto it = kNamed.find(sig); it != kNamed.end()) {
                base = it->second;
            } else {
                base = "SharedBase" + std::to_string(index + 1);
            }
            ++index;

            for (const Group* g : groups) {
                const std::size_t keep = g->indices.front();
                NodeDesc& d = out[keep];
                const bool method = d.emit == "method";
                d.id = base + (method ? ":" : ".") + d.target;
                d.category = std::string("Objects/") + base + (method ? "" : "/Fields");
                for (PinDesc& p : d.pins)
                    if (p.name == "self") p.type = typeId(base);
                for (std::size_t k = 1; k < g->indices.size(); ++k)
                    drop.insert(g->indices[k]);

                for (const std::string& owner : g->owners) {
                    TypeDesc td;
                    td.name = owner;
                    td.coercesTo = {base};
                    types.intern(td);
                }
            }
        }

        if (!drop.empty()) {
            std::vector<NodeDesc> kept;
            kept.reserve(out.size() - drop.size());
            for (std::size_t i = 0; i < out.size(); ++i)
                if (!drop.count(i)) kept.push_back(std::move(out[i]));
            out = std::move(kept);
        }
    }

    if (!enumsPath_.empty()) {
        std::ifstream ef(enumsPath_, std::ios::binary);
        if (ef) {
            std::ostringstream es;
            es << ef.rdbuf();
            const std::string etext = es.str();
            const std::string_view eall(etext);
            std::size_t p = 0;
            std::string current;
            while (p <= eall.size()) {
                const std::size_t nl = eall.find('\n', p);
                const std::string_view l = trim(eall.substr(
                    p, nl == std::string_view::npos ? std::string_view::npos : nl - p));
                p = (nl == std::string_view::npos) ? eall.size() + 1 : nl + 1;

                if (startsWith(l, "--- @class") || startsWith(l, "---@class")) {
                    std::string_view a = l.substr(l.find("class") + 5);
                    current = std::string(trim(a));
                } else if (!current.empty() && l.find('=') != std::string_view::npos
                           && l.find("{}") == std::string_view::npos) {
                    const std::size_t eq = l.find('=');
                    std::string_view key = trim(l.substr(0, eq));
                    const std::size_t dot = key.find_last_of('.');
                    if (dot != std::string_view::npos) key = key.substr(dot + 1);
                    TypeDesc td;
                    td.name = current;
                    td.isEnum = true;
                    td.enumValues.emplace_back(std::string(key), 0);
                    types.intern(td);
                }
            }
        }
    }
}

}
