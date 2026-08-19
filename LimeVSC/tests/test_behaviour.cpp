#include <doctest/doctest.h>

#include "api/data_provider.h"
#include "api/graphfn_provider.h"
#include "scene/component_provider.h"
#include "scene/scene_compile.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace lime;

namespace {

struct Catalog {
    TypeRegistry      types;
    NodeRegistry      nodes;
    ComponentRegistry comps;
    EmitterRegistry   emitters = EmitterRegistry::withBuiltins();
    Diagnostics       diag;

    Catalog() {
        const std::string dir = LIMEVSC_DATA_DIR;
        types.loadFile(dir + "/core.limetypes", diag);
        nodes.addProvider(std::make_unique<DataFileProvider>(
            dir + "/core.limenodes", "core", kPriorityCore));
        nodes.rebuild(types, diag);
        comps.addProvider(std::make_unique<ComponentFileProvider>(
            dir + "/core.limecomponents", "core", kComponentPriorityCore));
        comps.rebuild(types, diag);
    }
};

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}

TEST_CASE("a graph rooted in a behaviour compiles to a module") {
    Catalog cat;

    Graph g;
    g.moduleName = "content.enemy";
    const NodeId spawn = g.addNode("behaviour.onSpawn", 0, 0);
    const NodeId tick = g.addNode("behaviour.onTick", 0, 200);
    REQUIRE(cat.nodes.find("behaviour.onTick") != nullptr);

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "enemy.lime", d);
    REQUIRE(r.ok);

    CHECK(contains(r.lua, "local M = {}"));
    CHECK(contains(r.lua, "function M.onSpawn(self)"));
    CHECK(contains(r.lua, "function M.onTick(self, dt)"));
    CHECK(contains(r.lua, "return M"));
    CHECK_FALSE(contains(r.lua, ":hook("));
    (void)spawn;
    (void)tick;
}

TEST_CASE("behaviour and hook graphs stay independent") {
    Catalog cat;
    Graph g;
    g.moduleName = "content.boot";
    REQUIRE(cat.nodes.find("Lime.onStart") != nullptr);
    g.addNode("Lime.onStart", 0, 0);

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "boot.lime", d);
    REQUIRE(r.ok);
    CHECK_FALSE(contains(r.lua, "local M = {}"));
    CHECK(contains(r.lua, "hook"));
}

TEST_CASE("!prop round-trips and produces read and write nodes") {
    Graph g;
    g.moduleName = "content.enemy";
    g.properties.push_back({"speed", "number", "5"});
    g.properties.push_back({"target", "any", ""});
    g.addNode("behaviour.onTick", 0, 0);

    const std::string once = writeLime(g);
    CHECK(contains(once, "!prop speed number = 5"));
    CHECK(contains(once, "!prop target any"));

    Graph back;
    Diagnostics d;
    REQUIRE(parseLime(once, back, d));
    REQUIRE(back.properties.size() == 2);
    CHECK(back.properties[0].name == "speed");
    CHECK(back.properties[0].type == "number");
    CHECK(back.properties[0].defaultValue == "5");
    CHECK(back.properties[1].defaultValue.empty());
    CHECK(writeLime(back) == once);
}

TEST_CASE("property nodes read and write self") {
    std::error_code ec;
    const std::string root =
        (std::filesystem::temp_directory_path(ec) / "limevsc_behaviour_test").string();
    std::filesystem::create_directories(root + "/content");
    {
        Graph g;
        g.moduleName = "content.enemy";
        g.properties.push_back({"speed", "number", "5"});
        Diagnostics wd;
        REQUIRE(writeLimeFile(root + "/content/enemy.lime", g, wd));
    }

    Catalog cat;
    cat.nodes.addProvider(std::make_unique<GraphFnProvider>(root, kPriorityOverrides));
    cat.nodes.rebuild(cat.types, cat.diag);

    const NodeDesc* get = cat.nodes.find(graphPropGetId("content.enemy", "speed"));
    const NodeDesc* set = cat.nodes.find(graphPropSetId("content.enemy", "speed"));
    REQUIRE(get != nullptr);
    REQUIRE(set != nullptr);
    CHECK(get->pure);
    CHECK(get->category == "Properties/content.enemy");

    Graph g;
    g.moduleName = "content.enemy";
    const NodeId tick = g.addNode("behaviour.onTick", 0, 0);
    const NodeId write = g.addNode(set->id, 200, 0);
    const NodeId read = g.addNode(get->id, 0, 200);
    g.connect(PinId::make(tick, "out"), PinId::make(write, "in"), PinKind::Exec);
    g.connect(PinId::make(read, "value"), PinId::make(write, "value"),
              PinKind::Data);

    Diagnostics d;
    const CompileResult r =
        compileGraph(g, cat.nodes, cat.types, cat.emitters, "enemy.lime", d);
    REQUIRE(r.ok);
    CHECK(contains(r.lua, "self.speed = self.speed"));
}

TEST_CASE("two entities share one graph with different property values") {
    Catalog cat;

    Scene s;
    s.name = "Arena";
    for (int i = 0; i < 2; ++i) {
        const EntityId e = s.addEntity(i == 0 ? "Fast" : "Slow", {});
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(" + std::to_string(i) + ", 0, 0)");
        s.entity(e)->components.push_back(t);

        Component b;
        b.type = "Behaviour";
        b.setValue("graph", "\"enemy.lime\"");
        b.setValue("speed", i == 0 ? "12" : "3");
        s.entity(e)->components.push_back(b);
    }

    Diagnostics d;
    const SceneCompileResult r = compileScene(s, cat.comps, "arena.limescene", d);
    REQUIRE(r.ok);

    CHECK(contains(r.lua, "{ module = \"content.enemy\", props = { speed = 12 } }"));
    CHECK(contains(r.lua, "{ module = \"content.enemy\", props = { speed = 3 } }"));
    CHECK(contains(r.lua, "name = \"Arena\""));
    CHECK(contains(r.lua, "Transform = { position = Vec3.new(0, 0, 0)"));
    CHECK(contains(r.lua, "scale = Vec3.new(1, 1, 1)"));

    Diagnostics d2;
    CHECK(compileScene(s, cat.comps, "arena.limescene", d2).lua == r.lua);
}

TEST_CASE("cooking is deterministic and quotes exotic keys") {
    Catalog cat;
    Scene s;
    const EntityId e = s.addEntity("Quote \"me\"", {});
    Component c;
    c.type = "Tag";
    c.setValue("name", "\"Enemy\"");
    s.entity(e)->components.push_back(c);

    Diagnostics d;
    const SceneCompileResult r = compileScene(s, cat.comps, "x.limescene", d);
    REQUIRE(r.ok);
    CHECK(contains(r.lua, "name = \"Quote \\\"me\\\"\""));
    CHECK(contains(r.lua, "Tag = { name = \"Enemy\" }"));
}

TEST_CASE("scene runtime is valid-looking Lua with the expected entry points") {
    const std::string rt = sceneRuntimeLua();
    CHECK(contains(rt, "function M.load(scene)"));
    CHECK(contains(rt, "function M.tick(world, dt)"));
    CHECK(contains(rt, "function M.destroy(world)"));
    CHECK(contains(rt, "M.builders"));
    CHECK(contains(rt, "return M"));
    CHECK(rt.compare(0, 3, "\xEF\xBB\xBF") != 0);
    CHECK(rt.find('\r') == std::string::npos);
}
