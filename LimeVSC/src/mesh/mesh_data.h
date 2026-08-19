#pragma once

#include "limecore.h"

#include <array>
#include <string>
#include <vector>

namespace lime {

struct MeshVertex {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> normal{0, 1, 0};
    std::array<float, 2> uv{0, 0};
};

struct MeshData {
    std::vector<MeshVertex>    vertices;
    std::vector<std::uint32_t> indices;

    std::array<float, 3> boundsMin{0, 0, 0};
    std::array<float, 3> boundsMax{0, 0, 0};

    bool empty() const noexcept { return indices.empty(); }
    std::array<float, 3> center() const;
    float radius() const;
    void computeBounds();
    void generateNormals();
};

bool parseObj(std::string_view text, MeshData& out, Diagnostics& diag);
bool readObj(const std::string& path, MeshData& out, Diagnostics& diag);

MeshData makeBox(float w, float h, float d);
MeshData makeSphere(float radius, int segments);
MeshData makeGrid(int halfExtent, float spacing);

}
