#include <doctest/doctest.h>

#include "mesh/mesh_data.h"

#include <cmath>
#include <string>

using namespace lime;

namespace {

MeshData parseOk(const std::string& text) {
    MeshData m;
    Diagnostics d;
    REQUIRE(parseObj(text, m, d));
    return m;
}

bool nearly(float a, float b) { return std::abs(a - b) < 1e-4f; }

}

TEST_CASE("a triangle parses") {
    const MeshData m = parseOk(
        "# a comment\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n");

    REQUIRE(m.vertices.size() == 3);
    REQUIRE(m.indices.size() == 3);
    CHECK(nearly(m.vertices[1].position[0], 1.0f));
    CHECK(nearly(m.vertices[2].position[1], 1.0f));
    CHECK(nearly(std::abs(m.vertices[0].normal[2]), 1.0f));
    CHECK(nearly(m.boundsMax[0], 1.0f));
    CHECK(nearly(m.boundsMin[0], 0.0f));
}

TEST_CASE("faces are fan-triangulated") {
    const MeshData quad = parseOk(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3 4\n");
    CHECK(quad.vertices.size() == 4);
    CHECK(quad.indices.size() == 6);

    const MeshData pentagon = parseOk(
        "v 0 0 0\nv 1 0 0\nv 2 1 0\nv 1 2 0\nv 0 2 0\n"
        "f 1 2 3 4 5\n");
    CHECK(pentagon.indices.size() == 9);
}

TEST_CASE("every index form is accepted") {
    SUBCASE("v/vt") {
        const MeshData m = parseOk(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vt 0 0\nvt 1 0\nvt 0 1\n"
            "f 1/1 2/2 3/3\n");
        REQUIRE(m.vertices.size() == 3);
        CHECK(nearly(m.vertices[1].uv[0], 1.0f));
    }
    SUBCASE("v//vn") {
        const MeshData m = parseOk(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vn 0 1 0\n"
            "f 1//1 2//1 3//1\n");
        REQUIRE(m.vertices.size() == 3);
        CHECK(nearly(m.vertices[0].normal[1], 1.0f));
    }
    SUBCASE("v/vt/vn") {
        const MeshData m = parseOk(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vt 0.25 0.75\n"
            "vn 1 0 0\n"
            "f 1/1/1 2/1/1 3/1/1\n");
        REQUIRE(m.vertices.size() == 3);
        CHECK(nearly(m.vertices[0].uv[1], 0.75f));
        CHECK(nearly(m.vertices[0].normal[0], 1.0f));
    }
    SUBCASE("negative indices count back from the end") {
        const MeshData m = parseOk(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "f -3 -2 -1\n");
        REQUIRE(m.vertices.size() == 3);
        CHECK(nearly(m.vertices[0].position[0], 0.0f));
        CHECK(nearly(m.vertices[1].position[0], 1.0f));
    }
}

TEST_CASE("identical vertex references are shared") {
    const MeshData m = parseOk(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3\n"
        "f 1 3 4\n");
    CHECK(m.vertices.size() == 4);
    CHECK(m.indices.size() == 6);
}

TEST_CASE("a vertex referenced with different attributes is not shared") {
    const MeshData m = parseOk(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vn 0 1 0\nvn 1 0 0\n"
        "f 1//1 2//1 3//1\n"
        "f 1//2 2//2 3//2\n");
    CHECK(m.vertices.size() == 6);
}

TEST_CASE("malformed input is survivable") {
    SUBCASE("a file with no faces is rejected, not silently empty") {
        MeshData m;
        Diagnostics d;
        CHECK_FALSE(parseObj("v 0 0 0\nv 1 0 0\n", m, d));
        CHECK(d.hasErrors());
    }
    SUBCASE("out-of-range indices are skipped and reported") {
        MeshData m;
        Diagnostics d;
        CHECK_FALSE(parseObj("v 0 0 0\nf 1 2 3\n", m, d));
        CHECK_FALSE(d.all().empty());
    }
    SUBCASE("index zero is invalid in OBJ") {
        MeshData m;
        Diagnostics d;
        CHECK_FALSE(parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n", m, d));
    }
    SUBCASE("unknown directives are ignored, not fatal") {
        MeshData m;
        Diagnostics d;
        CHECK(parseObj("mtllib scene.mtl\n"
                       "o Cube\n"
                       "g mesh1\n"
                       "s off\n"
                       "usemtl wood\n"
                       "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                       "f 1 2 3\n",
                       m, d));
        CHECK(m.indices.size() == 3);
    }
    SUBCASE("a garbage coordinate does not derail the rest of the file") {
        MeshData m;
        Diagnostics d;
        CHECK(parseObj("v nope nope nope\n"
                       "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                       "f 1 2 3\n",
                       m, d));
        CHECK(m.indices.size() == 3);
        CHECK_FALSE(d.all().empty());
    }
    SUBCASE("CRLF line endings") {
        MeshData m;
        Diagnostics d;
        CHECK(parseObj("v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n", m, d));
        CHECK(m.indices.size() == 3);
    }
}

TEST_CASE("bounds and framing") {
    const MeshData m = parseOk(
        "v -2 -1 -3\nv 4 5 6\nv 0 0 0\n"
        "f 1 2 3\n");
    CHECK(nearly(m.boundsMin[0], -2.0f));
    CHECK(nearly(m.boundsMax[2], 6.0f));
    CHECK(nearly(m.center()[0], 1.0f));
    CHECK(nearly(m.center()[1], 2.0f));
    CHECK(nearly(m.radius(), 4.5f));
}

TEST_CASE("primitives are well formed") {
    const MeshData box = makeBox(2, 2, 2);
    CHECK(box.vertices.size() == 24);
    CHECK(box.indices.size() == 36);
    CHECK(nearly(box.boundsMax[0], 1.0f));
    CHECK(nearly(box.boundsMin[1], -1.0f));

    const MeshData sphere = makeSphere(1.0f, 16);
    CHECK_FALSE(sphere.empty());
    CHECK(sphere.indices.size() % 3 == 0);
    for (const MeshVertex& v : sphere.vertices) {
        const float r = std::sqrt(v.position[0] * v.position[0]
                                  + v.position[1] * v.position[1]
                                  + v.position[2] * v.position[2]);
        CHECK(nearly(r, 1.0f));
    }

    const MeshData grid = makeGrid(4, 1.0f);
    CHECK(grid.indices.size() % 2 == 0);
    CHECK(grid.vertices.size() == 9 * 4);
    CHECK(nearly(grid.boundsMax[0], 4.0f));
    for (const MeshVertex& v : grid.vertices) CHECK(nearly(v.position[1], 0.0f));

    for (const MeshData* m : {&box, &sphere, &grid})
        for (std::uint32_t i : m->indices) CHECK(i < m->vertices.size());
}

TEST_CASE("generated normals point outward on a closed shape") {
    MeshData m = parseOk(
        "v 1 1 1\nv -1 -1 1\nv -1 1 -1\nv 1 -1 -1\n"
        "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n");
    m.generateNormals();
    for (const MeshVertex& v : m.vertices) {
        const float d = v.position[0] * v.normal[0] + v.position[1] * v.normal[1]
                        + v.position[2] * v.normal[2];
        CHECK(d > 0.0f);
    }
}
