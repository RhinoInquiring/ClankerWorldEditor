#include "Minimap.hpp"

#include <wrl/client.h>

#include <cstring>

using namespace DirectX;

std::vector<uint8_t> RenderTopDown(ID3D11Device* device, ID3D11DeviceContext* context, Renderer& renderer, ModelRenderer& models,
                                   DrawOptions options, ModelRenderer::DrawSettings settings, float x0, float z0, float spanX,
                                   float spanZ, float top, UINT width, UINT height, bool water)
{
    std::vector<uint8_t> rgba;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color, depth, staging;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    device->CreateTexture2D(&d, nullptr, &color);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&d, nullptr, &staging);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    device->CreateTexture2D(&d, nullptr, &depth);
    if (!color || !depth || !staging || FAILED(device->CreateRenderTargetView(color.Get(), nullptr, &rtv)) ||
        FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)))
        return rgba;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDsv;
    context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    UINT viewports = 1;
    D3D11_VIEWPORT oldVp{};
    context->RSGetViewports(&viewports, &oldVp);
    const float cx = x0 + spanX / 2, cz = z0 + spanZ / 2;
    const XMFLOAT3 eye{ cx, top + 1000.0f, cz };
    const XMMATRIX viewProj = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMVectorSet(cx, top, cz, 1), XMVectorSet(0, 0, -1, 0)) *
                              XMMatrixOrthographicRH(spanX, spanZ, 1.0f, 4000.0f);
    const float background[4] = { 0.16f, 0.17f, 0.20f, 1 };
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
    context->RSSetViewports(1, &vp);
    context->ClearRenderTargetView(rtv.Get(), background);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    options.wireframe = false;
    options.lod = false;   // seen whole, at full detail
    renderer.Draw(viewProj, options);
    settings.distance = 100000.0f;
    models.Draw(viewProj, eye, settings);
    if (water) renderer.DrawWater(viewProj, options.solo);
    context->CopyResource(staging.Get(), color.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
    {
        rgba.resize(size_t(width) * height * 4);
        for (UINT y = 0; y < height; ++y)
            memcpy(rgba.data() + size_t(y) * width * 4, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(width) * 4);
        context->Unmap(staging.Get(), 0);
    }
    context->OMSetRenderTargets(1, oldRtv.GetAddressOf(), oldDsv.Get());
    if (viewports) context->RSSetViewports(1, &oldVp);
    return rgba;
}

std::vector<uint8_t> MinimapFromTopDown(const std::vector<uint8_t>& rgba, UINT size)
{
    // The render's layout is the client's: checked against Blizzard's own pictures (--minimap-check, by eye).
    std::vector<uint8_t> out = rgba;
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;   // opaque, as the client's
    (void)size;
    return out;
}
