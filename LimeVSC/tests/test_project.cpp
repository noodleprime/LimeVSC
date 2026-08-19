#include "api/data_provider.h"
#include "api/luals_provider.h"
#include "limecore.h"
#include "project/project.h"

#include <ostream>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lime;

namespace {

fs::path repoRoot() {
    return fs::path(LIMEVSC_TEST_DIR).parent_path();
}
fs::path limexRoot() {
    const fs::path candidates[] = {
        repoRoot().parent_path() / "LimeX",
        repoRoot().parent_path(),
    };
    for (const fs::path& p : candidates)
        if (fs::exists(p / "LimeVS" / "template")) return p;
    return {};
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Env {
    TypeRegistry    types;
    NodeRegistry    nodes;
    EmitterRegistry emitters = EmitterRegistry::withBuiltins();
    Diagnostics     diag;

    Env() {
        const std::string data = LIMEVSC_DATA_DIR;
        types.loadFile(data + "/core.limetypes", diag);
        const fs::path api = limexRoot().empty()
                                 ? fs::path{}
                                 : limexRoot() / "LimeEngine" / "api";
        if (!api.empty())
            nodes.addProvider(std::make_unique<LuaLSProvider>(
                (api / "Lime.lua").string(), (api / "Enums.lua").string(),
                kPriorityGenerated));
        nodes.addProvider(std::make_unique<DataFileProvider>(
            data + "/core.limenodes", "core", kPriorityCore));
        nodes.rebuild(types, diag);
    }
};

}

TEST_CASE("runtime errors map back to the node that produced the line") {
    SourceMap map;
    map.lines = {{2, NodeId{0}}, {6, NodeId{5}}, {10, NodeId{7}}};
    const std::vector<LoadedMap> maps = {
        {"C:/proj/content/main.lua", "C:/proj/content/main.lime", map}};

    const std::string log =
        "[12:00:00] Lime started with 1 module loaded\n"
        "[12:00:01] [string \"content.main\"]:7: attempt to index a nil value\n";

    const std::vector<Diagnostic> errs = mapRuntimeErrors(log, maps);
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].line == 7);
    CHECK(errs[0].message == "attempt to index a nil value");
    CHECK(errs[0].file == "C:/proj/content/main.lime");
    CHECK(errs[0].node == NodeId{5});
}

TEST_CASE("a log with no Lua error yields no diagnostics") {
    SourceMap map;
    const std::vector<LoadedMap> maps = {{"a/main.lua", "a/main.lime", map}};
    CHECK(mapRuntimeErrors(
              "[12:00:00] Lime ended with 0 warnings, 0 errors\n", maps)
              .empty());
}

TEST_CASE("full project workflow" * doctest::skip(false)) {
    const fs::path limex = limexRoot();
    if (limex.empty()) {
        MESSAGE("no LimeX checkout beside LimeVSC; skipping");
        return;
    }

    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "limevsc_project_test";
    fs::remove_all(tmp, ec);

    Env env;
    REQUIRE_FALSE(env.diag.hasErrors());

    SUBCASE("create, compile, pack and package") {
        Diagnostics d;

        const std::string tpl =
            (limex / "LimeVS" / "template").string();
        REQUIRE(createProject(tpl, tmp.string(), d));
        REQUIRE(fs::exists(tmp / "content"));
        REQUIRE(fs::exists(tmp / "lib" / "LimeEngine.dll"));

        fs::remove(tmp / "content" / "main.lua", ec);
        fs::copy_file(repoRoot() / "examples" / "demo.lime",
                      tmp / "content" / "main.lime",
                      fs::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);

        ProjectContext proj;
        proj.root = tmp.string();
        proj.limeBuilder = ProjectContext::findLimeBuilder(proj.root);
        proj.scan();

        CHECK(proj.valid());
        REQUIRE(proj.limeFiles.size() == 1);

        std::vector<LoadedMap> maps;
        REQUIRE(compileProject(proj, env.nodes, env.types, env.emitters, maps, d));
        for (const Diagnostic& x : d.all()) INFO(x.message);
        CHECK_FALSE(d.hasErrors());
        REQUIRE(maps.size() == 1);

        const fs::path lua = tmp / "content" / "main.lua";
        REQUIRE(fs::exists(lua));
        const std::string src = readAll(lua);
        CHECK(src.find("Lime.onStart:hook") != std::string::npos);
        REQUIRE(src.size() > 3);
        CHECK(static_cast<unsigned char>(src[0]) != 0xEF);

        if (proj.limeBuilder.empty()) {
            MESSAGE("LimeBuilder.exe not found; skipping pack");
            return;
        }
        std::string out;
        REQUIRE(packProject(proj, out, d));
        INFO(out);
        CHECK(fs::exists(proj.appExe()));

        REQUIRE(packageProject(proj, d));
        const fs::path bin = tmp / "bin";
        CHECK(fs::exists(bin / "app.exe"));
        CHECK(fs::exists(bin / "lib" / "LimeEngine.dll"));
        CHECK_FALSE(fs::exists(bin / "content" / "main.lime"));
        CHECK_FALSE(fs::exists(bin / "content" / "main.lua"));
    }

    fs::remove_all(tmp, ec);
}
