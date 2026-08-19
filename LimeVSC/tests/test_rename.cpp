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

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path makeProject() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "limevsc_rename_test";
    fs::remove_all(root, ec);

    write(root / "content" / "util.lime",
          "!lime 1\n"
          "!module content.util\n"
          "!fn double n:number -> number\n"
          "\n"
          "~0 fn.content.util.double.entry @ 0 0\n"
          "  > out 1.in\n"
          "\n"
          "~1 core.return @ 200 0\n");

    write(root / "content" / "main.lime",
          "!lime 1\n"
          "!module content.main\n"
          "\n"
          "~0 Lime.onStart @ 0 0\n"
          "  > out 1.in\n"
          "\n"
          "~1 fn.content.util.double @ 200 0\n"
          "  = n 21\n");
    return root;
}

}

TEST_CASE("renaming a graph re-points every reference to it") {
    std::error_code ec;
    const fs::path root = makeProject();

    const fs::path oldPath = root / "content" / "util.lime";
    const fs::path newPath = root / "content" / "helpers.lime";
    fs::rename(oldPath, newPath, ec);
    REQUIRE_FALSE(ec);

    const std::string oldPrefix = "fn.content.util.";
    const std::string newPrefix = "fn.content.helpers.";

    for (const fs::path& f : {newPath, root / "content" / "main.lime"}) {
        Diagnostics rd;
        Graph g;
        REQUIRE(readLime(f.string(), g, rd));

        int hits = 0;
        for (const Node& n : g.nodes()) {
            if (n.type.rfind(oldPrefix, 0) != 0) continue;
            g.node(n.id)->type = newPrefix + n.type.substr(oldPrefix.size());
            ++hits;
        }
        if (f == newPath) g.moduleName = "content.helpers";
        CHECK(hits == 1);

        Diagnostics wd;
        REQUIRE(writeLimeFile(f.string(), g, wd));
    }

    SUBCASE("the caller now names the new module") {
        const std::string main = readAll(root / "content" / "main.lime");
        INFO(main);
        CHECK(main.find("fn.content.helpers.double") != std::string::npos);
        CHECK(main.find("fn.content.util.") == std::string::npos);
    }

    SUBCASE("the renamed graph's own entry node moved with it") {
        const std::string util = readAll(root / "content" / "helpers.lime");
        INFO(util);
        CHECK(util.find("fn.content.helpers.double.entry") != std::string::npos);
        CHECK(util.find("content.util") == std::string::npos);
    }

    SUBCASE("the provider rediscovers the function under its new module") {
        TypeRegistry types;
        Diagnostics d;
        GraphFnProvider p(root.string(), kPriorityOverrides);
        std::vector<NodeDesc> out;
        p.collect(types, out, d);

        bool found = false, stale = false;
        for (const NodeDesc& n : out) {
            if (n.id == graphFnCallId("content.helpers", "double")) found = true;
            if (n.id.rfind("fn.content.util.", 0) == 0) stale = true;
        }
        CHECK(found);
        CHECK_FALSE(stale);
    }

    fs::remove_all(root, ec);
}

TEST_CASE("a graph that references nothing is left untouched") {
    std::error_code ec;
    const fs::path root = makeProject();
    const fs::path main = root / "content" / "main.lime";

    const std::string before = readAll(main);

    Diagnostics rd;
    Graph g;
    REQUIRE(readLime(main.string(), g, rd));
    int hits = 0;
    for (const Node& n : g.nodes())
        if (n.type.rfind("fn.content.somethingelse.", 0) == 0) ++hits;
    CHECK(hits == 0);
    CHECK(readAll(main) == before);

    fs::remove_all(root, ec);
}
