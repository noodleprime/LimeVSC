#include <doctest/doctest.h>

#include "ui/pixel_wire.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

struct Rect {
    ImVec2 lo, hi;
    float  w() const { return hi.x - lo.x; }
    float  h() const { return hi.y - lo.y; }
};

std::vector<Rect> wire(ImVec2 a, ImVec2 b, float cell = 1.0f,
                       float thickness = 1.0f) {
    const float span = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
    std::vector<Rect> out;
    lime::buildPixelWire(a, ImVec2(a.x + span, a.y), ImVec2(b.x - span, b.y), b,
                         cell, thickness,
                         [&](ImVec2 lo, ImVec2 hi) { out.push_back({lo, hi}); });
    return out;
}

bool covers(const std::vector<Rect>& rs, ImVec2 p) {
    return std::any_of(rs.begin(), rs.end(), [&](const Rect& r) {
        return p.x >= r.lo.x - 1.001f && p.x <= r.hi.x + 1.001f
               && p.y >= r.lo.y - 1.001f && p.y <= r.hi.y + 1.001f;
    });
}

}

TEST_CASE("a shallow wire draws no vertical runs") {
    for (const Rect& r : wire(ImVec2(0, 0), ImVec2(400, 8))) {
        INFO("run ", r.w(), " x ", r.h());
        CHECK(r.h() == doctest::Approx(1.0f));
    }
}

TEST_CASE("a steep wire keeps the verticals it needs") {
    const std::vector<Rect> rs = wire(ImVec2(0, 0), ImVec2(12, 400));
    CHECK(std::any_of(rs.begin(), rs.end(),
                      [](const Rect& r) { return r.h() > 1.5f; }));
}

TEST_CASE("every run is one cell thick on its minor axis") {
    for (const Rect& r : wire(ImVec2(0, 0), ImVec2(300, 120))) {
        INFO("run ", r.w(), " x ", r.h());
        CHECK((r.w() == doctest::Approx(1.0f) || r.h() == doctest::Approx(1.0f)));
    }
}

TEST_CASE("a level wire is a single run") {
    const std::vector<Rect> rs = wire(ImVec2(0, 64), ImVec2(280, 64));
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].h() == doctest::Approx(1.0f));
    CHECK(rs[0].w() == doctest::Approx(281.0f));
}

TEST_CASE("pins that are all but level get a dead level wire") {
    const std::vector<Rect> rs =
        wire(ImVec2(0, 200), ImVec2(500, 200 + lime::kLevelTolerance - 1.0f));
    CHECK(rs.size() == 1);
}

TEST_CASE("a real slope is still drawn as one") {
    const std::vector<Rect> rs =
        wire(ImVec2(0, 200), ImVec2(500, 200 + lime::kLevelTolerance + 1.0f));
    CHECK(rs.size() > 1);

    const std::vector<Rect> row = wire(ImVec2(0, 200), ImVec2(500, 218));
    CHECK(row.size() > 1);
}

TEST_CASE("a level wire is levelled between its two pins") {
    const float drop = lime::kLevelTolerance - 1.0f;
    const std::vector<Rect> rs = wire(ImVec2(0, 200), ImVec2(500, 200 + drop));
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].lo.y == doctest::Approx(std::floor(200.0f + drop * 0.5f)));
}

TEST_CASE("a wire that doubles back is not flattened") {
    const std::vector<Rect> rs = wire(ImVec2(500, 200), ImVec2(0, 202));
    CHECK(rs.size() > 1);
}

TEST_CASE("a wire reaches both of its pins") {
    const ImVec2 a(10.5f, 33.25f), b(407.0f, 190.75f);
    const std::vector<Rect> rs = wire(a, b);
    REQUIRE(!rs.empty());
    CHECK(covers(rs, a));
    CHECK(covers(rs, b));
}

TEST_CASE("a wire has no gaps in it") {
    const std::vector<Rect> rs = wire(ImVec2(0, 0), ImVec2(360, 210));
    REQUIRE(rs.size() > 2);
    for (std::size_t i = 1; i < rs.size(); ++i) {
        const Rect& p = rs[i - 1];
        const Rect& q = rs[i];
        INFO("run ", i, " starts at ", q.lo.x, ",", q.lo.y, " after ", p.hi.x,
             ",", p.hi.y);
        CHECK(q.lo.x <= p.hi.x + 0.001f);
        CHECK(q.hi.x >= p.lo.x - 0.001f);
        CHECK(q.lo.y <= p.hi.y + 0.001f);
        CHECK(q.hi.y >= p.lo.y - 0.001f);
    }
}

TEST_CASE("runs land on the shared lattice") {
    for (const Rect& r : wire(ImVec2(3.7f, 11.2f), ImVec2(203.1f, 84.9f), 2.0f)) {
        CHECK(std::fmod(r.lo.x, 2.0f) == doctest::Approx(0.0f));
        CHECK(std::fmod(r.lo.y, 2.0f) == doctest::Approx(0.0f));
    }
}

TEST_CASE("a thick wire fattens evenly and a thin one does not") {
    for (const Rect& r : wire(ImVec2(0, 0), ImVec2(400, 8), 1.0f, 1.0f))
        CHECK(r.h() == doctest::Approx(1.0f));

    for (const Rect& r : wire(ImVec2(0, 0), ImVec2(400, 8), 1.0f, 5.0f))
        CHECK(r.h() == doctest::Approx(5.0f));
}
