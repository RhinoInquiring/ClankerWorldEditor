#pragma once

#include "Formats.hpp"
#include "Models.hpp"

#include <d3d11.h>
#include <DirectXMath.h>

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class MpqChain;

/// How one model looks: replaceable textures by M2 texture type and the submeshes (geosets) drawn.
struct ModelLook
{
    std::string model;                          // .m2 / .mdx
    std::map<uint32_t, std::string> textures;   // M2 texture type (1 body, 2 cape, 6 hair, 8 fur, 11-13 creature skins) -> BLP
    std::vector<uint16_t> geosets;              // sorted submesh ids to draw; empty = every submesh
    std::string Key() const;
};
class Renderer;

/// Draws the M2 doodads and WMOs placed by loaded tiles: one GPU mesh per model file, instanced.
/// An object listed by several tiles (same unique id and position) is drawn once.
class ModelRenderer
{
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, Renderer& textures, std::string& error);
    /// Meshes built in the background are taken from here instead of being parsed on this thread.
    void SetLoader(class Loader* loader) { m_loader = loader; }

    /// `layer`: 0 the map, a positive number a ghost layer (drawn only when DrawSettings::layer names it).
    /// Tiles with negative keys on layer 0 are previews (drawn with any layer, never picked).
    void AddTile(int key, const Adt& adt, const MpqChain& mpq, int layer = 0);
    /// One M2 with a look (creatures, gameobjects) on a tile key; `scale` is already in `world`, given for culling.
    void AddModel(int key, const ModelLook& look, DirectX::FXMMATRIX world, float scale, uint32_t uid, const MpqChain& mpq, int layer = 0);
    void RemoveTile(int key);
    void Clear();
    bool HasTile(int key) const { return m_tileInstances.count(key) != 0; }
    std::vector<int> TileKeys() const;

    struct DrawSettings { bool doodads = true, wmos = true; float distance = 600.0f; int layer = 0; };
    void Draw(DirectX::FXMMATRIX viewProj, const DirectX::XMFLOAT3& eye, const DrawSettings& settings);

    /// Nearest object along a ray (triangle-exact), within `maxDistance`; previews (negative tile keys) are skipped.
    struct Hit { bool wmo; uint32_t uid; float distance; };
    std::optional<Hit> Pick(DirectX::FXMVECTOR origin, DirectX::FXMVECTOR dir, float maxDistance, const DrawSettings& settings) const;
    /// Nearest object of one tile key (previews included: spawn models live on negative keys).
    std::optional<Hit> PickTile(int key, DirectX::FXMVECTOR origin, DirectX::FXMVECTOR dir, float maxDistance) const;
    /// The eight corners of an object's model box in the world; false when it is not loaded.
    bool Corners(bool wmo, uint32_t uid, DirectX::XMFLOAT3 corners[8]) const;
    /// Where an M2 attachment point (helmet 11, hands 1 / 2, ...) sits in the model, posed at its first animation
    /// frame: attachment offset x bone pose, in editor axes. Null when the model or the point is missing.
    /// `pose` / `timeMs`: posed by that skeleton at that time instead (the NPC viewer).
    std::optional<DirectX::XMFLOAT4X4> AttachmentMatrix(const std::string& model, uint32_t id, const MpqChain& mpq,
                                                        const ModelSkeleton* pose = nullptr, uint32_t timeMs = 0);

    /// One model of a preview scene (the NPC viewer), posed by `pose` at `timeMs` instead of the shared clock
    /// (null pose: the model's own animation on the shared clock).
    struct Part { ModelLook look; DirectX::XMFLOAT4X4 world; const ModelSkeleton* pose = nullptr; uint32_t timeMs = 0; };
    /// Draws parts into the render target bound now (the caller sets it, its viewport, and clears it).
    void DrawParts(const std::vector<Part>& parts, DirectX::FXMMATRIX viewProj, const MpqChain& mpq);
    /// What the NPC viewer lists about a model: its own skeleton (with the animation list), submesh ids, fixed
    /// textures and bounds (model space, editor axes). Null when it cannot be loaded.
    struct ModelInfo
    {
        std::shared_ptr<const ModelSkeleton> skeleton;
        std::vector<uint16_t> geosets;      // sorted, once each
        std::vector<std::string> textures;  // fixed textures
        std::vector<std::string> doodadSets; // WMO: its doodad sets by index (MODS names; set 0 always shows)
        DirectX::XMFLOAT3 boundsMin{}, boundsMax{};
    };
    std::optional<ModelInfo> Info(const std::string& model, const MpqChain& mpq);
    /// Every loaded object (previews excluded) with its box centre in the world.
    void ForEachObject(const std::function<void(bool wmo, uint32_t uid, const DirectX::XMFLOAT3& center)>& fn) const;

    /// A small picture of a model for the catalog, rendered on first request when `render` is true (it loads the
    /// model, which can take a while for a big WMO); null until then, or when the model cannot be loaded.
    ID3D11ShaderResourceView* Thumbnail(const std::string& name, bool wmo, const MpqChain& mpq, bool render);
    bool ThumbnailFailed(const std::string& name) const;
    /// The same for models in looks (a creature with its skin and what it carries), cached under `key`: each part placed by
    /// its matrix, the view framed on the first.
    ID3D11ShaderResourceView* LookThumbnail(const std::string& key, const std::vector<std::pair<ModelLook, DirectX::XMFLOAT4X4>>& parts,
                                            const MpqChain& mpq, bool render);
    /// Forget models and thumbnails that failed to load, so they are tried again (after a source was attached).
    void ForgetFailed()
    {
        std::erase_if(m_meshes, [](const auto& m) { return !m.second; });
        std::erase_if(m_thumbs, [](const auto& t) { return t.second.failed; });
    }

    size_t ModelCount() const { return m_meshes.size(); }
    size_t InstanceCount() const { return m_instances.size(); }
    size_t DrawnLastFrame() const { return m_drawn; }

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;

    struct GpuMesh
    {
        struct Batch
        {
            uint32_t start, count; ID3D11ShaderResourceView* texture; ModelMesh::Blend blend; uint32_t textureType; uint16_t geoset;
            uint16_t liquid = 0;                                                // WMO liquid: LiquidType id
            const std::vector<ID3D11ShaderResourceView*>* frames = nullptr;   // its animation (owned by Renderer)
            bool rawLiquid = false;                                             // magma / slime: texture colours as they are
            int16_t uvAnim = -1, weight = -1, color = -1;                       // texture animation links (ModelSkeleton)
            bool twoSided = true;                                               // false: back faces hidden
        };
        Com<ID3D11Buffer> vertices, indices;
        std::vector<Batch> batches;
        DirectX::XMFLOAT3 center{};
        float radius = 0;
        bool wmo = false;
        DirectX::XMFLOAT3 boundsMin{}, boundsMax{};
        std::vector<DirectX::XMFLOAT3> positions;   // CPU copy for picking
        std::vector<uint32_t> triangles;
        std::shared_ptr<const ModelSkeleton> skeleton;   // posed every frame when set
        std::vector<ModelMesh::Doodad> doodads;           // WMO: its doodads and sets (see ModelMesh)
        std::vector<ModelMesh::DoodadSet> doodadSets;
        std::vector<std::string> textureNames;            // the batches' fixed textures, once each
    };
    struct Look;
    struct Instance
    {
        GpuMesh* mesh = nullptr;
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT3 center;
        float radius = 0;
        int refs = 0;
        bool wmo = false;
        int layer = 0;   // -1: preview
        uint32_t uid = 0;
        const Look* look = nullptr;   // null: the model's own textures, every submesh
        bool attached = false;        // a WMO's doodad: picks as its WMO, no outline or box select of its own
    };
    /// A ModelLook resolved to GPU textures (shared by every instance with the same look).
    struct Look
    {
        std::array<ID3D11ShaderResourceView*, 32> textures{};   // by M2 texture type; null = keep the batch's
        std::vector<uint16_t> geosets;
    };

    /// World distance along a unit ray to an instance's triangles, within `maxDistance`.
    std::optional<float> HitDistance(const Instance& inst, DirectX::FXMVECTOR origin, DirectX::FXMVECTOR dir, float maxDistance) const;
    GpuMesh* Mesh(const std::string& name, bool wmo, const MpqChain& mpq);
    /// A look resolved to GPU textures, shared by every user of the same look.
    const Look* ResolveLook(const ModelLook& look, const MpqChain& mpq);
    struct Thumb;
    /// Renders meshes (each with a world matrix and look) into a fresh thumbnail, framed on the first one.
    void RenderThumb(Thumb& thumb, const std::vector<std::tuple<GpuMesh*, DirectX::XMFLOAT4X4, const Look*>>& parts);
    struct Run { GpuMesh* mesh; UINT first, count; const Look* look = nullptr; const ModelSkeleton* pose = nullptr; uint32_t timeMs = 0; };
    /// Draws runs of instances already written to the instance buffer: opaque pass, then blended pass.
    void Submit(const std::vector<Run>& runs, DirectX::FXMMATRIX viewProj);
    void AddInstance(int tile, const std::string& key, GpuMesh* mesh, DirectX::FXMMATRIX world, float scale, uint32_t uid, int layer,
                     const Look* look = nullptr);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    Renderer* m_textures = nullptr;
    class Loader* m_loader = nullptr;

    Com<ID3D11VertexShader> m_vs;
    Com<ID3D11PixelShader> m_ps;
    Com<ID3D11InputLayout> m_layout;
    Com<ID3D11Buffer> m_frameCb, m_batchCb, m_skinCb, m_instanceBuffer;
    Com<ID3D11Buffer> m_boneBuffer;            // bone palettes of the meshes drawn this frame; entry 0 = identity
    Com<ID3D11ShaderResourceView> m_boneSrv;
    UINT m_boneCapacity = 0;
    std::chrono::steady_clock::time_point m_start;   // animation clock
    UINT m_instanceCapacity = 0;
    Com<ID3D11SamplerState> m_sampler;
    Com<ID3D11RasterizerState> m_noCull, m_cullBack;
    Com<ID3D11DepthStencilState> m_depth, m_depthNoWrite;
    Com<ID3D11BlendState> m_alphaBlend, m_additive;

    std::unordered_map<std::string, std::unique_ptr<GpuMesh>> m_meshes;   // null entry = failed to load
    std::unordered_map<std::string, Instance> m_instances;               // key: kind, unique id, position
    std::unordered_map<std::string, std::unique_ptr<Look>> m_looks;      // key: ModelLook::Key()
    std::map<int, std::vector<std::string>> m_tileInstances;
    struct Thumb { Com<ID3D11ShaderResourceView> srv; bool failed = false; };
    std::unordered_map<std::string, Thumb> m_thumbs;   // by lower-case name
    Com<ID3D11DepthStencilView> m_thumbDepth;
    size_t m_drawn = 0;
};
