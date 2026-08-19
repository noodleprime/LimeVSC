#include <doctest/doctest.h>

#include "asset/asset_db.h"
#include "scene/component_provider.h"
#include "scene/scene_compile.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace lime;

namespace {

struct Sandbox {
    fs::path root;

    explicit Sandbox(const char* name) {
        std::error_code ec;
        root = fs::temp_directory_path(ec) / (std::string("limevsc_asset_") + name);
        fs::remove_all(root, ec);
        fs::create_directories(root / "content", ec);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& rel, const std::string& bytes) const {
        const fs::path p = root / "content" / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool exists(const std::string& rel) const {
        std::error_code ec;
        return fs::exists(root / "content" / rel, ec);
    }
    std::string str() const { return root.string(); }
};

AssetTypeRegistry types() {
    AssetTypeRegistry t;
    Diagnostics d;
    t.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limeassets", d);
    REQUIRE_FALSE(d.hasErrors());
    return t;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}

TEST_CASE("guids round-trip through text") {
    const AssetGuid g = AssetGuid::mint();
    CHECK(g.valid());
    CHECK(g.str().size() == 32);
    CHECK(AssetGuid::parse(g.str()) == g);

    CHECK_FALSE(AssetGuid::parse("").valid());
    CHECK_FALSE(AssetGuid::parse("nothex").valid());
    CHECK_FALSE(AssetGuid::parse(std::string(31, 'a')).valid());
    CHECK_FALSE(AssetGuid::parse(std::string(32, 'z')).valid());

    CHECK_FALSE(AssetGuid::mint() == AssetGuid::mint());

    const std::string ref = AssetDatabase::makeRef(g);
    CHECK(AssetDatabase::parseRef(ref) == g);
    CHECK(AssetDatabase::parseRef("\"" + ref + "\"") == g);
    CHECK_FALSE(AssetDatabase::parseRef("models/x.obj").valid());
    CHECK_FALSE(AssetDatabase::parseRef("asset:").valid());
}

TEST_CASE("scanning imports assets and ignores everything else") {
    const Sandbox box("import");
    box.write("models/crate.obj", "v 0 0 0\n");
    box.write("textures/wood.png", "\x89PNG fake");
    box.write("player.lime", "!lime 1\n");
    box.write("player.lua", "return {}\n");

    const AssetTypeRegistry t = types();
    AssetDatabase db;
    Diagnostics d;
    db.scan(box.str(), t, d);

    CHECK(db.size() == 2);
    CHECK_FALSE(d.hasErrors());
    CHECK(box.exists("models/crate.obj.limeasset"));
    CHECK(box.exists("textures/wood.png.limeasset"));
    CHECK_FALSE(box.exists("player.lime.limeasset"));

    const AssetRecord* crate = db.findByPath("models/crate.obj");
    REQUIRE(crate != nullptr);
    CHECK(crate->type == "Mesh");
    CHECK(crate->guid.valid());
    CHECK(crate->size == 8);
    CHECK_FALSE(crate->missing);
    CHECK(db.findByPath("textures/wood.png")->type == "Texture");

    const AssetGuid was = crate->guid;
    Diagnostics d2;
    db.scan(box.str(), t, d2);
    CHECK(db.size() == 2);
    CHECK(db.findByPath("models/crate.obj")->guid == was);
}

TEST_CASE("renaming an asset keeps its guid, and its references") {
    const Sandbox box("rename");
    box.write("models/player.obj", "v 1 2 3\n");

    const AssetTypeRegistry t = types();
    AssetDatabase db;
    Diagnostics d;
    db.scan(box.str(), t, d);
    REQUIRE(db.size() == 1);
    const AssetGuid original = db.all()[0].guid;

    SUBCASE("renamed inside the editor, sidecar and all") {
        std::error_code ec;
        fs::rename(box.root / "content/models/player.obj",
                   box.root / "content/models/hero.obj", ec);
        fs::rename(box.root / "content/models/player.obj.limeasset",
                   box.root / "content/models/hero.obj.limeasset", ec);

        Diagnostics d2;
        db.scan(box.str(), t, d2);
        REQUIRE(db.size() == 1);
        CHECK(db.all()[0].guid == original);
        CHECK(db.all()[0].relPath == "models/hero.obj");
        CHECK(db.pathOf(original) == "models/hero.obj");
    }

    SUBCASE("renamed outside the editor, leaving the sidecar orphaned") {
        std::error_code ec;
        fs::rename(box.root / "content/models/player.obj",
                   box.root / "content/models/hero.obj", ec);

        Diagnostics d2;
        db.scan(box.str(), t, d2);
        REQUIRE(db.size() == 1);
        CHECK(db.all()[0].guid == original);
        CHECK(db.all()[0].relPath == "models/hero.obj");
        CHECK_FALSE(db.all()[0].missing);
        CHECK_FALSE(box.exists("models/player.obj.limeasset"));
        CHECK(box.exists("models/hero.obj.limeasset"));
    }

    SUBCASE("moved to another folder") {
        std::error_code ec;
        fs::create_directories(box.root / "content/characters", ec);
        fs::rename(box.root / "content/models/player.obj",
                   box.root / "content/characters/player.obj", ec);
        fs::rename(box.root / "content/models/player.obj.limeasset",
                   box.root / "content/characters/player.obj.limeasset", ec);

        Diagnostics d2;
        db.scan(box.str(), t, d2);
        REQUIRE(db.size() == 1);
        CHECK(db.all()[0].guid == original);
        CHECK(db.pathOf(original) == "characters/player.obj");
    }
}

TEST_CASE("a deleted asset stays known, so references can be named") {
    const Sandbox box("delete");
    box.write("models/gone.obj", "v 9\n");

    const AssetTypeRegistry t = types();
    AssetDatabase db;
    Diagnostics d;
    db.scan(box.str(), t, d);
    const AssetGuid guid = db.all()[0].guid;

    std::error_code ec;
    fs::remove(box.root / "content/models/gone.obj", ec);

    Diagnostics d2;
    db.scan(box.str(), t, d2);
    REQUIRE(db.size() == 1);
    CHECK(db.all()[0].missing);
    CHECK(db.all()[0].relPath == "models/gone.obj");
    CHECK(db.pathOf(guid).empty());
    CHECK(db.find(guid) != nullptr);
}

TEST_CASE("scenes reference assets by guid and cook to paths") {
    const Sandbox box("cook");
    box.write("models/crate.obj", "v 0\n");

    const AssetTypeRegistry t = types();
    AssetDatabase db;
    Diagnostics d;
    db.scan(box.str(), t, d);
    const AssetGuid crate = db.all()[0].guid;

    TypeRegistry tr;
    ComponentRegistry comps;
    Diagnostics cd;
    comps.addProvider(std::make_unique<ComponentFileProvider>(
        std::string(LIMEVSC_DATA_DIR) + "/core.limecomponents", "core",
        kComponentPriorityCore));
    comps.rebuild(tr, cd);

    Scene s;
    const EntityId e = s.addEntity("Crate", {});
    Component m;
    m.type = "MeshRenderer";
    m.setValue("mesh", "\"" + AssetDatabase::makeRef(crate) + "\"");
    s.entity(e)->components.push_back(m);

    SUBCASE("resolves to the current path") {
        Diagnostics od;
        const SceneCompileResult r =
            compileScene(s, comps, "x.limescene", od, &db);
        CHECK(r.ok);
        CHECK(contains(r.lua, "mesh = \"content/models/crate.obj\""));
        CHECK_FALSE(contains(r.lua, "asset:"));
        CHECK(findBrokenRefs(s, comps, db).empty());
    }

    SUBCASE("follows a rename with no edit to the scene") {
        std::error_code ec;
        fs::rename(box.root / "content/models/crate.obj",
                   box.root / "content/models/box.obj", ec);
        fs::rename(box.root / "content/models/crate.obj.limeasset",
                   box.root / "content/models/box.obj.limeasset", ec);
        Diagnostics sd;
        db.scan(box.str(), t, sd);

        Diagnostics od;
        const SceneCompileResult r =
            compileScene(s, comps, "x.limescene", od, &db);
        CHECK(r.ok);
        CHECK(contains(r.lua, "mesh = \"content/models/box.obj\""));
        CHECK(findBrokenRefs(s, comps, db).empty());
    }

    SUBCASE("names the entity when the asset is deleted") {
        std::error_code ec;
        fs::remove(box.root / "content/models/crate.obj", ec);
        Diagnostics sd;
        db.scan(box.str(), t, sd);

        const std::vector<BrokenRef> broken = findBrokenRefs(s, comps, db);
        REQUIRE(broken.size() == 1);
        CHECK(broken[0].entity == "Crate");
        CHECK(broken[0].component == "MeshRenderer");
        CHECK(broken[0].prop == "mesh");
        CHECK(broken[0].lastKnownPath == "models/crate.obj");

        Diagnostics od;
        const SceneCompileResult r =
            compileScene(s, comps, "x.limescene", od, &db);
        CHECK_FALSE(r.ok);
        CHECK(od.hasErrors());
        CHECK(contains(r.lua, "mesh = nil"));
    }
}

TEST_CASE("a project with no asset database still cooks plain paths") {
    TypeRegistry tr;
    ComponentRegistry comps;
    Diagnostics cd;
    comps.addProvider(std::make_unique<ComponentFileProvider>(
        std::string(LIMEVSC_DATA_DIR) + "/core.limecomponents", "core",
        kComponentPriorityCore));
    comps.rebuild(tr, cd);

    Scene s;
    const EntityId e = s.addEntity("Old", {});
    Component m;
    m.type = "MeshRenderer";
    m.setValue("mesh", "\"models/legacy.obj\"");
    s.entity(e)->components.push_back(m);

    Diagnostics d;
    const SceneCompileResult r = compileScene(s, comps, "x.limescene", d, nullptr);
    CHECK(r.ok);
    CHECK(contains(r.lua, "mesh = \"models/legacy.obj\""));
}

TEST_CASE("import settings survive a rescan") {
    const Sandbox box("settings");
    box.write("models/thing.obj", "v 0\n");

    const AssetTypeRegistry t = types();
    AssetDatabase db;
    Diagnostics d;
    db.scan(box.str(), t, d);

    AssetRecord rec = db.all()[0];
    rec.importSettings.emplace_back("scale", "2.5");
    const std::string side =
        sidecarPath((box.root / "content/models/thing.obj").string());
    Diagnostics wd;
    REQUIRE(writeSidecar(side, rec, wd));

    Diagnostics d2;
    db.scan(box.str(), t, d2);
    REQUIRE(db.size() == 1);
    REQUIRE(db.all()[0].importSettings.size() == 1);
    CHECK(db.all()[0].importSettings[0].first == "scale");
    CHECK(db.all()[0].importSettings[0].second == "2.5");
    CHECK(db.all()[0].guid == rec.guid);
}

TEST_CASE("asset type registry maps extensions case-insensitively") {
    const AssetTypeRegistry t = types();
    CHECK(t.typeForExtension(".obj") == "Mesh");
    CHECK(t.typeForExtension(".OBJ") == "Mesh");
    CHECK(t.typeForExtension(".PnG") == "Texture");
    CHECK(t.typeForExtension(".wav") == "Sound");
    CHECK(t.typeForExtension(".lime").empty());
    CHECK(t.typeForExtension(".lua").empty());
    CHECK(t.typeForExtension("").empty());
    REQUIRE(t.find("Mesh") != nullptr);
    CHECK(t.find("Mesh")->display == "Model");
    CHECK(t.find("Nope") == nullptr);
}
