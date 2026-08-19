#include "api/data_provider.h"
#include "api/graphfn_provider.h"
#include "lime/lua_import.h"
#include "api/luals_provider.h"
#include "limecore.h"
#include "project/project.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace lime;

namespace {

int usage() {
    std::puts("usage: limec <file.lime> [--canon] [--check] [--emit [out.lua]]");
    std::puts("             [--data <dir>] [--api <Lime.lua>] [--stdout]");
    std::puts("       limec <file.lua> --import [out.lime]");
    std::puts("       limec <projectDir> --cook");
    return 2;
}

std::string findLimeXApi(const std::string& start) {
    std::filesystem::path p = std::filesystem::absolute(start);
    for (int up = 0; up < 6 && !p.empty(); ++up, p = p.parent_path()) {
        const std::filesystem::path api = p / "LimeX" / "LimeEngine" / "api" / "Lime.lua";
        if (std::filesystem::exists(api)) return api.parent_path().string();
    }
    return {};
}

void report(const Diagnostics& diag) {
    for (const Diagnostic& d : diag.all()) {
        const char* sev = d.severity == Severity::Error   ? "error"
                        : d.severity == Severity::Warning ? "warning" : "info";
        std::fprintf(stderr, "%s: %s\n", sev, d.message.c_str());
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2) return usage();

    const std::string path = argv[1];
    bool canon = false, check = false, emit = false, toStdout = false;
    bool doImport = false, cook = false;
    std::string outPath, dataDir = LIMEVSC_DATA_DIR, apiPath;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--canon")      canon = true;
        else if (a == "--check") check = true;
        else if (a == "--stdout") toStdout = true;
        else if (a == "--import") {
            doImport = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
        }
        else if (a == "--emit") {
            emit = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
        }
        else if (a == "--cook") cook = true;
        else if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--api" && i + 1 < argc)  apiPath = argv[++i];
        else return usage();
    }

    Diagnostics diag;
    Graph g;

    if (cook) {
        ProjectContext proj;
        proj.root = path;
        proj.scan();
        if (!proj.isEngine()) {
            std::puts("not an engine project (no project.limeproj, or mode = framework)");
            return 1;
        }
        const bool ok = cookScenes(proj, diag);
        report(diag);
        std::printf("cooked %zu scene(s)\n", proj.sceneFiles.size());
        return ok ? 0 : 1;
    }

    if (doImport) {
        TypeRegistry types;
        types.loadFile(dataDir + "/core.limetypes", diag);
        NodeRegistry nodes;
        nodes.addProvider(std::make_unique<DataFileProvider>(
            dataDir + "/core.limenodes", "core", kPriorityCore));
        nodes.rebuild(types, diag);

        ImportOptions o;
        if (!importLuaFile(path, &nodes, o, g, diag)) {
            report(diag);
            return 1;
        }
        report(diag);

        const std::filesystem::path out =
            outPath.empty() ? std::filesystem::path(path).replace_extension(".lime")
                            : std::filesystem::path(outPath);
        if (toStdout) {
            const std::string text = writeLime(g);
            std::fwrite(text.data(), 1, text.size(), stdout);
        } else {
            Diagnostics wd;
            if (!writeLimeFile(out.string(), g, wd)) { report(wd); return 1; }
            std::fprintf(stderr, "imported %s -> %s (%zu nodes)\n", path.c_str(),
                         out.string().c_str(), g.nodes().size());
        }
        return 0;
    }

    if (!readLime(path, g, diag)) {
        report(diag);
        return 1;
    }
    report(diag);

    const std::string canonical = writeLime(g);

    if (check) {
        Diagnostics d2;
        Graph g2;
        if (!parseLime(canonical, g2, d2)) {
            report(d2);
            std::fputs("round-trip FAILED: canonical output does not parse\n", stderr);
            return 1;
        }
        if (writeLime(g2) != canonical) {
            std::fputs("round-trip FAILED: not byte-identical\n", stderr);
            return 1;
        }
        std::printf("round-trip OK  %zu nodes, %zu links\n",
                    g.nodes().size(), g.links().size());
        return 0;
    }

    if (canon) {
        std::ofstream f(path, std::ios::binary);
        f.write(canonical.data(), static_cast<std::streamsize>(canonical.size()));
        return 0;
    }

    if (emit) {
        TypeRegistry types;
        types.loadFile(dataDir + "/core.limetypes", diag);

        NodeRegistry nodes;
        if (apiPath.empty()) {
            if (const std::string dir = findLimeXApi(path); !dir.empty())
                apiPath = dir + "/Lime.lua";
        }
        if (!apiPath.empty()) {
            const std::filesystem::path enums =
                std::filesystem::path(apiPath).parent_path() / "Enums.lua";
            nodes.addProvider(std::make_unique<LuaLSProvider>(
                apiPath, std::filesystem::exists(enums) ? enums.string() : "",
                kPriorityGenerated));
        }
        nodes.addProvider(std::make_unique<DataFileProvider>(
            dataDir + "/core.limenodes", "core", kPriorityCore));

        const std::filesystem::path content =
            std::filesystem::path(path).parent_path();
        if (content.filename() == "content")
            nodes.addProvider(std::make_unique<GraphFnProvider>(
                content.parent_path().string(), kPriorityOverrides));

        nodes.rebuild(types, diag);

        const EmitterRegistry emitters = EmitterRegistry::withBuiltins();
        const std::string srcName = std::filesystem::path(path).filename().string();

        Diagnostics cdiag;
        const CompileResult r =
            compileGraph(g, nodes, types, emitters, srcName, cdiag);
        report(cdiag);
        if (!r.ok) return 1;

        if (toStdout) {
            std::fwrite(r.lua.data(), 1, r.lua.size(), stdout);
        } else {
            std::filesystem::path out = outPath.empty()
                                            ? std::filesystem::path(path).replace_extension(".lua")
                                            : std::filesystem::path(outPath);
            std::ofstream f(out, std::ios::binary);
            if (!f) { std::fprintf(stderr, "cannot write %s\n", out.string().c_str()); return 1; }
            f.write(r.lua.data(), static_cast<std::streamsize>(r.lua.size()));
            f.close();

            std::filesystem::path mapPath = out;
            mapPath += ".map";
            std::ofstream m(mapPath, std::ios::binary);
            for (const auto& [line, node] : r.map.lines)
                m << line << ' ' << encodeId(node.v) << '\n';

            std::fprintf(stderr, "wrote %s (%zu bytes, %d goto)\n",
                         out.string().c_str(), r.lua.size(), r.gotoCount);
        }
        return 0;
    }

    std::fwrite(canonical.data(), 1, canonical.size(), stdout);
    return 0;
}
