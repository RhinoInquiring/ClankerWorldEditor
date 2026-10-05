#pragma once

#include "ModelRenderer.hpp"
#include "Renderer.hpp"

#include <d3d11.h>

#include <cstdint>
#include <vector>

/// A picture of a world area seen straight down (orthographic): what the renderer holds of layer `options.solo` and
/// the models of `models.layer`, water too when asked. RGBA rows; the top row is the area's smallest z, the left
/// column its smallest x. Restores the caller's render target and viewport.
std::vector<uint8_t> RenderTopDown(ID3D11Device* device, ID3D11DeviceContext* context, Renderer& renderer, ModelRenderer& models,
                                   DrawOptions options, ModelRenderer::DrawSettings settings, float x0, float z0, float spanX,
                                   float spanZ, float top, UINT width, UINT height, bool water);

/// Size of a minimap tile image, as the client's own.
constexpr UINT kMinimapSize = 256;

/// What a client minimap shows: terrain, buildings and water; no doodads (trees, rocks) and no editor outlines.
inline void MinimapLook(DrawOptions& options, ModelRenderer::DrawSettings& settings)
{
    options.showObjects = false;
    options.farTerrain = false;
    settings.doodads = false;
    settings.wmos = true;
}

/// A top-down render of one tile (RenderTopDown rows) turned into the client's minimap image layout (BLP pixels).
std::vector<uint8_t> MinimapFromTopDown(const std::vector<uint8_t>& rgba, UINT size);
