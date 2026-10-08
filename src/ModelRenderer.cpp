#include "ModelRenderer.hpp"

#include "Loader.hpp"

#include "Mpq.hpp"
#include "Renderer.hpp"

#include <d3dcompiler.h>
#include <DirectXCollision.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace DirectX;

namespace
{
    const char* kShader = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; float4 lightDir; float4 ambient; float4 diffuse; float4 fogColor; float4 fog; };   // ambient.w: game light on
// mode x: alpha test threshold (0 = off); y: liquid opacity (0 = not a liquid); z: 1 = liquid keeps its texture colours;
// w: opacity (texture animation). uvAnim: uv' = (uv - 0.5) * xy + 0.5 + zw (scrolling water, waterfalls).
cbuffer Batch : register(b1) { float4 mode; float4 uvAnim; };
cbuffer Skin : register(b2) { uint4 skin; };     // x: first palette entry of this run's mesh
struct Bone { row_major float4x4 m; };
StructuredBuffer<Bone> bones : register(t1);
struct VsIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; uint4 bi : BLENDINDICES; float4 bw : BLENDWEIGHT;
              float4 r0 : WORLD0; float4 r1 : WORLD1; float4 r2 : WORLD2; float4 r3 : WORLD3; };
struct VsOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
VsOut VsMain(VsIn i)
{
    float4x4 world = float4x4(i.r0, i.r1, i.r2, i.r3);
    float4x4 pose = bones[skin.x + i.bi.x].m * i.bw.x + bones[skin.x + i.bi.y].m * i.bw.y +
                    bones[skin.x + i.bi.z].m * i.bw.z + bones[skin.x + i.bi.w].m * i.bw.w;
    VsOut o;
    o.pos = mul(mul(mul(float4(i.pos, 1), pose), world), viewProj);
    o.nrm = mul(mul(i.nrm, (float3x3)pose), (float3x3)world);
    o.uv = (i.uv - 0.5) * uvAnim.xy + 0.5 + uvAnim.zw;
    return o;
}
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 PsMain(VsOut i, bool front : SV_IsFrontFace) : SV_TARGET
{
    float4 c = tex.Sample(samp, i.uv);
    if (mode.x > 0) clip(c.a - mode.x);
    c.a *= mode.w;
    if (mode.y > 0)   // a liquid: blue tinted by the grey ripple texture, like terrain water (magma and slime keep their colours)
        return float4(mode.z > 0.5 ? c.rgb : float3(0.2, 0.38, 0.46) * (0.65 + 0.9 * dot(c.rgb, 0.3333)), mode.y);
    float3 n = normalize(i.nrm);
    float d = abs(dot(n, -lightDir.xyz));   // two-sided: leaves and cards are lit from both sides
    if (ambient.w < 0.5) return float4(c.rgb * (0.8 + 0.4 * d), c.a);   // bright ambient like the client's daylight; walls keep their texture tone
    // The game's light and fog. ponytail: WMO interiors use their own vertex colours in the client; lit like outdoors here.
    float3 lit = c.rgb * (ambient.rgb + diffuse.rgb * d);
    lit = lerp(lit, fogColor.rgb, saturate((i.pos.w - fog.x) / max(fog.y - fog.x, 1)));
    return float4(lit, c.a);
}
)";

    struct FrameConstants { XMFLOAT4X4 viewProj; XMFLOAT4 lightDir; XMFLOAT4 ambient{}, diffuse{}, fogColor{}, fog{}; };

    bool Compile(const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& out, std::string& error)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        if (SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, entry, target, 0, 0, &out, &errors))) return true;
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "model shader compile failed";
        return false;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }
}

bool ModelRenderer::Init(ID3D11Device* device, ID3D11DeviceContext* context, Renderer& textures, std::string& error)
{
    m_device = device;
    m_context = context;
    m_textures = &textures;

    Com<ID3DBlob> vs, ps;
    if (!Compile("VsMain", "vs_5_0", vs, error) || !Compile("PsMain", "ps_5_0", ps, error)) return false;
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "WORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "WORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "WORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "WORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 } };
    device->CreateInputLayout(layout, UINT(std::size(layout)), vs->GetBufferPointer(), vs->GetBufferSize(), &m_layout);

    D3D11_BUFFER_DESC cb{ sizeof(FrameConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    device->CreateBuffer(&cb, nullptr, &m_frameCb);
    cb.ByteWidth = 32;
    device->CreateBuffer(&cb, nullptr, &m_batchCb);
    cb.ByteWidth = 16;
    device->CreateBuffer(&cb, nullptr, &m_skinCb);
    m_start = std::chrono::steady_clock::now();

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_ANISOTROPIC;
    s.MaxAnisotropy = 8;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&s, &m_sampler);

    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&r, &m_noCull);
    // One-sided materials hide their back, as the client does (the axis change mirrors the models, so their front
    // faces wind counter-clockwise on screen). WWE_FLIP_CULL swaps the side, to check the winding.
    r.CullMode = D3D11_CULL_BACK;
    r.FrontCounterClockwise = std::getenv("WWE_FLIP_CULL") ? FALSE : TRUE;
    device->CreateRasterizerState(&r, &m_cullBack);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&ds, &m_depth);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&ds, &m_depthNoWrite);

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &m_alphaBlend);
    rt.DestBlend = D3D11_BLEND_ONE;
    device->CreateBlendState(&bd, &m_additive);
    return m_vs && m_ps && m_layout;
}

void ModelRenderer::RefreshTextures(const MpqChain& mpq)
{
    ID3D11ShaderResourceView* const white = m_textures->TextureFor({}, mpq);
    for (auto& [key, mesh] : m_meshes)
        if (mesh)
            for (auto& b : mesh->batches)
                if (b.texture == white && !b.textureName.empty()) b.texture = m_textures->TextureFor(b.textureName, mpq);
}

ModelRenderer::GpuMesh* ModelRenderer::Mesh(const std::string& name, bool wmo, const MpqChain& mpq)
{
    const std::string key = Lower(name);
    if (auto it = m_meshes.find(key); it != m_meshes.end())
    {
        if (m_loader) m_loader->TakeMesh(key);   // a background copy of a mesh already here: drop it
        return it->second.get();
    }

    std::optional<ModelMesh> mesh = m_loader ? m_loader->TakeMesh(key) : std::nullopt;
    if (!mesh && wmo)
    {
        uint32_t groups = 0;
        float bounds[6];
        if (auto root = mpq.Read(name); root && WmoRootInfo(*root, groups, bounds))
        {
            std::vector<std::vector<uint8_t>> files;
            for (uint32_t g = 0; g < groups; ++g) files.push_back(mpq.Read(WmoGroupFile(name, *root, g)).value_or(std::vector<uint8_t>{}));
            mesh = ParseWmo(*root, files);
        }
    }
    else if (!mesh)
        mesh = LoadM2(name, [&mpq](const std::string& path) { return mpq.Read(path); });

    std::unique_ptr<GpuMesh>& slot = m_meshes[key];
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) return nullptr;

    auto gpu = std::make_unique<GpuMesh>();
    D3D11_BUFFER_DESC vb{ UINT(mesh->vertices.size() * sizeof(ModelVertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA vd{ mesh->vertices.data() };
    D3D11_BUFFER_DESC ib{ UINT(mesh->indices.size() * sizeof(uint32_t)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
    D3D11_SUBRESOURCE_DATA id{ mesh->indices.data() };
    if (FAILED(m_device->CreateBuffer(&vb, &vd, &gpu->vertices)) || FAILED(m_device->CreateBuffer(&ib, &id, &gpu->indices))) return nullptr;
    for (const auto& b : mesh->batches)
    {
        gpu->batches.push_back({ b.indexStart, b.indexCount, m_textures->TextureFor(b.texture, mpq), b.blend, b.textureType, b.geoset });
        gpu->batches.back().twoSided = b.twoSided || wmo;   // ponytail: WMOs stay two-sided (their materials are checked separately)
        gpu->batches.back().uvAnim = b.uvAnim;
        gpu->batches.back().weight = b.weight;
        gpu->batches.back().color = b.color;
        gpu->batches.back().textureName = b.texture;
        if (!b.texture.empty() && std::find(gpu->textureNames.begin(), gpu->textureNames.end(), b.texture) == gpu->textureNames.end())
            gpu->textureNames.push_back(b.texture);
        if (b.liquid)
        {
            auto& out = gpu->batches.back();
            out.liquid = b.liquid;
            out.frames = &m_textures->LiquidFrames(b.liquid, mpq);
            // ponytail: magma / slime by stock LiquidType ids; LiquidType.dbc's type column for custom liquids.
            static const std::set<uint16_t> kRaw = { 3, 4, 7, 8, 11, 12, 15, 19, 20, 21, 121, 141, 181 };
            out.rawLiquid = kRaw.count(b.liquid) != 0;
        }
    }
    gpu->center = { (mesh->boundsMin.x + mesh->boundsMax.x) / 2, (mesh->boundsMin.y + mesh->boundsMax.y) / 2, (mesh->boundsMin.z + mesh->boundsMax.z) / 2 };
    const float dx = mesh->boundsMax.x - mesh->boundsMin.x, dy = mesh->boundsMax.y - mesh->boundsMin.y, dz = mesh->boundsMax.z - mesh->boundsMin.z;
    gpu->radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    gpu->wmo = wmo;
    gpu->skeleton = mesh->skeleton;
    gpu->doodads = mesh->doodads;
    gpu->doodadSets = mesh->doodadSets;
    gpu->boundsMin = mesh->boundsMin;
    gpu->boundsMax = mesh->boundsMax;
    gpu->positions.reserve(mesh->vertices.size());
    for (const ModelVertex& v : mesh->vertices) gpu->positions.push_back(v.pos);
    gpu->triangles = std::move(mesh->indices);
    slot = std::move(gpu);
    return slot.get();
}

std::string ModelLook::Key() const
{
    std::string key = Lower(model);
    for (const auto& [type, path] : textures) key += "|" + std::to_string(type) + "=" + Lower(path);
    key += "|g";
    for (uint16_t g : geosets) key += "," + std::to_string(g);
    return key;
}

void ModelRenderer::AddModel(int tileKey, const ModelLook& look, FXMMATRIX world, float scale, uint32_t uid, const MpqChain& mpq, int layer)
{
    m_tileInstances[tileKey];
    if (layer == 0 && tileKey < 0) layer = -1;
    const bool wmo = Lower(look.model).ends_with(".wmo");   // a few gameobjects (transports) are buildings
    GpuMesh* mesh = Mesh(wmo ? look.model : M2Name(look.model), wmo, mpq);
    if (!mesh) return;
    const Look* resolved = ResolveLook(look, mpq);
    XMFLOAT4X4 w;
    XMStoreFloat4x4(&w, world);
    char key[160];
    snprintf(key, sizeof key, "%dl%u:%d:%d:%d:%p", layer, uid, int(w._41 * 4), int(w._42 * 4), int(w._43 * 4), static_cast<const void*>(resolved));
    AddInstance(tileKey, key, mesh, world, scale, uid, layer, resolved);
}

const ModelRenderer::Look* ModelRenderer::ResolveLook(const ModelLook& look, const MpqChain& mpq)
{
    std::unique_ptr<Look>& slot = m_looks[look.Key()];
    if (!slot)
    {
        slot = std::make_unique<Look>();
        for (const auto& [type, path] : look.textures)
            if (type < slot->textures.size() && !path.empty()) slot->textures[type] = m_textures->TextureFor(path, mpq);
        slot->geosets = look.geosets;
        std::sort(slot->geosets.begin(), slot->geosets.end());
    }
    return slot.get();
}

void ModelRenderer::AddInstance(int tile, const std::string& key, GpuMesh* mesh, FXMMATRIX world, float scale, uint32_t uid, int layer, const Look* look)
{
    Instance& inst = m_instances[key];
    if (inst.refs++ == 0)
    {
        inst.mesh = mesh;
        inst.look = look;
        inst.wmo = mesh->wmo;
        inst.uid = uid;
        inst.layer = layer;
        XMStoreFloat4x4(&inst.world, world);
        XMStoreFloat3(&inst.center, XMVector3TransformCoord(XMLoadFloat3(&mesh->center), world));
        inst.radius = mesh->radius * scale;
    }
    m_tileInstances[tile].push_back(key);
}

void ModelRenderer::AddTile(int tileKey, const Adt& adt, const MpqChain& mpq, int layer)
{
    m_tileInstances[tileKey];   // a tile with no objects still counts as added
    if (layer == 0 && tileKey < 0) layer = -1;
    char key[96];
    for (const auto& d : adt.doodads)
        if (GpuMesh* mesh = Mesh(M2Name(d.model), false, mpq))
        {
            snprintf(key, sizeof key, "%dd%u:%d:%d:%d", layer, d.uniqueId, int(d.pos[0] * 4), int(d.pos[1] * 4), int(d.pos[2] * 4));
            AddInstance(tileKey, key, mesh, PlacementMatrix(d.pos, d.rot, d.scale), d.scale, d.uniqueId, layer);
        }
    for (const auto& w : adt.wmos)
        if (GpuMesh* mesh = Mesh(w.model, true, mpq))
        {
            snprintf(key, sizeof key, "%dw%u:%d:%d:%d", layer, w.uniqueId, int(w.pos[0] * 4), int(w.pos[1] * 4), int(w.pos[2] * 4));
            const XMMATRIX world = PlacementMatrix(w.pos, w.rot, 1.0f);
            AddInstance(tileKey, key, mesh, world, 1.0f, w.uniqueId, layer);
            // Its doodads: set 0 (always shown) and the set the placement picks.
            for (const uint32_t s : { 0u, uint32_t(w.doodadSet) })
            {
                if (s >= mesh->doodadSets.size()) continue;
                const ModelMesh::DoodadSet& set = mesh->doodadSets[s];
                for (uint32_t i = set.first; i < set.first + set.count && i < mesh->doodads.size(); ++i)
                {
                    const ModelMesh::Doodad& d = mesh->doodads[i];
                    GpuMesh* m2 = d.model.empty() ? nullptr : Mesh(M2Name(d.model), false, mpq);
                    if (!m2) continue;
                    char dkey[128];
                    snprintf(dkey, sizeof dkey, "%s/d%u", key, i);
                    AddInstance(tileKey, dkey, m2, WmoDoodadMatrix(d) * world, d.scale, w.uniqueId, layer);
                    Instance& inst = m_instances[dkey];
                    inst.wmo = true;   // picks, hides and moves with its WMO
                    inst.attached = true;
                }
                if (w.doodadSet == 0) break;
            }
        }
}

void ModelRenderer::RemoveTile(int tileKey)
{
    auto it = m_tileInstances.find(tileKey);
    if (it == m_tileInstances.end()) return;
    for (const std::string& key : it->second)
        if (auto inst = m_instances.find(key); inst != m_instances.end() && --inst->second.refs <= 0) m_instances.erase(inst);
    m_tileInstances.erase(it);
}

void ModelRenderer::Clear()
{
    m_instances.clear();
    m_tileInstances.clear();
}

std::vector<int> ModelRenderer::TileKeys() const
{
    std::vector<int> keys;
    for (const auto& [key, list] : m_tileInstances) keys.push_back(key);
    return keys;
}

std::optional<float> ModelRenderer::HitDistance(const Instance& inst, FXMVECTOR origin, FXMVECTOR dir, float maxDistance) const
{
    // Sphere first (cheap), then the model box in model space, then every triangle.
    const XMVECTOR toCenter = XMVectorSubtract(XMLoadFloat3(&inst.center), origin);
    const float along = XMVectorGetX(XMVector3Dot(toCenter, dir));
    const float miss = XMVectorGetX(XMVector3LengthSq(toCenter)) - along * along;
    if (miss > inst.radius * inst.radius || along + inst.radius < 0 || along - inst.radius > maxDistance) return std::nullopt;

    const XMMATRIX world = XMLoadFloat4x4(&inst.world);
    const XMMATRIX inv = XMMatrixInverse(nullptr, world);
    const XMVECTOR o = XMVector3TransformCoord(origin, inv);
    const XMVECTOR end = XMVector3TransformCoord(XMVectorAdd(origin, XMVectorScale(dir, maxDistance)), inv);
    const XMVECTOR d = XMVectorSubtract(end, o);
    const float len = XMVectorGetX(XMVector3Length(d));
    if (len <= 0) return std::nullopt;
    const XMVECTOR dn = XMVectorScale(d, 1.0f / len);   // model-space unit ray; t * (maxDistance / len) = world distance
    BoundingBox box;
    BoundingBox::CreateFromPoints(box, XMLoadFloat3(&inst.mesh->boundsMin), XMLoadFloat3(&inst.mesh->boundsMax));
    float tBox = 0;
    if (!box.Contains(o) && (!box.Intersects(o, dn, tBox) || tBox > len)) return std::nullopt;

    const auto& p = inst.mesh->positions;
    const auto& tri = inst.mesh->triangles;
    float nearest = len;
    for (size_t i = 0; i + 2 < tri.size(); i += 3)
    {
        float t = 0;
        if (tri[i] >= p.size() || tri[i + 1] >= p.size() || tri[i + 2] >= p.size()) continue;
        if (TriangleTests::Intersects(o, dn, XMLoadFloat3(&p[tri[i]]), XMLoadFloat3(&p[tri[i + 1]]), XMLoadFloat3(&p[tri[i + 2]]), t) && t < nearest)
            nearest = t;
    }
    if (nearest >= len) return std::nullopt;
    return nearest * (maxDistance / len);
}

std::optional<ModelRenderer::Hit> ModelRenderer::Pick(FXMVECTOR origin, FXMVECTOR dir, float maxDistance, const DrawSettings& settings) const
{
    std::optional<Hit> best;
    float bestT = maxDistance;
    for (const auto& [key, inst] : m_instances)
    {
        if (inst.layer != 0 || (inst.wmo ? !settings.wmos : !settings.doodads)) continue;
        if (const auto t = HitDistance(inst, origin, dir, bestT))
        {
            bestT = *t;
            best = Hit{ inst.wmo, inst.uid, bestT };
        }
    }
    return best;
}

std::optional<ModelRenderer::Hit> ModelRenderer::PickTile(int tileKey, FXMVECTOR origin, FXMVECTOR dir, float maxDistance) const
{
    std::optional<Hit> best;
    float bestT = maxDistance;
    const auto tile = m_tileInstances.find(tileKey);
    if (tile == m_tileInstances.end()) return best;
    for (const std::string& key : tile->second)
        if (const auto inst = m_instances.find(key); inst != m_instances.end())
            if (const auto t = HitDistance(inst->second, origin, dir, bestT))
            {
                bestT = *t;
                best = Hit{ inst->second.wmo, inst->second.uid, bestT };
            }
    return best;
}

bool ModelRenderer::ThumbnailFailed(const std::string& name) const
{
    auto it = m_thumbs.find(Lower(name));
    return it != m_thumbs.end() && it->second.failed;
}

ID3D11ShaderResourceView* ModelRenderer::Thumbnail(const std::string& name, bool wmo, const MpqChain& mpq, bool render)
{
    const std::string key = Lower(name);
    if (auto it = m_thumbs.find(key); it != m_thumbs.end() || !render) return it != m_thumbs.end() ? it->second.srv.Get() : nullptr;
    Thumb& thumb = m_thumbs[key];
    GpuMesh* mesh = Mesh(wmo ? name : M2Name(name), wmo, mpq);
    if (!mesh) { thumb.failed = true; return nullptr; }
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    RenderThumb(thumb, { { mesh, identity, nullptr } });
    return thumb.srv.Get();
}

ID3D11ShaderResourceView* ModelRenderer::LookThumbnail(const std::string& name, const std::vector<std::pair<ModelLook, XMFLOAT4X4>>& parts,
                                                       const MpqChain& mpq, bool render)
{
    const std::string key = "look:" + name;
    if (auto it = m_thumbs.find(key); it != m_thumbs.end() || !render) return it != m_thumbs.end() ? it->second.srv.Get() : nullptr;
    Thumb& thumb = m_thumbs[key];
    std::vector<std::tuple<GpuMesh*, XMFLOAT4X4, const Look*>> meshes;
    for (const auto& [look, world] : parts)
    {
        const bool wmo = Lower(look.model).ends_with(".wmo");
        if (GpuMesh* mesh = Mesh(wmo ? look.model : M2Name(look.model), wmo, mpq)) meshes.emplace_back(mesh, world, ResolveLook(look, mpq));
        else if (meshes.empty()) break;   // no body: nothing to frame
    }
    if (meshes.empty()) { thumb.failed = true; return nullptr; }
    RenderThumb(thumb, meshes);
    return thumb.srv.Get();
}

void ModelRenderer::RenderThumb(Thumb& thumb, const std::vector<std::tuple<GpuMesh*, XMFLOAT4X4, const Look*>>& parts)
{
    constexpr UINT kSize = 128;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = kSize;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> color;
    Com<ID3D11RenderTargetView> rtv;
    if (FAILED(m_device->CreateTexture2D(&d, nullptr, &color)) || FAILED(m_device->CreateRenderTargetView(color.Get(), nullptr, &rtv)) ||
        FAILED(m_device->CreateShaderResourceView(color.Get(), nullptr, &thumb.srv)))
    { thumb.failed = true; return; }
    if (!m_thumbDepth)
    {
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Com<ID3D11Texture2D> depth;
        m_device->CreateTexture2D(&d, nullptr, &depth);
        if (depth) m_device->CreateDepthStencilView(depth.Get(), nullptr, &m_thumbDepth);
    }
    if (!m_instanceBuffer)
    {
        m_instanceCapacity = 1024;
        D3D11_BUFFER_DESC desc{ UINT(m_instanceCapacity * sizeof(XMFLOAT4X4)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
        m_device->CreateBuffer(&desc, nullptr, &m_instanceBuffer);
    }

    // Keep whatever target the caller had; the catalog asks for thumbnails in the middle of building the UI.
    Com<ID3D11RenderTargetView> oldRtv;
    Com<ID3D11DepthStencilView> oldDsv;
    m_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    UINT viewports = 1;
    D3D11_VIEWPORT oldVp{};
    m_context->RSGetViewports(&viewports, &oldVp);

    const float background[4] = { 0.16f, 0.17f, 0.20f, 1 };
    m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), m_thumbDepth.Get());
    const D3D11_VIEWPORT vp{ 0, 0, float(kSize), float(kSize), 0, 1 };
    m_context->RSSetViewports(1, &vp);
    m_context->ClearRenderTargetView(rtv.Get(), background);
    if (m_thumbDepth) m_context->ClearDepthStencilView(m_thumbDepth.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Three-quarter view from the front-right and a little above, framed on the model's bounding sphere.
    const auto& [body, bodyWorld, bodyLook] = parts.front();
    const XMMATRIX bw = XMLoadFloat4x4(&bodyWorld);
    const XMVECTOR center = XMVector3TransformCoord(XMLoadFloat3(&body->center), bw);
    const float radius = std::max(body->radius * XMVectorGetX(XMVector3Length(bw.r[0])), 0.1f);
    const XMVECTOR eye = XMVectorAdd(center, XMVectorScale(XMVector3Normalize(XMVectorSet(1.0f, 0.65f, 1.0f, 0)), radius * 2.1f));
    const XMMATRIX viewProj = XMMatrixLookAtRH(eye, center, XMVectorSet(0, 1, 0, 0)) *
                              XMMatrixPerspectiveFovRH(XMConvertToRadians(45.0f), 1.0f, radius * 0.05f, radius * 10.0f);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (m_instanceBuffer && SUCCEEDED(m_context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::vector<Run> runs;
        for (size_t i = 0; i < parts.size() && i < m_instanceCapacity; ++i)
        {
            static_cast<XMFLOAT4X4*>(mapped.pData)[i] = std::get<1>(parts[i]);
            runs.push_back({ std::get<0>(parts[i]), UINT(i), 1, std::get<2>(parts[i]) });
        }
        m_context->Unmap(m_instanceBuffer.Get(), 0);
        Submit(runs, viewProj);
    }

    m_context->OMSetRenderTargets(1, oldRtv.GetAddressOf(), oldDsv.Get());
    if (viewports) m_context->RSSetViewports(1, &oldVp);
}

bool ModelRenderer::Corners(bool wmo, uint32_t uid, XMFLOAT3 corners[8]) const
{
    for (const auto& [key, inst] : m_instances)
    {
        if (inst.layer != 0 || inst.attached || inst.wmo != wmo || inst.uid != uid) continue;
        const XMMATRIX world = XMLoadFloat4x4(&inst.world);
        const XMFLOAT3& a = inst.mesh->boundsMin;
        const XMFLOAT3& b = inst.mesh->boundsMax;
        for (int i = 0; i < 8; ++i)
            XMStoreFloat3(&corners[i], XMVector3TransformCoord(XMVectorSet(i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z, 1), world));
        return true;
    }
    return false;
}

void ModelRenderer::ForEachObject(const std::function<void(bool, uint32_t, const XMFLOAT3&)>& fn) const
{
    for (const auto& [key, inst] : m_instances)
        if (inst.layer == 0 && !inst.attached) fn(inst.wmo, inst.uid, inst.center);
}

void ModelRenderer::Draw(FXMMATRIX viewProj, const XMFLOAT3& eye, const DrawSettings& settings)
{
    m_drawn = 0;
    if (m_instances.empty()) return;

    // Frustum planes (row-vector convention: columns of viewProj) for sphere culling.
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);
    auto col = [&](int j) { return XMVectorSet(m(0, j), m(1, j), m(2, j), m(3, j)); };
    const XMVECTOR planes[6] = { col(3) + col(0), col(3) - col(0), col(3) + col(1), col(3) - col(1), col(2), col(3) - col(2) };
    auto visible = [&](const Instance& inst) {
        const float dx = inst.center.x - eye.x, dy = inst.center.y - eye.y, dz = inst.center.z - eye.z;
        const float reach = settings.distance + inst.radius;
        if (dx * dx + dy * dy + dz * dz > reach * reach) return false;
        const XMVECTOR c = XMVectorSet(inst.center.x, inst.center.y, inst.center.z, 1);
        for (const XMVECTOR& p : planes)
            if (XMVectorGetX(XMVector4Dot(p, c)) < -inst.radius * XMVectorGetX(XMVector3Length(p))) return false;
        return true;
    };

    // Group the visible instances by mesh into one instance buffer.
    std::map<std::pair<GpuMesh*, const Look*>, std::vector<const XMFLOAT4X4*>> groups;
    for (const auto& [key, inst] : m_instances)
    {
        if ((inst.layer != settings.layer && inst.layer != -1) || (inst.mesh->wmo ? !settings.wmos : !settings.doodads)) continue;
        if (visible(inst)) groups[{ inst.mesh, inst.look }].push_back(&inst.world);
    }
    size_t total = 0;
    for (const auto& [mesh, list] : groups) total += list.size();
    if (!total) return;
    if (total > m_instanceCapacity)
    {
        m_instanceCapacity = UINT(std::max<size_t>(total * 2, 1024));
        D3D11_BUFFER_DESC desc{ UINT(m_instanceCapacity * sizeof(XMFLOAT4X4)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
        m_instanceBuffer.Reset();
        m_device->CreateBuffer(&desc, nullptr, &m_instanceBuffer);
    }
    std::vector<Run> runs;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* out = static_cast<XMFLOAT4X4*>(mapped.pData);
    UINT next = 0;
    for (const auto& [group, list] : groups)
    {
        runs.push_back({ group.first, next, UINT(list.size()), group.second });
        for (const XMFLOAT4X4* w : list) out[next++] = *w;
    }
    m_context->Unmap(m_instanceBuffer.Get(), 0);
    m_drawn = total;
    Submit(runs, viewProj, true);
}

void ModelRenderer::Submit(const std::vector<Run>& runs, FXMMATRIX viewProj, bool sceneLit)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    FrameConstants fc;
    XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4(&fc.lightDir, XMVector3Normalize(XMVectorSet(-0.4f, -1.0f, -0.3f, 0)));
    if (const SceneLight& l = m_textures->Light(); sceneLit && l.on)
    {
        fc.ambient = { l.ambient.x, l.ambient.y, l.ambient.z, 1 };
        fc.diffuse = { l.diffuse.x, l.diffuse.y, l.diffuse.z, 0 };
        fc.fogColor = { l.fog.x, l.fog.y, l.fog.z, 1 };
        fc.fog = { l.fogStart, l.fogEnd, 1, 0 };
    }
    if (SUCCEEDED(m_context->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &fc, sizeof fc);
        m_context->Unmap(m_frameCb.Get(), 0);
    }
    float current[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    auto setMode = [&](float alphaTest, float liquid = 0, float raw = 0, float alpha = 1, XMFLOAT4 uv = { 1, 1, 0, 0 }) {
        const float v[8] = { alphaTest, liquid, raw, alpha, uv.x, uv.y, uv.z, uv.w };
        if (!memcmp(v, current, sizeof v)) return;
        memcpy(current, v, sizeof v);
        if (SUCCEEDED(m_context->Map(m_batchCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, v, sizeof v);
            m_context->Unmap(m_batchCb.Get(), 0);
        }
    };

    // Bone palettes: entry 0 is the identity (meshes without bones); each mesh with bones posed once per frame.
    // ponytail: every instance of a model shares one pose (same phase); per-instance palettes if that shows.
    const uint32_t now = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count());
    std::vector<XMFLOAT4X4> palette(1);
    XMStoreFloat4x4(&palette[0], XMMatrixIdentity());
    // A run with its own pose (the NPC viewer) is posed by that skeleton at its own time.
    auto skeletonOf = [](const Run& run) -> const ModelSkeleton* {
        const ModelSkeleton* own = run.mesh->skeleton.get();
        return own && run.pose && run.pose->bones.size() == own->bones.size() ? run.pose : own;
    };
    auto timeOf = [&](const Run& run) { return skeletonOf(run) == run.pose && run.pose ? run.timeMs : now; };
    std::map<std::pair<const GpuMesh*, const ModelSkeleton*>, UINT> boneBase;
    std::vector<XMFLOAT4X4> pose;
    for (const Run& run : runs)
        if (const ModelSkeleton* skel = skeletonOf(run); skel && boneBase.emplace(std::pair(run.mesh, skel), UINT(palette.size())).second)
        {
            PoseBones(*skel, timeOf(run), pose);
            palette.insert(palette.end(), pose.begin(), pose.end());
        }
    if (palette.size() > m_boneCapacity)
    {
        m_boneCapacity = UINT(std::max<size_t>(palette.size() * 2, 4096));
        D3D11_BUFFER_DESC desc{ UINT(m_boneCapacity * sizeof(XMFLOAT4X4)), D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                                D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(XMFLOAT4X4) };
        m_boneBuffer.Reset();
        m_boneSrv.Reset();
        m_device->CreateBuffer(&desc, nullptr, &m_boneBuffer);
        if (m_boneBuffer) m_device->CreateShaderResourceView(m_boneBuffer.Get(), nullptr, &m_boneSrv);
    }
    if (!m_boneBuffer || FAILED(m_context->Map(m_boneBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, palette.data(), palette.size() * sizeof(XMFLOAT4X4));
    m_context->Unmap(m_boneBuffer.Get(), 0);
    UINT currentBase = UINT_MAX;
    auto setBase = [&](UINT base) {
        if (base == currentBase) return;
        currentBase = base;
        const UINT v[4] = { base, 0, 0, 0 };
        if (SUCCEEDED(m_context->Map(m_skinCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, v, sizeof v);
            m_context->Unmap(m_skinCb.Get(), 0);
        }
    };

    m_context->IASetInputLayout(m_layout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[3] = { m_frameCb.Get(), m_batchCb.Get(), m_skinCb.Get() };
    m_context->VSSetConstantBuffers(0, 3, cbs);
    m_context->VSSetShaderResources(1, 1, m_boneSrv.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 2, cbs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->RSSetState(m_noCull.Get());

    // Pass 0: opaque and alpha-tested. Pass 1: blended, without depth writes.
    for (int pass = 0; pass < 2; ++pass)
    {
        m_context->OMSetDepthStencilState(pass == 0 ? m_depth.Get() : m_depthNoWrite.Get(), 0);
        for (const Run& run : runs)
        {
            ID3D11Buffer* buffers[2] = { run.mesh->vertices.Get(), m_instanceBuffer.Get() };
            const UINT strides[2] = { sizeof(ModelVertex), sizeof(XMFLOAT4X4) }, offsets[2] = { 0, 0 };
            m_context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
            m_context->IASetIndexBuffer(run.mesh->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            const ModelSkeleton* skel = skeletonOf(run);
            const uint32_t time = timeOf(run);
            auto base = boneBase.find(std::pair(run.mesh, skel));
            setBase(base == boneBase.end() ? 0 : base->second);
            for (const auto& b : run.mesh->batches)
            {
                const bool blended = b.blend == ModelMesh::Blend::AlphaBlend || b.blend == ModelMesh::Blend::Additive;
                if (blended != (pass == 1)) continue;
                const Look* look = run.look;
                if (look && !look->geosets.empty() && !std::binary_search(look->geosets.begin(), look->geosets.end(), b.geoset)) continue;
                ID3D11ShaderResourceView* texture = b.texture;
                if (look && b.textureType && b.textureType < look->textures.size() && look->textures[b.textureType])
                    texture = look->textures[b.textureType];
                if (b.frames && !b.frames->empty()) texture = (*b.frames)[m_textures->LiquidFrame(b.frames->size())];
                // Texture animation (scrolling, fading) from the model's animation clock.
                const float alpha = skel && (b.weight >= 0 || b.color >= 0) ? BatchAlpha(*skel, b.weight, b.color, time) : 1.0f;
                const XMFLOAT4 uv = skel && b.uvAnim >= 0 ? UvTransform(*skel, b.uvAnim, time) : XMFLOAT4{ 1, 1, 0, 0 };
                if (alpha <= 0.001f) continue;
                setMode(b.blend == ModelMesh::Blend::AlphaTest ? 0.5f : 0.0f, b.liquid ? (b.rawLiquid ? 1.0f : 0.72f) : 0.0f, b.rawLiquid ? 1.0f : 0.0f, alpha, uv);
                m_context->OMSetBlendState(!blended ? nullptr : b.blend == ModelMesh::Blend::Additive ? m_additive.Get() : m_alphaBlend.Get(),
                                           nullptr, 0xffffffff);
                m_context->RSSetState(b.twoSided ? m_noCull.Get() : m_cullBack.Get());
                m_context->PSSetShaderResources(0, 1, &texture);
                m_context->DrawIndexedInstanced(b.count, run.count, b.start, 0, run.first);
            }
        }
    }
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ID3D11ShaderResourceView* none = nullptr;
    m_context->VSSetShaderResources(1, 1, &none);
}

std::optional<XMFLOAT4X4> ModelRenderer::AttachmentMatrix(const std::string& model, uint32_t id, const MpqChain& mpq,
                                                          const ModelSkeleton* pose, uint32_t timeMs)
{
    const GpuMesh* mesh = Mesh(M2Name(model), false, mpq);
    if (!mesh || !mesh->skeleton) return std::nullopt;
    const ModelSkeleton& skel = pose && pose->bones.size() == mesh->skeleton->bones.size() ? *pose : *mesh->skeleton;
    for (const auto& a : skel.attachments)
        if (a.id == id)
        {
            std::vector<XMFLOAT4X4> palette;
            PoseBones(skel, &skel == pose ? timeMs : 0, palette);
            const XMFLOAT3 p = FromWowAxes(a.pos.x, a.pos.y, a.pos.z);
            XMFLOAT4X4 out;
            XMStoreFloat4x4(&out, XMMatrixTranslation(p.x, p.y, p.z) * XMLoadFloat4x4(&palette[a.bone]));
            return out;
        }
    return std::nullopt;
}

void ModelRenderer::DrawParts(const std::vector<Part>& parts, FXMMATRIX viewProj, const MpqChain& mpq)
{
    if (!m_instanceBuffer)
    {
        m_instanceCapacity = 1024;
        D3D11_BUFFER_DESC desc{ UINT(m_instanceCapacity * sizeof(XMFLOAT4X4)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
        m_device->CreateBuffer(&desc, nullptr, &m_instanceBuffer);
    }
    std::vector<Run> runs;
    std::vector<XMFLOAT4X4> worlds;
    for (const Part& p : parts)
    {
        const bool wmo = Lower(p.look.model).ends_with(".wmo");
        GpuMesh* mesh = Mesh(wmo ? p.look.model : M2Name(p.look.model), wmo, mpq);
        if (!mesh || worlds.size() >= m_instanceCapacity) continue;
        runs.push_back({ mesh, UINT(worlds.size()), 1, ResolveLook(p.look, mpq), p.pose, p.timeMs });
        worlds.push_back(p.world);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (runs.empty() || !m_instanceBuffer || FAILED(m_context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, worlds.data(), worlds.size() * sizeof(XMFLOAT4X4));
    m_context->Unmap(m_instanceBuffer.Get(), 0);
    Submit(runs, viewProj);
}

std::optional<ModelRenderer::ModelInfo> ModelRenderer::Info(const std::string& model, const MpqChain& mpq)
{
    const bool wmo = Lower(model).ends_with(".wmo");
    const GpuMesh* mesh = Mesh(wmo ? model : M2Name(model), wmo, mpq);
    if (!mesh) return std::nullopt;
    ModelInfo info{ mesh->skeleton, {}, mesh->textureNames, {}, mesh->boundsMin, mesh->boundsMax };
    for (const auto& set : mesh->doodadSets) info.doodadSets.push_back(set.name);
    for (const auto& b : mesh->batches) info.geosets.push_back(b.geoset);
    std::sort(info.geosets.begin(), info.geosets.end());
    info.geosets.erase(std::unique(info.geosets.begin(), info.geosets.end()), info.geosets.end());
    return info;
}
