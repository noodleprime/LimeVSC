#include "api/data_provider.h"
#include "lime/lua_import.h"
#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

#include <memory>

using namespace lime;

namespace {

struct Env {
    TypeRegistry types;
    NodeRegistry nodes;
    Diagnostics  diag;
    Env() {
        types.loadFile(std::string(LIMEVSC_DATA_DIR) + "/core.limetypes", diag);
        nodes.addProvider(std::make_unique<DataFileProvider>(
            std::string(LIMEVSC_DATA_DIR) + "/core.limenodes", "core",
            kPriorityCore));
        nodes.rebuild(types, diag);
    }
};

Graph importOf(const std::string& lua, const Env& env, Diagnostics& d) {
    Graph g;
    ImportOptions o;
    o.moduleName = "content.main";
    REQUIRE(importLua(lua, &env.nodes, o, g, d));
    return g;
}

int countOf(const Graph& g, const std::string& type) {
    int n = 0;
    for (const Node& x : g.nodes()) if (x.type == type) ++n;
    return n;
}

const Node* firstOf(const Graph& g, const std::string& type) {
    for (const Node& x : g.nodes()) if (x.type == type) return &x;
    return nullptr;
}

}

TEST_CASE("hook registrations become the matching event nodes") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Lime.onStart:hook(function()\n"
        "  Lime.log(\"hi\")\n"
        "end)\n"
        "\n"
        "Lime.onUpdate:hook(function(dt)\n"
        "  Lime.log(dt)\n"
        "end)\n",
        env, d);

    CHECK(countOf(g, "Lime.onStart") == 1);
    CHECK(countOf(g, "Lime.onUpdate") == 1);
    CHECK(countOf(g, "core.raw") == 2);
    CHECK(g.moduleName == "content.main");
}

TEST_CASE("control flow becomes real nodes, leaves stay raw") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Lime.onStart:hook(function()\n"
        "  if x > 1 then\n"
        "    doA()\n"
        "  else\n"
        "    doB()\n"
        "  end\n"
        "  while running do\n"
        "    tick()\n"
        "  end\n"
        "  for i = 1, 10 do\n"
        "    step(i)\n"
        "  end\n"
        "  for k, v in pairs(t) do\n"
        "    use(k, v)\n"
        "  end\n"
        "end)\n",
        env, d);

    CHECK(countOf(g, "core.branch") == 1);
    CHECK(countOf(g, "core.while") == 1);
    CHECK(countOf(g, "core.forNum") == 1);
    CHECK(countOf(g, "core.forIn") == 1);

    const Node* br = firstOf(g, "core.branch");
    REQUIRE(br);
    bool found = false;
    for (const auto& [k, v] : br->values)
        if (k == "cond") { CHECK(v == "x > 1"); found = true; }
    CHECK(found);
}

TEST_CASE("a string containing 'end' does not terminate a block") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Lime.onStart:hook(function()\n"
        "  if a then\n"
        "    print(\"the end is nigh\")\n"
        "    print('another end here')\n"
        "  end\n"
        "  after()\n"
        "end)\n",
        env, d);

    CHECK(countOf(g, "core.branch") == 1);
    CHECK(countOf(g, "core.raw") == 3);
}

TEST_CASE("long comments and long strings are skipped whole") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Lime.onStart:hook(function()\n"
        "  --[[ this comment mentions end and if and while ]]\n"
        "  local s = [==[ nested ]] still inside ]==]\n"
        "  done()\n"
        "end)\n",
        env, d);

    CHECK(countOf(g, "Lime.onStart") == 1);
    CHECK(countOf(g, "core.branch") == 0);
    CHECK(countOf(g, "core.raw") == 2);
}

TEST_CASE("break and return become their own nodes") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Lime.onStart:hook(function()\n"
        "  while true do\n"
        "    break\n"
        "  end\n"
        "  return 42\n"
        "end)\n",
        env, d);

    CHECK(countOf(g, "core.break") == 1);
    REQUIRE(countOf(g, "core.return") == 1);

    const Node* ret = firstOf(g, "core.return");
    REQUIRE(ret);
    bool found = false;
    for (const auto& [k, v] : ret->values)
        if (k == "value") { CHECK(v == "42"); found = true; }
    CHECK(found);
}

TEST_CASE("an unknown hook target still imports, with a warning") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Some.Unknown.event:hook(function()\n"
        "  work()\n"
        "end)\n",
        env, d);

    CHECK(countOf(g, "core.raw") == 1);
    CHECK(d.warningCount() > 0);
    CHECK_FALSE(d.hasErrors());
}

TEST_CASE("top-level statements outside any hook are preserved") {
    Env env;
    Diagnostics d;
    const Graph g = importOf("local cfg = 1\nsetup(cfg)\n", env, d);

    CHECK(countOf(g, "Lime.onStart") == 1);
    CHECK(countOf(g, "core.raw") == 2);
}

TEST_CASE("an imported graph round-trips through the .lime writer") {
    Env env;
    Diagnostics d;
    const Graph g = importOf(
        "Lime.onStart:hook(function()\n"
        "  if ready then\n"
        "    go()\n"
        "  end\n"
        "end)\n",
        env, d);

    const std::string text = writeLime(g);
    Diagnostics d2;
    Graph back;
    REQUIRE(parseLime(text, back, d2));
    CHECK_FALSE(d2.hasErrors());
    CHECK(writeLime(back) == text);
    CHECK(back.nodes().size() == g.nodes().size());
    CHECK(back.links().size() == g.links().size());
}

TEST_CASE("importing never fails, even on malformed input") {
    Env env;
    for (const char* bad : {"", "end end end", "if", "function(",
                            "\"unterminated", "--[[ unterminated"}) {
        INFO("input: " << bad);
        Diagnostics d;
        Graph g;
        ImportOptions o;
        CHECK(importLua(bad, &env.nodes, o, g, d));
        CHECK_FALSE(d.hasErrors());
    }
}
