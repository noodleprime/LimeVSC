#include "mesh/mesh_data.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace lime {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::string_view nextToken(std::string_view& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    const std::size_t sp = s.find_first_of(" \t");
    const std::string_view tok = s.substr(0, sp);
    s = (sp == std::string_view::npos) ? std::string_view{} : s.substr(sp);
    return tok;
}

bool toFloat(std::string_view s, float& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool resolveIndex(std::string_view tok, std::size_t count, std::size_t& out) {
    if (tok.empty()) return false;
    long long v = 0;
    const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v);
    if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size()) return false;
    if (v > 0) {
        if (static_cast<std::size_t>(v) > count) return false;
        out = static_cast<std::size_t>(v) - 1;
        return true;
    }
    if (v < 0) {
        const long long from = static_cast<long long>(count) + v;
        if (from < 0) return false;
        out = static_cast<std::size_t>(from);
        return true;
    }
    return false;
}

struct Ref {
    std::size_t v = 0, vt = 0, vn = 0;
    bool hasVt = false, hasVn = false;
    bool operator==(const Ref&) const = default;
};

struct RefHash {
    std::size_t operator()(const Ref& r) const noexcept {
        std::size_t h = r.v * 73856093u;
        h ^= (r.hasVt ? r.vt : 0) * 19349663u;
        h ^= (r.hasVn ? r.vn : 0) * 83492791u;
        return h;
    }
};

}

void MeshData::computeBounds() {
    if (vertices.empty()) {
        boundsMin = boundsMax = {0, 0, 0};
        return;
    }
    boundsMin = boundsMax = vertices[0].position;
    for (const MeshVertex& v : vertices)
        for (int i = 0; i < 3; ++i) {
            boundsMin[i] = std::min(boundsMin[i], v.position[i]);
            boundsMax[i] = std::max(boundsMax[i], v.position[i]);
        }
}

std::array<float, 3> MeshData::center() const {
    return {(boundsMin[0] + boundsMax[0]) * 0.5f,
            (boundsMin[1] + boundsMax[1]) * 0.5f,
            (boundsMin[2] + boundsMax[2]) * 0.5f};
}

float MeshData::radius() const {
    float r = 0;
    for (int i = 0; i < 3; ++i)
        r = std::max(r, (boundsMax[i] - boundsMin[i]) * 0.5f);
    return r;
}

void MeshData::generateNormals() {
    std::vector<std::array<float, 3>> accum(vertices.size(), {0, 0, 0});
    std::vector<int> hits(vertices.size(), 0);

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
            continue;
        const auto& pa = vertices[a].position;
        const auto& pb = vertices[b].position;
        const auto& pc = vertices[c].position;
        const float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
        const float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
        const std::array<float, 3> n{uy * vz - uz * vy, uz * vx - ux * vz,
                                     ux * vy - uy * vx};
        for (std::uint32_t idx : {a, b, c}) {
            for (int k = 0; k < 3; ++k) accum[idx][k] += n[k];
            ++hits[idx];
        }
    }

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (!hits[i]) continue;
        const float len = std::sqrt(accum[i][0] * accum[i][0]
                                    + accum[i][1] * accum[i][1]
                                    + accum[i][2] * accum[i][2]);
        if (len < 1e-8f) continue;
        for (int k = 0; k < 3; ++k) vertices[i].normal[k] = accum[i][k] / len;
    }
}

bool parseObj(std::string_view text, MeshData& out, Diagnostics& diag) {
    out = MeshData{};

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uvs;
    std::unordered_map<Ref, std::uint32_t, RefHash> dedup;
    bool anyNormals = false;
    int badLines = 0;
    int lineNo = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                          : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        line = trim(line);
        if (line.empty() || line.front() == '#') continue;

        const std::string_view key = nextToken(line);
        if (key == "v" || key == "vn") {
            std::array<float, 3> v{0, 0, 0};
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) ok = toFloat(nextToken(line), v[i]);
            if (!ok) { ++badLines; continue; }
            (key == "v" ? positions : normals).push_back(v);
            if (key == "vn") anyNormals = true;
        } else if (key == "vt") {
            std::array<float, 2> t{0, 0};
            if (!toFloat(nextToken(line), t[0])) { ++badLines; continue; }
            toFloat(nextToken(line), t[1]);
            uvs.push_back(t);
        } else if (key == "f") {
            std::vector<std::uint32_t> face;
            for (;;) {
                const std::string_view tok = nextToken(line);
                if (tok.empty()) break;

                Ref ref;
                std::string_view rest = tok;
                const std::size_t s1 = rest.find('/');
                if (!resolveIndex(rest.substr(0, s1), positions.size(), ref.v)) {
                    ++badLines;
                    face.clear();
                    break;
                }
                if (s1 != std::string_view::npos) {
                    rest = rest.substr(s1 + 1);
                    const std::size_t s2 = rest.find('/');
                    const std::string_view vt = rest.substr(0, s2);
                    if (!vt.empty())
                        ref.hasVt = resolveIndex(vt, uvs.size(), ref.vt);
                    if (s2 != std::string_view::npos)
                        ref.hasVn = resolveIndex(rest.substr(s2 + 1),
                                                 normals.size(), ref.vn);
                }

                const auto it = dedup.find(ref);
                if (it != dedup.end()) {
                    face.push_back(it->second);
                } else {
                    MeshVertex mv;
                    mv.position = positions[ref.v];
                    if (ref.hasVn) mv.normal = normals[ref.vn];
                    if (ref.hasVt) mv.uv = uvs[ref.vt];
                    const auto idx = static_cast<std::uint32_t>(out.vertices.size());
                    out.vertices.push_back(mv);
                    dedup.emplace(ref, idx);
                    face.push_back(idx);
                }
            }
            for (std::size_t i = 2; i < face.size(); ++i) {
                out.indices.push_back(face[0]);
                out.indices.push_back(face[i - 1]);
                out.indices.push_back(face[i]);
            }
        }
    }

    if (badLines)
        diag.warn("skipped " + std::to_string(badLines)
                  + " malformed line(s) while reading the model");

    if (out.indices.empty()) {
        diag.error("model has no faces");
        return false;
    }
    if (!anyNormals) out.generateNormals();
    out.computeBounds();
    return true;
}

bool readObj(const std::string& path, MeshData& out, Diagnostics& diag) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        diag.error("cannot open " + path);
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseObj(ss.str(), out, diag);
}

MeshData makeBox(float w, float h, float d) {
    const float x = w * 0.5f, y = h * 0.5f, z = d * 0.5f;
    const std::array<std::array<float, 3>, 6> faceNormals{{
        {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}}};
    const std::array<std::array<std::array<float, 3>, 4>, 6> corners{{
        {{{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z}}},
        {{{x, -y, z}, {-x, -y, z}, {-x, y, z}, {x, y, z}}},
        {{{-x, -y, z}, {-x, -y, -z}, {-x, y, -z}, {-x, y, z}}},
        {{{x, -y, -z}, {x, -y, z}, {x, y, z}, {x, y, -z}}},
        {{{-x, -y, z}, {x, -y, z}, {x, -y, -z}, {-x, -y, -z}}},
        {{{-x, y, -z}, {x, y, -z}, {x, y, z}, {-x, y, z}}},
    }};

    MeshData m;
    for (int f = 0; f < 6; ++f) {
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (int c = 0; c < 4; ++c) {
            MeshVertex v;
            v.position = corners[f][c];
            v.normal = faceNormals[f];
            v.uv = {c == 1 || c == 2 ? 1.0f : 0.0f, c >= 2 ? 1.0f : 0.0f};
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
            m.indices.push_back(base + i);
    }
    m.computeBounds();
    return m;
}

MeshData makeSphere(float radius, int segments) {
    segments = std::max(3, segments);
    const int rings = std::max(2, segments / 2);
    MeshData m;

    for (int r = 0; r <= rings; ++r) {
        const float phi = 3.14159265f * static_cast<float>(r)
                          / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            const float theta = 6.2831853f * static_cast<float>(s)
                                / static_cast<float>(segments);
            MeshVertex v;
            v.normal = {std::sin(phi) * std::cos(theta), std::cos(phi),
                        std::sin(phi) * std::sin(theta)};
            for (int i = 0; i < 3; ++i) v.position[i] = v.normal[i] * radius;
            v.uv = {static_cast<float>(s) / static_cast<float>(segments),
                    static_cast<float>(r) / static_cast<float>(rings)};
            m.vertices.push_back(v);
        }
    }
    const int stride = segments + 1;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const auto a = static_cast<std::uint32_t>(r * stride + s);
            const auto b = static_cast<std::uint32_t>(a + stride);
            for (std::uint32_t i : {a, b, a + 1u}) m.indices.push_back(i);
            for (std::uint32_t i : {a + 1u, b, b + 1u}) m.indices.push_back(i);
        }
    m.computeBounds();
    return m;
}

MeshData makeGrid(int halfExtent, float spacing) {
    MeshData m;
    const float e = static_cast<float>(halfExtent) * spacing;
    for (int i = -halfExtent; i <= halfExtent; ++i) {
        const float o = static_cast<float>(i) * spacing;
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        MeshVertex a, b, c, d;
        a.position = {o, 0, -e};
        b.position = {o, 0, e};
        c.position = {-e, 0, o};
        d.position = {e, 0, o};
        m.vertices.insert(m.vertices.end(), {a, b, c, d});
        for (std::uint32_t k : {0u, 1u, 2u, 3u}) m.indices.push_back(base + k);
    }
    m.computeBounds();
    return m;
}

}
