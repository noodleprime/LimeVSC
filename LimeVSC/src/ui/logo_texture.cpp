#include "ui/panels.h"

#include <d3d11.h>

#include <cstdint>

namespace lime {

extern const int kLogoSize;
extern const std::uint8_t kLogoRGBA[];

void* createLogoTexture(void* devicePtr) {
    auto* device = static_cast<ID3D11Device*>(devicePtr);
    if (!device) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(kLogoSize);
    td.Height = static_cast<UINT>(kLogoSize);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = kLogoRGBA;
    sd.SysMemPitch = static_cast<UINT>(kLogoSize) * 4u;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) return nullptr;

    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return FAILED(hr) ? nullptr : srv;
}

}
