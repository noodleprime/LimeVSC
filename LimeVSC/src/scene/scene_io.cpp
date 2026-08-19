#include "scene/scene.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace lime {
namespace {

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

std::string_view nextToken(std::string_view& s) {
    s = trimLeft(s);
    const std::size_t sp = s.find_first_of(" \t");
    const std::string_view tok = s.substr(0, sp);
    s = (sp == std::string_view::npos) ? std::string_view{} : trimLeft(s.substr(sp));
    return tok;
}

}

bool parseScene(std::string_view text, Scene& out, Diagnostics& diag) {
    out.clear();

    Entity* entity = nullptr;
    Component* comp = nullptr;
    bool sawHeader = false;
    int lineNo = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                          : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        const char sigil = line.front();
        std::string_view rest = trimLeft(line.substr(1));

        switch (sigil) {
        case '!': {
            const std::string_view key = nextToken(rest);
            if (key == "limescene") {
                sawHeader = true;
                if (rest != "1")
                    diag.warn("unknown .limescene version '" + std::string(rest)
                              + "', parsing as 1");
            } else if (key == "name") {
                out.name = std::string(rest);
            } else {
                diag.warn("unknown scene directive '" + std::string(key)
                          + "' on line " + std::to_string(lineNo));
            }
            break;
        }
        case '@': {
            comp = nullptr;
            const std::string_view idTok = nextToken(rest);
            const auto id = decodeId(idTok);
            if (!id) {
                diag.error("bad entity id '" + std::string(idTok) + "' on line "
                           + std::to_string(lineNo));
                return false;
            }
            const std::string_view parentTok = nextToken(rest);
            EntityId parent{};
            if (parentTok != "-") {
                const auto p = decodeId(parentTok);
                if (!p) {
                    diag.error("bad parent id '" + std::string(parentTok)
                               + "' on line " + std::to_string(lineNo));
                    return false;
                }
                parent = EntityId{*p};
            }
            const EntityId made =
                out.addEntityWithId(EntityId{*id}, std::string(rest), parent);
            entity = out.entity(made);
            break;
        }
        case '~': {
            if (!entity) {
                diag.error("component outside an entity on line "
                           + std::to_string(lineNo));
                return false;
            }
            Component c;
            c.type = std::string(trim(rest));
            if (c.type.empty()) {
                diag.error("component with no type on line "
                           + std::to_string(lineNo));
                return false;
            }
            entity->components.push_back(std::move(c));
            comp = &entity->components.back();
            break;
        }
        case '=': {
            if (!comp) {
                diag.error("property outside a component on line "
                           + std::to_string(lineNo));
                return false;
            }
            const std::string_view prop = nextToken(rest);
            if (prop.empty()) {
                diag.error("property with no name on line "
                           + std::to_string(lineNo));
                return false;
            }
            comp->setValue(prop, std::string(rest));
            break;
        }
        default:
            diag.warn("ignoring unrecognised line " + std::to_string(lineNo));
            break;
        }
    }

    if (!sawHeader) {
        diag.error("not a .limescene file (missing !limescene header)");
        return false;
    }

    for (const Entity& e : out.entities()) {
        if (e.parent.valid() && !out.entity(e.parent)) {
            diag.warn("entity '" + e.name + "' names a missing parent; "
                      "attaching it to the scene root");
            const_cast<Entity&>(e).parent = EntityId{};
        }
    }
    return true;
}

bool readScene(const std::string& path, Scene& out, Diagnostics& diag) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot open " + path);
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    std::string_view sv = text;
    if (sv.size() >= 3 && sv.compare(0, 3, "\xEF\xBB\xBF") == 0)
        sv.remove_prefix(3);
    return parseScene(sv, out, diag);
}

std::string writeScene(const Scene& s) {
    std::string out;
    out += "!limescene 1\n";
    if (!s.name.empty()) out += "!name " + s.name + "\n";

    std::vector<const Entity*> ordered;
    ordered.reserve(s.entities().size());
    for (const Entity& e : s.entities()) ordered.push_back(&e);
    std::sort(ordered.begin(), ordered.end(),
              [](const Entity* a, const Entity* b) { return a->id < b->id; });

    for (const Entity* e : ordered) {
        out += "\n@" + encodeId(e->id.v) + " ";
        out += e->parent.valid() ? encodeId(e->parent.v) : std::string("-");
        if (!e->name.empty()) out += " " + e->name;
        out += "\n";

        std::vector<const Component*> comps;
        comps.reserve(e->components.size());
        for (const Component& c : e->components) comps.push_back(&c);
        std::stable_sort(comps.begin(), comps.end(),
                         [](const Component* a, const Component* b) {
                             return a->type < b->type;
                         });

        for (const Component* c : comps) {
            out += "  ~ " + c->type + "\n";
            std::vector<const std::pair<std::string, std::string>*> vals;
            vals.reserve(c->values.size());
            for (const auto& kv : c->values) vals.push_back(&kv);
            std::sort(vals.begin(), vals.end(),
                      [](const auto* a, const auto* b) { return a->first < b->first; });
            for (const auto* kv : vals)
                out += "    = " + kv->first + " " + kv->second + "\n";
        }
    }
    return out;
}

bool writeSceneFile(const std::string& path, const Scene& s, Diagnostics& diag) {
    const std::string text = writeScene(s);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot write " + path);
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

}
