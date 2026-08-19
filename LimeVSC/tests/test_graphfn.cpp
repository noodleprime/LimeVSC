#include "api/data_provider.h"
#include "api/graphfn_provider.h"
#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lime;

namespace {

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string compileFile(const fs::path& limePath, const std::string& projectRoot,
                        Diagnostics& diag) {
    TypeRegistry types;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);

    NodeRegistry nodes;
    nodes.addProvider(std::make_unique<DataFileProvider>(
        std::string(LIMEVSC_DATA_DIR) + "/core.limenodes", "core", kPriorityCore));
    nodes.addProvider(std::make_unique<DataFileProvider>(
        std::string(LIMEVSC_TEST_DIR) + "/data/golden.limenodes", "golden",
        kPriorityOverrides));
    nodes.addProvider(
        std::make_unique<GraphFnProvider>(projectRoot, kPriorityOverrides));
    nodes.rebuild(types, diag);

    Graph g;
    if (!readLime(limePath.string(), g, diag)) return {};

    const EmitterRegistry emitters = EmitterRegistry::withBuiltins();
    const CompileResult r = compileGraph(g, nodes, types, emitters,
                                         limePath.filename().string(), diag);
    return r.ok ? r.lua : std::string{};
}

fs::path makeProject() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "limevsc_graphfn_test";
    fs::remove_all(root, ec);

    write(root / "content" / "util.lime",
          "!lime 1\n"
          "!module content.util\n"
          "!fn double n:number -> number\n"
          "\n"
          "~0 fn.content.util.double.entry @ 0 0\n"
          "  > out 2.in\n"
          "\n"
          "~1 core.mul @ 0 200\n"
          "  < a 0.n\n"
          "  = b 2\n"
          "\n"
          "~2 core.return @ 200 0\n"
          "  < value 1.ret\n");

    write(root / "content" / "main.lime",
          "!lime 1\n"
          "!module content.main\n"
          "\n"
          "~0 Lime.onStart @ 0 0\n"
          "  > out 1.in\n"
          "\n"
          "~1 fn.content.util.double @ 200 0\n"
          "  = n 21\n"
          "  > out 2.in\n"
          "\n"
          "~2 test.log @ 400 0\n"
          "  < msg 1.ret\n");
    return root;
}

}

TEST_CASE("a function signature round-trips through the file header") {
    const char* text =
        "!lime 1\n"
        "!module content.util\n"
        "!fn damage target:any amount:number -> number\n"
        "!fn ping\n";

    Diagnostics d;
    Graph g;
    REQUIRE(parseLime(text, g, d));
    REQUIRE_FALSE(d.hasErrors());
    REQUIRE(g.functions.size() == 2);

    CHECK(g.functions[0].name == "damage");
    REQUIRE(g.functions[0].params.size() == 2);
    CHECK(g.functions[0].params[0].name == "target");
    CHECK(g.functions[0].params[0].type == "any");
    CHECK(g.functions[0].params[1].type == "number");
    CHECK(g.functions[0].ret == "number");

    CHECK(g.functions[1].name == "ping");
    CHECK(g.functions[1].params.empty());
    CHECK(g.functions[1].ret.empty());

    CHECK(writeLime(g) == text);
}

TEST_CASE("the provider reads signatures without parsing node bodies") {
    const fs::path root = makeProject();

    std::string module;
    std::vector<FnDecl> fns;
    REQUIRE(GraphFnProvider::readSignatures(
        (root / "content" / "util.lime").string(), module, fns));
    CHECK(module == "content.util");
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].name == "double");
    REQUIRE(fns[0].params.size() == 1);
    CHECK(fns[0].params[0].name == "n");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("a declared function becomes an entry node and a call node") {
    const fs::path root = makeProject();

    TypeRegistry types;
    Diagnostics d;
    types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", d);

    GraphFnProvider p(root.string(), kPriorityOverrides);
    std::vector<NodeDesc> out;
    p.collect(types, out, d);
    REQUIRE_FALSE(d.hasErrors());

    auto find = [&](const std::string& id) -> const NodeDesc* {
        for (const NodeDesc& x : out) if (x.id == id) return &x;
        return nullptr;
    };

    const NodeDesc* entry = find(graphFnEntryId("content.util", "double"));
    REQUIRE(entry);
    CHECK(entry->isEvent);
    CHECK(entry->emit == "struct:fnentry");
    REQUIRE(entry->findPin("n"));
    CHECK(entry->findPin("n")->dir == PinDir::Out);

    const NodeDesc* call = find(graphFnCallId("content.util", "double"));
    REQUIRE(call);
    CHECK(call->emit == "graphcall");
    CHECK(call->target == "content.util:double");
    REQUIRE(call->findPin("n"));
    CHECK(call->findPin("n")->dir == PinDir::In);
    REQUIRE(call->findPin("ret"));
    CHECK(types.get(call->findPin("ret")->type).name == "number");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("a function graph compiles to a Lua module") {
    const fs::path root = makeProject();
    Diagnostics d;
    const std::string lua =
        compileFile(root / "content" / "util.lime", root.string(), d);

    for (const Diagnostic& x : d.all()) INFO(x.message);
    CHECK_FALSE(d.hasErrors());
    INFO(lua);

    CHECK(lua ==
          "-- Generated by LimeVSC from util.lime -- do not edit\n"
          "local M = {}\n"
          "\n"
          "function M.double(n)\n"
          "  return n * 2\n"
          "end\n"
          "\n"
          "return M\n");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("calling across modules emits a require") {
    const fs::path root = makeProject();
    Diagnostics d;
    const std::string lua =
        compileFile(root / "content" / "main.lime", root.string(), d);

    for (const Diagnostic& x : d.all()) INFO(x.message);
    CHECK_FALSE(d.hasErrors());
    INFO(lua);

    CHECK(lua ==
          "-- Generated by LimeVSC from main.lime -- do not edit\n"
          "local util = require(\"content.util\")\n"
          "\n"
          "Lime.onStart:hook(function()\n"
          "  local double = util.double(21)\n"
          "  test.log(double)\n"
          "end)\n");

    std::error_code ec;
    fs::remove_all(root, ec);
}
