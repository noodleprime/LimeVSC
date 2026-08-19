#pragma once

#include "mesh/mesh_data.h"
#include "render/mathx.h"

#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace lime {

struct CameraState {
    V3    position{0, 4, -12};
    float yaw = 0.0f;
    float pitch = -0.25f;
    float fov = 50.0f;

    float pivotDistance = 12.0f;
    float flySpeed = 8.0f;

    V3 forward() const;
    V3 right() const;
    V3 up() const;
    V3 pivot() const { return position + forward() * pivotDistance; }
    void focusOn(V3 target, float distance);
    void lookAt(V3 target);

    V3 eye() const { return position; }
    M4 view() const;
    M4 proj(float aspect) const;
};

struct DrawItem {
    const MeshData* mesh = nullptr;
    M4       transform = M4::identity();
    std::array<float, 4> color{0.8f, 0.8f, 0.85f, 1.0f};
    bool     wireframe = false;
    bool     lines = false;
    bool     unlit = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool init(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();
    bool ready() const;

    bool begin(int width, int height, const CameraState& cam);
    void draw(const DrawItem& item);
    void end();

    ID3D11ShaderResourceView* texture() const;

    const MeshData* cacheMesh(const std::string& key, MeshData data);
    const MeshData* findMesh(const std::string& key) const;
    void            clearMeshCache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
