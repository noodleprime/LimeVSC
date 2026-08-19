#include "render/renderer.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <unordered_map>
#include <vector>

namespace lime {
namespace {

const char* kShaderSource = R"HLSL(
cbuffer Frame : register(b0) {
    row_major float4x4 viewProj;
    row_major float4x4 world;
    float4 color;
    float4 params;
};

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; };

VSOut vsMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), world);
    o.pos = mul(wp, viewProj);
    o.nrm = mul(float4(i.nrm, 0.0), world).xyz;
    return o;
}

float4 psMain(VSOut i) : SV_TARGET {
    if (params.x > 0.5) return color;
    float3 n = normalize(i.nrm);
    float3 l = normalize(params.yzw);
    float d = saturate(dot(n, l)) * 0.75 + 0.25;
    return float4(color.rgb * d, color.a);
}
)HLSL";

struct FrameCB {
    float viewProj[16];
    float world[16];
    float color[4];
    float params[4];
};

void flatten(const M4& m, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = m.m[i][j];
}

template <class T>
void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct GpuMesh {
    MeshData        cpu;
    ID3D11Buffer*   vb = nullptr;
    ID3D11Buffer*   ib = nullptr;
    std::uint32_t   indexCount = 0;
};

}

V3 CameraState::forward() const {
    const float cp = std::cos(pitch);
    return {std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
}

V3 CameraState::right() const {
    return normalize(cross(V3{0, 1, 0}, forward()));
}

V3 CameraState::up() const { return cross(forward(), right()); }

void CameraState::focusOn(V3 target, float distance) {
    pivotDistance = distance;
    position = target - forward() * distance;
}

void CameraState::lookAt(V3 target) {
    const V3 d = target - position;
    const float len = length(d);
    if (len < 1e-5f) return;
    yaw = std::atan2(d.x, d.z);
    pitch = std::asin(d.y / len);
    pivotDistance = len;
}

M4 CameraState::view() const {
    return M4::lookAt(position, position + forward(), {0, 1, 0});
}

M4 CameraState::proj(float aspect) const {
    return M4::perspective(fov, aspect < 0.01f ? 1.0f : aspect, 0.05f, 5000.0f);
}

struct Renderer::Impl {
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    ID3D11VertexShader*  vs = nullptr;
    ID3D11PixelShader*   ps = nullptr;
    ID3D11InputLayout*   layout = nullptr;
    ID3D11Buffer*        cb = nullptr;
    ID3D11RasterizerState* rsSolid = nullptr;
    ID3D11RasterizerState* rsWire = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11BlendState*    blend = nullptr;

    ID3D11Texture2D*          colorTex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D*          depthTex = nullptr;
    ID3D11DepthStencilView*   dsv = nullptr;
    int width = 0, height = 0;

    M4 viewProj = M4::identity();
    std::unordered_map<std::string, GpuMesh> meshes;
    bool ok = false;

    bool createTargets(int w, int h);
    void releaseTargets();
    GpuMesh* upload(const std::string& key, MeshData data);
};

void Renderer::Impl::releaseTargets() {
    release(dsv);
    release(depthTex);
    release(srv);
    release(rtv);
    release(colorTex);
    width = height = 0;
}

bool Renderer::Impl::createTargets(int w, int h) {
    releaseTargets();
    if (w <= 0 || h <= 0) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &colorTex))) return false;
    if (FAILED(device->CreateRenderTargetView(colorTex, nullptr, &rtv)))
        return false;
    if (FAILED(device->CreateShaderResourceView(colorTex, nullptr, &srv)))
        return false;

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&dd, nullptr, &depthTex))) return false;
    if (FAILED(device->CreateDepthStencilView(depthTex, nullptr, &dsv)))
        return false;

    width = w;
    height = h;
    return true;
}

GpuMesh* Renderer::Impl::upload(const std::string& key, MeshData data) {
    if (data.vertices.empty() || data.indices.empty()) return nullptr;

    GpuMesh g;
    g.indexCount = static_cast<std::uint32_t>(data.indices.size());

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(data.vertices.size() * sizeof(MeshVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data.vertices.data();
    if (FAILED(device->CreateBuffer(&bd, &sd, &g.vb))) return nullptr;

    bd.ByteWidth = static_cast<UINT>(data.indices.size() * sizeof(std::uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = data.indices.data();
    if (FAILED(device->CreateBuffer(&bd, &sd, &g.ib))) {
        release(g.vb);
        return nullptr;
    }

    g.cpu = std::move(data);
    auto it = meshes.find(key);
    if (it != meshes.end()) {
        release(it->second.vb);
        release(it->second.ib);
        it->second = std::move(g);
        return &it->second;
    }
    return &meshes.emplace(key, std::move(g)).first->second;
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { shutdown(); }

bool Renderer::ready() const { return impl_ && impl_->ok; }

bool Renderer::init(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    impl_->device = device;
    impl_->ctx = context;

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "viewport",
                          nullptr, nullptr, "vsMain", "vs_4_0", flags, 0, &vsb,
                          &err))) {
        release(err);
        return false;
    }
    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "viewport",
                          nullptr, nullptr, "psMain", "ps_4_0", flags, 0, &psb,
                          &err))) {
        release(err);
        release(vsb);
        return false;
    }

    bool good =
        SUCCEEDED(device->CreateVertexShader(vsb->GetBufferPointer(),
                                             vsb->GetBufferSize(), nullptr,
                                             &impl_->vs))
        && SUCCEEDED(device->CreatePixelShader(psb->GetBufferPointer(),
                                               psb->GetBufferSize(), nullptr,
                                               &impl_->ps));

    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    good = good
           && SUCCEEDED(device->CreateInputLayout(elems, 3,
                                                  vsb->GetBufferPointer(),
                                                  vsb->GetBufferSize(),
                                                  &impl_->layout));
    release(vsb);
    release(psb);
    if (!good) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(FrameCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &impl_->cb))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    rd.FrontCounterClockwise = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &impl_->rsSolid))) return false;
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthBias = -1000;
    if (FAILED(device->CreateRasterizerState(&rd, &impl_->rsWire))) return false;

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&dsd, &impl_->dss))) return false;

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bld, &impl_->blend))) return false;

    impl_->ok = true;
    return true;
}

void Renderer::shutdown() {
    if (!impl_) return;
    for (auto& [k, g] : impl_->meshes) {
        release(g.vb);
        release(g.ib);
    }
    impl_->meshes.clear();
    impl_->releaseTargets();
    release(impl_->blend);
    release(impl_->dss);
    release(impl_->rsWire);
    release(impl_->rsSolid);
    release(impl_->cb);
    release(impl_->layout);
    release(impl_->ps);
    release(impl_->vs);
    impl_->ok = false;
}

bool Renderer::begin(int width, int height, const CameraState& cam) {
    if (!impl_->ok) return false;
    if (width != impl_->width || height != impl_->height)
        if (!impl_->createTargets(width, height)) return false;

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    impl_->viewProj = cam.view() * cam.proj(aspect);

    ID3D11DeviceContext* c = impl_->ctx;
    const float clear[4] = {0.11f, 0.12f, 0.14f, 1.0f};
    c->OMSetRenderTargets(1, &impl_->rtv, impl_->dsv);
    c->ClearRenderTargetView(impl_->rtv, clear);
    c->ClearDepthStencilView(impl_->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    c->RSSetViewports(1, &vp);

    c->IASetInputLayout(impl_->layout);
    c->VSSetShader(impl_->vs, nullptr, 0);
    c->PSSetShader(impl_->ps, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &impl_->cb);
    c->PSSetConstantBuffers(0, 1, &impl_->cb);
    c->OMSetDepthStencilState(impl_->dss, 0);
    const float bf[4] = {0, 0, 0, 0};
    c->OMSetBlendState(impl_->blend, bf, 0xffffffff);
    return true;
}

void Renderer::draw(const DrawItem& item) {
    if (!impl_->ok || !item.mesh || item.mesh->empty()) return;

    const GpuMesh* g = nullptr;
    for (const auto& [k, m] : impl_->meshes)
        if (&m.cpu == item.mesh) { g = &m; break; }
    if (!g || !g->vb) return;

    ID3D11DeviceContext* c = impl_->ctx;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(impl_->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    auto* cb = static_cast<FrameCB*>(map.pData);
    flatten(impl_->viewProj, cb->viewProj);
    flatten(item.transform, cb->world);
    for (int i = 0; i < 4; ++i) cb->color[i] = item.color[i];
    cb->params[0] = (item.unlit || item.lines) ? 1.0f : 0.0f;
    cb->params[1] = 0.45f;
    cb->params[2] = 0.80f;
    cb->params[3] = -0.40f;
    c->Unmap(impl_->cb, 0);

    const UINT stride = sizeof(MeshVertex), offset = 0;
    c->IASetVertexBuffers(0, 1, &g->vb, &stride, &offset);
    c->IASetIndexBuffer(g->ib, DXGI_FORMAT_R32_UINT, 0);
    c->IASetPrimitiveTopology(item.lines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                         : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->RSSetState(item.wireframe ? impl_->rsWire : impl_->rsSolid);
    c->DrawIndexed(g->indexCount, 0, 0);
}

void Renderer::end() {
    if (!impl_->ok) return;
    ID3D11RenderTargetView* none = nullptr;
    impl_->ctx->OMSetRenderTargets(1, &none, nullptr);
}

ID3D11ShaderResourceView* Renderer::texture() const { return impl_->srv; }

const MeshData* Renderer::cacheMesh(const std::string& key, MeshData data) {
    if (!impl_->ok) return nullptr;
    GpuMesh* g = impl_->upload(key, std::move(data));
    return g ? &g->cpu : nullptr;
}

const MeshData* Renderer::findMesh(const std::string& key) const {
    const auto it = impl_->meshes.find(key);
    return it == impl_->meshes.end() ? nullptr : &it->second.cpu;
}

void Renderer::clearMeshCache() {
    for (auto& [k, g] : impl_->meshes) {
        release(g.vb);
        release(g.ib);
    }
    impl_->meshes.clear();
}

}
