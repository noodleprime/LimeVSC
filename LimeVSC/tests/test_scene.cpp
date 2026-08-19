#include <doctest/doctest.h>

#include "app/editor.h"

#include "scene/component_provider.h"
#include "scene/scene.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace lime;

namespace {

Scene parseOk(const std::string& text) {
    Scene s;
    Diagnostics d;
    REQUIRE(parseScene(text, s, d));
    return s;
}

}

TEST_CASE("scene round-trips byte-identically") {
    Scene s;
    s.name = "Level01";

    const EntityId player = s.addEntity("Player", {});
    {
        Entity* e = s.entity(player);
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(0, 1, 0)");
        e->components.push_back(t);
        Component m;
        m.type = "MeshRenderer";
        m.setValue("mesh", "\"models/player.obj\"");
        e->components.push_back(m);
    }
    const EntityId cam = s.addEntity("Main Camera", player);
    {
        Entity* e = s.entity(cam);
        Component c;
        c.type = "Camera";
        c.setValue("fov", "70");
        e->components.push_back(c);
    }
    s.addEntity("Empty Group", {});

    const std::string once = writeScene(s);
    const Scene reread = parseOk(once);
    const std::string twice = writeScene(reread);
    CHECK(once == twice);

    CHECK(reread.name == "Level01");
    CHECK(reread.size() == 3);
    CHECK(reread.entity(cam)->parent == player);
    CHECK(*reread.entity(player)->component("Transform")->value("position")
          == "Vec3.new(0, 1, 0)");
}

TEST_CASE("scene writer is canonical regardless of insertion order") {
    Scene a, b;
    const EntityId ea = a.addEntity("E", {});
    Component c1;
    c1.type = "Transform";
    c1.setValue("scale", "Vec3.new(1, 1, 1)");
    c1.setValue("position", "Vec3.new(0, 0, 0)");
    Component c2;
    c2.type = "Camera";
    c2.setValue("fov", "60");
    a.entity(ea)->components = {c1, c2};

    const EntityId eb = b.addEntity("E", {});
    Component d1 = c2, d2 = c1;
    std::reverse(d2.values.begin(), d2.values.end());
    b.entity(eb)->components = {d1, d2};

    CHECK(writeScene(a) == writeScene(b));
}

TEST_CASE("scene names survive punctuation the suffix form would break") {
    Scene s;
    s.addEntity("Boss : 3", {});
    const EntityId messy = s.addEntity("  spaced\nname  ", {});
    CHECK(s.entity(messy)->name == "spaced name");

    const std::string once = writeScene(s);
    const Scene back = parseOk(once);
    CHECK(writeScene(back) == once);
    CHECK(back.childrenOf({}).size() == 2);
    CHECK(back.entity(EntityId{0})->name == "Boss : 3");
}

TEST_CASE("unknown entity ids and missing parents are reported, not fatal") {
    Scene s;
    Diagnostics d;
    REQUIRE(parseScene("!limescene 1\n@0 3 Orphan\n", s, d));
    REQUIRE(s.size() == 1);
    REQUIRE(s.entity(EntityId{0}) != nullptr);
    CHECK_FALSE(s.entity(EntityId{0})->parent.valid());
    CHECK(d.all().size() == 1);

    Scene wide;
    Diagnostics wd;
    REQUIRE(parseScene("!limescene 1\n@a0 - Far\n", wide, wd));
    CHECK(wide.entity(EntityId{360}) != nullptr);
    CHECK(wide.nextId == 361);

    Scene bad;
    Diagnostics bd;
    CHECK_FALSE(parseScene("@0 - X\n", bad, bd));
    Diagnostics bd2;
    CHECK_FALSE(parseScene("!limescene 1\n  ~ Transform\n", bad, bd2));
    Diagnostics bd3;
    CHECK_FALSE(parseScene("!limescene 1\n@zz! - X\n", bad, bd3));
}

TEST_CASE("a UTF-8 BOM on a scene file is stripped, never written") {
    std::error_code ec;
    const std::filesystem::path p =
        std::filesystem::temp_directory_path(ec) / "limevsc_bom.limescene";
    {
        std::ofstream f(p, std::ios::binary);
        const std::string text =
            std::string("\xEF\xBB\xBF") + "!limescene 1\n!name Boot\n@0 - E\n";
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    Scene s;
    Diagnostics d;
    REQUIRE(readScene(p.string(), s, d));
    CHECK(s.name == "Boot");
    CHECK(s.size() == 1);
    CHECK_FALSE(d.hasErrors());
    CHECK(writeScene(s).compare(0, 3, "\xEF\xBB\xBF") != 0);
    std::filesystem::remove(p, ec);
}

TEST_CASE("removing an entity takes its subtree, deepest first") {
    Scene s;
    const EntityId a = s.addEntity("A", {});
    const EntityId b = s.addEntity("B", a);
    const EntityId c = s.addEntity("C", b);
    const EntityId other = s.addEntity("D", {});

    const std::vector<EntityId> gone = s.removeEntity(a);
    CHECK(gone.size() == 3);
    CHECK(gone.front() == c);
    CHECK(gone.back() == a);
    CHECK(s.size() == 1);
    CHECK(s.entity(other) != nullptr);
    CHECK(s.entity(b) == nullptr);
}

TEST_CASE("reparenting refuses to build a cycle") {
    Scene s;
    const EntityId a = s.addEntity("A", {});
    const EntityId b = s.addEntity("B", a);
    const EntityId c = s.addEntity("C", b);

    CHECK(s.isDescendantOf(c, a));
    CHECK_FALSE(s.isDescendantOf(a, c));
    CHECK_FALSE(s.reparent(a, c));
    CHECK_FALSE(s.reparent(a, a));
    CHECK(s.entity(a)->parent == EntityId{});
    CHECK(s.reparent(c, {}));
    CHECK_FALSE(s.entity(c)->parent.valid());
}

TEST_CASE("id lookup stays correct across append and removal") {
    Scene s;
    std::vector<EntityId> ids;
    for (int i = 0; i < 500; ++i) ids.push_back(s.addEntity("E", {}));
    for (EntityId id : ids) REQUIRE(s.entity(id) != nullptr);

    s.removeEntity(ids[250]);
    CHECK(s.entity(ids[250]) == nullptr);
    CHECK(s.entity(ids[249]) != nullptr);
    CHECK(s.entity(ids[251]) != nullptr);

    const EntityId fresh = s.addEntity("After", {});
    CHECK(s.entity(fresh) != nullptr);
    CHECK(s.entity(ids[250]) == nullptr);
}

TEST_CASE("component provider conformance") {
    TypeRegistry types;
    Diagnostics d;
    ComponentRegistry reg;
    reg.addProvider(std::make_unique<ComponentFileProvider>(
        std::string(LIMEVSC_DATA_DIR) + "/core.limecomponents", "core",
        kComponentPriorityCore));
    reg.rebuild(types, d);

    for (const Diagnostic& x : d.all()) CHECK(x.severity != Severity::Error);
    REQUIRE(reg.all().size() >= 8);

    for (std::size_t i = 1; i < reg.all().size(); ++i)
        CHECK(reg.all()[i - 1].id < reg.all()[i].id);
    for (const ComponentDesc& c : reg.all()) {
        CHECK(reg.find(c.id) == &c);
        CHECK_FALSE(c.id.empty());
        for (const PropDesc& p : c.props) {
            CHECK_FALSE(p.name.empty());
            CHECK(p.type.valid());
            CHECK(c.findProp(p.name) == &p);
        }
    }
    CHECK(reg.find("Nope") == nullptr);

    const ComponentDesc* tr = reg.find("Transform");
    REQUIRE(tr != nullptr);
    CHECK(tr->unique);
    CHECK(tr->findProp("position")->defaultValue == "Vec3.new(0, 0, 0)");
    CHECK_FALSE(reg.find("Tag")->unique);
    CHECK(reg.categories().size() >= 3);
}

TEST_CASE("higher priority wins a component id collision") {
    struct Fake final : IComponentProvider {
        std::string doc;
        int prio;
        Fake(std::string d, int p) : doc(std::move(d)), prio(p) {}
        std::string_view name() const override { return "fake"; }
        int priority() const override { return prio; }
        void collect(TypeRegistry&, std::vector<ComponentDesc>& out,
                     Diagnostics&) override {
            ComponentDesc c;
            c.id = "Health";
            c.doc = doc;
            out.push_back(std::move(c));
        }
    };

    TypeRegistry types;
    Diagnostics d;
    ComponentRegistry reg;
    reg.addProvider(std::make_unique<Fake>("low", 10));
    reg.addProvider(std::make_unique<Fake>("high", 30));
    reg.rebuild(types, d);

    REQUIRE(reg.all().size() == 1);
    CHECK(reg.find("Health")->doc == "high");
}

TEST_CASE("copying an entity takes its whole subtree") {
    EditorContext ed;
    const EntityId parent = ed.addEntity("Parent", {});
    const EntityId child = ed.addEntity("Child", parent);
    ed.addEntity("Grandchild", child);
    {
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(3, 0, 0)");
        ed.scene.entity(parent)->components.push_back(std::move(t));
    }

    ed.copyEntity(parent);
    CHECK(ed.clipEntities.size() == 3);

    const std::size_t before = ed.scene.size();
    ed.pasteEntity({});
    CHECK(ed.scene.size() == before + 3);

    const std::vector<EntityId> roots = ed.scene.childrenOf({});
    REQUIRE(roots.size() == 2);
    const Entity* copy = ed.scene.entity(roots.back());
    REQUIRE(copy != nullptr);
    CHECK(copy->name == "Parent");
    CHECK(copy->component("Transform") != nullptr);

    const std::vector<EntityId> kids = ed.scene.childrenOf(copy->id);
    REQUIRE(kids.size() == 1);
    CHECK(ed.scene.entity(kids.front())->name == "Child");
    CHECK(ed.scene.childrenOf(kids.front()).size() == 1);
}

TEST_CASE("pasting an entity into another parents it there") {
    EditorContext ed;
    const EntityId a = ed.addEntity("A", {});
    const EntityId b = ed.addEntity("B", {});

    ed.copyEntity(a);
    ed.pasteEntity(b);

    const std::vector<EntityId> kids = ed.scene.childrenOf(b);
    REQUIRE(kids.size() == 1);
    CHECK(ed.scene.entity(kids.front())->name == "A");
}

TEST_CASE("an entity cannot be parented to its own child") {
    EditorContext ed;
    const EntityId parent = ed.addEntity("Parent", {});
    const EntityId child = ed.addEntity("Child", parent);

    ed.reparentEntity(parent, child);
    CHECK(ed.scene.entity(parent)->parent == EntityId{});
    CHECK(ed.scene.entity(child)->parent == parent);
}

TEST_CASE("a component can be copied onto another entity") {
    EditorContext ed;
    const EntityId a = ed.addEntity("A", {});
    const EntityId b = ed.addEntity("B", {});
    {
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(7, 0, 0)");
        ed.scene.entity(a)->components.push_back(std::move(t));
    }

    ed.copyComponent(a, "Transform");
    CHECK(ed.hasClipComponent);
    ed.pasteComponent(b);

    const Component* on = ed.scene.entity(b)->component("Transform");
    REQUIRE(on != nullptr);
    CHECK(*on->value("position") == "Vec3.new(7, 0, 0)");

    REQUIRE(ed.undoLast());
    CHECK(ed.scene.entity(b)->component("Transform") == nullptr);
}

TEST_CASE("pasting over an existing component replaces its values") {
    EditorContext ed;
    const EntityId a = ed.addEntity("A", {});
    Component t;
    t.type = "Transform";
    t.setValue("position", "Vec3.new(1, 1, 1)");
    ed.scene.entity(a)->components.push_back(t);

    ed.clipComponent = t;
    ed.clipComponent.setValue("position", "Vec3.new(9, 9, 9)");
    ed.hasClipComponent = true;
    ed.pasteComponent(a);

    CHECK(*ed.scene.entity(a)->component("Transform")->value("position")
          == "Vec3.new(9, 9, 9)");
    CHECK(ed.scene.entity(a)->components.size() == 1);

    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(a)->component("Transform")->value("position")
          == "Vec3.new(1, 1, 1)");
}

TEST_CASE("unparenting returns an entity to the root") {
    EditorContext ed;
    const EntityId parent = ed.addEntity("Parent", {});
    const EntityId child = ed.addEntity("Child", parent);
    REQUIRE(ed.scene.entity(child)->parent == parent);

    ed.reparentEntity(child, EntityId{});
    CHECK(ed.scene.entity(child)->parent == EntityId{});
    CHECK(ed.scene.childrenOf(parent).empty());

    REQUIRE(ed.undoLast());
    CHECK(ed.scene.entity(child)->parent == parent);
}
