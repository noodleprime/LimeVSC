#include "scene/component_provider.h"

#include <toml.hpp>

namespace lime {

ComponentFileProvider::ComponentFileProvider(std::string path, std::string name,
                                             int priority)
    : path_(std::move(path)), name_(std::move(name)), priority_(priority) {}

void ComponentFileProvider::collect(TypeRegistry& types,
                                    std::vector<ComponentDesc>& out,
                                    Diagnostics& diag) {
    toml::parse_result res = toml::parse_file(path_);
    if (!res) {
        diag.error("failed to parse " + path_ + ": "
                   + std::string(res.error().description()));
        return;
    }

    const toml::array* arr = res.table()["component"].as_array();
    if (!arr) {
        diag.error("no [[component]] entries in " + path_);
        return;
    }

    for (const toml::node& n : *arr) {
        const toml::table* t = n.as_table();
        if (!t) continue;

        ComponentDesc d;
        d.id = (*t)["id"].value_or(std::string{});
        if (d.id.empty()) {
            diag.warn("skipping [[component]] with no id in " + path_);
            continue;
        }
        d.display  = (*t)["display"].value_or(std::string{});
        d.category = (*t)["category"].value_or(std::string{});
        d.doc      = (*t)["doc"].value_or(std::string{});
        d.unique   = (*t)["unique"].value_or(true);

        if (auto c = (*t)["color"].value<std::string>())
            d.color = static_cast<std::uint32_t>(std::stoul(*c, nullptr, 16));

        if (const toml::array* props = (*t)["prop"].as_array()) {
            for (const toml::node& pn : *props) {
                const toml::table* pt = pn.as_table();
                if (!pt) continue;

                PropDesc p;
                p.name = (*pt)["name"].value_or(std::string{});
                if (p.name.empty()) {
                    diag.warn("component '" + d.id + "' has a property with no name");
                    continue;
                }
                p.typeName     = (*pt)["type"].value_or(std::string{"any"});
                p.optional     = (*pt)["optional"].value_or(false);
                p.defaultValue = (*pt)["default"].value_or(std::string{});
                p.doc          = (*pt)["doc"].value_or(std::string{});
                p.type = types.intern(TypeDesc{p.typeName, 0, {}, false, {}});
                d.props.push_back(std::move(p));
            }
        }
        out.push_back(std::move(d));
    }
}

}
