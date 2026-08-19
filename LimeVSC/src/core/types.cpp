#include "limecore.h"

#include <algorithm>
#include <toml.hpp>

namespace lime {

namespace {
std::uint32_t deriveColor(std::string_view name) {
    std::uint32_t h = 2166136261u;
    for (char c : name) { h ^= static_cast<unsigned char>(c); h *= 16777619u; }
    const std::uint32_t r = 90 + (h & 0x7F);
    const std::uint32_t g = 90 + ((h >> 8) & 0x7F);
    const std::uint32_t b = 90 + ((h >> 16) & 0x7F);
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}
}

TypeId TypeRegistry::intern(const TypeDesc& d) {
    for (std::uint32_t i = 0; i < types_.size(); ++i) {
        if (types_[i].name != d.name) continue;
        TypeDesc& t = types_[i];
        if (t.color == 0 && d.color != 0) t.color = d.color;
        if (!d.coercesTo.empty()) {
            for (const auto& c : d.coercesTo)
                if (std::find(t.coercesTo.begin(), t.coercesTo.end(), c) == t.coercesTo.end())
                    t.coercesTo.push_back(c);
        }
        if (d.isEnum) {
            t.isEnum = true;
            if (!d.enumValues.empty()) t.enumValues = d.enumValues;
        }
        return TypeId{i};
    }
    TypeDesc t = d;
    if (t.color == 0) t.color = deriveColor(t.name);
    types_.push_back(std::move(t));
    return TypeId{static_cast<std::uint32_t>(types_.size() - 1)};
}

TypeId TypeRegistry::find(std::string_view name) const {
    for (std::uint32_t i = 0; i < types_.size(); ++i)
        if (types_[i].name == name) return TypeId{i};
    return TypeId{};
}

const TypeDesc& TypeRegistry::get(TypeId id) const {
    static const TypeDesc kUnknown{"<unknown>", 0x808080FF, {}, false, {}};
    if (!id.valid() || id.v >= types_.size()) return kUnknown;
    return types_[id.v];
}

bool TypeRegistry::canConnect(TypeId from, TypeId to) const {
    if (!from.valid() || !to.valid()) return false;
    if (from == to) return true;

    const TypeDesc& f = get(from);
    const TypeDesc& t = get(to);

    if (f.name == "any" || t.name == "any") return true;

    return std::find(f.coercesTo.begin(), f.coercesTo.end(), t.name)
           != f.coercesTo.end();
}

bool TypeRegistry::loadFile(const std::string& path, Diagnostics& diag) {
    toml::parse_result res = toml::parse_file(path);
    if (!res) {
        diag.error("failed to parse " + path + ": "
                   + std::string(res.error().description()));
        return false;
    }
    const toml::table& tbl = res.table();
    const toml::array* arr = tbl["type"].as_array();
    if (!arr) {
        diag.error("no [[type]] entries in " + path);
        return false;
    }

    for (const toml::node& n : *arr) {
        const toml::table* t = n.as_table();
        if (!t) continue;

        TypeDesc d;
        d.name = (*t)["name"].value_or(std::string{});
        if (d.name.empty()) {
            diag.warn("skipping [[type]] with no name in " + path);
            continue;
        }
        if (auto c = (*t)["color"].value<std::int64_t>())
            d.color = static_cast<std::uint32_t>(*c);
        else if (auto s = (*t)["color"].value<std::string>())
            d.color = static_cast<std::uint32_t>(std::stoul(*s, nullptr, 16));

        if (const toml::array* co = (*t)["coercesTo"].as_array())
            for (const toml::node& c : *co)
                if (auto s = c.value<std::string>()) d.coercesTo.push_back(*s);

        d.isEnum = (*t)["isEnum"].value_or(false);
        intern(d);
    }
    return true;
}

}
