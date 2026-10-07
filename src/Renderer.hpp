#pragma once

#include "Formats.hpp"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class MpqChain;

struct TileStats
{
    size_t chunks = 0, texturesLoaded = 0, texturesMissing = 0, doodads = 0, wmos = 0;
    float minHeight = 0, maxHeight = 0;
};

struct DrawOptions
{
    bool wireframe = false;
    bool showObjects = true;
    float textureRepeat = 8.0f;   // diffuse texture repeats per chunk
    int solo = 0;                 // non-zero: draw only that ghost layer, as if it were the map
    bool farTerrain = true;       // low-detail map (WDL) beyond the loaded tiles
    bool lod = true;              // fewer triangles with distance; far tiles drawn whole from a baked texture
    DirectX::XMFLOAT3 eye{};      // camera position, for the LOD distances
};

/// Overlay line vertex (brush ring, selection); drawn on top of everything.
struct LineVertex { DirectX::XMFLOAT3 pos; DirectX::XMFLOAT4 col; };

/// Appends a lit solid sphere (triangle list, light baked into the colours) for DrawSolids: path points, handles.
void AddSphere(std::vector<LineVertex>& triangles, const DirectX::XMFLOAT3& center, float radius, const DirectX::XMFLOAT4& color);
/// Appends a lit solid cone (8-sided) with its base disc at `base` and its point at `tip`: arrow heads.
void AddCone(std::vector<LineVertex>& triangles, const DirectX::XMFLOAT3& base, const DirectX::XMFLOAT3& tip, float radius, const DirectX::XMFLOAT4& color);
/// Appends a lit solid tube (8-sided) from `a` to `b` for DrawSolids: thick path lines.
void AddTube(std::vector<LineVertex>& triangles, const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float radius, const DirectX::XMFLOAT4& color);

/// The game's light where the camera is (Lights::At), for the viewport's game-lighting preview. Off: the editor's own
/// even daylight.
struct SceneLight
{
    bool on = false;
    DirectX::XMFLOAT3 ambient{}, diffuse{}, fog{};
    float fogStart = 0, fogEnd = 0;   // yards from the eye
    std::array<DirectX::XMFLOAT3, 5> sky{};   // top, middle, middle to horizon, above horizon, horizon
};

/// Draws the loaded ADT tiles: blended terrain plus boxes for doodad and WMO placements.
class Renderer
{
public:
    /// A map chunk as it should show, when something is drawn on top of the data it was given (roads): the chunk and the
    /// texture list its ids index. Layer 0 tiles only; nothing = show the chunk as given.
    struct ShownChunk { AdtChunk chunk; std::vector<std::string> textures; };
    /// `paint` false: only the heights are wanted (a height change).
    using ChunkView = std::function<std::optional<ShownChunk>(int key, size_t index, const AdtChunk& chunk, const std::vector<std::string>& textures, bool paint)>;
    void SetChunkView(ChunkView view) { m_chunkView = std::move(view); }

    void SetSceneLight(const SceneLight& light) { m_light = light; }
    const SceneLight& Light() const { return m_light; }
    /// The sky gradient of the scene light behind everything (call first; nothing when the scene light is off).
    void DrawSky(DirectX::FXMMATRIX viewProj);

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);

    /// Adds (or replaces) tile `key`; textures come from the MPQ chain and are shared between tiles.
    /// Layer 0 is the map. Any other layer is a ghost, drawn last, tinted and see-through over the map:
    /// -1 is the paste preview, positive layers are other versions of the map.
    TileStats LoadTile(int key, const Adt& adt, const MpqChain& mpq, int layer = 0);
    void UnloadTile(int key) { m_tiles.erase(key); }
    void UnloadLayer(int layer) { std::erase_if(m_tiles, [&](const auto& t) { return t.second.layer == layer; }); }
    void Clear() { m_tiles.clear(); }
    /// A ghost layer's tint (rgb) and opacity (a), and whether it shows.
    void SetLayerStyle(int layer, DirectX::XMFLOAT4 tint, bool visible) { m_layers[layer] = { tint, visible }; }

    /// Re-uploads one chunk's vertices after its heights changed.
    void UpdateChunk(int key, size_t index, const AdtChunk& chunk);
    /// Changes one chunk's hole mask and rebuilds that tile's triangles.
    void SetChunkHoles(int key, size_t index, uint16_t holes);
    /// Rebuilds a tile's liquid surfaces (after a paste changed them, or to preview a paste's).
    void UpdateWater(int key, const std::vector<AdtLiquid>& liquids, const MpqChain& mpq);
    /// Rebinds one chunk's texture layers and alpha map after a texture edit.
    void UpdateChunkTextures(int key, size_t index, const AdtChunk& chunk, const std::vector<std::string>& textures, const MpqChain& mpq);

    void Draw(DirectX::FXMMATRIX viewProj, const DrawOptions& options);
    /// Translucent liquid surfaces (MH2O); call after the opaque world (terrain and models).
    void DrawWater(DirectX::FXMMATRIX viewProj, int solo = 0);
    /// The whole map at low detail (ParseWdl output), shaded by slope and height; replaces any earlier one.
    void LoadFar(const std::vector<std::vector<int16_t>>& tiles);
    void ClearFar() { m_farVertices.Reset(); m_farIndices.Reset(); m_farTiles.clear(); }
    /// Draws the low-detail tiles that have no loaded tile (call first, with a projection reaching far; then
    /// clear depth and draw the rest).
    void DrawFar(DirectX::FXMMATRIX viewProj);
    /// Lines on top of everything drawn so far (brush rings, outlines); call last.
    void DrawOverlay(DirectX::FXMMATRIX viewProj, const std::vector<LineVertex>& overlay);
    /// Solid coloured triangles (LineVertex triangle list), depth-tested like the world: spheres for path points.
    void DrawSolids(DirectX::FXMMATRIX viewProj, const std::vector<LineVertex>& triangles);

    /// A texture from the MPQs, shared with every user of this renderer; white when missing or empty.
    ID3D11ShaderResourceView* TextureFor(const std::string& name, const MpqChain& mpq);
    /// The animated frames of a LiquidType (LiquidType.dbc Texture[0], "...%d.blp" numbered from 1); empty when it
    /// has none. Shared by terrain water and WMO liquids.
    const std::vector<ID3D11ShaderResourceView*>& LiquidFrames(uint16_t type, const MpqChain& mpq);
    /// The frame of a liquid animation to show now (about 15 frames a second).
    size_t LiquidFrame(size_t frames) const;
    /// Forget textures that failed to load, so they are tried again (after a source was attached).
    /// Textures decoded in the background are taken from here instead of being read and decoded on this thread.
    void SetLoader(class Loader* loader) { m_loader = loader; }
    /// Uploads a texture decoded elsewhere under its name, unless one is cached already.
    void CacheTexture(const std::string& name, const BlpImage& image);
    void ForgetMissingTextures() { std::erase_if(m_textureCache, [](const auto& t) { return !t.second; }); }
    /// A texture from raw RGBA pixels (thumbnails).
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateRgbaTexture(UINT width, UINT height, const std::vector<uint8_t>& rgba);
    /// The texture if it is already loaded (nullptr otherwise, or the white texture when it failed to load).
    ID3D11ShaderResourceView* CachedTexture(const std::string& name) const;

private:
    class Loader* m_loader = nullptr;
    SceneLight m_light;
    ChunkView m_chunkView;
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    Com<ID3D11Buffer> m_farVertices, m_farIndices;
    std::vector<std::pair<int, UINT>> m_farTiles;   // tile key, first index (1536 indices each)

    struct TerrainVertex { DirectX::XMFLOAT3 pos; DirectX::XMFLOAT3 nrm; DirectX::XMFLOAT2 uv; uint32_t col; };   // col: MCCV as RGBA8, 0x7F neutral
    /// Level of detail: 0 full (256 triangles a chunk), 1 outer grid (128), 2 coarse (about 72). Every level keeps
    /// all nine vertices on each chunk edge, so neighbouring chunks at different levels never open a crack.
    static constexpr int kLods = 3;
    struct ChunkDraw
    {
        std::array<UINT, kLods> indexStart{}, indexCount{};
        std::array<ID3D11ShaderResourceView*, 4> textures{};
        Com<ID3D11ShaderResourceView> alpha;
    };
    struct TileGpu
    {
        Com<ID3D11Buffer> vertices, indices, lines, water;
        UINT lineVertexCount = 0, waterVertexCount = 0;
        struct WaterRun { uint16_t type; UINT first, count; };   // water vertices of one LiquidType, contiguous
        std::vector<WaterRun> waterRuns;
        std::vector<TerrainVertex> cpuVertices;
        std::vector<uint16_t> holes;   // per chunk
        std::vector<ChunkDraw> chunks;
        std::array<UINT, kLods> lodStart{}, lodCount{};   // every chunk of the tile at one level, contiguous
        DirectX::XMFLOAT3 boundsMin{}, boundsMax{};
        int layer = 0;
        Com<ID3D11ShaderResourceView> baked;   // the tile's blended, lit terrain seen from above (far drawing)
        Com<ID3D11RenderTargetView> bakedRtv;
        bool bakeDirty = true;
    };
    void BuildWater(TileGpu& tile, const std::vector<AdtLiquid>& liquids, const MpqChain& mpq);
    /// Renders a tile's terrain from above into its baked texture (restores the caller's render target).
    void BakeTile(TileGpu& tile, const DrawOptions& options);
    Com<ID3D11VertexShader> m_bakedVs;
    Com<ID3D11PixelShader> m_bakedPs;
    Com<ID3D11DepthStencilView> m_bakeDepth;
    struct LayerStyle { DirectX::XMFLOAT4 tint{ 0.45f, 0.75f, 1.0f, 0.5f }; bool visible = true; };
    std::map<int, LayerStyle> m_layers;

    ID3D11ShaderResourceView* Texture(const std::string& name, const MpqChain& mpq, TileStats& stats);
    Com<ID3D11ShaderResourceView> CreateTexture(const BlpImage& image);
    static void BuildChunkVertices(TileGpu& tile, size_t index, const AdtChunk& chunk);
    void BuildIndices(TileGpu& tile);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    Com<ID3D11VertexShader> m_terrainVs, m_lineVs;
    Com<ID3D11PixelShader> m_terrainPs, m_linePs, m_waterPs;
    Com<ID3D11VertexShader> m_waterVs;
    Com<ID3D11VertexShader> m_skyVs;
    Com<ID3D11PixelShader> m_skyPs;
    std::chrono::steady_clock::time_point m_start;   // liquid animation clock
    Dbc m_liquidTypes;
    bool m_liquidTypesRead = false;
    std::map<uint16_t, std::vector<ID3D11ShaderResourceView*>> m_liquidFrames;
    Com<ID3D11InputLayout> m_terrainLayout, m_lineLayout;
    Com<ID3D11Buffer> m_frameCb;
    Com<ID3D11SamplerState> m_wrap, m_clamp;
    Com<ID3D11RasterizerState> m_solid, m_wire;
    Com<ID3D11DepthStencilState> m_depth, m_noDepth, m_ghostDepth;
    Com<ID3D11BlendState> m_ghostBlend;
    Com<ID3D11ShaderResourceView> m_white;

    Com<ID3D11Buffer> m_overlay;
    UINT m_overlayCapacity = 0;
    Com<ID3D11Buffer> m_solids;
    UINT m_solidsCapacity = 0;
    std::map<int, TileGpu> m_tiles;
    std::unordered_map<std::string, Com<ID3D11ShaderResourceView>> m_textureCache;
};
