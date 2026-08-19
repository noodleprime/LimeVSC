#include "limecore.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace lime {

namespace {

constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

std::string_view trimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}
std::string_view trimLeft(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    return s;
}
std::string_view trim(std::string_view s) { return trimLeft(trimRight(s)); }

bool splitRef(std::string_view s, std::string_view& node, std::string_view& pin) {
    const std::size_t dot = s.find('.');
    if (dot == std::string_view::npos) return false;
    node = s.substr(0, dot);
    pin = s.substr(dot + 1);
    return !node.empty() && !pin.empty();
}

std::string_view nextToken(std::string_view& s) {
    s = trimLeft(s);
    const std::size_t sp = s.find_first_of(" \t");
    std::string_view tok = s.substr(0, sp);
    s = (sp == std::string_view::npos) ? std::string_view{} : trimLeft(s.substr(sp));
    return tok;
}

std::string formatCoord(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    return buf;
}

}

std::string encodeId(std::uint32_t v) {
    if (v == 0) return "0";
    std::string out;
    while (v > 0) { out.push_back(kDigits[v % 36]); v /= 36; }
    std::reverse(out.begin(), out.end());
    return out;
}

std::optional<std::uint32_t> decodeId(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::uint32_t v = 0;
    for (char c : s) {
        const char* p = std::find(std::begin(kDigits), std::end(kDigits) - 1, c);
        if (p == std::end(kDigits) - 1) return std::nullopt;
        v = v * 36 + static_cast<std::uint32_t>(p - std::begin(kDigits));
    }
    return v;
}

bool parseLime(std::string_view text, Graph& out, Diagnostics& diag) {
    out = Graph{};

    struct PendingLink { PinId from; PinId to; };
    std::vector<PendingLink> pending;
    struct RawLink {
        std::uint32_t selfNode; std::string selfPin;
        std::uint32_t otherNode; std::string otherPin;
        bool isExec;
    };
    std::vector<RawLink> rawLinks;

    Node* cur = nullptr;
    int lineNo = 0;
    bool sawHeader = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                          : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        line = trimRight(line);
        if (line.empty()) continue;

        const std::string_view body = trimLeft(line);
        const char sigil = body.front();

        if (sigil == '!') {
            std::string_view rest = body.substr(1);
            const std::string_view key = nextToken(rest);
            if (key == "lime") {
                sawHeader = true;
                if (trim(rest) != "1") {
                    diag.error("unsupported .lime format version '"
                               + std::string(trim(rest)) + "'");
                    return false;
                }
            } else if (key == "module") {
                out.moduleName = std::string(trim(rest));
            } else if (key == "graph") {
                out.graphName = std::string(trim(rest));
            } else if (key == "fn") {
                FnDecl fn;
                fn.name = std::string(nextToken(rest));
                while (!rest.empty()) {
                    const std::string_view tok = nextToken(rest);
                    if (tok.empty()) break;
                    if (tok == "->") { fn.ret = std::string(trim(rest)); break; }
                    FnParam p;
                    const std::size_t colon = tok.find(':');
                    if (colon == std::string_view::npos) {
                        p.name = std::string(tok);
                    } else {
                        p.name = std::string(tok.substr(0, colon));
                        p.type = std::string(tok.substr(colon + 1));
                    }
                    fn.params.push_back(std::move(p));
                }
                if (fn.name.empty())
                    diag.warn("!fn with no name on line " + std::to_string(lineNo));
                else
                    out.functions.push_back(std::move(fn));
            } else if (key == "var") {
                VarDecl vd;
                vd.name = std::string(nextToken(rest));
                vd.type = std::string(nextToken(rest));
                if (vd.type == "=") {
                    vd.type = "any";
                    vd.defaultValue = std::string(trim(rest));
                } else if (nextToken(rest) == "=") {
                    vd.defaultValue = std::string(trim(rest));
                }
                if (vd.type.empty()) vd.type = "any";
                if (vd.name.empty())
                    diag.warn("!var with no name on line " + std::to_string(lineNo));
                else
                    out.variables.push_back(std::move(vd));
            } else if (key == "prop") {
                PropDecl pd;
                pd.name = std::string(nextToken(rest));
                pd.type = std::string(nextToken(rest));
                if (pd.type == "=") {
                    pd.type = "any";
                    pd.defaultValue = std::string(trim(rest));
                } else if (nextToken(rest) == "=") {
                    pd.defaultValue = std::string(trim(rest));
                }
                if (pd.type.empty()) pd.type = "any";
                if (pd.name.empty())
                    diag.warn("!prop with no name on line " + std::to_string(lineNo));
                else
                    out.properties.push_back(std::move(pd));
            } else {
                diag.warn("unknown directive '!" + std::string(key) + "' on line "
                          + std::to_string(lineNo));
            }
            continue;
        }

        if (sigil == '~') {
            std::string_view rest = body.substr(1);
            const std::string_view idTok = nextToken(rest);
            const std::string_view typeTok = nextToken(rest);
            auto id = decodeId(idTok);
            if (!id) {
                diag.error("bad node id '" + std::string(idTok) + "' on line "
                           + std::to_string(lineNo));
                return false;
            }
            if (typeTok.empty()) {
                diag.error("node " + std::string(idTok) + " has no type on line "
                           + std::to_string(lineNo));
                return false;
            }
            float x = 0, y = 0;
            float w = 0, h = 0;
            if (nextToken(rest) == "@") {
                const std::string_view xs = nextToken(rest);
                const std::string_view ys = nextToken(rest);
                std::from_chars(xs.data(), xs.data() + xs.size(), x);
                std::from_chars(ys.data(), ys.data() + ys.size(), y);
                const std::string_view ws = nextToken(rest);
                const std::string_view hs = nextToken(rest);
                if (!ws.empty() && !hs.empty()) {
                    std::from_chars(ws.data(), ws.data() + ws.size(), w);
                    std::from_chars(hs.data(), hs.data() + hs.size(), h);
                }
            }
            const NodeId nid = out.addNodeWithId(NodeId{*id},
                                                 std::string(typeTok), x, y);
            cur = out.node(nid);
            if (cur) { cur->w = w; cur->h = h; }
            continue;
        }

        if (!cur) {
            diag.error("attribute before any node on line " + std::to_string(lineNo));
            return false;
        }

        std::string_view rest = body.substr(1);
        switch (sigil) {
        case '=': {
            const std::string_view pin = nextToken(rest);
            cur->values.emplace_back(std::string(pin), std::string(trim(rest)));
            break;
        }
        case '<': {
            const std::string_view pin = nextToken(rest);
            std::string_view sn, sp;
            if (!splitRef(trim(rest), sn, sp)) {
                diag.error("malformed data link on line " + std::to_string(lineNo));
                return false;
            }
            auto srcId = decodeId(sn);
            if (!srcId) {
                diag.error("bad source node id on line " + std::to_string(lineNo));
                return false;
            }
            rawLinks.push_back({cur->id.v, std::string(pin), *srcId,
                                std::string(sp), false});
            break;
        }
        case '>': {
            const std::string_view pin = nextToken(rest);
            std::string_view dn, dp;
            if (!splitRef(trim(rest), dn, dp)) {
                diag.error("malformed exec link on line " + std::to_string(lineNo));
                return false;
            }
            auto dstId = decodeId(dn);
            if (!dstId) {
                diag.error("bad target node id on line " + std::to_string(lineNo));
                return false;
            }
            rawLinks.push_back({cur->id.v, std::string(pin), *dstId,
                                std::string(dp), true});
            break;
        }
        case '|': {
            std::string_view rawLine = body.substr(1);
            if (!rawLine.empty() && rawLine.front() == ' ') rawLine.remove_prefix(1);
            if (!cur->rawBody.empty()) cur->rawBody.push_back('\n');
            cur->rawBody.append(rawLine);
            break;
        }
        case ';': {
            if (!cur->comment.empty()) cur->comment.push_back('\n');
            cur->comment.append(trim(rest));
            break;
        }
        default:
            diag.error("unknown line sigil '" + std::string(1, sigil)
                       + "' on line " + std::to_string(lineNo));
            return false;
        }
    }

    if (!sawHeader) {
        diag.error("missing '!lime' header");
        return false;
    }

    for (const RawLink& rl : rawLinks) {
        if (!out.node(NodeId{rl.selfNode})) continue;
        if (!out.node(NodeId{rl.otherNode})) {
            diag.warn("link to unknown node " + encodeId(rl.otherNode)
                      + " from " + encodeId(rl.selfNode));
            continue;
        }
        const PinId self  = PinId::make(NodeId{rl.selfNode}, rl.selfPin);
        const PinId other = PinId::make(NodeId{rl.otherNode}, rl.otherPin);
        if (rl.isExec) out.addLinkUnchecked(self, other, PinKind::Exec);
        else           out.addLinkUnchecked(other, self, PinKind::Data);
    }
    out.validateLinks(diag);

    return !diag.hasErrors();
}

bool readLime(const std::string& path, Graph& out, Diagnostics& diag) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot open " + path);
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    std::string_view sv = text;
    if (sv.size() >= 3 && static_cast<unsigned char>(sv[0]) == 0xEF
                       && static_cast<unsigned char>(sv[1]) == 0xBB
                       && static_cast<unsigned char>(sv[2]) == 0xBF)
        sv.remove_prefix(3);
    return parseLime(sv, out, diag);
}

std::string writeLime(const Graph& g) {
    std::string out;
    out += "!lime 1\n";
    if (!g.moduleName.empty()) out += "!module " + g.moduleName + "\n";
    if (!g.graphName.empty())  out += "!graph " + g.graphName + "\n";
    for (const FnDecl& fn : g.functions) {
        out += "!fn " + fn.name;
        for (const FnParam& p : fn.params) out += " " + p.name + ":" + p.type;
        if (!fn.ret.empty()) out += " -> " + fn.ret;
        out += "\n";
    }
    for (const PropDecl& pd : g.properties) {
        out += "!prop " + pd.name + " " + pd.type;
        if (!pd.defaultValue.empty()) out += " = " + pd.defaultValue;
        out += "\n";
    }
    for (const VarDecl& vd : g.variables) {
        out += "!var " + vd.name + " " + vd.type;
        if (!vd.defaultValue.empty()) out += " = " + vd.defaultValue;
        out += "\n";
    }

    std::vector<const Node*> ordered;
    ordered.reserve(g.nodes().size());
    for (const Node& n : g.nodes()) ordered.push_back(&n);
    std::sort(ordered.begin(), ordered.end(),
              [](const Node* a, const Node* b) { return a->id.v < b->id.v; });

    for (const Node* n : ordered) {
        out += "\n~" + encodeId(n->id.v) + " " + n->type + " @ "
             + formatCoord(n->x) + " " + formatCoord(n->y);
        if (n->w > 0 || n->h > 0)
            out += " " + formatCoord(n->w) + " " + formatCoord(n->h);
        out += "\n";

        {
            std::vector<std::pair<std::string, std::string>> rows(
                n->values.begin(), n->values.end());
            std::sort(rows.begin(), rows.end());
            for (const auto& [k, v] : rows) out += "  = " + k + " " + v + "\n";
        }

        {
            std::vector<std::pair<std::string, std::string>> rows;
            for (const Link& l : g.links()) {
                if (l.kind != PinKind::Data || l.to.node != n->id) continue;
                rows.emplace_back(std::string(l.to.pin.str()),
                                  encodeId(l.from.node.v) + "."
                                      + std::string(l.from.pin.str()));
            }
            std::sort(rows.begin(), rows.end());
            for (const auto& [k, v] : rows) out += "  < " + k + " " + v + "\n";
        }

        {
            std::vector<std::pair<std::string, std::string>> rows;
            for (const Link& l : g.links()) {
                if (l.kind != PinKind::Exec || l.from.node != n->id) continue;
                rows.emplace_back(std::string(l.from.pin.str()),
                                  encodeId(l.to.node.v) + "."
                                      + std::string(l.to.pin.str()));
            }
            std::sort(rows.begin(), rows.end());
            for (const auto& [k, v] : rows) out += "  > " + k + " " + v + "\n";
        }

        if (!n->rawBody.empty()) {
            std::size_t p = 0;
            while (p <= n->rawBody.size()) {
                const std::size_t nl = n->rawBody.find('\n', p);
                const std::string_view l =
                    std::string_view(n->rawBody)
                        .substr(p, nl == std::string::npos ? std::string::npos : nl - p);
                out += "  | " + std::string(l) + "\n";
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
        }
        if (!n->comment.empty()) {
            std::size_t p = 0;
            while (p <= n->comment.size()) {
                const std::size_t nl = n->comment.find('\n', p);
                const std::string_view l =
                    std::string_view(n->comment)
                        .substr(p, nl == std::string::npos ? std::string::npos : nl - p);
                out += "  ; " + std::string(l) + "\n";
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
        }
    }
    return out;
}

bool writeLimeFile(const std::string& path, const Graph& g, Diagnostics& diag) {
    const std::string text = writeLime(g);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot write " + path);
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

}
