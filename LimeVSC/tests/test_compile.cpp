#include "api/data_provider.h"
#include "limecore.h"

#include <ostream>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

using namespace lime;

namespace {

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

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}

TEST_CASE("golden corpus") {
    Env env;
    REQUIRE_FALSE(env.diag.hasErrors());

    const std::filesystem::path dir =
        std::filesystem::path(LIMEVSC_TEST_DIR) / "golden";
    REQUIRE(std::filesystem::exists(dir));

    int cases = 0, gotoCases = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".lime") continue;
        const std::filesystem::path expectPath =
            std::filesystem::path(entry.path()).replace_extension(".lua");

        INFO("golden: " << entry.path().filename().string());
        ++cases;

        Diagnostics rd;
        Graph g;
        REQUIRE(readLime(entry.path().string(), g, rd));
        REQUIRE_FALSE(rd.hasErrors());

        Diagnostics cd;
        const CompileResult r =
            compileGraph(g, env.nodes, env.types, env.emitters,
                         entry.path().filename().string(), cd);
        for (const Diagnostic& d : cd.all())
            INFO("diag: " << d.message);
        REQUIRE(r.ok);
        REQUIRE_FALSE(cd.hasErrors());

        if (r.gotoCount > 0) ++gotoCases;

        REQUIRE(std::filesystem::exists(expectPath));
        CHECK(r.lua == readFile(expectPath));

        REQUIRE(r.lua.size() >= 3);
        CHECK_FALSE(static_cast<unsigned char>(r.lua[0]) == 0xEF);
    }

    CHECK(cases >= 4);
    CHECK(gotoCases * 100 <= cases * 5);
}

TEST_CASE("a graph with no events compiles to nothing, with a warning") {
    Env env;
    Graph g;
    const NodeId n = g.addNode("test.log", 0, 0);
    g.node(n)->values.emplace_back("msg", "\"x\"");

    Diagnostics cd;
    const CompileResult r =
        compileGraph(g, env.nodes, env.types, env.emitters, "x.lime", cd);
    CHECK(r.ok);
    CHECK(cd.warningCount() > 0);
}

TEST_CASE("a value read from an unsequenced impure node is an error") {
    Env env;
    Graph g;
    const NodeId ev = g.addNode("Lime.onStart", 0, 0);
    const NodeId comp = g.addNode("test.compute", 100, 100);
    const NodeId log = g.addNode("test.log", 200, 0);
    g.connect(PinId::make(ev, "out"), PinId::make(log, "in"), PinKind::Exec);
    g.connect(PinId::make(comp, "ret"), PinId::make(log, "msg"), PinKind::Data);

    Diagnostics cd;
    compileGraph(g, env.nodes, env.types, env.emitters, "x.lime", cd);
    CHECK(cd.hasErrors());
}

TEST_CASE("an unreachable node is not validated") {
    Env env;
    Graph g;
    const NodeId ev = g.addNode("Lime.onStart", 0, 0);
    (void)ev;
    const NodeId orphan = g.addNode("core.branch", 200, 0);
    (void)orphan;

    Diagnostics cd;
    const CompileResult r =
        compileGraph(g, env.nodes, env.types, env.emitters, "x.lime", cd);
    CHECK(r.ok);
    CHECK_FALSE(cd.hasErrors());
}

TEST_CASE("a required unconnected input is rejected") {
    Env env;
    Graph g;
    const NodeId ev = g.addNode("Lime.onStart", 0, 0);
    const NodeId br = g.addNode("core.branch", 100, 0);
    g.connect(PinId::make(ev, "out"), PinId::make(br, "in"), PinKind::Exec);

    Diagnostics cd;
    compileGraph(g, env.nodes, env.types, env.emitters, "x.lime", cd);
    CHECK(cd.hasErrors());
}

TEST_CASE("a data cycle is rejected rather than overflowing the stack") {
    Env env;
    Graph g;
    const NodeId a = g.addNode("core.add", 0, 0);
    const NodeId b = g.addNode("core.add", 100, 0);
    g.connect(PinId::make(a, "ret"), PinId::make(b, "a"), PinKind::Data);
    g.connect(PinId::make(b, "ret"), PinId::make(a, "a"), PinKind::Data);

    Diagnostics cd;
    compileGraph(g, env.nodes, env.types, env.emitters, "x.lime", cd);
    CHECK(cd.hasErrors());
}

TEST_CASE("a shared pure value is bound once, not recomputed") {
    Env env;
    Graph g;
    const NodeId ev = g.addNode("Lime.onStart", 0, 0);
    const NodeId val = g.addNode("test.getValue", 0, 100);
    const NodeId add = g.addNode("core.add", 100, 100);
    const NodeId log = g.addNode("test.log", 200, 0);

    g.connect(PinId::make(ev, "out"), PinId::make(log, "in"), PinKind::Exec);
    g.connect(PinId::make(val, "ret"), PinId::make(add, "a"), PinKind::Data);
    g.connect(PinId::make(val, "ret"), PinId::make(add, "b"), PinKind::Data);
    g.connect(PinId::make(add, "ret"), PinId::make(log, "msg"), PinKind::Data);

    Diagnostics cd;
    const CompileResult r =
        compileGraph(g, env.nodes, env.types, env.emitters, "x.lime", cd);
    REQUIRE(r.ok);
    INFO(r.lua);
    const std::size_t first = r.lua.find("test.getValue()");
    REQUIRE(first != std::string::npos);
    CHECK(r.lua.find("test.getValue()", first + 1) == std::string::npos);
    CHECK(r.lua.find("local ") != std::string::npos);
}

TEST_CASE("source map points each line at the node that produced it") {
    Env env;
    Diagnostics rd;
    Graph g;
    REQUIRE(readLime(
        (std::filesystem::path(LIMEVSC_TEST_DIR) / "golden" / "seq.lime").string(),
        g, rd));

    Diagnostics cd;
    const CompileResult r =
        compileGraph(g, env.nodes, env.types, env.emitters, "seq.lime", cd);
    REQUIRE(r.ok);
    CHECK_FALSE(r.map.lines.empty());
    for (const auto& [line, node] : r.map.lines) {
        CHECK(line >= 1);
        CHECK(g.node(node) != nullptr);
    }
}

TEST_CASE("structuring fuzzer") {
    Env env;
    std::mt19937 rng(0xC0FFEE);

    constexpr int kGraphs = 10000;
    int compiled = 0, withGoto = 0;

    for (int iter = 0; iter < kGraphs; ++iter) {
        Graph g;
        const NodeId ev = g.addNode("Lime.onStart", 0, 0);

        const int n = 3 + static_cast<int>(rng() % 8);
        std::vector<NodeId> pool;
        for (int i = 0; i < n; ++i) {
            const int pick = static_cast<int>(rng() % 10);
            if (pick < 5)      pool.push_back(g.addNode("test.log", 0, 0));
            else if (pick < 7) pool.push_back(g.addNode("core.branch", 0, 0));
            else if (pick < 8) pool.push_back(g.addNode("core.while", 0, 0));
            else if (pick < 9) pool.push_back(g.addNode("core.forNum", 0, 0));
            else               pool.push_back(g.addNode("test.compute", 0, 0));
        }

        for (NodeId id : pool) {
            Node* node = g.node(id);
            if (!node) continue;
            if (node->type == "test.log") node->values.emplace_back("msg", "\"x\"");
            if (node->type == "core.branch" || node->type == "core.while")
                node->values.emplace_back("cond", "true");
        }

        g.connect(PinId::make(ev, "out"), PinId::make(pool[0], "in"), PinKind::Exec);

        for (std::size_t i = 0; i < pool.size(); ++i) {
            const Node* node = g.node(pool[i]);
            if (!node) continue;
            const NodeDesc* d = env.nodes.find(node->type);
            if (!d) continue;
            for (const PinDesc& p : d->pins) {
                if (p.dir != PinDir::Out || p.kind != PinKind::Exec) continue;
                if (rng() % 4 == 0) continue;
                const NodeId tgt = pool[rng() % pool.size()];
                if (tgt == pool[i]) continue;
                g.connect(PinId::make(pool[i], p.name), PinId::make(tgt, "in"),
                          PinKind::Exec);
            }
        }

        Diagnostics cd;
        const CompileResult r =
            compileGraph(g, env.nodes, env.types, env.emitters, "fuzz.lime", cd);
        if (!r.ok) continue;
        ++compiled;
        if (r.gotoCount > 0) ++withGoto;

        INFO("iteration " << iter);
        INFO(r.lua);

        CHECK_FALSE(r.lua.empty());

        auto countWord = [&](std::string_view w) {
            int c = 0;
            std::size_t p = 0;
            while ((p = r.lua.find(w, p)) != std::string::npos) {
                const bool leftOk = p == 0 || !std::isalnum(
                    static_cast<unsigned char>(r.lua[p - 1]));
                const std::size_t after = p + w.size();
                const bool rightOk = after >= r.lua.size() || !std::isalnum(
                    static_cast<unsigned char>(r.lua[after]));
                if (leftOk && rightOk) ++c;
                p = after;
            }
            return c;
        };
        const int opens = countWord("function") + countWord("if")
                          + countWord("while") + countWord("for");
        CHECK(countWord("end") == opens - countWord("elseif"));

        std::set<std::string> gotos, labels;
        std::size_t p = 0;
        while ((p = r.lua.find("goto ", p)) != std::string::npos) {
            const std::size_t e = r.lua.find('\n', p);
            gotos.insert(r.lua.substr(p + 5, e - p - 5));
            p = e == std::string::npos ? r.lua.size() : e;
        }
        p = 0;
        while ((p = r.lua.find("::", p)) != std::string::npos) {
            const std::size_t e = r.lua.find("::", p + 2);
            if (e == std::string::npos) break;
            labels.insert(r.lua.substr(p + 2, e - p - 2));
            p = e + 2;
        }
        for (const std::string& gt : gotos) {
            INFO("goto target: " << gt);
            CHECK(labels.count(gt) == 1);
        }
    }

    MESSAGE("fuzzer: " << compiled << "/" << kGraphs << " compiled, "
                       << withGoto << " needed goto");
    CHECK(compiled > kGraphs / 4);
}
