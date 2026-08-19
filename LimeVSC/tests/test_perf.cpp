#include "api/data_provider.h"
#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>

using namespace lime;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Env {
    TypeRegistry    types;
    NodeRegistry    nodes;
    EmitterRegistry emitters = EmitterRegistry::withBuiltins();
    Diagnostics     diag;

    Env() {
        const std::string data = LIMEVSC_DATA_DIR;
        const std::string test = LIMEVSC_TEST_DIR;
        types.loadFile(data + "/core.limetypes", diag);
        nodes.addProvider(std::make_unique<DataFileProvider>(
            data + "/core.limenodes", "core", kPriorityCore));
        nodes.addProvider(std::make_unique<DataFileProvider>(
            test + "/data/golden.limenodes", "golden", kPriorityOverrides));
        nodes.rebuild(types, diag);
    }
};

Graph buildLarge(int chainLength) {
    Graph g;
    const NodeId ev = g.addNode("Lime.onStart", 0, 0);
    NodeId prev{};

    for (int i = 0; i < chainLength; ++i) {
        const NodeId add = g.addNode("core.add", 0, 0);
        g.node(add)->values.emplace_back("a", std::to_string(i));
        g.node(add)->values.emplace_back("b", "1");

        const NodeId mul = g.addNode("core.mul", 0, 0);
        g.node(mul)->values.emplace_back("b", "2");
        g.connect(PinId::make(add, "ret"), PinId::make(mul, "a"), PinKind::Data);

        const NodeId log = g.addNode("test.log", 0, 0);
        g.connect(PinId::make(mul, "ret"), PinId::make(log, "msg"), PinKind::Data);

        if (prev.valid())
            g.connect(PinId::make(prev, "out"), PinId::make(log, "in"), PinKind::Exec);
        else
            g.connect(PinId::make(ev, "out"), PinId::make(log, "in"), PinKind::Exec);
        prev = log;
    }
    return g;
}

}

TEST_CASE("perf: 5000-node graph compiles well inside budget") {
    Env env;
    REQUIRE_FALSE(env.diag.hasErrors());

    const Graph g = buildLarge(1666);
    CHECK(g.nodes().size() >= 4990);

    Diagnostics d;
    const auto t0 = Clock::now();
    const CompileResult r =
        compileGraph(g, env.nodes, env.types, env.emitters, "perf.lime", d);
    const double ms = msSince(t0);

    for (const Diagnostic& x : d.all()) INFO(x.message);
    REQUIRE(r.ok);
    CHECK(r.gotoCount == 0);

    MESSAGE("compile of " << g.nodes().size() << " nodes: " << ms << " ms");
    CHECK(ms < 400.0);
}

TEST_CASE("perf: writing and re-reading a large graph stays linear") {
    const Graph g = buildLarge(1666);

    const auto t0 = Clock::now();
    const std::string text = writeLime(g);
    const double writeMs = msSince(t0);

    Diagnostics d;
    Graph back;
    const auto t1 = Clock::now();
    REQUIRE(parseLime(text, back, d));
    const double readMs = msSince(t1);

    REQUIRE_FALSE(d.hasErrors());
    CHECK(back.nodes().size() == g.nodes().size());
    CHECK(back.links().size() == g.links().size());
    CHECK(writeLime(back) == text);

    MESSAGE("write " << writeMs << " ms, read " << readMs << " ms, "
                     << text.size() << " bytes");
    CHECK(writeMs < 500.0);
    CHECK(readMs < 500.0);
}

TEST_CASE("perf: the engine node catalog builds quickly enough for startup") {
    const auto t0 = Clock::now();
    Env env;
    const double ms = msSince(t0);
    MESSAGE("catalog build: " << ms << " ms for " << env.nodes.all().size()
                              << " nodes");
    CHECK(ms < 1000.0);
}
