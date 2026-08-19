#include <doctest/doctest.h>

#include "api/data_provider.h"
#include "api/graphfn_provider.h"
#include "api/luals_provider.h"

#include <filesystem>
#include <string>

using namespace lime;

namespace {

struct Catalog {
    TypeRegistry    types;
    NodeRegistry    nodes;
    EmitterRegistry emitters = EmitterRegistry::withBuiltins();
    Diagnostics     diag;

    explicit Catalog(const std::string& projectRoot = {}) {
        const std::string dir = LIMEVSC_DATA_DIR;
        types.loadFile(dir + "/core.limetypes", diag);
        nodes.addProvider(std::make_unique<DataFileProvider>(
            dir + "/core.limenodes", "core", kPriorityCore));
        if (!projectRoot.empty())
            nodes.addProvider(
                std::make_unique<GraphFnProvider>(projectRoot, kPriorityOverrides));
        nodes.rebuild(types, diag);
    }
};

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::string projectWith(const char* name, const Graph& g) {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / (std::string("limevsc_var_") + name);
    std::filesystem::create_directories(root / "content", ec);
    Diagnostics d;
    REQUIRE(writeLimeFile((root / "content/logic.lime").string(), g, d));
    return root.string();
}

}

TEST_CASE("variables round-trip through the .lime header") {
    Graph g;
    g.moduleName = "content.logic";
    g.variables.push_back({"score", "number", "0"});
    g.variables.push_back({"playerName", "string", "\"nobody\""});
    g.variables.push_back({"loose", "any", ""});

    const std::string once = writeLime(g);
    CHECK(contains(once, "!var score number = 0"));
    CHECK(contains(once, "!var playerName string = \"nobody\""));
    CHECK(contains(once, "!var loose any"));

    Graph back;
    Diagnostics d;
    REQUIRE(parseLime(once, back, d));
    REQUIRE(back.variables.size() == 3);
    CHECK(back.variables[0].name == "score");
    CHECK(back.variables[0].type == "number");
    CHECK(back.variables[0].defaultValue == "0");
    CHECK(back.variables[1].defaultValue == "\"nobody\"");
    CHECK(back.variables[2].defaultValue.empty());
    CHECK(writeLime(back) == once);

    CHECK(back.properties.empty());
}

TEST_CASE("variables compile to a file-scope local with their default") {
    Catalog cat;
    Graph g;
    g.moduleName = "content.logic";
    g.variables.push_back({"score", "number", "0"});
    g.variables.push_back({"label", "string", "\"hi\""});
    g.variables.push_back({"empty", "any", ""});
    g.addNode("Lime.onStart", 0, 0);

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "logic.lime", d);
    REQUIRE(r.ok);

    CHECK(contains(r.lua, "local v_score = 0"));
    CHECK(contains(r.lua, "local v_label = \"hi\""));
    CHECK(contains(r.lua, "local v_empty = nil"));

    CHECK(r.lua.find("local v_score") < r.lua.find("Lime.onStart"));
}

TEST_CASE("identifiers are prefixed so a variable cannot shadow the runtime") {
    CHECK(varIdent("score") == "v_score");
    CHECK(varIdent("print") == "v_print");
    CHECK(varIdent("M") == "v_M");
    CHECK(varIdent("has space") == "v_has_space");
    CHECK(varIdent("dots.and-dashes") == "v_dots_and_dashes");
}

TEST_CASE("get and set nodes read and write the variable") {
    Graph decl;
    decl.moduleName = "content.logic";
    decl.variables.push_back({"score", "number", "0"});
    const std::string root = projectWith("getset", decl);

    Catalog cat(root);
    const NodeDesc* get = cat.nodes.find(graphVarGetId("content.logic", "score"));
    const NodeDesc* set = cat.nodes.find(graphVarSetId("content.logic", "score"));
    REQUIRE(get != nullptr);
    REQUIRE(set != nullptr);
    CHECK(get->pure);
    CHECK(get->category == "Variables/content.logic");
    CHECK(get->display == "Get score");
    CHECK(set->display == "Set score");

    Graph g;
    g.moduleName = "content.logic";
    g.variables = decl.variables;
    const NodeId start = g.addNode("Lime.onStart", 0, 0);
    const NodeId write = g.addNode(set->id, 200, 0);
    const NodeId read = g.addNode(get->id, 0, 200);
    g.connect(PinId::make(start, "out"), PinId::make(write, "in"), PinKind::Exec);
    g.connect(PinId::make(read, "value"), PinId::make(write, "value"),
              PinKind::Data);

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "logic.lime", d);
    REQUIRE(r.ok);
    CHECK(contains(r.lua, "local v_score = 0"));
    CHECK(contains(r.lua, "v_score = v_score"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("a variable from another graph is refused at compile time") {
    Graph decl;
    decl.moduleName = "content.other";
    decl.variables.push_back({"score", "number", "0"});
    const std::string root = projectWith("foreign", decl);

    Catalog cat(root);
    const NodeDesc* get = cat.nodes.find(graphVarGetId("content.other", "score"));
    REQUIRE(get != nullptr);

    Graph g;
    g.moduleName = "content.logic";
    const NodeId start = g.addNode("Lime.onStart", 0, 0);
    const NodeId read = g.addNode(get->id, 0, 200);
    const NodeId log = g.addNode("Lime.log", 200, 0);
    if (cat.nodes.find("Lime.log")) {
        g.connect(PinId::make(start, "out"), PinId::make(log, "in"), PinKind::Exec);
        g.connect(PinId::make(read, "value"), PinId::make(log, "message"),
                  PinKind::Data);
    }

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "logic.lime", d);
    CHECK_FALSE(r.ok);
    bool named = false;
    for (const Diagnostic& x : d.all())
        if (x.severity == Severity::Error
            && x.message.find("another graph") != std::string::npos)
            named = true;
    CHECK(named);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("variables and properties coexist in one graph") {
    Graph g;
    g.moduleName = "content.enemy";
    g.properties.push_back({"speed", "number", "5"});
    g.variables.push_back({"cooldown", "number", "1.5"});
    g.addNode("behaviour.onTick", 0, 0);

    const std::string once = writeLime(g);
    Graph back;
    Diagnostics d;
    REQUIRE(parseLime(once, back, d));
    REQUIRE(back.properties.size() == 1);
    REQUIRE(back.variables.size() == 1);
    CHECK(back.properties[0].name == "speed");
    CHECK(back.variables[0].name == "cooldown");
    CHECK(writeLime(back) == once);

    Catalog cat;
    const CompileResult r =
        compileGraph(back, cat.nodes, cat.types, cat.emitters, "enemy.lime", d);
    REQUIRE(r.ok);
    CHECK(contains(r.lua, "local v_cooldown = 1.5"));
    CHECK(contains(r.lua, "local M = {}"));
    CHECK(r.lua.find("local v_cooldown") < r.lua.find("local M = {}"));
}

TEST_CASE("every lifecycle root exposes a draggable exec output") {
    Catalog cat;
    for (const char* id : {"Lime.onInit", "Lime.onStart", "Lime.onUpdate",
                           "Lime.onClose", "behaviour.onSpawn",
                           "behaviour.onTick", "behaviour.onDestroy"}) {
        CAPTURE(id);
        const NodeDesc* d = cat.nodes.find(id);
        REQUIRE(d != nullptr);
        CHECK(d->isEvent);
        const PinDesc* out = d->findPin("out");
        REQUIRE(out != nullptr);
        CHECK(out->dir == PinDir::Out);
        CHECK(out->kind == PinKind::Exec);
        CHECK(d->hasExecPins());
    }

    const std::filesystem::path vsc =
        std::filesystem::path(LIMEVSC_TEST_DIR).parent_path();
    std::filesystem::path api;
    for (const std::filesystem::path& c :
         {vsc.parent_path() / "LimeX" / "LimeEngine" / "api",
          vsc.parent_path() / "LimeEngine" / "api"})
        if (std::filesystem::exists(c / "Lime.lua")) { api = c; break; }
    REQUIRE_FALSE(api.empty());

    Catalog full;
    full.nodes.addProvider(std::make_unique<LuaLSProvider>(
        (api / "Lime.lua").string(), (api / "Enums.lua").string(),
        kPriorityGenerated));
    full.nodes.rebuild(full.types, full.diag);

    int landings = 0;
    for (const NodeDesc& d : full.nodes.all())
        for (const PinDesc& p : d.pins)
            if (p.kind == PinKind::Exec && p.dir == PinDir::In) { ++landings; break; }
    CHECK(full.nodes.all().size() > 100);
    CHECK(landings > 50);
}

TEST_CASE("a raw node referencing a script requires it") {
    Catalog cat;
    Graph g;
    g.moduleName = "content.main";
    const NodeId start = g.addNode("Lime.onStart", 0, 0);
    const NodeId raw = g.addNode("core.raw", 200, 0);
    g.connect(PinId::make(start, "out"), PinId::make(raw, "in"), PinKind::Exec);
    g.node(raw)->values.emplace_back("script", "\"helpers.lua\"");

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "main.lime", d);
    REQUIRE(r.ok);
    CHECK(contains(r.lua, "require(\"content.helpers\")"));
}

TEST_CASE("the module name matches what LimeBuilder packs") {
    Catalog cat;
    struct Case { const char* stored; const char* want; };
    for (const Case& c : {
             Case{"\"helpers.lua\"",              "content.helpers"},
             Case{"\"util/math.lua\"",            "content.util.math"},
             Case{"\"content/helpers.lua\"",      "content.helpers"},
             Case{"\"helpers\"",                  "content.helpers"},
         }) {
        CAPTURE(c.stored);
        Graph g;
        g.moduleName = "content.main";
        const NodeId start = g.addNode("Lime.onStart", 0, 0);
        const NodeId raw = g.addNode("core.raw", 200, 0);
        g.connect(PinId::make(start, "out"), PinId::make(raw, "in"),
                  PinKind::Exec);
        g.node(raw)->values.emplace_back("script", c.stored);

        Diagnostics d;
        const CompileResult r =
            compileGraph(g, cat.nodes, cat.types, cat.emitters, "main.lime", d);
        REQUIRE(r.ok);
        CHECK(contains(r.lua, std::string("require(\"") + c.want + "\")"));
    }
}

TEST_CASE("an unset script falls back to the inline body") {
    Catalog cat;
    Graph g;
    g.moduleName = "content.main";
    const NodeId start = g.addNode("Lime.onStart", 0, 0);
    const NodeId raw = g.addNode("core.raw", 200, 0);
    g.connect(PinId::make(start, "out"), PinId::make(raw, "in"), PinKind::Exec);
    g.node(raw)->rawBody = "print('inline')";

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "main.lime", d);
    REQUIRE(r.ok);
    CHECK(contains(r.lua, "print('inline')"));
    CHECK_FALSE(contains(r.lua, "require("));

    g.node(raw)->values.emplace_back("script", "\"\"");
    Diagnostics d2;
    const CompileResult r2 =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "main.lime", d2);
    REQUIRE(r2.ok);
    CHECK(contains(r2.lua, "print('inline')"));
    CHECK_FALSE(contains(r2.lua, "require("));
}

TEST_CASE("an exec waypoint compiles to nothing at all") {
    Catalog cat;
    REQUIRE(cat.nodes.find("core.reroute.exec") != nullptr);

    Graph plain;
    plain.moduleName = "content.main";
    {
        const NodeId s = plain.addNode("Lime.onStart", 0, 0);
        const NodeId r = plain.addNode("core.raw", 400, 0);
        plain.node(r)->rawBody = "print('hi')";
        plain.connect(PinId::make(s, "out"), PinId::make(r, "in"), PinKind::Exec);
    }

    Graph routed;
    routed.moduleName = "content.main";
    {
        const NodeId s = routed.addNode("Lime.onStart", 0, 0);
        const NodeId w = routed.addNode("core.reroute.exec", 200, 0);
        const NodeId r = routed.addNode("core.raw", 400, 0);
        routed.node(r)->rawBody = "print('hi')";
        routed.connect(PinId::make(s, "out"), PinId::make(w, "in"), PinKind::Exec);
        routed.connect(PinId::make(w, "out"), PinId::make(r, "in"), PinKind::Exec);
    }

    Diagnostics d1, d2;
    const CompileResult a =
        compileGraph(plain, cat.nodes, cat.types, cat.emitters, "main.lime", d1);
    const CompileResult b =
        compileGraph(routed, cat.nodes, cat.types, cat.emitters, "main.lime", d2);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(b.lua == a.lua);
}

TEST_CASE("a chain of exec waypoints still compiles to nothing") {
    Catalog cat;
    Graph g;
    g.moduleName = "content.main";
    const NodeId s = g.addNode("Lime.onStart", 0, 0);
    NodeId prev = s;
    const char* prevPin = "out";
    for (int i = 0; i < 4; ++i) {
        const NodeId w = g.addNode("core.reroute.exec", 100.0f * (i + 1), 0);
        g.connect(PinId::make(prev, prevPin), PinId::make(w, "in"), PinKind::Exec);
        prev = w;
        prevPin = "out";
    }
    const NodeId r = g.addNode("core.raw", 600, 0);
    g.node(r)->rawBody = "print('end')";
    g.connect(PinId::make(prev, prevPin), PinId::make(r, "in"), PinKind::Exec);

    Diagnostics d;
    const CompileResult res =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "main.lime", d);
    REQUIRE(res.ok);
    CHECK(contains(res.lua, "print('end')"));
    CHECK_FALSE(contains(res.lua, "reroute"));
}
