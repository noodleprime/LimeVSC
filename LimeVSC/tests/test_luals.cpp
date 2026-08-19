#include "api/data_provider.h"
#include "api/luals_provider.h"
#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

#include <filesystem>
#include <set>

using namespace lime;

namespace {

std::string limexApiDir() {
    const std::filesystem::path vsc =
        std::filesystem::path(LIMEVSC_TEST_DIR).parent_path();
    const std::filesystem::path candidates[] = {
        vsc.parent_path() / "LimeX" / "LimeEngine" / "api",
        vsc.parent_path() / "LimeEngine" / "api",
    };
    for (const std::filesystem::path& api : candidates)
        if (std::filesystem::exists(api / "Lime.lua")) return api.string();
    return {};
}

}

TEST_CASE("event class names map to their Lua path") {
    CHECK(eventClassToPath("Lime_Input_onKeyPressed") == "Lime.Input.onKeyPressed");
    CHECK(eventClassToPath("EditBox_onPressed") == "EditBox.onPressed");
    CHECK(eventClassToPath("Lime_onUpdate") == "Lime.onUpdate");
}

TEST_CASE("identifiers humanize into spoken labels") {
    CHECK(humanizeIdentifier("setPosition") == "Set Position");
    CHECK(humanizeIdentifier("getForward") == "Get Forward");
    CHECK(humanizeIdentifier("isDirectory") == "Is Directory");
    CHECK(humanizeIdentifier("onKeyPressed") == "On Key Pressed");
    CHECK(humanizeIdentifier("x") == "X");

    CHECK(humanizeIdentifier("loadXML") == "Load XML");
    CHECK(humanizeIdentifier("getHEX") == "Get HEX");
    CHECK(humanizeIdentifier("materialID") == "Material ID");
    CHECK(humanizeIdentifier("banIP") == "Ban IP");
    CHECK(humanizeIdentifier("setIgnoreEqualID") == "Set Ignore Equal ID");
    CHECK(humanizeIdentifier("getPSPath") == "Get PS Path");
    CHECK(humanizeIdentifier("clearBannedIPs") == "Clear Banned IPs");
    CHECK(humanizeIdentifier("getVSync") == "Get VSync");
    CHECK(humanizeIdentifier("setVSync") == "Set VSync");

    CHECK(humanizeIdentifier("createText2D") == "Create Text2D");
    CHECK(humanizeIdentifier("Vec3") == "Vec3");

    CHECK(humanizeIdentifier("").empty());
}

TEST_CASE("doc comments are reduced to plain prose") {
    CHECK(cleanDoc("**This cannot run** until `window` creation.")
          == "This cannot run until window creation.");
    CHECK(cleanDoc("  spaced   out\n  text  ") == "spaced out text");
    CHECK(cleanDoc("").empty());
}

TEST_CASE("fun() signatures parse into typed arguments") {
    auto a = parseFunSignature("fun(key: Lime.Enum.Key)");
    REQUIRE(a.size() == 1);
    CHECK(a[0].first == "key");
    CHECK(a[0].second == "Lime.Enum.Key");

    auto b = parseFunSignature("fun(id: number, button: Lime.Enum.Controller)");
    REQUIRE(b.size() == 2);
    CHECK(b[1].first == "button");

    CHECK(parseFunSignature("fun()").empty());

    auto c = parseFunSignature("fun(cb: fun(x: number, y: number), n: number)");
    REQUIRE(c.size() == 2);
    CHECK(c[0].first == "cb");
    CHECK(c[1].first == "n");
    CHECK(c[1].second == "number");
}

TEST_CASE("provider parses a synthetic stub end to end") {
    const std::string path =
        (std::filesystem::path(LIMEVSC_TEST_DIR) / "data" / "mini.lua").string();

    TypeRegistry types;
    Diagnostics diag;
    LuaLSProvider p(path, "", kPriorityGenerated);
    std::vector<NodeDesc> out;
    p.collect(types, out, diag);
    REQUIRE_FALSE(diag.hasErrors());

    auto find = [&](std::string_view id) -> const NodeDesc* {
        for (const NodeDesc& d : out) if (d.id == id) return &d;
        return nullptr;
    };

    SUBCASE("a returning getter becomes a pure data node") {
        const NodeDesc* d = find("Demo.getCount");
        REQUIRE(d);
        CHECK(d->emit == "call");
        CHECK(d->target == "Demo.getCount");
        CHECK(d->pure);
        CHECK_FALSE(d->hasExecPins());
        REQUIRE(d->findPin("ret"));
        CHECK(types.get(d->findPin("ret")->type).name == "number");
    }

    SUBCASE("a void setter becomes an exec node") {
        const NodeDesc* d = find("Demo.setName");
        REQUIRE(d);
        CHECK_FALSE(d->pure);
        CHECK(d->hasExecPins());
        REQUIRE(d->findPin("in"));
        REQUIRE(d->findPin("out"));
        REQUIRE(d->findPin("name"));
    }

    SUBCASE("an optional parameter is marked optional") {
        const NodeDesc* d = find("Demo.setName");
        REQUIRE(d);
        REQUIRE(d->findPin("prefix"));
        CHECK(d->findPin("prefix")->optional);
        CHECK_FALSE(d->findPin("name")->optional);
    }

    SUBCASE("a colon method gains a self pin") {
        const NodeDesc* d = find("Widget:resize");
        REQUIRE(d);
        CHECK(d->emit == "method");
        CHECK(d->target == "resize");
        REQUIRE(d->findPin("self"));
        CHECK(types.get(d->findPin("self")->type).name == "Widget");
    }

    SUBCASE("constructor pins come from @overload when the signature is empty") {
        const NodeDesc* d = find("Widget.new");
        REQUIRE(d);
        CHECK(d->emit == "construct");
        REQUIRE(d->findPin("w"));
        REQUIRE(d->findPin("h"));
    }

    SUBCASE("fields become pure index nodes") {
        const NodeDesc* d = find("Widget.width");
        REQUIRE(d);
        CHECK(d->emit == "index");
        CHECK(d->pure);
        REQUIRE(d->findPin("self"));
    }

    SUBCASE("@operator becomes real arithmetic nodes") {
        const NodeDesc* d = find("Widget.op.add.Widget");
        REQUIRE(d);
        CHECK(d->emit == "binop");
        CHECK(d->target == "+");
        CHECK(d->pure);
        REQUIRE(d->findPin("b"));
        CHECK(types.get(d->findPin("b")->type).name == "Widget");
    }

    SUBCASE("a module-level event is global and exposes its callback args") {
        const NodeDesc* d = find("Demo.onFired");
        REQUIRE(d);
        CHECK(d->isEvent);
        CHECK(d->emit == "struct:event");
        CHECK(d->target == "Demo.onFired");
        CHECK_FALSE(d->findPin("self"));
        REQUIRE(d->findPin("out"));
        CHECK(d->findPin("out")->kind == PinKind::Exec);
        REQUIRE(d->findPin("amount"));
        CHECK(d->findPin("amount")->dir == PinDir::Out);
        CHECK(types.get(d->findPin("amount")->type).name == "number");
    }

    SUBCASE("an event on an instantiable type takes its receiver") {
        const NodeDesc* d = find("Widget.onResized");
        REQUIRE(d);
        CHECK(d->isEvent);
        CHECK(d->target == "onResized");
        REQUIRE(d->findPin("self"));
        CHECK(types.get(d->findPin("self")->type).name == "Widget");
    }

    SUBCASE("a field typed as an event class does not shadow the event") {
        const NodeDesc* d = find("Demo.onFired");
        REQUIRE(d);
        CHECK(d->emit == "struct:event");
    }
}

TEST_CASE("provider conforms against the real engine API"
          * doctest::skip(false)) {
    const std::string dir = limexApiDir();
    if (dir.empty()) {
        MESSAGE("no LimeX checkout beside LimeVSC; skipping");
        return;
    }

    TypeRegistry types;
    Diagnostics diag;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);

    LuaLSProvider p(dir + "/Lime.lua", dir + "/Enums.lua", kPriorityGenerated);
    std::vector<NodeDesc> out;
    p.collect(types, out, diag);

    CHECK_FALSE(diag.hasErrors());
    CHECK(out.size() > 450);

    bool sawSceneObject = false;
    for (const NodeDesc& d : out)
        if (d.id.rfind("SceneObject", 0) == 0) sawSceneObject = true;
    CHECK(sawSceneObject);

    std::set<std::string> ids;
    int events = 0;
    for (const NodeDesc& d : out) {
        INFO("node: " << d.id);
        CHECK_FALSE(d.id.empty());
        CHECK_FALSE(d.emit.empty());
        CHECK(ids.insert(d.id).second);

        std::set<std::string> pins;
        for (const PinDesc& pin : d.pins) {
            CHECK_FALSE(pin.name.empty());
            CHECK(pins.insert(pin.name).second);
            if (pin.kind == PinKind::Data) CHECK(pin.type.valid());
            else                           CHECK_FALSE(pin.type.valid());
        }
        if (d.pure) CHECK_FALSE(d.hasExecPins());
        if (d.isEvent) ++events;
    }

    CHECK(events > 20);

    auto find = [&](std::string_view id) -> const NodeDesc* {
        for (const NodeDesc& d : out) if (d.id == id) return &d;
        return nullptr;
    };

    const NodeDesc* key = find("Lime.Input.onKeyPressed");
    REQUIRE(key);
    CHECK(key->isEvent);
    REQUIRE(key->findPin("key"));

    const NodeDesc* isFile = find("Lime.File.isFile");
    REQUIRE(isFile);
    CHECK(isFile->pure);
    REQUIRE(isFile->findPin("path"));

    const NodeDesc* v3 = find("Vec3.new");
    REQUIRE(v3);
    REQUIRE(v3->findPin("x"));
    REQUIRE(v3->findPin("z"));

    CHECK(isFile->display == "Is File");
    CHECK(key->display == "On Key Pressed");
    CHECK(v3->display == "Make Vec3");
    for (const NodeDesc& d : out) {
        INFO("node: " << d.id);
        CHECK_FALSE(d.display.empty());
        CHECK_FALSE(d.doc.empty());
        CHECK(d.doc.find("**") == std::string::npos);
        CHECK(d.doc.find('`') == std::string::npos);
    }
}

TEST_CASE("generated descriptions are one sentence") {
    const std::string dir = LIMEVSC_DATA_DIR;
    const std::string api = limexApiDir();
    if (api.empty()) return;

    TypeRegistry types;
    NodeRegistry nodes;
    Diagnostics d;
    nodes.addProvider(std::make_unique<LuaLSProvider>(
        api + "/Lime.lua", api + "/Enums.lua", kPriorityGenerated));
    nodes.rebuild(types, d);
    REQUIRE(nodes.all().size() > 100);

    int withDoc = 0;
    for (const NodeDesc& n : nodes.all()) {
        if (n.doc.empty()) continue;
        ++withDoc;
        for (std::size_t i = 0; i + 2 < n.doc.size(); ++i) {
            if (n.doc[i] == '.' && n.doc[i + 1] == ' '
                && std::isupper(static_cast<unsigned char>(n.doc[i + 2])) != 0) {
                CAPTURE(n.id);
                CAPTURE(n.doc);
                FAIL_CHECK("description runs to a second sentence");
                break;
            }
        }
    }
    CHECK(withDoc > 20);
}
