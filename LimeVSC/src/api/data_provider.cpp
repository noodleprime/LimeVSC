#include "api/data_provider.h"

#include <toml.hpp>

namespace lime {

DataFileProvider::DataFileProvider(std::string path, std::string name, int priority)
    : path_(std::move(path)), name_(std::move(name)), priority_(priority) {}

void DataFileProvider::collect(TypeRegistry& types, std::vector<NodeDesc>& out,
                               Diagnostics& diag) {
    toml::parse_result res = toml::parse_file(path_);
    if (!res) {
        diag.error("failed to parse " + path_ + ": "
                   + std::string(res.error().description()));
        return;
    }

    const toml::array* arr = res.table()["node"].as_array();
    if (!arr) {
        diag.error("no [[node]] entries in " + path_);
        return;
    }

    for (const toml::node& n : *arr) {
        const toml::table* t = n.as_table();
        if (!t) continue;

        NodeDesc d;
        d.id = (*t)["id"].value_or(std::string{});
        if (d.id.empty()) {
            diag.warn("skipping [[node]] with no id in " + path_);
            continue;
        }
        d.display  = (*t)["display"].value_or(std::string{});
        d.category = (*t)["category"].value_or(std::string{});
        d.doc      = (*t)["doc"].value_or(std::string{});
        d.emit     = (*t)["emit"].value_or(std::string{});
        d.target   = (*t)["target"].value_or(std::string{});
        d.pure     = (*t)["pure"].value_or(false);
        d.isEvent  = (*t)["isEvent"].value_or(false);

        if (auto c = (*t)["color"].value<std::string>())
            d.color = static_cast<std::uint32_t>(std::stoul(*c, nullptr, 16));

        if (d.emit.empty()) {
            diag.error("node '" + d.id + "' has no emit kind in " + path_);
            continue;
        }

        if (const toml::array* pins = (*t)["pin"].as_array()) {
            for (const toml::node& pn : *pins) {
                const toml::table* pt = pn.as_table();
                if (!pt) continue;

                PinDesc p;
                p.name = (*pt)["name"].value_or(std::string{});
                if (p.name.empty()) {
                    diag.warn("node '" + d.id + "' has a pin with no name");
                    continue;
                }
                p.dir = ((*pt)["dir"].value_or(std::string{"in"}) == "out")
                            ? PinDir::Out : PinDir::In;
                p.kind = ((*pt)["kind"].value_or(std::string{"data"}) == "exec")
                            ? PinKind::Exec : PinKind::Data;
                p.optional = (*pt)["optional"].value_or(false);

                if (auto dv = (*pt)["default"].value<std::string>())
                    p.defaultValue = *dv;

                if (p.kind == PinKind::Data) {
                    const std::string tn =
                        (*pt)["type"].value_or(std::string{"any"});
                    p.type = types.intern(TypeDesc{tn, 0, {}, false, {}});
                }
                d.pins.push_back(std::move(p));
            }
        }
        out.push_back(std::move(d));
    }
}

}
