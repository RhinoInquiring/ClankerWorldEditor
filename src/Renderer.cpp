#include "Renderer.hpp"

#include "Loader.hpp"

#include "Mpq.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>

using namespace DirectX;

namespace
{
    const char* kTerrainShader = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; float4 lightDir; float4 params; float4 tint; float4 ambient; float4 diffuse; float4 fogColor; float4 fog; };
struct VsIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float4 col : COLOR; };
struct VsOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float3 col : COLOR; };
VsOut VsMain(VsIn i) { VsOut o; o.pos = mul(float4(i.pos, 1), viewProj); o.nrm = i.nrm; o.uv = i.uv; o.col = i.col.rgb * (255.0 / 127.0); return o; }

Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);
Texture2D alphaMap : register(t4);
SamplerState wrapS : register(s0); SamplerState clampS : register(s1);
float4 PsMain(VsOut i) : SV_TARGET
{
    float2 tuv = i.uv * params.x;
    float2 auv = i.uv * (63.0 / 64.0) + 0.5 / 64.0;
    float4 a = alphaMap.Sample(clampS, auv);
    float3 c = t0.Sample(wrapS, tuv).rgb;
    c = lerp(c, t1.Sample(wrapS, tuv).rgb, a.r);
    c = lerp(c, t2.Sample(wrapS, tuv).rgb, a.g);
    c = lerp(c, t3.Sample(wrapS, tuv).rgb, a.b);
    c *= i.col;   // vertex shading (MCCV): 0x7F = unchanged
    float d = saturate(dot(normalize(i.nrm), -lightDir.xyz));
    c *= ambient.w > 0.5 ? ambient.rgb + diffuse.rgb * d : 0.45 + 0.55 * d;   // the game's light, or even daylight
    if (params.y > 0.5) return float4(lerp(c, tint.rgb, 0.3), params.z);   // ghost: tinted, see-through
    if (fog.z > 0.5) c = lerp(c, fogColor.rgb, saturate((i.pos.w - fog.x) / max(fog.y - fog.x, 1)));
    return float4(c, 1);
}

// The sky: a full-screen triangle at the far plane; the view ray's height picks between the five sky colours. For this
// pass viewProj is the inverse view-projection without translation, and the five colours ride in params, tint,
// ambient, diffuse and fog (top to horizon); fogColor below the horizon.
struct SkyOut { float4 pos : SV_POSITION; float3 ray : TEXCOORD0; };
SkyOut VsSky(uint id : SV_VertexID)
{
    SkyOut o;
    float2 p = float2((id << 1) & 2, id & 2) * 2 - 1;
    o.pos = float4(p, 1, 1);
    float4 w = mul(float4(p, 1, 1), viewProj);
    o.ray = w.xyz / w.w;
    return o;
}
float4 PsSky(SkyOut i) : SV_TARGET
{
    float e = normalize(i.ray).y;   // 1 straight up, 0 at the horizon
    // ponytail: band heights eyeballed; the client's sky dome has its own fixed angles (and clouds, sun, skybox M2s).
    float3 c = e > 0.5 ? lerp(tint.rgb, params.rgb, (e - 0.5) / 0.5)
             : e > 0.25 ? lerp(ambient.rgb, tint.rgb, (e - 0.25) / 0.25)
             : e > 0.1 ? lerp(diffuse.rgb, ambient.rgb, (e - 0.1) / 0.15)
             : e > 0 ? lerp(fog.rgb, diffuse.rgb, e / 0.1)
             : lerp(fog.rgb, fogColor.rgb, saturate(-e * 8));
    return float4(c, 1);
}

// A far tile in one draw: its baked top-down picture, looked up by world position (tint.xy = tile corner x, z;
// tint.z = tile size).
struct BakedOut { float4 pos : SV_POSITION; float2 world : TEXCOORD0; };
BakedOut VsBaked(VsIn i) { BakedOut o; o.pos = mul(float4(i.pos, 1), viewProj); o.world = i.pos.xz; return o; }
Texture2D baked : register(t5);
float4 PsBaked(BakedOut i) : SV_TARGET { return float4(baked.Sample(clampS, (i.world - tint.xy) / tint.z).rgb, 1); }
)";

    const char* kLineShader = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; float4 lightDir; float4 params; float4 tint; float4 ambient; float4 diffuse; float4 fogColor; float4 fog; };
struct VsIn { float3 pos : POSITION; float4 col : COLOR; };
struct VsOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VsOut VsMain(VsIn i) { VsOut o; o.pos = mul(float4(i.pos, 1), viewProj); o.col = i.col; return o; }
float4 PsMain(VsOut i) : SV_TARGET
{
    if (params.y > 0.5) return float4(tint.rgb, max(params.z, 0.35));   // ghost: layer colour
    float4 c = i.col;
    if (fog.z > 0.5) c.rgb = lerp(c.rgb, fogColor.rgb, saturate((i.pos.w - fog.x) / max(fog.y - fog.x, 1)));   // far terrain only
    return c;
}
)";

    // Liquids: the colour pipeline's vertices (position, tint), textured with the liquid's animated frame; the
    // texture repeats every params.x yards across the surface.
    const char* kWaterShader = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; float4 lightDir; float4 params; float4 tint; float4 ambient; float4 diffuse; float4 fogColor; float4 fog; };
struct VsIn { float3 pos : POSITION; float4 col : COLOR; };
struct VsOut { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD0; };
VsOut VsMain(VsIn i) { VsOut o; o.pos = mul(float4(i.pos, 1), viewProj); o.col = i.col; o.uv = i.pos.xz / params.x; return o; }
Texture2D tex : register(t0);
SamplerState samp : register(s0);
// Water and ocean: the tint is the colour (the client takes it from its light settings), the grey texture adds the
// ripples. A white tint (magma, slime) shows the texture's own colours.
float4 PsMain(VsOut i) : SV_TARGET
{
    float4 t = tex.Sample(samp, i.uv);
    float3 c = dot(i.col.rgb, 1) > 2.9 ? t.rgb : i.col.rgb * (0.65 + 0.9 * dot(t.rgb, 0.3333));
    return float4(c, i.col.a);
}
)";

    // ambient.w: game lighting on; fog: start, end (yards from the eye), on.
    struct FrameConstants { XMFLOAT4X4 viewProj; XMFLOAT4 lightDir; XMFLOAT4 params; XMFLOAT4 tint; XMFLOAT4 ambient; XMFLOAT4 diffuse; XMFLOAT4 fogColor; XMFLOAT4 fog; };

    /// The scene light's part of the frame constants (left zero, editor lighting, when it is off).
    void LightConstants(const SceneLight& light, FrameConstants& fc)
    {
        if (!light.on) return;
        fc.ambient = { light.ambient.x, light.ambient.y, light.ambient.z, 1 };
        fc.diffuse = { light.diffuse.x, light.diffuse.y, light.diffuse.z, 0 };
        fc.fogColor = { light.fog.x, light.fog.y, light.fog.z, 1 };
        fc.fog = { light.fogStart, light.fogEnd, 1, 0 };
    }

    bool Compile(const char* src, const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& out, std::string& error)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        if (SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &out, &errors))) return true;
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compile failed";
        return false;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    /// Outer vertex (row 0..8, col 0..8) and inner vertex (row 0..7, col 0..7) indices into MCVT order.
    constexpr UINT Outer(UINT r, UINT c) { return r * 17 + c; }
    constexpr UINT Inner(UINT r, UINT c) { return r * 17 + 9 + c; }

    void AddBox(std::vector<LineVertex>& out, XMFLOAT3 lo, XMFLOAT3 hi, XMFLOAT4 color)
    {
        const XMFLOAT3 p[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { lo.x, lo.y, hi.z },
                                { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
        const int edges[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
                                   { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& e : edges)
        {
            out.push_back({ p[e[0]], color });
            out.push_back({ p[e[1]], color });
        }
    }
}

bool Renderer::Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error)
{
    m_device = device;
    m_context = context;

    Com<ID3DBlob> vs, ps, lvs, lps;
    Microsoft::WRL::ComPtr<ID3DBlob> bakedVs, bakedPs;
    if (!Compile(kTerrainShader, "VsBaked", "vs_5_0", bakedVs, error) || !Compile(kTerrainShader, "PsBaked", "ps_5_0", bakedPs, error)) return false;
    device->CreateVertexShader(bakedVs->GetBufferPointer(), bakedVs->GetBufferSize(), nullptr, &m_bakedVs);
    device->CreatePixelShader(bakedPs->GetBufferPointer(), bakedPs->GetBufferSize(), nullptr, &m_bakedPs);
    if (!Compile(kTerrainShader, "VsMain", "vs_5_0", vs, error) || !Compile(kTerrainShader, "PsMain", "ps_5_0", ps, error) ||
        !Compile(kLineShader, "VsMain", "vs_5_0", lvs, error) || !Compile(kLineShader, "PsMain", "ps_5_0", lps, error))
        return false;
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_terrainVs);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_terrainPs);
    device->CreateVertexShader(lvs->GetBufferPointer(), lvs->GetBufferSize(), nullptr, &m_lineVs);
    device->CreatePixelShader(lps->GetBufferPointer(), lps->GetBufferSize(), nullptr, &m_linePs);
    Com<ID3DBlob> wvs, wps;
    if (!Compile(kWaterShader, "VsMain", "vs_5_0", wvs, error) || !Compile(kWaterShader, "PsMain", "ps_5_0", wps, error)) return false;
    device->CreateVertexShader(wvs->GetBufferPointer(), wvs->GetBufferSize(), nullptr, &m_waterVs);
    device->CreatePixelShader(wps->GetBufferPointer(), wps->GetBufferSize(), nullptr, &m_waterPs);
    Com<ID3DBlob> svs, sps;
    if (!Compile(kTerrainShader, "VsSky", "vs_5_0", svs, error) || !Compile(kTerrainShader, "PsSky", "ps_5_0", sps, error)) return false;
    device->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, &m_skyVs);
    device->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, &m_skyPs);
    m_start = std::chrono::steady_clock::now();

    const D3D11_INPUT_ELEMENT_DESC terrain[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    const D3D11_INPUT_ELEMENT_DESC line[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    device->CreateInputLayout(terrain, 4, vs->GetBufferPointer(), vs->GetBufferSize(), &m_terrainLayout);
    device->CreateInputLayout(line, 2, lvs->GetBufferPointer(), lvs->GetBufferSize(), &m_lineLayout);

    D3D11_BUFFER_DESC cb{ sizeof(FrameConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    device->CreateBuffer(&cb, nullptr, &m_frameCb);

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_ANISOTROPIC;
    s.MaxAnisotropy = 8;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&s, &m_wrap);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&s, &m_clamp);

    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&r, &m_solid);
    r.FillMode = D3D11_FILL_WIREFRAME;
    device->CreateRasterizerState(&r, &m_wire);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&ds, &m_depth);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&ds, &m_ghostDepth);   // tested against the map, writes nothing
    ds.DepthEnable = FALSE;
    device->CreateDepthStencilState(&ds, &m_noDepth);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &m_ghostBlend);

    BlpImage white;
    white.width = white.height = 1;
    white.mips.push_back({ 255, 255, 255, 255 });
    m_white = CreateTexture(white);
    return m_terrainVs && m_terrainPs && m_lineVs && m_linePs && m_white;
}

Renderer::Com<ID3D11ShaderResourceView> Renderer::CreateTexture(const BlpImage& image)
{
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT blockBytes = 0;
    switch (image.format)
    {
    case BlpImage::Format::BC1: format = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; break;
    case BlpImage::Format::BC2: format = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; break;
    case BlpImage::Format::BC3: format = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; break;
    case BlpImage::Format::RGBA8: break;
    }
    if (blockBytes && (image.width % 4 || image.height % 4)) return nullptr;   // BC top level must be whole blocks

    std::vector<D3D11_SUBRESOURCE_DATA> data;
    for (size_t i = 0; i < image.mips.size(); ++i)
    {
        const UINT w = std::max(1u, image.width >> i);
        const UINT pitch = blockBytes ? std::max(1u, (w + 3) / 4) * blockBytes : w * 4;
        data.push_back({ image.mips[i].data(), pitch, 0 });
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = UINT(data.size());
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    if (FAILED(m_device->CreateTexture2D(&desc, data.data(), &tex)) ||
        FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return nullptr;
    return srv;
}

ID3D11ShaderResourceView* Renderer::Texture(const std::string& name, const MpqChain& mpq, TileStats& stats)
{
    const std::string key = Lower(name);
    if (auto it = m_textureCache.find(key); it != m_textureCache.end())
    {
        if (m_loader) m_loader->TakeImage(key);   // a background copy of a texture already here: drop it
        return it->second ? it->second.Get() : m_white.Get();
    }

    Com<ID3D11ShaderResourceView> srv;
    if (auto image = m_loader ? m_loader->TakeImage(key) : std::nullopt) srv = CreateTexture(*image);
    else if (auto bytes = mpq.Read(name))
        if (auto image = ParseBlp(*bytes)) srv = CreateTexture(*image);
    srv ? ++stats.texturesLoaded : ++stats.texturesMissing;
    m_textureCache[key] = srv;
    return srv ? srv.Get() : m_white.Get();
}

void Renderer::BuildWater(TileGpu& tile, const std::vector<AdtLiquid>& liquids, const MpqChain& mpq)
{
    tile.water.Reset();
    tile.waterVertexCount = 0;
    tile.waterRuns.clear();
    // Liquids: two triangles per existing cell, grouped by LiquidType (each type draws with its own animated
    // texture), tinted by kind.
    // ponytail: kind from the stock 3.3.5 LiquidType ids; read LiquidType.dbc's type column when custom liquids appear.
    std::map<uint16_t, std::vector<LineVertex>> byType;
    for (const AdtLiquid& l : liquids)
    {
        const uint16_t t = l.type;
        const bool magma = t == 3 || t == 7 || t == 11 || t == 19 || t == 121 || t == 141;
        const bool slime = t == 4 || t == 8 || t == 12 || t == 20 || t == 21 || t == 181;
        const bool ocean = l.format == 2 || t == 2 || t == 6 || t == 10 || t == 14;
        const XMFLOAT4 col = magma ? XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f } : slime ? XMFLOAT4{ 1.0f, 1.0f, 1.0f, 0.9f }
                           : ocean ? XMFLOAT4{ 0.12f, 0.27f, 0.42f, 0.8f } : XMFLOAT4{ 0.2f, 0.38f, 0.46f, 0.72f };   // Elwynn-like lake and open-sea blues
        LiquidFrames(t, mpq);   // load its frames now, not in the middle of a draw
        std::vector<LineVertex>& water = byType[t];
        auto vertex = [&](int r, int c) {
            return LineVertex{ { l.cornerX + (l.x + c) * kUnitSize, l.heights[size_t(r) * (l.w + 1) + c], l.cornerZ + (l.y + r) * kUnitSize }, col };
        };
        for (int r = 0; r < l.h; ++r)
            for (int c = 0; c < l.w; ++c)
            {
                if (!l.exists[size_t(r) * l.w + c]) continue;
                const LineVertex a = vertex(r, c), b = vertex(r, c + 1), d = vertex(r + 1, c), e = vertex(r + 1, c + 1);
                water.insert(water.end(), { a, b, e, a, e, d });
            }
    }
    std::vector<LineVertex> water;
    for (auto& [type, vertices] : byType)
    {
        if (vertices.empty()) continue;
        tile.waterRuns.push_back({ type, UINT(water.size()), UINT(vertices.size()) });
        water.insert(water.end(), vertices.begin(), vertices.end());
    }
    if (!water.empty())   // ghost tiles too: DrawWater shows only the layer on screen (the map, or the soloed ghost)
    {
        D3D11_BUFFER_DESC wb{ UINT(water.size() * sizeof(LineVertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
        D3D11_SUBRESOURCE_DATA wd{ water.data() };
        m_device->CreateBuffer(&wb, &wd, &tile.water);
        tile.waterVertexCount = UINT(water.size());
    }

}

void Renderer::UpdateWater(int key, const std::vector<AdtLiquid>& liquids, const MpqChain& mpq)
{
    if (auto it = m_tiles.find(key); it != m_tiles.end()) BuildWater(it->second, liquids, mpq);
}

TileStats Renderer::LoadTile(int key, const Adt& adt, const MpqChain& mpq, int layer)
{
    TileStats stats;
    TileGpu tile;
    tile.layer = layer;
    tile.cpuVertices.assign(adt.chunks.size() * 145, {});
    stats.minHeight = 1e9f;
    stats.maxHeight = -1e9f;

    for (size_t ci = 0; ci < adt.chunks.size(); ++ci)
    {
        const auto shown = layer == 0 && m_chunkView ? m_chunkView(key, ci, adt.chunks[ci], adt.textures, true) : std::nullopt;
        const AdtChunk& c = shown ? shown->chunk : adt.chunks[ci];
        const std::vector<std::string>& textures = shown ? shown->textures : adt.textures;
        for (float h : c.heights)
        {
            stats.minHeight = std::min(stats.minHeight, c.baseY + h);
            stats.maxHeight = std::max(stats.maxHeight, c.baseY + h);
        }

        ChunkDraw draw;
        tile.holes.push_back(c.holes);

        for (UINT l = 0; l < 4; ++l)
            draw.textures[l] = l < c.layerCount && c.textureIds[l] < textures.size()
                ? Texture(textures[c.textureIds[l]], mpq, stats) : m_white.Get();
        BlpImage alpha;
        alpha.width = alpha.height = 64;
        alpha.mips.push_back(c.alpha);
        draw.alpha = CreateTexture(alpha);
        tile.chunks.push_back(std::move(draw));
        BuildChunkVertices(tile, ci, c);
    }

    if (!tile.cpuVertices.empty())
    {
        D3D11_BUFFER_DESC vb{ UINT(tile.cpuVertices.size() * sizeof(TerrainVertex)), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER };
        D3D11_SUBRESOURCE_DATA vd{ tile.cpuVertices.data() };
        m_device->CreateBuffer(&vb, &vd, &tile.vertices);
        BuildIndices(tile);
    }

    // Placement markers: a 4 yd box (times scale) per doodad, the stored extents per WMO.
    std::vector<LineVertex> lines;
    for (const auto& d : adt.doodads)
    {
        const float h = 2.0f * d.scale;
        AddBox(lines, { d.pos[0] - h, d.pos[1], d.pos[2] - h }, { d.pos[0] + h, d.pos[1] + 2 * h, d.pos[2] + h }, { 1, 0.85f, 0.2f, 1 });
    }
    for (const auto& w : adt.wmos)
        AddBox(lines, { w.extMin[0], w.extMin[1], w.extMin[2] }, { w.extMax[0], w.extMax[1], w.extMax[2] }, { 0.3f, 0.9f, 1, 1 });
    if (!lines.empty())
    {
        D3D11_BUFFER_DESC lb{ UINT(lines.size() * sizeof(LineVertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
        D3D11_SUBRESOURCE_DATA ld{ lines.data() };
        m_device->CreateBuffer(&lb, &ld, &tile.lines);
        tile.lineVertexCount = UINT(lines.size());
    }

    BuildWater(tile, adt.liquids, mpq);

    if (!adt.chunks.empty())
    {
        tile.boundsMin = { adt.chunks.front().baseX, stats.minHeight, adt.chunks.front().baseZ };
        tile.boundsMax = { tile.boundsMin.x, stats.maxHeight, tile.boundsMin.z };
        for (const AdtChunk& c : adt.chunks)
        {
            tile.boundsMin.x = std::min(tile.boundsMin.x, c.baseX);
            tile.boundsMin.z = std::min(tile.boundsMin.z, c.baseZ);
            tile.boundsMax.x = std::max(tile.boundsMax.x, c.baseX + kChunkSize);
            tile.boundsMax.z = std::max(tile.boundsMax.z, c.baseZ + kChunkSize);
        }
    }

    m_tiles[key] = std::move(tile);
    stats.chunks = adt.chunks.size();
    stats.doodads = adt.doodads.size();
    stats.wmos = adt.wmos.size();
    return stats;
}

void Renderer::BuildIndices(TileGpu& tile)
{
    // Three levels per chunk, each level contiguous over the whole tile (so a far tile is one draw call).
    std::vector<uint32_t> indices;
    for (int lod = 0; lod < kLods; ++lod)
    {
        tile.lodStart[size_t(lod)] = UINT(indices.size());
        for (size_t ci = 0; ci < tile.chunks.size(); ++ci)
        {
            ChunkDraw& draw = tile.chunks[ci];
            const UINT base = UINT(ci * 145);
            draw.indexStart[size_t(lod)] = UINT(indices.size());
            auto o = [&](UINT r, UINT c) { return base + Outer(r, c); };
            if (lod == 0 || tile.holes[ci])   // holes: full detail at every level (the cut-outs need the fine grid)
            {
                for (UINT r = 0; r < 8; ++r)
                    for (UINT col = 0; col < 8; ++col)
                    {
                        if (tile.holes[ci] & (1u << ((r / 2) * 4 + col / 2))) continue;
                        const UINT tl = o(r, col), tr = o(r, col + 1), bl = o(r + 1, col), br = o(r + 1, col + 1), m = base + Inner(r, col);
                        const uint32_t tris[12] = { tl, tr, m, tr, br, m, br, bl, m, bl, tl, m };
                        indices.insert(indices.end(), tris, tris + 12);
                    }
            }
            else if (lod == 1)   // the 9 x 9 outer grid, two triangles a cell
            {
                for (UINT r = 0; r < 8; ++r)
                    for (UINT col = 0; col < 8; ++col)
                        indices.insert(indices.end(), { o(r, col), o(r, col + 1), o(r + 1, col + 1), o(r, col), o(r + 1, col + 1), o(r + 1, col) });
            }
            else   // 4 x 4 quads of 2 x 2 cells, fanned from their centre vertex; on the chunk edge every vertex is kept
            {
                for (UINT r0 = 0; r0 < 8; r0 += 2)
                    for (UINT c0 = 0; c0 < 8; c0 += 2)
                    {
                        std::vector<UINT> ring{ o(r0, c0) };
                        if (r0 == 0) ring.push_back(o(r0, c0 + 1));
                        ring.push_back(o(r0, c0 + 2));
                        if (c0 + 2 == 8) ring.push_back(o(r0 + 1, c0 + 2));
                        ring.push_back(o(r0 + 2, c0 + 2));
                        if (r0 + 2 == 8) ring.push_back(o(r0 + 2, c0 + 1));
                        ring.push_back(o(r0 + 2, c0));
                        if (c0 == 0) ring.push_back(o(r0 + 1, c0));
                        const UINT centre = o(r0 + 1, c0 + 1);
                        for (size_t k = 0; k < ring.size(); ++k) indices.insert(indices.end(), { ring[k], ring[(k + 1) % ring.size()], centre });
                    }
            }
            draw.indexCount[size_t(lod)] = UINT(indices.size()) - draw.indexStart[size_t(lod)];
        }
        tile.lodCount[size_t(lod)] = UINT(indices.size()) - tile.lodStart[size_t(lod)];
    }
    tile.indices.Reset();
    if (indices.empty()) return;
    D3D11_BUFFER_DESC ib{ UINT(indices.size() * sizeof(uint32_t)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
    D3D11_SUBRESOURCE_DATA id{ indices.data() };
    m_device->CreateBuffer(&ib, &id, &tile.indices);
}

void Renderer::SetChunkHoles(int key, size_t index, uint16_t holes)
{
    auto it = m_tiles.find(key);
    if (it == m_tiles.end() || index >= it->second.holes.size() || it->second.holes[index] == holes) return;
    it->second.holes[index] = holes;
    it->second.bakeDirty = true;
    BuildIndices(it->second);
}

void Renderer::BuildChunkVertices(TileGpu& tile, size_t index, const AdtChunk& c)
{
    TerrainVertex* v = tile.cpuVertices.data() + index * 145;
    for (UINT j = 0; j < 145; ++j)
    {
        const UINT row = j / 17, col = j % 17;
        const bool inner = col >= 9;
        const float x = inner ? (col - 9 + 0.5f) : float(col);
        const float z = inner ? (row + 0.5f) : float(row);
        // MCCV is B, G, R, A; the layout reads R first.
        const uint8_t* m = c.colors.size() >= 145 * 4 ? c.colors.data() + j * 4 : nullptr;
        const uint32_t rgba = m ? uint32_t(m[2]) | uint32_t(m[1]) << 8 | uint32_t(m[0]) << 16 | 0xFF000000u : 0xFF7F7F7Fu;
        v[j] = { { c.baseX + x * kUnitSize, c.baseY + c.heights[j], c.baseZ + z * kUnitSize }, { 0, 0, 0 }, { x / 8.0f, z / 8.0f }, rgba };
    }

    // Smooth normals from every cell, holes included, so lighting does not change at a hole's rim.
    for (UINT r = 0; r < 8; ++r)
        for (UINT col = 0; col < 8; ++col)
        {
            const UINT ring[4] = { Outer(r, col), Outer(r, col + 1), Outer(r + 1, col + 1), Outer(r + 1, col) };
            const UINT m = Inner(r, col);
            for (int k = 0; k < 4; ++k)
            {
                const UINT a = ring[k], b = ring[(k + 1) % 4];
                XMVECTOR pa = XMLoadFloat3(&v[a].pos), pb = XMLoadFloat3(&v[b].pos), pm = XMLoadFloat3(&v[m].pos);
                XMVECTOR n = XMVector3Cross(XMVectorSubtract(pb, pa), XMVectorSubtract(pm, pa));
                if (XMVectorGetY(n) < 0) n = XMVectorNegate(n);
                for (UINT idx : { a, b, m }) XMStoreFloat3(&v[idx].nrm, XMVectorAdd(XMLoadFloat3(&v[idx].nrm), n));
            }
        }
    for (UINT j = 0; j < 145; ++j) XMStoreFloat3(&v[j].nrm, XMVector3Normalize(XMLoadFloat3(&v[j].nrm)));
}

void Renderer::UpdateChunk(int key, size_t index, const AdtChunk& chunk)
{
    auto it = m_tiles.find(key);
    if (it == m_tiles.end() || !it->second.vertices || index >= it->second.chunks.size()) return;
    TileGpu& tile = it->second;
    tile.bakeDirty = true;
    const auto shown = tile.layer == 0 && m_chunkView ? m_chunkView(key, index, chunk, {}, false) : std::nullopt;
    BuildChunkVertices(tile, index, shown ? shown->chunk : chunk);
    const UINT bytes = 145 * sizeof(TerrainVertex);
    const D3D11_BOX box{ UINT(index) * bytes, 0, 0, UINT(index + 1) * bytes, 1, 1 };
    m_context->UpdateSubresource(tile.vertices.Get(), 0, &box, tile.cpuVertices.data() + index * 145, 0, 0);
    for (UINT j = 0; j < 145; ++j)
    {
        tile.boundsMin.y = std::min(tile.boundsMin.y, tile.cpuVertices[index * 145 + j].pos.y);
        tile.boundsMax.y = std::max(tile.boundsMax.y, tile.cpuVertices[index * 145 + j].pos.y);
    }
}

void Renderer::UpdateChunkTextures(int key, size_t index, const AdtChunk& c, const std::vector<std::string>& textures, const MpqChain& mpq)
{
    auto it = m_tiles.find(key);
    if (it == m_tiles.end() || index >= it->second.chunks.size()) return;
    if (it->second.layer == 0 && m_chunkView)
        if (const auto shown = m_chunkView(key, index, c, textures, true))
        {
            const ChunkView view = std::exchange(m_chunkView, nullptr);   // once: the shown chunk is final
            UpdateChunkTextures(key, index, shown->chunk, shown->textures, mpq);
            m_chunkView = view;
            return;
        }
    ChunkDraw& draw = it->second.chunks[index];
    it->second.bakeDirty = true;
    TileStats ignored;
    for (UINT l = 0; l < 4; ++l)
        draw.textures[l] = l < c.layerCount && c.textureIds[l] < textures.size() ? Texture(textures[c.textureIds[l]], mpq, ignored) : m_white.Get();
    BlpImage alpha;
    alpha.width = alpha.height = 64;
    alpha.mips.push_back(c.alpha);
    draw.alpha = CreateTexture(alpha);
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Renderer::CreateRgbaTexture(UINT width, UINT height, const std::vector<uint8_t>& rgba)
{
    if (rgba.size() < size_t(width) * height * 4) return nullptr;
    BlpImage image;
    image.width = width;
    image.height = height;
    image.mips.push_back(rgba);
    return CreateTexture(image);
}

ID3D11ShaderResourceView* Renderer::CachedTexture(const std::string& name) const
{
    auto it = m_textureCache.find(Lower(name));
    if (it == m_textureCache.end()) return nullptr;
    return it->second ? it->second.Get() : m_white.Get();
}

void Renderer::CacheTexture(const std::string& name, const BlpImage& image)
{
    const std::string key = Lower(name);
    if (!m_textureCache.count(key)) m_textureCache[key] = CreateTexture(image);
}

ID3D11ShaderResourceView* Renderer::TextureFor(const std::string& name, const MpqChain& mpq)
{
    if (name.empty()) return m_white.Get();
    TileStats ignored;
    return Texture(name, mpq, ignored);
}

void Renderer::BakeTile(TileGpu& tile, const DrawOptions& options)
{
    constexpr UINT kSize = 512;   // 1 texel ~ 1 yd: plenty at the distance a baked tile is drawn
    if (!tile.vertices || !tile.indices || tile.chunks.empty()) return;
    if (!tile.baked)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kSize;
        d.MipLevels = 0;   // full chain, filled by GenerateMips
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        Com<ID3D11Texture2D> texture;
        if (FAILED(m_device->CreateTexture2D(&d, nullptr, &texture))) return;
        m_device->CreateShaderResourceView(texture.Get(), nullptr, &tile.baked);
        m_device->CreateRenderTargetView(texture.Get(), nullptr, &tile.bakedRtv);
    }
    if (!m_bakeDepth)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kSize;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Com<ID3D11Texture2D> depth;
        if (FAILED(m_device->CreateTexture2D(&d, nullptr, &depth))) return;
        m_device->CreateDepthStencilView(depth.Get(), nullptr, &m_bakeDepth);
    }

    // Keep the caller's target: baking happens in the middle of drawing the scene.
    Com<ID3D11RenderTargetView> oldRtv;
    Com<ID3D11DepthStencilView> oldDsv;
    m_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    UINT viewports = 1;
    D3D11_VIEWPORT oldVp{};
    m_context->RSGetViewports(&viewports, &oldVp);

    // Straight down: x across the tile -> u, z -> v (the baked shader looks it up the same way); higher ground nearer.
    const float x0 = tile.boundsMin.x, z0 = tile.boundsMin.z, t = kTileSize;
    const float top = tile.boundsMax.y + 1, height = std::max(1.0f, tile.boundsMax.y - tile.boundsMin.y + 2);
    const XMMATRIX ortho(2 / t, 0, 0, 0,
                         0, 0, -1 / height, 0,
                         0, -2 / t, 0, 0,
                         -2 * x0 / t - 1, 1 + 2 * z0 / t, top / height, 1);
    FrameConstants fc{};
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(ortho));
    XMStoreFloat4(&fc.lightDir, XMVector3Normalize(XMVectorSet(-0.4f, -1.0f, -0.3f, 0)));
    fc.params = { options.textureRepeat, 0, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    const float clear[4] = { 0, 0, 0, 1 };
    m_context->ClearRenderTargetView(tile.bakedRtv.Get(), clear);
    m_context->ClearDepthStencilView(m_bakeDepth.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    m_context->OMSetRenderTargets(1, tile.bakedRtv.GetAddressOf(), m_bakeDepth.Get());
    const D3D11_VIEWPORT vp{ 0, 0, float(kSize), float(kSize), 0, 1 };
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_context->OMSetDepthStencilState(m_depth.Get(), 0);
    m_context->RSSetState(m_solid.Get());
    const UINT stride = sizeof(TerrainVertex), offset = 0;
    m_context->IASetInputLayout(m_terrainLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetVertexBuffers(0, 1, tile.vertices.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(tile.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_context->VSSetShader(m_terrainVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_terrainPs.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    ID3D11SamplerState* samplers[] = { m_wrap.Get(), m_clamp.Get() };
    m_context->PSSetSamplers(0, 2, samplers);
    for (const ChunkDraw& c : tile.chunks)
    {
        if (!c.indexCount[0]) continue;
        ID3D11ShaderResourceView* srvs[5] = { c.textures[0], c.textures[1], c.textures[2], c.textures[3], c.alpha.Get() };
        m_context->PSSetShaderResources(0, 5, srvs);
        m_context->DrawIndexed(c.indexCount[0], c.indexStart[0], 0);
    }
    ID3D11RenderTargetView* none = nullptr;
    m_context->OMSetRenderTargets(1, &none, nullptr);
    m_context->GenerateMips(tile.baked.Get());
    m_context->OMSetRenderTargets(1, oldRtv.GetAddressOf(), oldDsv.Get());
    m_context->RSSetViewports(1, &oldVp);
    tile.bakeDirty = false;
}

void Renderer::Draw(FXMMATRIX viewProj, const DrawOptions& options)
{
    FrameConstants fc{};
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4(&fc.lightDir, XMVector3Normalize(XMVectorSet(-0.4f, -1.0f, -0.3f, 0)));
    fc.params = { options.textureRepeat, 0, 0, 0 };
    LightConstants(m_light, fc);
    auto upload = [&](float tinted, float opacity = 1.0f, XMFLOAT4 tint = {}) {
        fc.params.y = tinted;
        fc.params.z = opacity;
        fc.tint = tint;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, &fc, sizeof fc);
            m_context->Unmap(m_frameCb.Get(), 0);
        }
    };
    upload(0);

    // Tiles outside the view are skipped whole: test each tile's box against the clip-space volume.
    auto visible = [&](const TileGpu& t) {
        const XMFLOAT3& a = t.boundsMin;
        const XMFLOAT3& b = t.boundsMax;
        int outside[6] = {};
        for (int i = 0; i < 8; ++i)
        {
            const XMVECTOR p = XMVector4Transform(XMVectorSet(i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z, 1), viewProj);
            const float x = XMVectorGetX(p), y = XMVectorGetY(p), z = XMVectorGetZ(p), w = XMVectorGetW(p);
            outside[0] += x < -w; outside[1] += x > w; outside[2] += y < -w; outside[3] += y > w; outside[4] += z < 0; outside[5] += z > w;
        }
        for (int o : outside)
            if (o == 8) return false;
        return true;
    };

    // Distance from the camera picks the detail: chunks by their centre vertex, whole tiles by their box.
    auto ChunkLod = [&](const TileGpu& tile, size_t ci) -> size_t {
        const XMFLOAT3& p = tile.cpuVertices[ci * 145 + Outer(4, 4)].pos;
        const float dx = p.x - options.eye.x, dy = p.y - options.eye.y, dz = p.z - options.eye.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        return d2 < 200.0f * 200.0f ? 0 : d2 < 550.0f * 550.0f ? 1 : 2;
    };
    auto tileDistance = [&](const TileGpu& tile) {
        const float dx = std::max({ tile.boundsMin.x - options.eye.x, 0.0f, options.eye.x - tile.boundsMax.x });
        const float dz = std::max({ tile.boundsMin.z - options.eye.z, 0.0f, options.eye.z - tile.boundsMax.z });
        return std::sqrt(dx * dx + dz * dz);
    };
    // Far tiles of the map (not ghosts, nothing soloed) draw whole from their baked picture; a few bakes a frame.
    int bakes = 0;
    std::vector<int> wholeTiles;
    if (options.lod && !options.solo && !options.wireframe)
        for (auto& [key, tile] : m_tiles)
        {
            if (tile.layer != 0 || tileDistance(tile) < 800.0f || !visible(tile)) continue;
            if (tile.bakeDirty && bakes < 2) { BakeTile(tile, options); ++bakes; }
            if (!tile.bakeDirty) wholeTiles.push_back(key);
        }
    upload(0);

    m_context->OMSetDepthStencilState(m_depth.Get(), 0);
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    ID3D11SamplerState* samplers[] = { m_wrap.Get(), m_clamp.Get() };

    if (!wholeTiles.empty())
    {
        const UINT stride = sizeof(TerrainVertex), offset = 0;
        m_context->RSSetState(m_solid.Get());
        m_context->IASetInputLayout(m_terrainLayout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_bakedVs.Get(), nullptr, 0);
        m_context->PSSetShader(m_bakedPs.Get(), nullptr, 0);
        m_context->PSSetSamplers(0, 2, samplers);
        for (int key : wholeTiles)
        {
            const TileGpu& tile = m_tiles.at(key);
            upload(0, 1.0f, { tile.boundsMin.x, tile.boundsMin.z, kTileSize, 0 });   // the baked shader's lookup
            ID3D11ShaderResourceView* srv = tile.baked.Get();
            m_context->PSSetShaderResources(5, 1, &srv);
            m_context->IASetVertexBuffers(0, 1, tile.vertices.GetAddressOf(), &stride, &offset);
            m_context->IASetIndexBuffer(tile.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            m_context->DrawIndexed(tile.lodCount[2], tile.lodStart[2], 0);
        }
        upload(0);
    }
    auto isFar = [&](int key) { return std::find(wholeTiles.begin(), wholeTiles.end(), key) != wholeTiles.end(); };

    auto drawTile = [&](int key, const TileGpu& tile) {
        if (tile.vertices && tile.indices && !isFar(key))
        {
            const UINT stride = sizeof(TerrainVertex), offset = 0;
            m_context->RSSetState(options.wireframe ? m_wire.Get() : m_solid.Get());
            m_context->IASetInputLayout(m_terrainLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetVertexBuffers(0, 1, tile.vertices.GetAddressOf(), &stride, &offset);
            m_context->IASetIndexBuffer(tile.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            m_context->VSSetShader(m_terrainVs.Get(), nullptr, 0);
            m_context->PSSetShader(m_terrainPs.Get(), nullptr, 0);
            m_context->PSSetSamplers(0, 2, samplers);
            for (size_t ci = 0; ci < tile.chunks.size(); ++ci)
            {
                const ChunkDraw& c = tile.chunks[ci];
                const size_t lod = options.lod ? ChunkLod(tile, ci) : 0;
                if (!c.indexCount[lod]) continue;
                ID3D11ShaderResourceView* srvs[5] = { c.textures[0], c.textures[1], c.textures[2], c.textures[3], c.alpha.Get() };
                m_context->PSSetShaderResources(0, 5, srvs);
                m_context->DrawIndexed(c.indexCount[lod], c.indexStart[lod], 0);
            }
        }

        if ((options.showObjects || (tile.layer > 0 && tile.layer != options.solo)) && tile.lines)   // ghosts outline their objects (solo shows the models)
        {
            const UINT stride = sizeof(LineVertex), offset = 0;
            m_context->RSSetState(m_solid.Get());
            m_context->IASetInputLayout(m_lineLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            m_context->IASetVertexBuffers(0, 1, tile.lines.GetAddressOf(), &stride, &offset);
            m_context->VSSetShader(m_lineVs.Get(), nullptr, 0);
            m_context->PSSetShader(m_linePs.Get(), nullptr, 0);
            m_context->Draw(tile.lineVertexCount, 0);
        }
    };
    // Solo: one ghost layer drawn as the map, nothing else.
    for (const auto& [key, tile] : m_tiles)
        if (tile.layer == options.solo && visible(tile)) drawTile(key, tile);
    if (options.solo) return;

    // Ghosts over the finished map, blended so the map shows through: a faint pass ignoring depth, then a
    // clearer depth-tested pass, so what sits above the ground reads strongest.
    for (const auto& [key, tile] : m_tiles)
    {
        if (tile.layer == 0 || !visible(tile)) continue;
        const LayerStyle style = m_layers.count(tile.layer) ? m_layers.at(tile.layer) : LayerStyle{};
        if (!style.visible) continue;
        m_context->OMSetBlendState(m_ghostBlend.Get(), nullptr, 0xffffffff);
        upload(1, style.tint.w * 0.3f, style.tint);   // faint: the parts below the current ground, seen through it
        m_context->OMSetDepthStencilState(m_noDepth.Get(), 0);
        drawTile(key, tile);
        upload(1, style.tint.w, style.tint);          // clear: the parts above the ground
        m_context->OMSetDepthStencilState(m_ghostDepth.Get(), 0);
        drawTile(key, tile);
        m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        m_context->OMSetDepthStencilState(m_depth.Get(), 0);
        upload(0);
    }

}

void Renderer::DrawWater(FXMMATRIX viewProj, int solo)
{
    FrameConstants fc{};
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(viewProj));
    fc.params = { kUnitSize * 2, 0, 0, 0 };   // the liquid texture repeats every two vertex steps (8.3 yd)
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    const UINT stride = sizeof(LineVertex), offset = 0;
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->OMSetBlendState(m_ghostBlend.Get(), nullptr, 0xffffffff);
    m_context->OMSetDepthStencilState(m_ghostDepth.Get(), 0);
    m_context->RSSetState(m_solid.Get());
    m_context->IASetInputLayout(m_lineLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_waterVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_waterPs.Get(), nullptr, 0);
    m_context->PSSetSamplers(0, 1, m_wrap.GetAddressOf());
    for (const auto& [key, tile] : m_tiles)
    {
        if (!tile.water || tile.layer != solo) continue;
        m_context->IASetVertexBuffers(0, 1, tile.water.GetAddressOf(), &stride, &offset);
        for (const auto& run : tile.waterRuns)
        {
            // The liquid's current frame; a type without frames draws its tint over white.
            const auto frames = m_liquidFrames.find(run.type);
            ID3D11ShaderResourceView* srv = frames != m_liquidFrames.end() && !frames->second.empty()
                                                ? frames->second[LiquidFrame(frames->second.size())] : m_white.Get();
            m_context->PSSetShaderResources(0, 1, &srv);
            m_context->Draw(run.count, run.first);
        }
    }
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_context->OMSetDepthStencilState(m_depth.Get(), 0);
}

void Renderer::LoadFar(const std::vector<std::vector<int16_t>>& tiles)
{
    ClearFar();
    // One 17 x 17 grid per tile (16 x 16 quads), lit on the CPU with the terrain's light and coloured by slope and
    // height, in the overlay vertex format so the colour pipeline draws it.
    std::vector<LineVertex> vertices;
    std::vector<uint32_t> indices;
    const XMVECTOR light = XMVector3Normalize(XMVectorSet(0.4f, 1.0f, 0.3f, 0));   // towards the light
    const float step = kTileSize / 16;
    for (int key = 0; key < int(tiles.size()); ++key)
    {
        const auto& h = tiles[size_t(key)];
        if (h.size() < 289) continue;
        const int tx = key % 64, ty = key / 64;
        const UINT base = UINT(vertices.size());
        auto at = [&](int r, int c) { return float(h[size_t(std::clamp(r, 0, 16) * 17 + std::clamp(c, 0, 16))]); };
        for (int r = 0; r <= 16; ++r)
            for (int c = 0; c <= 16; ++c)
            {
                const float y = at(r, c);
                const XMVECTOR n = XMVector3Normalize(XMVectorSet(at(r, c - 1) - at(r, c + 1), 2 * step, at(r - 1, c) - at(r + 1, c), 0));
                const float lit = 0.45f + 0.55f * std::max(0.0f, XMVectorGetX(XMVector3Dot(n, light)));
                const float steep = std::clamp((1.0f - XMVectorGetY(n)) * 4.0f, 0.0f, 1.0f);
                // Earth on flat ground, rock on slopes, snow up high, sea floor blue below sea level.
                XMFLOAT3 col{ 0.44f + 0.04f * steep, 0.40f + 0.04f * steep, 0.28f + 0.14f * steep };
                if (y > 450) { const float s = std::min(1.0f, (y - 450) / 250); col = { col.x + (0.9f - col.x) * s, col.y + (0.92f - col.y) * s, col.z + (0.95f - col.z) * s }; }
                if (y < 0) col = { 0.16f, 0.28f, 0.40f };
                vertices.push_back({ { tx * kTileSize + c * step, y, ty * kTileSize + r * step }, { col.x * lit, col.y * lit, col.z * lit, 1 } });
            }
        m_farTiles.push_back({ key, UINT(indices.size()) });
        for (UINT r = 0; r < 16; ++r)
            for (UINT c = 0; c < 16; ++c)
            {
                const UINT a = base + r * 17 + c, b = a + 1, d = a + 17, e = d + 1;
                indices.insert(indices.end(), { a, b, e, a, e, d });
            }
    }
    if (vertices.empty()) return;
    D3D11_BUFFER_DESC vb{ UINT(vertices.size() * sizeof(LineVertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA vd{ vertices.data() };
    D3D11_BUFFER_DESC ib{ UINT(indices.size() * sizeof(uint32_t)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
    D3D11_SUBRESOURCE_DATA id{ indices.data() };
    if (FAILED(m_device->CreateBuffer(&vb, &vd, &m_farVertices)) || FAILED(m_device->CreateBuffer(&ib, &id, &m_farIndices))) ClearFar();
}

void Renderer::DrawSky(FXMMATRIX viewProj)
{
    if (!m_light.on) return;
    FrameConstants fc{};
    LightConstants(m_light, fc);
    // The view's rotation only: rays from the eye, whatever the eye's position.
    XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
    const XMVECTOR eye = XMVector3TransformCoord(XMVectorSet(0, 0, 0, 1), inv);   // near-plane centre, close enough to the eye
    inv = XMMatrixMultiply(inv, XMMatrixTranslationFromVector(XMVectorNegate(eye)));
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(inv));
    const auto& s = m_light.sky;
    fc.params = { s[0].x, s[0].y, s[0].z, 0 };
    fc.tint = { s[1].x, s[1].y, s[1].z, 0 };
    fc.ambient = { s[2].x, s[2].y, s[2].z, 0 };
    fc.diffuse = { s[3].x, s[3].y, s[3].z, 0 };
    fc.fog = { s[4].x, s[4].y, s[4].z, 0 };
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_context->OMSetDepthStencilState(m_noDepth.Get(), 0);
    m_context->RSSetState(m_solid.Get());
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_skyVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_skyPs.Get(), nullptr, 0);
    m_context->Draw(3, 0);
}

void Renderer::DrawFar(FXMMATRIX viewProj)
{
    if (!m_farVertices) return;
    FrameConstants fc{};
    LightConstants(m_light, fc);
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(viewProj));
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    const UINT stride = sizeof(LineVertex), offset = 0;
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_context->OMSetDepthStencilState(m_depth.Get(), 0);
    m_context->RSSetState(m_solid.Get());
    m_context->IASetInputLayout(m_lineLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_lineVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_linePs.Get(), nullptr, 0);
    m_context->IASetVertexBuffers(0, 1, m_farVertices.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_farIndices.Get(), DXGI_FORMAT_R32_UINT, 0);
    for (const auto& [key, first] : m_farTiles)
    {
        auto loaded = m_tiles.find(key);   // the detailed tile replaces its low-detail one
        if (loaded != m_tiles.end() && loaded->second.layer == 0) continue;
        m_context->DrawIndexed(16 * 16 * 6, first, 0);
    }
}

void Renderer::DrawOverlay(FXMMATRIX viewProj, const std::vector<LineVertex>& overlay)
{
    FrameConstants fc{};
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(viewProj));
    fc.lightDir = { 0, -1, 0, 0 };
    fc.params = { 8, 0, 1, 0 };
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    if (!overlay.empty())
    {
        if (overlay.size() > m_overlayCapacity)
        {
            m_overlayCapacity = UINT(std::max<size_t>(overlay.size(), 4096));
            D3D11_BUFFER_DESC ob{ UINT(m_overlayCapacity * sizeof(LineVertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
            m_overlay.Reset();
            m_device->CreateBuffer(&ob, nullptr, &m_overlay);
        }
        D3D11_MAPPED_SUBRESOURCE om;
        if (m_overlay && SUCCEEDED(m_context->Map(m_overlay.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &om)))
        {
            memcpy(om.pData, overlay.data(), overlay.size() * sizeof(LineVertex));
            m_context->Unmap(m_overlay.Get(), 0);
            const UINT stride = sizeof(LineVertex), offset = 0;
            m_context->OMSetDepthStencilState(m_noDepth.Get(), 0);
            m_context->RSSetState(m_solid.Get());
            m_context->IASetInputLayout(m_lineLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            m_context->IASetVertexBuffers(0, 1, m_overlay.GetAddressOf(), &stride, &offset);
            m_context->VSSetShader(m_lineVs.Get(), nullptr, 0);
            m_context->PSSetShader(m_linePs.Get(), nullptr, 0);
            m_context->Draw(UINT(overlay.size()), 0);
        }
    }
}

void AddSphere(std::vector<LineVertex>& out, const XMFLOAT3& c, float r, const XMFLOAT4& color)
{
    constexpr int kRings = 8, kSegments = 12;
    const XMVECTOR light = XMVector3Normalize(XMVectorSet(0.4f, 1.0f, 0.3f, 0));
    auto vertex = [&](int ring, int seg) {
        const float theta = XM_PI * ring / kRings, phi = XM_2PI * seg / kSegments;
        const XMFLOAT3 n{ std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi) };
        const float lit = 0.45f + 0.55f * std::max(0.0f, XMVectorGetX(XMVector3Dot(XMLoadFloat3(&n), light)));
        return LineVertex{ { c.x + n.x * r, c.y + n.y * r, c.z + n.z * r }, { color.x * lit, color.y * lit, color.z * lit, color.w } };
    };
    for (int ring = 0; ring < kRings; ++ring)
        for (int seg = 0; seg < kSegments; ++seg)
        {
            const LineVertex a = vertex(ring, seg), b = vertex(ring + 1, seg), d = vertex(ring, seg + 1), e = vertex(ring + 1, seg + 1);
            out.insert(out.end(), { a, b, e, a, e, d });
        }
}

void Renderer::DrawSolids(FXMMATRIX viewProj, const std::vector<LineVertex>& triangles)
{
    if (triangles.empty()) return;
    FrameConstants fc{};
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(viewProj));
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    if (triangles.size() > m_solidsCapacity)
    {
        m_solidsCapacity = UINT(std::max<size_t>(triangles.size() * 2, 4096));
        D3D11_BUFFER_DESC desc{ UINT(m_solidsCapacity * sizeof(LineVertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
        m_solids.Reset();
        m_device->CreateBuffer(&desc, nullptr, &m_solids);
    }
    if (!m_solids || FAILED(m_context->Map(m_solids.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, triangles.data(), triangles.size() * sizeof(LineVertex));
    m_context->Unmap(m_solids.Get(), 0);
    const UINT stride = sizeof(LineVertex), offset = 0;
    m_context->VSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_frameCb.GetAddressOf());
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_context->OMSetDepthStencilState(m_depth.Get(), 0);
    m_context->RSSetState(m_solid.Get());
    m_context->IASetInputLayout(m_lineLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetVertexBuffers(0, 1, m_solids.GetAddressOf(), &stride, &offset);
    m_context->VSSetShader(m_lineVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_linePs.Get(), nullptr, 0);
    m_context->Draw(UINT(triangles.size()), 0);
}

void AddTube(std::vector<LineVertex>& out, const XMFLOAT3& a, const XMFLOAT3& b, float r, const XMFLOAT4& color)
{
    constexpr int kSides = 8;
    const XMVECTOR pa = XMLoadFloat3(&a), pb = XMLoadFloat3(&b);
    const XMVECTOR axis = XMVectorSubtract(pb, pa);
    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f) return;
    const XMVECTOR dir = XMVector3Normalize(axis);
    // Two directions across the tube.
    const XMVECTOR helper = std::fabs(XMVectorGetY(dir)) > 0.9f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
    const XMVECTOR u = XMVector3Normalize(XMVector3Cross(dir, helper)), v = XMVector3Cross(dir, u);
    const XMVECTOR light = XMVector3Normalize(XMVectorSet(0.4f, 1.0f, 0.3f, 0));
    auto vertex = [&](XMVECTOR end, int side) {
        const float t = XM_2PI * side / kSides;
        const XMVECTOR n = XMVectorAdd(XMVectorScale(u, std::cos(t)), XMVectorScale(v, std::sin(t)));
        const float lit = 0.45f + 0.55f * std::max(0.0f, XMVectorGetX(XMVector3Dot(n, light)));
        LineVertex out;
        XMStoreFloat3(&out.pos, XMVectorAdd(end, XMVectorScale(n, r)));
        out.col = { color.x * lit, color.y * lit, color.z * lit, color.w };
        return out;
    };
    for (int s = 0; s < kSides; ++s)
    {
        const LineVertex a0 = vertex(pa, s), a1 = vertex(pa, s + 1), b0 = vertex(pb, s), b1 = vertex(pb, s + 1);
        out.insert(out.end(), { a0, b0, b1, a0, b1, a1 });
    }
}

void AddCone(std::vector<LineVertex>& out, const XMFLOAT3& base, const XMFLOAT3& tip, float r, const XMFLOAT4& color)
{
    constexpr int kSides = 8;
    const XMVECTOR pb = XMLoadFloat3(&base), pt = XMLoadFloat3(&tip);
    const XMVECTOR axis = XMVectorSubtract(pt, pb);
    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f) return;
    const XMVECTOR dir = XMVector3Normalize(axis);
    const XMVECTOR helper = std::fabs(XMVectorGetY(dir)) > 0.9f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
    const XMVECTOR u = XMVector3Normalize(XMVector3Cross(dir, helper)), v = XMVector3Cross(dir, u);
    const XMVECTOR light = XMVector3Normalize(XMVectorSet(0.4f, 1.0f, 0.3f, 0));
    auto shade = [&](XMVECTOR n) {
        const float lit = 0.45f + 0.55f * std::max(0.0f, XMVectorGetX(XMVector3Dot(XMVector3Normalize(n), light)));
        return XMFLOAT4{ color.x * lit, color.y * lit, color.z * lit, color.w };
    };
    auto rim = [&](int side) {
        const float t = XM_2PI * side / kSides;
        return XMVectorAdd(XMVectorScale(u, std::cos(t)), XMVectorScale(v, std::sin(t)));
    };
    auto point = [](XMVECTOR p, const XMFLOAT4& c) { LineVertex o; XMStoreFloat3(&o.pos, p); o.col = c; return o; };
    for (int s = 0; s < kSides; ++s)
    {
        const XMVECTOR n0 = rim(s), n1 = rim(s + 1);
        const XMVECTOR r0 = XMVectorAdd(pb, XMVectorScale(n0, r)), r1 = XMVectorAdd(pb, XMVectorScale(n1, r));
        const XMFLOAT4 side = shade(XMVectorAdd(XMVectorAdd(n0, n1), XMVectorScale(dir, 0.5f))), cap = shade(XMVectorNegate(dir));
        out.insert(out.end(), { point(r0, side), point(r1, side), point(pt, side), point(r0, cap), point(pb, cap), point(r1, cap) });
    }
}

const std::vector<ID3D11ShaderResourceView*>& Renderer::LiquidFrames(uint16_t type, const MpqChain& mpq)
{
    if (auto it = m_liquidFrames.find(type); it != m_liquidFrames.end()) return it->second;
    if (!m_liquidTypesRead)
    {
        m_liquidTypesRead = true;
        m_liquidTypes.Load(mpq.Read("DBFilesClient\\LiquidType.dbc").value_or(std::vector<uint8_t>{}));
    }
    std::vector<ID3D11ShaderResourceView*>& frames = m_liquidFrames[type];
    const auto row = m_liquidTypes.Find(type);
    const std::string pattern = row ? m_liquidTypes.Str(*row, 15) : std::string();   // Texture[0], e.g. XTextures\river\lake_a.%d.blp
    const size_t at = pattern.find("%d");
    if (at == std::string::npos) return frames;
    for (int i = 1; i <= 64; ++i)
    {
        const std::string name = pattern.substr(0, at) + std::to_string(i) + pattern.substr(at + 2);
        if (!mpq.HasOwn(name)) break;
        frames.push_back(TextureFor(name, mpq));
    }
    return frames;
}

size_t Renderer::LiquidFrame(size_t frames) const
{
    if (!frames) return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
    return size_t(ms / 66) % frames;
}
