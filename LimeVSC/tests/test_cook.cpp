#include <doctest/doctest.h>

#include "app/editor.h"
#include "scene/scene_compile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace lime;

namespace {

struct Project {
    fs::path root;

    explicit Project(const char* name) {
        std::error_code ec;
        root = fs::temp_directory_path(ec) / (std::string("limevsc_cook_") + name);
        fs::remove_all(root, ec);
        fs::create_directories(root / "content", ec);
    }
    ~Project() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& rel, const std::string& bytes) const {
        const fs::path p = root / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool exists(const std::string& rel) const {
        std::error_code ec;
        return fs::exists(root / rel, ec);
    }
    std::string read(const std::string& rel) const {
        std::ifstream f(root / rel, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    std::string str() const { return root.string(); }
};

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::vector<std::string> packagedFiles(const Project& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p.root / "bin", ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        out.push_back(fs::relative(it->path(), p.root / "bin", ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}

TEST_CASE("the start scene resolves to the module LimeBuilder will pack") {
    ProjectContext proj;
    proj.root = "C:/game";

    proj.startScene = "content/main.limescene";
    CHECK(sceneModuleName(proj) == "content.main_scene");

    proj.startScene = "content/levels/arena.limescene";
    CHECK(sceneModuleName(proj) == "content.levels.arena_scene");

    proj.startScene = "content\\levels\\arena.limescene";
    CHECK(sceneModuleName(proj) == "content.levels.arena_scene");

    proj.startScene.clear();
    CHECK(sceneModuleName(proj).empty());

    proj.sceneFiles = {"C:/game/content/only.limescene"};
    CHECK(sceneModuleName(proj) == "content.only_scene");
}

TEST_CASE("the boot module wires the runtime to the engine's update hook") {
    const std::string boot = sceneBootLua("content.main_scene");

    CHECK(contains(boot, "require(\"content.lime_scene\")"));
    CHECK(contains(boot, "require(\"content.main_scene\")"));
    CHECK(contains(boot, "runtime.load(scene)"));
    CHECK(contains(boot, "Lime.onUpdate:hook("));
    CHECK(contains(boot, "runtime.tick(M.world, dt)"));
    CHECK(contains(boot, "runtime.destroy(M.world)"));
    CHECK(contains(boot, "return M"));

    CHECK(boot.compare(0, 3, "\xEF\xBB\xBF") != 0);
    CHECK(boot.find('\r') == std::string::npos);

    CHECK(sceneBootLua("content.main_scene") == boot);
    CHECK(sceneBootLua("content.other_scene") != boot);
}

TEST_CASE("cooking an engine project produces exactly the modules LimeBuilder needs") {
    const Project p("engine");
    p.write("project.limeproj",
            "[project]\nschema = 1\nmode = \"engine\"\n\n"
            "[engine]\nstartScene = \"content/main.limescene\"\n");
    p.write("content/main.limescene",
            "!limescene 1\n!name Main\n\n@0 - Player\n  ~ Transform\n");

    ProjectContext proj;
    proj.root = p.str();
    proj.scan();
    REQUIRE(proj.isEngine());
    REQUIRE(proj.sceneFiles.size() == 1);

    Diagnostics d;
    REQUIRE(cookScenes(proj, d));

    CHECK(p.exists("content/main_scene.lua"));
    CHECK(p.exists("content/lime_scene.lua"));
    CHECK(p.exists("content/lime_boot.lua"));

    CHECK(contains(p.read("content/main_scene.lua"), "name = \"Main\""));
    CHECK(contains(p.read("content/lime_boot.lua"),
                   "require(\"content.main_scene\")"));

    SUBCASE("the runtime is never overwritten once it exists") {
        p.write("content/lime_scene.lua", "-- my edits\nreturn {}\n");
        Diagnostics d2;
        REQUIRE(cookScenes(proj, d2));
        CHECK(p.read("content/lime_scene.lua") == "-- my edits\nreturn {}\n");
    }

    SUBCASE("the boot module IS rewritten, because it is derived") {
        p.write("content/lime_boot.lua", "-- stale\n");
        Diagnostics d2;
        REQUIRE(cookScenes(proj, d2));
        CHECK(contains(p.read("content/lime_boot.lua"), "runtime.load(scene)"));
    }

    SUBCASE("cooking twice changes nothing") {
        const std::string once = p.read("content/main_scene.lua");
        Diagnostics d2;
        REQUIRE(cookScenes(proj, d2));
        CHECK(p.read("content/main_scene.lua") == once);
    }
}

TEST_CASE("a framework project cooks no scene files at all") {
    const Project p("framework");
    p.write("content/main.lua", "print('hi')\n");

    ProjectContext proj;
    proj.root = p.str();
    proj.scan();
    REQUIRE_FALSE(proj.isEngine());
    CHECK(proj.sceneFiles.empty());

    NodeRegistry nodes;
    TypeRegistry types;
    const EmitterRegistry emitters = EmitterRegistry::withBuiltins();
    std::vector<LoadedMap> maps;
    Diagnostics d;
    compileProject(proj, nodes, types, emitters, maps, d);

    CHECK_FALSE(p.exists("content/lime_scene.lua"));
    CHECK_FALSE(p.exists("content/lime_boot.lua"));
    CHECK_FALSE(p.exists("project.limeproj"));
}

TEST_CASE("packaging ships cooked output and no editor sources") {
    const Project p("package");
    p.write("project.limeproj", "[project]\nmode = \"engine\"\n");
    p.write("content/main.lua", "return {}\n");
    p.write("content/main_scene.lua", "return { entities = {} }\n");
    p.write("content/lime_scene.lua", "return {}\n");
    p.write("content/main.limescene", "!limescene 1\n");
    p.write("content/main.lime", "!lime 1\n");
    p.write("content/main.map", "1 a0\n");
    p.write("content/models/crate.obj", "v 0 0 0\n");
    p.write("content/models/crate.obj.limeasset", "[asset]\nguid = \"x\"\n");
    p.write("app.exe", "MZ fake");

    ProjectContext proj;
    proj.root = p.str();
    Diagnostics d;
    REQUIRE(packageProject(proj, d));

    const std::vector<std::string> shipped = packagedFiles(p);
    auto has = [&](const char* f) {
        return std::find(shipped.begin(), shipped.end(), f) != shipped.end();
    };

    CHECK(has("app.exe"));
    CHECK(has("content/main.lua"));
    CHECK(has("content/main_scene.lua"));
    CHECK(has("content/lime_scene.lua"));
    CHECK(has("content/models/crate.obj"));

    CHECK_FALSE(has("content/main.limescene"));
    CHECK_FALSE(has("content/main.lime"));
    CHECK_FALSE(has("content/main.map"));
    CHECK_FALSE(has("content/models/crate.obj.limeasset"));
    CHECK_FALSE(has("project.limeproj"));
}

TEST_CASE("scene loading and cooking stay inside budget") {
    Scene s;
    s.name = "Stress";
    std::vector<EntityId> ids;
    ids.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        const EntityId parent = (i > 2 && i % 3 == 0) ? ids[i / 3] : EntityId{};
        const EntityId e = s.addEntity("Entity" + std::to_string(i), parent);
        ids.push_back(e);
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(" + std::to_string(i) + ", 0, 0)");
        s.entity(e)->components.push_back(std::move(t));
    }
    CHECK(s.size() == 10000);

    for (EntityId id : ids) REQUIRE(s.entity(id) != nullptr);

    const std::string text = writeScene(s);
    Scene back;
    Diagnostics d;
    REQUIRE(parseScene(text, back, d));
    CHECK(back.size() == 10000);
    CHECK(writeScene(back) == text);
}
