#include "api/data_provider.h"
#include "api/luals_provider.h"
#include "limecore.h"

#include <cstdio>
#include <map>
#include <string>

using namespace lime;

namespace {

int usage() {
    std::puts("usage: limeapi [--data <dir>] [--api <Lime.lua>] [--enums <Enums.lua>]");
    std::puts("               [--list] [--node <id>] [--cats]");
    return 2;
}

const char* dirName(PinDir d) { return d == PinDir::In ? "in" : "out"; }
const char* kindName(PinKind k) { return k == PinKind::Exec ? "exec" : "data"; }

}

int main(int argc, char** argv) {
    std::string dataDir = LIMEVSC_DATA_DIR;
    std::string apiPath, enumsPath;
    bool list = false, cats = false;
    std::string nodeQuery;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--data" && i + 1 < argc)       dataDir = argv[++i];
        else if (a == "--api" && i + 1 < argc)   apiPath = argv[++i];
        else if (a == "--enums" && i + 1 < argc) enumsPath = argv[++i];
        else if (a == "--list")                  list = true;
        else if (a == "--cats")                  cats = true;
        else if (a == "--node" && i + 1 < argc)  nodeQuery = argv[++i];
        else return usage();
    }

    Diagnostics diag;
    TypeRegistry types;
    types.loadFile(dataDir + "/core.limetypes", diag);

    NodeRegistry nodes;
    if (!apiPath.empty())
        nodes.addProvider(std::make_unique<LuaLSProvider>(
            apiPath, enumsPath, kPriorityGenerated));
    nodes.addProvider(std::make_unique<DataFileProvider>(
        dataDir + "/core.limenodes", "core", kPriorityCore));
    nodes.rebuild(types, diag);

    for (const Diagnostic& d : diag.all()) {
        const char* sev = d.severity == Severity::Error   ? "error"
                        : d.severity == Severity::Warning ? "warning" : "info";
        std::fprintf(stderr, "%s: %s\n", sev, d.message.c_str());
    }
    if (diag.hasErrors()) return 1;

    if (!nodeQuery.empty()) {
        const NodeDesc* d = nodes.find(nodeQuery);
        if (!d) {
            std::fprintf(stderr, "no such node: %s\n", nodeQuery.c_str());
            return 1;
        }
        std::printf("%s\n  display  %s\n  category %s\n  emit     %s\n"
                    "  target   %s\n  pure     %s\n  event    %s\n",
                    d->id.c_str(), d->display.c_str(), d->category.c_str(),
                    d->emit.c_str(), d->target.c_str(),
                    d->pure ? "yes" : "no", d->isEvent ? "yes" : "no");
        for (const PinDesc& p : d->pins)
            std::printf("  pin %-8s %-4s %-4s %s\n", p.name.c_str(),
                        dirName(p.dir), kindName(p.kind),
                        p.kind == PinKind::Exec
                            ? "" : std::string(types.get(p.type).name).c_str());
        return 0;
    }

    if (list) {
        for (const NodeDesc& d : nodes.all())
            std::printf("%-40s %-22s %-14s %s\n", d.id.c_str(), d.category.c_str(),
                        d.emit.c_str(), d.pure ? "pure" : "");
        return 0;
    }

    if (cats) {
        std::map<std::string, int> byCat;
        std::map<std::string, int> byEmit;
        int pure = 0, events = 0;
        for (const NodeDesc& d : nodes.all()) {
            ++byCat[d.category];
            ++byEmit[d.emit];
            pure += d.pure ? 1 : 0;
            events += d.isEvent ? 1 : 0;
        }
        std::printf("-- categories --\n");
        for (const auto& [c, n] : byCat) std::printf("%5d  %s\n", n, c.c_str());
        std::printf("-- emit kinds --\n");
        for (const auto& [e, n] : byEmit) std::printf("%5d  %s\n", n, e.c_str());
        std::printf("-- totals --\n%5d  pure\n%5d  events\n", pure, events);
        return 0;
    }

    std::printf("types %zu, nodes %zu, categories %zu\n",
                types.size(), nodes.all().size(), nodes.categories().size());
    return 0;
}
