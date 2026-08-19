#include <doctest/doctest.h>

#include "app/editor.h"
#include "ui/canvas_ids.h"
#include "limecore.h"

#include <algorithm>
#include <set>
#include <span>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace lime;

TEST_CASE("an editor always has exactly one document to start with") {
    EditorContext ed;
    REQUIRE(ed.docs.size() == 1);
    CHECK(ed.activeDoc == 0);
    CHECK(ed.filePath().empty());
    CHECK_FALSE(ed.dirty());
    CHECK(ed.graph().nodes().empty());
}

TEST_CASE("documents carry their own graph, undo and selection") {
    EditorContext ed;

    const NodeId a = ed.graph().addNode("core.raw", 0, 0);
    ed.selection().push_back(a);
    ed.dirty() = true;
    ed.docs[0]->filePath = "a.lime";

    const std::size_t second = ed.addDoc();
    CHECK(second == 1);
    CHECK(ed.activeDoc == 1);

    CHECK(ed.graph().nodes().empty());
    CHECK(ed.selection().empty());
    CHECK_FALSE(ed.dirty());
    CHECK(ed.filePath().empty());

    ed.activeDoc = 0;
    CHECK(ed.graph().nodes().size() == 1);
    CHECK(ed.selection().size() == 1);
    CHECK(ed.dirty());

    ed.undo().perform(ed.graph(), EditAction{"x", [](Graph&) {}, [](Graph&) {}});
    CHECK(ed.undo().canUndo());
    ed.activeDoc = 1;
    CHECK_FALSE(ed.undo().canUndo());
}

TEST_CASE("tab labels name the file") {
    EditorContext ed;
    CHECK(ed.doc().displayName() == "untitled");

    ed.docs[0]->filePath = "C:/game/content/player.lime";
    CHECK(ed.doc().displayName() == "player.lime");
    ed.docs[0]->filePath = "content/enemies/boss.lime";
    CHECK(ed.doc().displayName() == "boss.lime");

    const std::string title = ed.doc().windowTitle();
    CHECK(title.rfind("boss.lime###", 0) == 0);
    ed.docs[0]->filePath = "content/enemies/king.lime";
    CHECK(ed.doc().windowTitle().substr(ed.doc().windowTitle().find("###"))
          == title.substr(title.find("###")));
}

TEST_CASE("documents have distinct, non-reused ids") {
    EditorContext ed;
    const std::uint32_t first = ed.docs[0]->id;
    ed.addDoc();
    const std::uint32_t second = ed.docs[1]->id;
    CHECK(first != second);

    ed.closeDoc(1);
    ed.addDoc();
    CHECK(ed.docs[1]->id != second);
    CHECK(ed.docs[1]->id != first);
}

TEST_CASE("finding a document by path") {
    EditorContext ed;
    ed.docs[0]->filePath = "a.lime";
    ed.addDoc();
    ed.docs[1]->filePath = "b.lime";

    CHECK(ed.findDoc("a.lime") == 0);
    CHECK(ed.findDoc("b.lime") == 1);
    CHECK(ed.findDoc("c.lime") == ed.docs.size());
    ed.addDoc();
    CHECK(ed.findDoc("") == ed.docs.size());
}

TEST_CASE("closing never leaves the editor with no document") {
    EditorContext ed;
    ed.docs[0]->filePath = "a.lime";
    ed.addDoc();
    ed.docs[1]->filePath = "b.lime";
    ed.addDoc();
    ed.docs[2]->filePath = "c.lime";

    ed.activeDoc = 2;
    ed.closeDoc(2);
    CHECK(ed.docs.size() == 2);
    CHECK(ed.activeDoc == 1);

    ed.closeDoc(0);
    CHECK(ed.docs.size() == 1);
    CHECK(ed.docs[0]->filePath == "b.lime");

    ed.closeDoc(0);
    CHECK(ed.docs.size() == 1);
    CHECK(ed.docs[0]->filePath.empty());
    CHECK(ed.activeDoc == 0);

    ed.closeDoc(99);
    CHECK(ed.docs.size() == 1);
}

TEST_CASE("placement bookkeeping is per document") {
    EditorContext ed;
    ed.doc().placed.insert(0);
    ed.doc().placed.insert(1);
    ed.addDoc();
    CHECK(ed.doc().placed.empty());
    ed.activeDoc = 0;
    CHECK(ed.doc().placed.size() == 2);
}

TEST_CASE("undo reverses the last edit, in whichever document it happened") {
    EditorContext ed;
    ed.scene.name = "S";
    const EntityId ent = ed.addEntity("Thing", {});
    {
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(0, 0, 0)");
        ed.scene.entity(ent)->components.push_back(std::move(t));
    }

    const NodeId n = ed.graph().addNode("core.raw", 10, 20);
    ed.moveNode(n, 50, 60);
    CHECK(ed.lastEdit == EditorContext::LastEdit::Graph);

    ed.setProp(ent, "Transform", "position", "Vec3.new(5, 0, 0)");
    CHECK(ed.lastEdit == EditorContext::LastEdit::Scene);

    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(0, 0, 0)");
    CHECK(ed.graph().node(n)->x == 50);

    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(n)->x == 10);
    CHECK(ed.graph().node(n)->y == 20);

    REQUIRE(ed.redoLast());
    CHECK(ed.graph().node(n)->x == 50);
}

TEST_CASE("a gizmo drag is one undo step, not one per frame") {
    EditorContext ed;
    const EntityId ent = ed.addEntity("Thing", {});
    Component t;
    t.type = "Transform";
    t.setValue("position", "Vec3.new(0, 0, 0)");
    ed.scene.entity(ent)->components.push_back(std::move(t));

    for (int i = 1; i <= 60; ++i)
        ed.putProp(ent, "Transform", "position",
                   "Vec3.new(" + std::to_string(i) + ", 0, 0)");
    CHECK(ed.history.size() == 1);
    ed.recordProp(ent, "Transform", "position", "Vec3.new(0, 0, 0)", true);
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(60, 0, 0)");

    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(0, 0, 0)");
    CHECK(ed.history.size() == 2);
    CHECK(ed.canUndoAny());

    REQUIRE(ed.redoLast());
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(60, 0, 0)");
}

TEST_CASE("two separate drags do not merge into one undo step") {
    EditorContext ed;
    const EntityId ent = ed.addEntity("Thing", {});
    Component t;
    t.type = "Transform";
    t.setValue("position", "Vec3.new(0, 0, 0)");
    ed.scene.entity(ent)->components.push_back(std::move(t));

    for (int i = 1; i <= 5; ++i)
        ed.putProp(ent, "Transform", "position",
                   "Vec3.new(" + std::to_string(i) + ", 0, 0)");
    ed.recordProp(ent, "Transform", "position", "Vec3.new(0, 0, 0)", true);

    for (int i = 6; i <= 10; ++i)
        ed.putProp(ent, "Transform", "position",
                   "Vec3.new(" + std::to_string(i) + ", 0, 0)");
    ed.recordProp(ent, "Transform", "position", "Vec3.new(5, 0, 0)", true);

    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(5, 0, 0)");
    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(0, 0, 0)");
}

TEST_CASE("dragging different properties keeps them separate") {
    EditorContext ed;
    const EntityId a = ed.addEntity("A", {});
    Component t;
    t.type = "Transform";
    t.setValue("position", "Vec3.new(0, 0, 0)");
    t.setValue("scale", "Vec3.new(1, 1, 1)");
    ed.scene.entity(a)->components.push_back(std::move(t));

    ed.putProp(a, "Transform", "position", "Vec3.new(9, 0, 0)");
    ed.recordProp(a, "Transform", "position", "Vec3.new(0, 0, 0)", true);
    ed.putProp(a, "Transform", "scale", "Vec3.new(2, 2, 2)");
    ed.recordProp(a, "Transform", "scale", "Vec3.new(1, 1, 1)", true);

    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(a)->component("Transform")->value("scale")
          == "Vec3.new(1, 1, 1)");
    CHECK(*ed.scene.entity(a)->component("Transform")->value("position")
          == "Vec3.new(9, 0, 0)");
    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(a)->component("Transform")->value("position")
          == "Vec3.new(0, 0, 0)");
}

TEST_CASE("undoing a node move asks the canvas to re-place it") {
    EditorContext ed;
    const NodeId n = ed.graph().addNode("core.raw", 0, 0);
    ed.doc().placed.insert(n.v);

    ed.moveNode(n, 120, 240);
    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(n)->x == 0);
    CHECK(ed.doc().placed.empty());

    ed.doc().placed.insert(n.v);
    REQUIRE(ed.redoLast());
    CHECK(ed.graph().node(n)->x == 120);
    CHECK(ed.doc().placed.empty());
}

TEST_CASE("a node drag undoes to its start position") {
    EditorContext ed;
    const NodeId n = ed.graph().addNode("core.raw", 5, 5);
    for (int i = 1; i <= 30; ++i)
        ed.placeNode(n, 5.0f + static_cast<float>(i), 5);
    CHECK(ed.graph().node(n)->x == 35);
    CHECK(ed.history.empty());

    const EditorContext::NodeMove m{n, 5, 5, 35, 5};
    ed.moveNodes({&m, 1});
    CHECK(ed.history.size() == 1);

    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(n)->x == 5);
    CHECK(ed.graph().node(n)->y == 5);
}

TEST_CASE("the Inspector follows the focused graph, not a stale entity") {
    EditorContext ed;
    ed.project.root = "C:/game";
    ed.project.mode = ProjectMode::Engine;
    ed.scenePath = "C:/game/content/main.limescene";

    const EntityId ent = ed.addEntity("Thing", {});
    const NodeId n = ed.graph().addNode("core.raw", 0, 0);

    CHECK_FALSE(ed.showsEntity());

    ed.inspecting = EditorContext::Inspecting::Entity;
    CHECK(ed.showsEntity());

    ed.inspecting = EditorContext::Inspecting::Node;
    ed.inspected() = NodeId{};
    CHECK_FALSE(ed.showsEntity());
    CHECK_FALSE(ed.graph().node(ed.inspected()) != nullptr);

    ed.inspected() = n;
    CHECK_FALSE(ed.showsEntity());

    ed.inspecting = EditorContext::Inspecting::Entity;
    CHECK(ed.showsEntity());
    ed.deleteEntity(ent);
    CHECK_FALSE(ed.showsEntity());
}

TEST_CASE("a framework project never shows the entity half") {
    EditorContext ed;
    ed.project.root = "C:/game";
    ed.project.mode = ProjectMode::Framework;
    const EntityId ent = ed.addEntity("Thing", {});
    ed.selectedEntity = ent;
    ed.inspecting = EditorContext::Inspecting::Entity;
    CHECK_FALSE(ed.showsEntity());

    ed.project.mode = ProjectMode::Engine;
    ed.scenePath.clear();
    CHECK_FALSE(ed.showsEntity());
}

TEST_CASE("a .lua opens as a text document, a .lime as a graph") {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "limevsc_luadoc";
    std::filesystem::create_directories(root / "content", ec);
    const std::string lua = (root / "content/util.lua").string();
    {
        std::ofstream f(lua, std::ios::binary);
        const std::string body = "local M = {}\nfunction M.hi() end\nreturn M\n";
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    EditorContext ed;
    ed.openDoc(lua);
    REQUIRE(ed.doc().isText());
    CHECK(ed.doc().displayName() == "util.lua");
    CHECK(ed.doc().text.find("function M.hi()") != std::string::npos);
    CHECK_FALSE(ed.dirty());
    CHECK(ed.project.root == root.string());

    const std::size_t was = ed.activeDoc;
    ed.addDoc();
    CHECK(ed.openDoc(lua) == was);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("a BOM is stripped from a .lua on the way in") {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "limevsc_luabom";
    std::filesystem::create_directories(root / "content", ec);
    const std::string lua = (root / "content/b.lua").string();
    {
        std::ofstream f(lua, std::ios::binary);
        const std::string body = "\xEF\xBB\xBFreturn 1\n";
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    EditorContext ed;
    ed.openDoc(lua);
    REQUIRE(ed.doc().isText());
    CHECK(ed.doc().text == "return 1\n");

    ed.doc().text = "return 2\n";
    ed.noteTextEdit(false);
    REQUIRE(ed.saveText());
    std::ifstream in(lua, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str() == "return 2\n");

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("text edits join the shared undo history") {
    EditorContext ed;
    GraphDoc& d = ed.doc();
    d.kind = GraphDoc::Kind::Text;
    d.text = "one";
    d.textBefore = d.text;

    for (const char* s : {"onex", "onexy", "onexyz"}) {
        d.text = s;
        ed.noteTextEdit( true);
    }
    CHECK(ed.history.empty());
    ed.endTextBurst();
    CHECK(ed.history.size() == 1);
    CHECK(ed.dirty());

    REQUIRE(ed.undoLast());
    CHECK(ed.doc().text == "one");
    REQUIRE(ed.redoLast());
    CHECK(ed.doc().text == "onexyz");

    d.text = "onexyz!";
    ed.noteTextEdit(true);
    ed.endTextBurst();
    CHECK(ed.history.size() == 2);
    REQUIRE(ed.undoLast());
    CHECK(ed.doc().text == "onexyz");
}

TEST_CASE("undo spans graph and text documents in order") {
    EditorContext ed;
    const NodeId n = ed.graph().addNode("core.raw", 0, 0);
    ed.moveNode(n, 40, 0);

    ed.addDoc();
    GraphDoc& t = ed.doc();
    t.kind = GraphDoc::Kind::Text;
    t.text = "a";
    t.textBefore = t.text;
    t.text = "ab";
    ed.noteTextEdit(true);
    ed.endTextBurst();

    REQUIRE(ed.undoLast());
    CHECK(ed.docs[1]->text == "a");
    REQUIRE(ed.undoLast());
    CHECK(ed.docs[0]->graph.node(n)->x == 0);
    CHECK_FALSE(ed.undoLast());
}

TEST_CASE("generated .lua is told apart from hand-written") {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "limevsc_luagen";
    std::filesystem::create_directories(root / "content", ec);
    std::filesystem::create_directories(root / "content/Scripts/Generated", ec);
    const std::string mine = (root / "content/util.lua").string();
    const std::string built =
        (root / "content/Scripts/Generated/main.lua").string();
    for (const std::string& p : {mine, built, (root / "content/main.lime").string()})
        std::ofstream(p, std::ios::binary) << "x";

    EditorContext ed;
    CHECK(ed.isGeneratedLua(built));
    CHECK_FALSE(ed.isGeneratedLua(mine));
    CHECK_FALSE(ed.isGeneratedLua((root / "content/main.lime").string()));

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("valid Lua produces no complaints") {
    for (const char* src : {
             "local M = {}\nreturn M\n",
             "for i = 1, 10 do print(i) end\n",
             "while true do break end\n",
             "repeat x = 1 until x > 0\n",
             "if a then b() elseif c then d() else e() end\n",
             "local s = \"a string with 'quotes' and \\\"escapes\\\"\"\n",
             "local t = { a = 1, b = { c = 2 } }\n",
             "--[[ a long\ncomment ]]\nreturn 1\n",
             "local s = [[a long\nstring]]\n",
             "local s = [==[ nested ]] still going ]==]\n",
             "-- a comment with an unbalanced ( in it\nreturn 1\n",
             "local s = '-- not a comment'\n",
             "function M.f(a, b) return a + b end\n",
             "do local x = 1 end\n",
             "goto done\n::done::\n",
         }) {
        CAPTURE(src);
        CHECK(checkLua(src).empty());
    }
}

TEST_CASE("the mistakes people actually make are caught, with a line") {
    auto first = [](const char* src) {
        const auto d = checkLua(src);
        REQUIRE_FALSE(d.empty());
        return d.front();
    };

    CHECK(first("local s = \"open\nreturn 1\n").line == 1);
    CHECK(first("local s = \"open\nreturn 1\n").message.find("unterminated")
          != std::string::npos);

    const auto missingEnd = first("function f()\n  return 1\n");
    CHECK(missingEnd.line == 1);
    CHECK(missingEnd.message.find("never closed") != std::string::npos);

    CHECK(first("if a then\n  b()\n").line == 1);
    CHECK(first("repeat\n  x()\n").message.find("until") != std::string::npos);
    CHECK(first("local t = {\n  a = 1,\n").line == 1);
    CHECK(first("return 1)\n").message.find("stray") != std::string::npos);
    CHECK(first("--[[ never closed\nreturn 1\n").message.find("long comment")
          != std::string::npos);
    CHECK(first("local t = (1]\n").message.find("closes") != std::string::npos);
    CHECK(first("end\n").message.find("nothing to close") != std::string::npos);
    CHECK(first("until x\n").message.find("without 'repeat'") != std::string::npos);
}

TEST_CASE("one complaint per line, so a single typo does not cascade") {
    const auto d = checkLua("local t = {\n  a = 1,\n  b = 2,\n");
    CHECK(d.size() <= 2);
}

TEST_CASE("recent projects move to front and stay capped") {
    AppSettings s;
    for (int i = 0; i < 15; ++i) s.noteProject("p" + std::to_string(i));
    CHECK(s.recentProjects.size() == AppSettings::kMaxRecent);
    CHECK(s.recentProjects.front() == "p14");

    s.noteProject("p10");
    CHECK(s.recentProjects.front() == "p10");
    CHECK(std::count(s.recentProjects.begin(), s.recentProjects.end(), "p10") == 1);

    s.forgetProject("p10");
    CHECK(std::count(s.recentProjects.begin(), s.recentProjects.end(), "p10") == 0);

    s.noteProject("");
    CHECK(std::count(s.recentProjects.begin(), s.recentProjects.end(), "") == 0);
}

TEST_CASE("the load queue runs one step per call") {
    LoadQueue q;
    int a = 0, b = 0;
    q.push("First",  [&](std::string& d) { ++a; d = "one"; return 1.0f; });
    q.push("Second", [&](std::string& d) { ++b; d = "two"; return 1.0f; });

    CHECK(q.active());
    CHECK(q.label() == "First");
    CHECK(q.count() == 2);
    CHECK(q.index() == 0);

    CHECK(q.step());
    CHECK(a == 1);
    CHECK(b == 0);
    CHECK(q.label() == "Second");
    CHECK(q.index() == 1);

    CHECK_FALSE(q.step());
    CHECK(b == 1);
    CHECK_FALSE(q.active());
    CHECK(q.count() == 0);
}

TEST_CASE("a step that reports partial progress is called again") {
    LoadQueue q;
    int calls = 0;
    q.push("Chunked", [&](std::string& d) {
        ++calls;
        d = "item " + std::to_string(calls);
        return static_cast<float>(calls) / 4.0f;
    });

    for (int i = 1; i <= 3; ++i) {
        CHECK(q.step());
        CHECK(q.active());
        CHECK(q.fraction() == doctest::Approx(static_cast<float>(i) / 4.0f));
        CHECK(q.detail() == "item " + std::to_string(i));
        CHECK(q.index() == 0);
    }
    CHECK_FALSE(q.step());
    CHECK(calls == 4);
}

TEST_CASE("an empty queue is inert") {
    LoadQueue q;
    CHECK_FALSE(q.active());
    CHECK_FALSE(q.step());
    CHECK(q.label().empty());
    CHECK(q.fraction() == 0.0f);
}

TEST_CASE("pushing after a queue finishes starts a fresh one") {
    LoadQueue q;
    q.push("One", [](std::string&) { return 1.0f; });
    q.step();
    CHECK_FALSE(q.active());

    bool ran = false;
    q.push("Two", [&](std::string&) { ran = true; return 1.0f; });
    CHECK(q.active());
    CHECK(q.index() == 0);
    q.step();
    CHECK(ran);
}

TEST_CASE("the editor list has a command for every named editor") {
    const auto list = AppSettings::knownEditors();
    CHECK(list.size() >= 20);
    for (const auto& e : list) {
        CAPTURE(e.name);
        CHECK(e.name != nullptr);
        CHECK(e.command != nullptr);
        CHECK(std::strlen(e.name) > 0);
        CHECK(std::strlen(e.command) > 0);
    }

    AppSettings s;
    s.externalEditor = "code";
    REQUIRE(s.matchKnownEditor() >= 0);
    CHECK(std::string(list[static_cast<std::size_t>(s.matchKnownEditor())].name)
          == "Visual Studio Code");

    s.externalEditor = "my-editor --wait";
    CHECK(s.matchKnownEditor() == -1);
    s.externalEditor.clear();
    CHECK(s.matchKnownEditor() == -1);
}

TEST_CASE("splitting a wire keeps both ends connected through the waypoint") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId a = g.addNode("core.raw", 0, 0);
    const NodeId b = g.addNode("core.raw", 200, 0);
    const NodeId r = g.addNode("core.reroute", 100, 0);

    const PinId from = PinId::make(a, "ret");
    const PinId to   = PinId::make(b, "value");
    ed.connect(from, to, PinKind::Data);
    REQUIRE(g.sourceOf(to).has_value());

    ed.connect(from, PinId::make(r, "in"), PinKind::Data);
    ed.connect(PinId::make(r, "ret"), to, PinKind::Data);

    REQUIRE(g.sourceOf(PinId::make(r, "in")).has_value());
    CHECK(*g.sourceOf(PinId::make(r, "in")) == from);
    REQUIRE(g.sourceOf(to).has_value());
    CHECK(*g.sourceOf(to) == PinId::make(r, "ret"));
}

TEST_CASE("lifting a wire off an input leaves the source reachable") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId a = g.addNode("core.raw", 0, 0);
    const NodeId b = g.addNode("core.raw", 200, 0);
    const NodeId c = g.addNode("core.raw", 200, 100);

    const PinId from = PinId::make(a, "ret");
    const PinId oldTo = PinId::make(b, "value");
    const PinId newTo = PinId::make(c, "value");
    ed.connect(from, oldTo, PinKind::Data);

    const auto carried = g.sourceOf(oldTo);
    REQUIRE(carried.has_value());
    ed.disconnectInput(oldTo);
    CHECK_FALSE(g.sourceOf(oldTo).has_value());

    ed.connect(*carried, newTo, PinKind::Data);
    REQUIRE(g.sourceOf(newTo).has_value());
    CHECK(*g.sourceOf(newTo) == from);
    CHECK_FALSE(g.sourceOf(oldTo).has_value());

    REQUIRE(ed.undoLast());
    CHECK_FALSE(g.sourceOf(newTo).has_value());
    REQUIRE(ed.undoLast());
    CHECK(g.sourceOf(oldTo).has_value());
}

TEST_CASE("an exec wire can be lifted too, and it has many possible sources") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId a = g.addNode("core.raw", 0, 0);
    const NodeId b = g.addNode("core.raw", 0, 100);
    const NodeId sink = g.addNode("core.raw", 200, 0);

    const PinId to = PinId::make(sink, "in");
    ed.connect(PinId::make(a, "out"), to, PinKind::Exec);
    ed.connect(PinId::make(b, "out"), to, PinKind::Exec);

    const auto srcs = g.execSourcesOf(to);
    CHECK(srcs.size() == 2);
}

TEST_CASE("node, pin and link ids never collide") {
    std::set<std::uint64_t> seen;
    for (std::uint32_t n = 0; n < 64; ++n) {
        CHECK(seen.insert(encNodeId(n)).second);
        for (std::uint32_t p = 0; p < 16; ++p)
            CHECK(seen.insert(encPinId(n, p)).second);
    }
    for (std::uint32_t l = 0; l < 64; ++l)
        CHECK(seen.insert(encLinkId(l)).second);

    CHECK(encNodeId(0) != encPinId(0, 0));
    CHECK(encNodeId(0) != encLinkId(0));
    CHECK(encPinId(0, 0) != encLinkId(0));
}

TEST_CASE("ids round-trip") {
    for (std::uint32_t n : {0u, 1u, 7u, 4095u, 100000u}) {
        CHECK(decNodeId(encNodeId(n)) == n);
        for (std::uint32_t p : {0u, 1u, 999u, 65535u}) {
            const std::uint64_t id = encPinId(n, p);
            CHECK(decPinNode(id) == n);
            CHECK(decPinName(id) == p);
        }
    }
    for (std::uint32_t l : {0u, 1u, 63u, 100000u})
        CHECK(decLinkIndex(encLinkId(l)) == l);
}

TEST_CASE("an id says which kind it is") {
    CHECK(isNodeId(encNodeId(0)));
    CHECK_FALSE(isPinId(encNodeId(0)));
    CHECK_FALSE(isLinkId(encNodeId(0)));

    CHECK(isPinId(encPinId(3, 4)));
    CHECK_FALSE(isNodeId(encPinId(3, 4)));
    CHECK_FALSE(isLinkId(encPinId(3, 4)));

    CHECK(isLinkId(encLinkId(0)));
    CHECK_FALSE(isNodeId(encLinkId(0)));
    CHECK_FALSE(isPinId(encLinkId(0)));

    CHECK_FALSE(isNodeId(0));
}

TEST_CASE("detaching an output drops every wire on it, as one undo step") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId src = g.addNode("core.raw", 0, 0);
    const NodeId a = g.addNode("core.raw", 200, 0);
    const NodeId b = g.addNode("core.raw", 200, 100);
    const NodeId c = g.addNode("core.raw", 200, 200);

    const PinId out = PinId::make(src, "ret");
    ed.connect(out, PinId::make(a, "value"), PinKind::Data);
    ed.connect(out, PinId::make(b, "value"), PinKind::Data);
    ed.connect(out, PinId::make(c, "value"), PinKind::Data);
    CHECK(g.targetsOf(out).size() == 3);

    ed.disconnectOutput(out);
    CHECK(g.targetsOf(out).empty());

    REQUIRE(ed.undoLast());
    CHECK(g.targetsOf(out).size() == 3);
    REQUIRE(ed.redoLast());
    CHECK(g.targetsOf(out).empty());
}

TEST_CASE("detaching an exec output drops its single target") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId a = g.addNode("core.raw", 0, 0);
    const NodeId b = g.addNode("core.raw", 200, 0);
    const PinId out = PinId::make(a, "out");
    ed.connect(out, PinId::make(b, "in"), PinKind::Exec);
    REQUIRE(g.execTargetOf(out).has_value());

    ed.disconnectOutput(out);
    CHECK_FALSE(g.execTargetOf(out).has_value());
    REQUIRE(ed.undoLast());
    CHECK(g.execTargetOf(out).has_value());
}

TEST_CASE("detaching a pin with nothing on it records no undo step") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId a = g.addNode("core.raw", 0, 0);
    const std::size_t before = ed.history.size();
    ed.disconnectOutput(PinId::make(a, "ret"));
    CHECK(ed.history.size() == before);
}

TEST_CASE("carrying an exec wire keeps the source and moves the destination") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId src = g.addNode("Lime.onStart", 0, 0);
    const NodeId oldDst = g.addNode("core.raw", 300, 0);
    const NodeId newDst = g.addNode("core.raw", 300, 150);

    const PinId out = PinId::make(src, "out");
    const PinId oldIn = PinId::make(oldDst, "in");
    const PinId newIn = PinId::make(newDst, "in");
    ed.connect(out, oldIn, PinKind::Exec);
    REQUIRE(g.execTargetOf(out).has_value());

    const auto sources = g.execSourcesOf(oldIn);
    REQUIRE(sources.size() == 1);
    const PinId carried = sources.front();
    CHECK(carried == out);
    ed.disconnectInput(oldIn);
    CHECK_FALSE(g.execTargetOf(out).has_value());

    ed.connect(carried, newIn, PinKind::Exec);
    REQUIRE(g.execTargetOf(out).has_value());
    CHECK(*g.execTargetOf(out) == newIn);
    CHECK(g.execSourcesOf(oldIn).empty());
    REQUIRE(g.execSourcesOf(newIn).size() == 1);
    CHECK(g.execSourcesOf(newIn).front() == out);
}

TEST_CASE("carrying a data wire does the same") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId src = g.addNode("core.raw", 0, 0);
    const NodeId oldDst = g.addNode("core.raw", 300, 0);
    const NodeId newDst = g.addNode("core.raw", 300, 150);

    const PinId out = PinId::make(src, "ret");
    const PinId oldIn = PinId::make(oldDst, "value");
    const PinId newIn = PinId::make(newDst, "value");
    ed.connect(out, oldIn, PinKind::Data);

    const auto carried = g.sourceOf(oldIn);
    REQUIRE(carried.has_value());
    CHECK(*carried == out);
    ed.disconnectInput(oldIn);
    ed.connect(*carried, newIn, PinKind::Data);

    CHECK_FALSE(g.sourceOf(oldIn).has_value());
    REQUIRE(g.sourceOf(newIn).has_value());
    CHECK(*g.sourceOf(newIn) == out);
}

TEST_CASE("every link index agrees, however the link got there") {
    Graph g;
    const NodeId a = g.addNode("core.raw", 0, 0);
    const NodeId b = g.addNode("core.raw", 100, 0);
    const NodeId c = g.addNode("core.raw", 200, 0);

    const PinId aOut = PinId::make(a, "out");
    const PinId bIn = PinId::make(b, "in");
    const PinId bOut = PinId::make(b, "out");
    const PinId cIn = PinId::make(c, "in");

    (void)g.execSourcesOf(bIn);
    g.connect(aOut, bIn, PinKind::Exec);
    CHECK(g.execTargetOf(aOut).has_value());
    REQUIRE(g.execSourcesOf(bIn).size() == 1);
    CHECK(g.execSourcesOf(bIn).front() == aOut);

    (void)g.execSourcesOf(cIn);
    g.connect(bOut, cIn, PinKind::Exec);
    REQUIRE(g.execSourcesOf(cIn).size() == 1);

    g.disconnectFrom(aOut);
    CHECK(g.execSourcesOf(bIn).empty());
    CHECK(g.execSourcesOf(cIn).size() == 1);

    const PinId aRet = PinId::make(a, "ret");
    const PinId bVal = PinId::make(b, "value");
    (void)g.sourceOf(bVal);
    g.connect(aRet, bVal, PinKind::Data);
    REQUIRE(g.sourceOf(bVal).has_value());
    CHECK(*g.sourceOf(bVal) == aRet);
    CHECK(g.targetsOf(aRet).size() == 1);
}

TEST_CASE("carrying by the output end retains the input side") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId oldSrc = g.addNode("Lime.onStart", 0, 0);
    const NodeId newSrc = g.addNode("core.raw", 0, 150);
    const NodeId dst = g.addNode("core.raw", 300, 0);

    const PinId oldOut = PinId::make(oldSrc, "out");
    const PinId newOut = PinId::make(newSrc, "out");
    const PinId in = PinId::make(dst, "in");
    ed.connect(oldOut, in, PinKind::Exec);

    std::vector<PinId> sinks;
    for (const Link& l : g.links())
        if (l.from == oldOut) sinks.push_back(l.to);
    REQUIRE(sinks.size() == 1);
    const PinId anchor = sinks.front();
    CHECK(anchor == in);
    ed.disconnectOutput(oldOut);
    CHECK_FALSE(g.execTargetOf(oldOut).has_value());

    ed.connect(newOut, anchor, PinKind::Exec);
    REQUIRE(g.execTargetOf(newOut).has_value());
    CHECK(*g.execTargetOf(newOut) == in);
    CHECK_FALSE(g.execTargetOf(oldOut).has_value());
    REQUIRE(g.execSourcesOf(in).size() == 1);
    CHECK(g.execSourcesOf(in).front() == newOut);
}

TEST_CASE("carrying works the same for a data wire held by its output") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId oldSrc = g.addNode("core.raw", 0, 0);
    const NodeId newSrc = g.addNode("core.raw", 0, 150);
    const NodeId dst = g.addNode("core.raw", 300, 0);

    const PinId oldOut = PinId::make(oldSrc, "ret");
    const PinId newOut = PinId::make(newSrc, "ret");
    const PinId in = PinId::make(dst, "value");
    ed.connect(oldOut, in, PinKind::Data);

    ed.disconnectOutput(oldOut);
    ed.connect(newOut, in, PinKind::Data);

    REQUIRE(g.sourceOf(in).has_value());
    CHECK(*g.sourceOf(in) == newOut);
    CHECK(g.targetsOf(oldOut).empty());
}

TEST_CASE("a data output with several wires has no single wire to hold") {
    EditorContext ed;
    Graph& g = ed.graph();
    const NodeId src = g.addNode("core.raw", 0, 0);
    const NodeId a = g.addNode("core.raw", 200, 0);
    const NodeId b = g.addNode("core.raw", 200, 100);

    const PinId out = PinId::make(src, "ret");
    ed.connect(out, PinId::make(a, "value"), PinKind::Data);
    ed.connect(out, PinId::make(b, "value"), PinKind::Data);

    std::vector<PinId> sinks;
    for (const Link& l : g.links())
        if (l.from == out) sinks.push_back(l.to);
    CHECK(sinks.size() == 2);

    ed.disconnectOutput(out);
    CHECK(g.targetsOf(out).empty());
    REQUIRE(ed.undoLast());
    CHECK(g.targetsOf(out).size() == 2);
}

namespace {

void makeEdits(EditorContext& ed, int n) {
    for (int i = 0; i < n; ++i) {
        const float x = static_cast<float>(i);
        ed.apply(EditAction{"edit",
                            [x](Graph& g) { g.addNode("core.raw", x, 0); },
                            [](Graph& g) {
                                const std::span<const Node> ns = g.nodes();
                                if (!ns.empty()) g.removeNode(ns.back().id);
                            },
                            0});
    }
}

}

TEST_CASE("undo history stops at the configured limit") {
    EditorContext ed;
    ed.settings.undoLimit = 8;
    makeEdits(ed, 30);

    CHECK(ed.history.size() == 8);
    CHECK(ed.historyCursor == 8);
    CHECK(ed.undo().size() == 8);
}

TEST_CASE("undo walks back exactly as far as the limit allows") {
    EditorContext ed;
    ed.settings.undoLimit = 5;
    makeEdits(ed, 20);

    int undone = 0;
    while (ed.undoLast()) ++undone;
    CHECK(undone == 5);
    CHECK_FALSE(ed.canUndoAny());

    CHECK(ed.graph().nodes().size() == 15);
}

TEST_CASE("a limit of zero keeps everything") {
    EditorContext ed;
    ed.settings.undoLimit = 0;
    makeEdits(ed, 200);
    CHECK(ed.history.size() == 200);
}

TEST_CASE("the default limit is a hundred and twenty eight") {
    EditorContext ed;
    CHECK(ed.settings.undoLimit == 128);
    makeEdits(ed, 200);
    CHECK(ed.history.size() == 128);
}

TEST_CASE("lowering the limit takes effect without waiting for an edit") {
    EditorContext ed;
    makeEdits(ed, 60);
    REQUIRE(ed.history.size() == 60);

    ed.settings.undoLimit = 10;
    ed.trimHistory();
    CHECK(ed.history.size() == 10);
    CHECK(ed.undo().size() == 10);

    int undone = 0;
    while (ed.undoLast()) ++undone;
    CHECK(undone == 10);
}

TEST_CASE("the cap is shared across documents, not applied per tab") {
    EditorContext ed;
    ed.settings.undoLimit = 6;

    makeEdits(ed, 4);
    ed.addDoc();
    makeEdits(ed, 4);

    CHECK(ed.history.size() == 6);
    CHECK(ed.docs[0]->undo.size() == 2);
    CHECK(ed.docs[1]->undo.size() == 4);
}

TEST_CASE("a drag is one undo entry however many frames it takes") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("core.raw", 0, 0);

    for (int f = 1; f <= 60; ++f) ed.placeNode(a, float(f), float(f * 2));
    CHECK(ed.history.empty());

    const EditorContext::NodeMove m{a, 0, 0, 60, 120};
    ed.moveNodes({&m, 1});
    CHECK(ed.history.size() == 1);

    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(a)->x == doctest::Approx(0.0f));
    CHECK(ed.graph().node(a)->y == doctest::Approx(0.0f));
}

TEST_CASE("dragging a selection is still one undo entry") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("core.raw", 0, 0);
    const NodeId b = ed.graph().addNode("core.raw", 10, 0);
    const NodeId c = ed.graph().addNode("core.raw", 20, 0);

    for (int f = 1; f <= 30; ++f) {
        ed.placeNode(a, float(f), 0);
        ed.placeNode(b, 10.0f + float(f), 0);
        ed.placeNode(c, 20.0f + float(f), 0);
    }
    const EditorContext::NodeMove moves[] = {
        {a, 0, 0, 30, 0}, {b, 10, 0, 40, 0}, {c, 20, 0, 50, 0}};
    ed.moveNodes(moves);

    CHECK(ed.history.size() == 1);

    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(a)->x == doctest::Approx(0.0f));
    CHECK(ed.graph().node(b)->x == doctest::Approx(10.0f));
    CHECK(ed.graph().node(c)->x == doctest::Approx(20.0f));
    CHECK_FALSE(ed.canUndoAny());
}

TEST_CASE("a drag that ends where it began records nothing") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("core.raw", 5, 5);
    ed.placeNode(a, 40, 40);
    ed.placeNode(a, 5, 5);

    const EditorContext::NodeMove m{a, 5, 5, 5, 5};
    ed.moveNodes({&m, 1});
    CHECK(ed.history.empty());
}

TEST_CASE("undo of a drag restores where it started, not where it paused") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("core.raw", 100, 100);
    for (int f = 0; f < 10; ++f) ed.placeNode(a, 100.0f + float(f) * 5, 100);

    const EditorContext::NodeMove m{a, 100, 100, 145, 100};
    ed.moveNodes({&m, 1});
    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(a)->x == doctest::Approx(100.0f));
}

TEST_CASE("a comment resize is one undo entry however long it is held") {
    EditorContext ed;
    const NodeId c = ed.graph().addNode("core.comment", 0, 0);
    ed.graph().node(c)->w = 200;
    ed.graph().node(c)->h = 120;

    for (int f = 1; f <= 40; ++f)
        ed.sizeNode(c, 200.0f + float(f) * 4, 120.0f + float(f));
    CHECK(ed.history.empty());

    const EditorContext::NodeResize r{c, 200, 120, 360, 160};
    ed.resizeNodes({&r, 1});
    CHECK(ed.history.size() == 1);

    REQUIRE(ed.undoLast());
    CHECK(ed.graph().node(c)->w == doctest::Approx(200.0f));
    CHECK(ed.graph().node(c)->h == doctest::Approx(120.0f));
}

TEST_CASE("a resize that ends where it began records nothing") {
    EditorContext ed;
    const NodeId c = ed.graph().addNode("core.comment", 0, 0);
    ed.graph().node(c)->w = 300;
    ed.graph().node(c)->h = 200;

    const EditorContext::NodeResize r{c, 300, 200, 300, 200};
    ed.resizeNodes({&r, 1});
    CHECK(ed.history.empty());
}

TEST_CASE("undo and redo say what they did") {
    EditorContext ed;
    const NodeId n = ed.graph().addNode("core.raw", 0, 0);
    CHECK(ed.notes.empty());

    ed.moveNode(n, 90, 0);
    CHECK(ed.notes.empty());

    REQUIRE(ed.undoLast());
    REQUIRE(ed.notes.size() == 1);
    CHECK(ed.notes.back().text == "Undo: Move node");
    CHECK(ed.notes.back().kind == EditorContext::NoteKind::Action);
    const std::uint64_t first = ed.notes.back().serial;
    CHECK(first > 0);

    REQUIRE(ed.redoLast());
    CHECK(ed.notes.back().text == "Redo: Move node");
    CHECK(ed.notes.size() == 2);
    CHECK(ed.notes.back().serial > first);
}

TEST_CASE("the note names the action, not the document") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("core.raw", 0, 0);
    const NodeId b = ed.graph().addNode("core.raw", 0, 0);
    const EditorContext::NodeMove moves[] = {{a, 0, 0, 10, 0}, {b, 0, 0, 20, 0}};
    ed.moveNodes(moves);

    REQUIRE(ed.undoLast());
    CHECK(ed.notes.back().text == "Undo: Move nodes");
}

TEST_CASE("nothing to undo says nothing") {
    EditorContext ed;
    CHECK_FALSE(ed.undoLast());
    CHECK(ed.notes.empty());
}

TEST_CASE("notes carry a severity, so they are not only for undo") {
    EditorContext ed;
    Diagnostics d;
    d.add({Severity::Error, "cannot open project.limeproj"});
    d.add({Severity::Warning, "asset re-linked by name"});
    d.add({Severity::Info, "scanned 12 assets"});
    ed.report(d);

    CHECK(ed.console.size() == 3);
    REQUIRE(ed.notes.size() == 2);
    CHECK(ed.notes[0].kind == EditorContext::NoteKind::Error);
    CHECK(ed.notes[0].text == "cannot open project.limeproj");
    CHECK(ed.notes[1].kind == EditorContext::NoteKind::Warning);
}

TEST_CASE("the note list is capped") {
    EditorContext ed;
    for (std::size_t i = 0; i < EditorContext::kMaxNotes * 3; ++i)
        ed.note(EditorContext::NoteKind::Action, "note " + std::to_string(i));

    CHECK(ed.notes.size() == EditorContext::kMaxNotes);
    CHECK(ed.notes.back().text
          == "note " + std::to_string(EditorContext::kMaxNotes * 3 - 1));
    CHECK(ed.notes.back().serial > ed.notes.front().serial);
}

TEST_CASE("a gizmo drag is one undo entry however long it is held") {
    EditorContext ed;
    const EntityId ent = ed.addEntity("Thing", {});
    {
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(0, 0, 0)");
        ed.scene.entity(ent)->components.push_back(std::move(t));
    }
    const std::size_t before = ed.history.size();

    for (int f = 1; f <= 60; ++f)
        ed.putProp(ent, "Transform", "position",
                   "Vec3.new(" + std::to_string(f) + ", 0, 0)");
    CHECK(ed.history.size() == before);

    ed.recordProp(ent, "Transform", "position", "Vec3.new(0, 0, 0)", true);
    CHECK(ed.history.size() == before + 1);

    REQUIRE(ed.undoLast());
    CHECK(*ed.scene.entity(ent)->component("Transform")->value("position")
          == "Vec3.new(0, 0, 0)");
}

TEST_CASE("a gizmo drag that ends where it began records nothing") {
    EditorContext ed;
    const EntityId ent = ed.addEntity("Thing", {});
    {
        Component t;
        t.type = "Transform";
        t.setValue("position", "Vec3.new(1, 2, 3)");
        ed.scene.entity(ent)->components.push_back(std::move(t));
    }
    const std::size_t before = ed.history.size();

    ed.putProp(ent, "Transform", "position", "Vec3.new(9, 9, 9)");
    ed.putProp(ent, "Transform", "position", "Vec3.new(1, 2, 3)");
    ed.recordProp(ent, "Transform", "position", "Vec3.new(1, 2, 3)", true);
    CHECK(ed.history.size() == before);
}

TEST_CASE("undoing a gizmo drag restores an unset property to unset") {
    EditorContext ed;
    const EntityId ent = ed.addEntity("Thing", {});
    {
        Component t;
        t.type = "Transform";
        ed.scene.entity(ent)->components.push_back(std::move(t));
    }
    ed.putProp(ent, "Transform", "scale", "Vec3.new(2, 2, 2)");
    ed.recordProp(ent, "Transform", "scale", "",  false);

    REQUIRE(ed.undoLast());
    CHECK(ed.scene.entity(ent)->component("Transform")->value("scale")
          == nullptr);
}

TEST_CASE("copying nodes brings the links between them") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("Lime.onStart", 0, 0);
    const NodeId b = ed.graph().addNode("core.raw", 200, 0);
    ed.graph().connect(PinId::make(a, "out"), PinId::make(b, "in"),
                       PinKind::Exec);

    ed.selection() = {a, b};
    ed.copySelection();
    CHECK(ed.clipNodes.size() == 2);
    CHECK(ed.clipLinks.size() == 1);

    const std::size_t before = ed.graph().nodes().size();
    ed.pasteClipboard();
    CHECK(ed.graph().nodes().size() == before + 2);
    CHECK(ed.selection().size() == 2);

    const NodeId na = ed.selection()[0];
    const NodeId nb = ed.selection()[1];
    CHECK(na != a);
    CHECK(!ed.graph().execSourcesOf(PinId::make(nb, "in")).empty());
}

TEST_CASE("a link with only one end copied is not carried over") {
    EditorContext ed;
    const NodeId a = ed.graph().addNode("Lime.onStart", 0, 0);
    const NodeId b = ed.graph().addNode("core.raw", 200, 0);
    ed.graph().connect(PinId::make(a, "out"), PinId::make(b, "in"),
                       PinKind::Exec);

    ed.selection() = {b};
    ed.copySelection();
    CHECK(ed.clipNodes.size() == 1);
    CHECK(ed.clipLinks.empty());
}

TEST_CASE("pasting keeps the clipboard for another paste") {
    EditorContext ed;
    ed.selection() = {ed.graph().addNode("core.raw", 0, 0)};
    ed.copySelection();

    ed.pasteClipboard();
    ed.pasteClipboard();
    CHECK(ed.graph().nodes().size() == 3);
}

TEST_CASE("closing a project leaves nothing of it behind") {
    EditorContext ed;
    ed.project.root = "C:/game";
    ed.project.mode = ProjectMode::Engine;
    ed.scenePath = "C:/game/content/Scenes/main.limescene";
    ed.addEntity("Thing", {});
    ed.sceneDirty = true;

    ed.docs[0]->filePath = "C:/game/content/Graphs/main.lime";
    ed.addDoc();
    ed.docs[1]->filePath = "C:/game/content/Graphs/other.lime";
    ed.selection() = {ed.graph().addNode("core.raw", 0, 0)};
    ed.copySelection();

    ed.closeProject();

    CHECK(ed.docs.size() == 1);
    CHECK(ed.docs[0]->filePath.empty());
    CHECK(ed.graph().nodes().empty());
    CHECK(ed.activeDoc == 0);
    CHECK(ed.history.empty());
    CHECK(ed.historyCursor == 0);
    CHECK(ed.scenePath.empty());
    CHECK(ed.scene.size() == 0);
    CHECK_FALSE(ed.sceneDirty);
    CHECK(ed.project.root.empty());
    CHECK(ed.project.mode == ProjectMode::Framework);
    CHECK(ed.clipNodes.empty());
    CHECK_FALSE(ed.canUndoAny());
}

TEST_CASE("the editor opens text files that are not Lua") {
    CHECK(EditorContext::isTextPath("notes.txt"));
    CHECK(EditorContext::isTextPath("data.json"));
    CHECK(EditorContext::isTextPath("shader.hlsl"));
    CHECK(EditorContext::isTextPath("README"));
    CHECK(EditorContext::isTextPath("C:/game/content/thing.md"));

    CHECK_FALSE(EditorContext::isTextPath("app.exe"));
    CHECK_FALSE(EditorContext::isTextPath("LimeEngine.dll"));
    CHECK_FALSE(EditorContext::isTextPath("icon.ico"));
    CHECK_FALSE(EditorContext::isTextPath("sound.WAV"));
}

TEST_CASE("only Lua files are syntax checked") {
    CHECK(EditorContext::isLuaPath("main.lua"));
    CHECK_FALSE(EditorContext::isLuaPath("data.json"));
    CHECK_FALSE(EditorContext::isLuaPath("notes.txt"));
}

TEST_CASE("a non-Lua text file reports no Lua errors") {
    EditorContext ed;
    ed.docs[0]->kind = GraphDoc::Kind::Text;
    ed.docs[0]->filePath = "C:/game/content/data.json";
    ed.docs[0]->text = "{ \"this\": is not lua at all ]]";
    ed.recheckLua();
    CHECK(ed.luaErrors.empty());

    ed.docs[0]->filePath = "C:/game/content/broken.lua";
    ed.recheckLua();
    CHECK_FALSE(ed.luaErrors.empty());
}
