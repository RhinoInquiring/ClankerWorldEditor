#pragma once

#include "Changes.hpp"
#include "Formats.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <compare>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Loader;
class MpqChain;
class Renderer;
struct TileStats;

constexpr int TileKey(int x, int y) { return y * 64 + x; }

struct LoadedTile
{
    int x = 0, y = 0;
    std::vector<uint8_t> bytes;        // the original ADT as read from the MPQs
    Adt adt;                           // parsed, with current (edited) heights
    std::array<int16_t, 256> byGrid;   // local chunk cell (row * 16 + col) -> index in adt.chunks, -1 if absent
    float maxHeight = 0;

    int Key() const { return TileKey(x, y); }

    /// A parsed tile with its grid lookup filled in.
    static LoadedTile Make(int x, int y, std::vector<uint8_t> bytes, Adt adt);
};

/// A chunk anywhere on the map: tile key plus index into that tile's chunk list.
struct ChunkRef
{
    int tile = 0;
    int chunk = 0;
    auto operator<=>(const ChunkRef&) const = default;
};

struct Brush
{
    enum class Mode { Raise, Lower, Flatten, Smooth };
    Mode mode = Mode::Raise;
    float radius = 20.0f;     // yards
    float strength = 25.0f;   // yards per second at the centre (Raise/Lower), blend rate otherwise
};

struct TerrainHit { DirectX::XMFLOAT3 pos; ChunkRef chunk; };

struct PaintBrush
{
    float radius = 12.0f;    // yards
    float pressure = 0.5f;   // 0..1: how fast the texture builds up
    float hardness = 0.5f;   // 0: soft falloff over the whole radius, 1: hard edge
};

/// A placed object, by kind and unique id (one object may be listed by several tiles).
struct ObjectRef
{
    bool wmo = false;
    uint32_t uid = 0;
    auto operator<=>(const ObjectRef&) const = default;
};

/// Copied chunks: grid offset from the copy's first chunk plus absolute heights.
struct TerrainClipboard
{
    struct Entry
    {
        int dx = 0, dz = 0;
        std::array<float, 145> heights{};   // absolute
        nlohmann::json layers;              // texture names, flags, effects, alpha (see LayerState)
        uint16_t holes = 0;
        nlohmann::json liquids;             // LiquidState (absolute heights); null = leave the target's water alone
    };
    std::vector<Entry> chunks;
    // Objects standing on the copied chunks: x and z relative to the first chunk's corner, y absolute.
    std::vector<DoodadPlacement> doodads;
    std::vector<WmoPlacement> wmos;
    int originX = 0, originZ = 0;   // grid cell the first chunk was copied from (for paste in place)
    bool Empty() const { return chunks.empty(); }
    int Width() const { int w = 0; for (const auto& e : chunks) w = std::max(w, e.dx + 1); return w; }
    int Depth() const { int d = 0; for (const auto& e : chunks) d = std::max(d, e.dz + 1); return d; }
    float MinHeight() const;
    float MeanHeight() const;
    /// Lossless JSON (heights as base64 floats) for blueprints.
    nlohmann::json ToJson() const;
    static TerrainClipboard FromJson(const nlohmann::json& j);
    /// The copy as a stand-alone tile at the spot it was copied from (for rendering previews).
    Adt ToAdt() const;
    /// Quarter turn clockwise seen from above: chunk layout, heights, alpha maps and objects; lossless.
    void RotateClockwise();
};

struct PasteOptions
{
    bool heights = true, textures = true, holes = true, objects = true, water = true;
    float slopeX = 0, slopeZ = 0;   // tilt added to the copy, yards per vertex step from its first chunk's corner
    bool blend = true;          // blend the seam into the ground around the paste
    float widthYards = 0;       // blend band per side; 0 = automatic from the height mismatch
};

/// What a paste would do, computed once and used for the preview, the ghost and the commit alike.
struct PastePlan
{
    struct Chunk
    {
        ChunkRef ref;
        bool setHeights = false;
        std::array<float, 145> heights{};   // MCVT values (relative to the chunk's base)
        nlohmann::json layers;              // LayerState; null = textures unchanged
        std::optional<uint16_t> holes;      // hole mask; none = unchanged
        nlohmann::json liquids;             // LiquidState; null = water unchanged, [] = no water
    };
    std::vector<Chunk> chunks;      // every chunk the paste changes, the blend band included
    std::vector<ChunkRef> footprint;
    std::vector<DoodadPlacement> doodads;   // objects to add, world positions (unique ids assigned on apply)
    std::vector<WmoPlacement> wmos;
    float widthYards = 0;           // blend width used (the automatic value when PasteOptions said 0)
};

/// Terrain heights for one map: streams tiles around the camera, sculpts, copies and pastes.
class TerrainAdapter final : public Adapter
{
public:
    TerrainAdapter(MpqChain& mpq, Renderer& renderer, ChangeStore& store) : m_mpq(mpq), m_renderer(renderer), m_store(store) {}

    const char* Domain() const override { return "terrain.heights"; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }

    /// Switches to a map (unloading every tile); false when the map has no WDT.
    bool SetMap(const std::string& directory, std::string& error);
    void Unload();
    const std::string& Map() const { return m_map; }
    const std::vector<bool>& Present() const { return m_present; }

    /// Loads missing tiles near (x, z) and drops tiles beyond the radius; pinned tiles load first and are never
    /// dropped. Without a loader: at most one tile, read on this thread. With one (SetLoader): the loader prepares
    /// tiles in the background and this uploads finished ones until `budgetMs` is spent. Returns what it loaded.
    std::vector<std::pair<int, TileStats>> Stream(float x, float z, int radius, std::string& error, float budgetMs = 6.0f);
    void SetLoader(Loader* loader) { m_loader = loader; }
    /// Tiles that must stay loaded whatever the camera does (a selection, a paste's footprint).
    void SetPinned(std::set<int> keys) { m_pinned = std::move(keys); }
    /// How many of these tiles (present on the map) are not loaded yet.
    size_t MissingTiles(const std::set<int>& keys) const;

    const std::map<int, LoadedTile>& Tiles() const { return m_tiles; }
    const AdtChunk* Chunk(ChunkRef ref) const;
    /// Global chunk grid position (0..1023 on each axis) of a chunk.
    std::pair<int, int> GridOf(ChunkRef ref) const;
    std::optional<ChunkRef> ChunkAtGrid(int gx, int gz) const;
    std::optional<float> HeightAt(float x, float z) const;

    /// Nearest terrain hit along a ray; `throughHoles` also hits the ground where holes are cut.
    std::optional<TerrainHit> Pick(DirectX::FXMVECTOR origin, DirectX::FXMVECTOR dir, bool throughHoles = false) const
    {
        return PickIn(m_tiles, origin, dir, throughHoles);
    }
    /// The same over any set of tiles (a ghost layer's); the hit's chunk refers to those tiles.
    static std::optional<TerrainHit> PickIn(const std::map<int, LoadedTile>& tiles, DirectX::FXMVECTOR origin, DirectX::FXMVECTOR dir,
                                            bool throughHoles = false);

    /// Hole cells (chunk, bit 0..15) within `radius` of a point; the cell under the point is always included.
    std::vector<std::pair<ChunkRef, int>> HoleCellsAt(const DirectX::XMFLOAT3& point, float radius) const;
    void BeginHoles() { m_holing = true; m_holeStroke.clear(); }
    void HoleStep(const DirectX::XMFLOAT3& center, float radius, bool cut);
    std::optional<Change> EndHoles();
    bool HoleStroking() const { return m_holing; }

    /// Zone painting: sets the area id (AreaTable) of every chunk within `radius` of a point (the chunk under it always).
    /// One change per stroke.
    void BeginAreas() { m_areaing = true; m_areaStroke.clear(); }
    void AreaStep(const DirectX::XMFLOAT3& center, float radius, uint32_t area);
    std::optional<Change> EndAreas(const std::string& label);
    bool AreaStroking() const { return m_areaing; }
    /// Chunks within `radius` of a point; the chunk under the point is always included.
    std::vector<ChunkRef> ChunksAt(const DirectX::XMFLOAT3& point, float radius) const;

    /// Texture painting: raises (or with erase, lowers) one texture's share under the brush. A chunk holds four
    /// textures; painting a fifth replaces the chunk's weakest one. One change per stroke.
    void BeginPaint() { m_painting = true; m_paintBefore.clear(); }
    void PaintStep(const DirectX::XMFLOAT3& center, const PaintBrush& brush, const std::string& texture, bool erase, float dt);
    std::optional<Change> EndPaint(const std::string& label);
    bool Painting() const { return m_painting; }
    /// The texture with the largest share at a point (the eyedropper).
    std::optional<std::string> TextureAt(float x, float z) const;

    void BeginStroke(const TerrainHit& at);
    void StrokeStep(const DirectX::XMFLOAT3& center, const Brush& brush, float dt);
    std::optional<Change> EndStroke(const Brush& brush);
    bool Stroking() const { return m_stroking; }

    TerrainClipboard Copy(const std::set<ChunkRef>& chunks) const;
    /// Copy from any set of tiles (a ghost layer's, say) by global grid cells.
    static TerrainClipboard CopyFrom(const std::map<int, LoadedTile>& tiles, const std::set<std::pair<int, int>>& cells);
    /// Works out a paste with the clipboard's first chunk on grid cell (gx, gz). Loaded chunks only.
    PastePlan PlanPaste(const TerrainClipboard& clip, int gx, int gz, float heightOffset, const PasteOptions& options) const;
    /// Applies a plan to the terrain and returns its change.
    std::optional<Change> ApplyPlan(const PastePlan& plan, const std::string& label);
    /// Shows a plan on the terrain without changing any data (nullptr puts the real terrain back).
    void PreviewPlan(const PastePlan* plan);
    /// Turns the chunks a quarter turn clockwise about their centre, with the objects standing on them (same unique
    /// ids, one change). `footprint` gets the turned chunks.
    std::optional<Change> RotateInPlace(const std::set<ChunkRef>& chunks, std::vector<ChunkRef>* footprint = nullptr);
    /// Hard-edged paste without a preview and without objects (rotate in place moves those itself).
    std::optional<Change> Paste(const TerrainClipboard& clip, int gx, int gz, float heightOffset, bool heights, bool textures);

    /// Chunks the clipboard would cover with its first chunk on (gx, gz), loaded tiles only.
    std::vector<ChunkRef> Footprint(const TerrainClipboard& clip, int gx, int gz) const;
    /// Lowest and mean ground height under that footprint, if any of it is loaded.
    std::optional<float> FootprintMin(const TerrainClipboard& clip, int gx, int gz) const;
    std::optional<float> FootprintMean(const TerrainClipboard& clip, int gx, int gz) const;
    /// Plane (offset, slopeX, slopeZ) that best fits ground minus copy over the footprint's outer vertices.
    std::optional<std::array<float, 3>> FitSlope(const TerrainClipboard& clip, int gx, int gz) const;
    /// A placed object as the first loaded tile listing it has it.
    std::optional<DoodadPlacement> FindDoodad(uint32_t uid) const;
    std::optional<WmoPlacement> FindWmo(uint32_t uid) const;

    /// Doodads placed on the map (MDDF) that stand inside these WMOs' own bounding boxes, tested in each WMO's frame so
    /// a turned building does not catch what is only near it: furniture that belongs to the building but is not part of the WMO.
    std::set<ObjectRef> DoodadsInside(const std::set<ObjectRef>& objects) const;

    /// Object edit: snapshot the objects, show transforms of that snapshot live, then commit or cancel.
    /// The transforms always start from the snapshot, so a drag is one change however long it runs.
    void BeginObjectEdit(const std::set<ObjectRef>& objects);
    void PreviewObjectEdit(const std::function<void(DoodadPlacement&)>& doodad, const std::function<void(WmoPlacement&)>& wmo);
    std::optional<Change> EndObjectEdit(const std::string& label);
    void CancelObjectEdit();
    bool ObjectEditing() const { return !m_objectEdit.empty(); }
    std::optional<Change> DeleteObjects(const std::set<ObjectRef>& objects);
    /// New objects at world positions (unique ids are assigned; WMO bounds are computed). `placed` gets their refs.
    std::optional<Change> PlaceObjects(const std::vector<DoodadPlacement>& doodads, const std::vector<WmoPlacement>& wmos,
                                       const std::string& label, std::vector<ObjectRef>* placed = nullptr);

    /// Tiles whose objects changed since the last call (the model renderer reloads them).
    std::set<int> TakeObjectChanges() { return std::exchange(m_objectsChanged, {}); }

    /// A plan's chunks as a stand-alone tile, for the see-through ghost.
    Adt BuildGhost(const PastePlan& plan) const;

    /// Writes every tile that has applied changes as a patched ADT under outDir; returns files written.
    /// With problems: a tile that fails is recorded there and skipped (the others are still written), and tiles
    /// whose ground effects had to be dropped are listed as warnings; without: the first failure stops the export.
    /// A tile of the client with the project's height edits applied (heights only; none when the map lacks it).
    std::optional<Adt> EditedHeights(const std::string& map, int x, int y) const;
    /// Edited chunks whose outer vertices do not meet a neighbour's copy of the same vertices (the client shows a
    /// gap there): one problem per tile, from the applied changes and the client's tiles (loaded or not).
    void FindCracks(std::vector<Problem>& problems) const;

    size_t Export(const std::filesystem::path& outDir, std::string& error, std::vector<std::filesystem::path>* files = nullptr,
                  std::vector<Problem>* problems = nullptr) const;

    /// A chunk's texture layers as JSON: names, MCLY flags, effect ids, base64 alpha (3 channels).
    static nlohmann::json LayerState(const LoadedTile& tile, const AdtChunk& chunk);
    /// Sets a chunk's layers from LayerState JSON (adding texture names to the tile's list as needed).
    static void SetLayerState(LoadedTile& tile, AdtChunk& chunk, const nlohmann::json& state);
    /// A chunk's liquid instances as JSON (absolute heights, base64 exists/extra), [] when it has none.
    static nlohmann::json LiquidState(const Adt& adt, const AdtChunk& chunk);
    /// Replaces a chunk's liquid instances with LiquidState JSON ([] removes its water).
    static void SetLiquidState(Adt& adt, const AdtChunk& chunk, const nlohmann::json& state);
    /// LiquidState turned a quarter clockwise within its chunk, the way TerrainClipboard::RotateClockwise turns heights.
    static nlohmann::json RotateLiquidState(const nlohmann::json& state);

    /// Applies the project's terrain changes for `map` (heights, layers, holes, areas, water, objects) to a tile read
    /// from the client, in order; true when objects changed. Static and self-contained: safe on a worker thread.
    static bool ReplayEdits(LoadedTile& tile, const std::string& map, const std::vector<Change>& done);

    /// Tiles of `map` that the project's applied changes touch.
    std::set<int> EditedTiles(const std::string& map) const;

private:
    using Edits = std::map<std::tuple<int, int, int>, std::pair<float, float>>;   // (tile, chunk, vertex) -> before, after

    void Set(const Change& change, bool after);
    void RefreshTextures(int key, size_t chunk);
    bool LoadTile(int x, int y, TileStats& stats, std::string& error);
    /// Replays the project's edits on a parsed tile and uploads it.
    TileStats FinishTile(int x, int y, std::vector<uint8_t> bytes, Adt adt);
    Change MakeChange(const Edits& edits, const nlohmann::json& layers, const std::string& label,
                      const nlohmann::json& holes = nlohmann::json::array(), const nlohmann::json& objects = nlohmann::json::array(),
                      const nlohmann::json& areas = nlohmann::json::array(), const nlohmann::json& liquids = nlohmann::json::array()) const;
    /// Adds (after) or removes (before) a change's objects on a loaded tile.
    void SetObjects(LoadedTile& tile, const nlohmann::json& objects, bool after);
    /// A WMO's world bounding box from its root file's bounds and the placement (unchanged if unreadable).
    void FitWmoExtents(WmoPlacement& w) const;
    uint32_t NextUniqueId() const;
    /// Adds objects to the loaded tiles under them and returns their "objects" entries (empty when none landed).
    nlohmann::json AddObjects(std::vector<DoodadPlacement> doodads, std::vector<WmoPlacement> wmos, std::vector<ObjectRef>* placed);

    MpqChain& m_mpq;
    Renderer& m_renderer;
    ChangeStore& m_store;

    std::string m_map;
    bool m_bigAlpha = false;
    std::vector<bool> m_present;
    std::map<int, LoadedTile> m_tiles;
    std::set<int> m_pinned;
    Loader* m_loader = nullptr;

    bool m_stroking = false;
    float m_flattenHeight = 0;
    Edits m_stroke;
    std::vector<ChunkRef> m_previewed;   // chunks the renderer currently shows from a plan
    std::set<int> m_previewedWater;      // tiles whose water the renderer currently shows from a plan
    bool m_holing = false;
    std::map<std::pair<int, int>, std::pair<uint16_t, uint16_t>> m_holeStroke;   // (tile, chunk) -> holes before, after
    bool m_areaing = false;
    std::map<std::pair<int, int>, std::pair<uint32_t, uint32_t>> m_areaStroke;   // (tile, chunk) -> area before, after
    std::set<int> m_objectsChanged;
    mutable std::optional<std::set<uint32_t>> m_groundEffects;   // GroundEffectTexture.dbc ids, read on first use
    /// The client has a GroundEffectTexture row for this id (0 = none counts as known; unreadable DBC: all known).
    bool KnownEffect(uint32_t id) const;
    mutable size_t m_effectsDropped = 0;
public:
    /// Ground effect ids export replaced with none because the client has no such row (since the last call).
    size_t TakeDroppedEffects() const { return std::exchange(m_effectsDropped, 0); }
private:
    bool m_painting = false;
    std::map<std::pair<int, int>, nlohmann::json> m_paintBefore;   // (tile, chunk) -> layers before the stroke
    struct ObjectSnap { int tile; bool wmo; DoodadPlacement doodad; WmoPlacement wmoPlacement; };
    std::vector<ObjectSnap> m_objectEdit;
    mutable std::map<std::string, std::optional<std::array<float, 6>>> m_wmoBounds;   // root bounds by model name
};

/// Clipboard rotation: one quarter turn moves corners as expected and four restore the original.
bool TerrainSelfTest();
