#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

using namespace lime;

namespace {

Graph parseOk(std::string_view text) {
    Diagnostics d;
    Graph g;
    REQUIRE(parseLime(text, g, d));
    REQUIRE_FALSE(d.hasErrors());
    return g;
}

}

TEST_CASE("base36 ids round-trip") {
    for (std::uint32_t v : {0u, 1u, 9u, 10u, 35u, 36u, 37u, 1295u, 1296u, 99999u}) {
        const std::string s = encodeId(v);
        auto back = decodeId(s);
        REQUIRE(back.has_value());
        CHECK(*back == v);
    }
    CHECK(encodeId(0) == "0");
    CHECK(encodeId(35) == "z");
    CHECK(encodeId(36) == "10");
    CHECK_FALSE(decodeId("").has_value());
    CHECK_FALSE(decodeId("A!").has_value());
}

TEST_CASE("header is required") {
    Diagnostics d;
    Graph g;
    CHECK_FALSE(parseLime("~0 core.branch @ 0 0\n", g, d));
    CHECK(d.hasErrors());
}

TEST_CASE("nodes, values, links parse") {
    const Graph g = parseOk(
        "!lime 1\n"
        "!module player.movement\n"
        "!graph OnUpdate\n"
        "\n"
        "~0 Lime.onUpdate @ 120 200\n"
        "  > out 2.in\n"
        "\n"
        "~1 core.boolean @ 300 320\n"
        "  = value true\n"
        "\n"
        "~2 core.branch @ 520 200\n"
        "  < cond 1.ret\n");

    CHECK(g.moduleName == "player.movement");
    CHECK(g.graphName == "OnUpdate");
    CHECK(g.nodes().size() == 3);
    CHECK(g.links().size() == 2);

    const Node* lit = g.node(NodeId{1});
    REQUIRE(lit != nullptr);
    CHECK(lit->type == "core.boolean");
    REQUIRE(lit->values.size() == 1);
    CHECK(lit->values[0].first == "value");
    CHECK(lit->values[0].second == "true");

    auto src = g.sourceOf(PinId::make(NodeId{2}, "cond"));
    REQUIRE(src.has_value());
    CHECK(src->node == NodeId{1});
    CHECK(src->pin.str() == "ret");

    auto tgt = g.execTargetOf(PinId::make(NodeId{0}, "out"));
    REQUIRE(tgt.has_value());
    CHECK(tgt->node == NodeId{2});
}

TEST_CASE("writer output is canonical and byte-stable") {
    const char* text =
        "!lime 1\n"
        "!module a.b\n"
        "\n"
        "~0 Lime.onStart @ 0 0\n"
        "  > out 1.in\n"
        "\n"
        "~1 core.raw @ 100 50\n"
        "  | print(\"one\")\n"
        "  | print(\"two\")\n"
        "  ; explains itself\n";

    const Graph g = parseOk(text);
    const std::string once = writeLime(g);
    CHECK(once == text);

    const Graph g2 = parseOk(once);
    CHECK(writeLime(g2) == once);
}

TEST_CASE("attribute order is normalised regardless of authoring order") {
    const Graph a = parseOk(
        "!lime 1\n~0 n.x @ 0 0\n  = z 1\n  = a 2\n");
    const Graph b = parseOk(
        "!lime 1\n~0 n.x @ 0 0\n  = a 2\n  = z 1\n");
    CHECK(writeLime(a) == writeLime(b));
}

TEST_CASE("raw body preserves interior whitespace") {
    const Graph g = parseOk(
        "!lime 1\n"
        "~0 core.raw @ 0 0\n"
        "  | if x then\n"
        "  |   return 1\n"
        "  | end\n");
    const Node* n = g.node(NodeId{0});
    REQUIRE(n != nullptr);
    CHECK(n->rawBody == "if x then\n  return 1\nend");
    CHECK(writeLime(g) == "!lime 1\n\n~0 core.raw @ 0 0\n"
                          "  | if x then\n  |   return 1\n  | end\n");
}

TEST_CASE("a BOM on input is tolerated") {
    Diagnostics d;
    Graph g;
    CHECK(parseLime("\xEF\xBB\xBF!lime 1\n~0 n.x @ 0 0\n", g, d) == false);
}

TEST_CASE("connecting an input twice replaces rather than fans in") {
    Graph g;
    g.addNodeWithId(NodeId{0}, "a", 0, 0);
    g.addNodeWithId(NodeId{1}, "b", 0, 0);
    g.addNodeWithId(NodeId{2}, "c", 0, 0);

    g.connect(PinId::make(NodeId{0}, "ret"), PinId::make(NodeId{2}, "x"), PinKind::Data);
    g.connect(PinId::make(NodeId{1}, "ret"), PinId::make(NodeId{2}, "x"), PinKind::Data);
    CHECK(g.links().size() == 1);
    CHECK(g.sourceOf(PinId::make(NodeId{2}, "x"))->node == NodeId{1});

    g.connect(PinId::make(NodeId{0}, "out"), PinId::make(NodeId{1}, "in"), PinKind::Exec);
    g.connect(PinId::make(NodeId{0}, "out"), PinId::make(NodeId{2}, "in"), PinKind::Exec);
    CHECK(g.execTargetOf(PinId::make(NodeId{0}, "out"))->node == NodeId{2});
}

TEST_CASE("ids are never reused after deletion") {
    Graph g;
    const NodeId a = g.addNode("x", 0, 0);
    g.removeNode(a);
    const NodeId b = g.addNode("y", 0, 0);
    CHECK(b.v != a.v);
}
