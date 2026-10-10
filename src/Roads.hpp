#pragma once

#include "Changes.hpp"
#include "Formats.hpp"

#include <DirectXMath.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

/// A point of a road's centre line, in the editor's axes; width 0 = the road's own width.
struct RoadPoint
{
    DirectX::XMFLOAT3 pos{};
    float width = 0;
};

/// An editor-only road: a spline through its points that paints and grades the terrain under it. The game never sees
/// the road itself, only the terrain it leaves (on export, or once baked).
struct Road
{
    uint32_t id = 0;
    std::string name, map;
    std::vector<RoadPoint> points;
    float width = 6;              // yards, edge to edge of the centre texture
    std::string texture;          // centre (cobbles, dirt); empty = no paint
    std::string shoulderTexture;  // around the centre (worn dirt); empty = none
    float shoulder = 3;           // yards of shoulder on each side
    float noise = 0.5f;           // 0..1: how ragged the texture edges are
    float grade = 0.5f;           // 0..1: how far the ground is pulled to the road's height (level side to side)
    float sink = 0.2f;            // yards the road sits below its centre line
    bool followGround = true;     // the line's height is the ground's, smoothed along the road (off: straight between the points' heights)

    nlohmann::json ToJson() const;
    static Road FromJson(const nlohmann::json& j);
};

/// The centre line sampled every yard or so: position, width there, and the distance along the road.
struct RoadSample { DirectX::XMFLOAT3 pos; float width; };
/// The ground's height at a point (the terrain under the roads); none where it is not known.
using RoadGround = std::function<std::optional<float>(float x, float z)>;
/// With `ground` and followGround, each sample's height is the ground under it averaged over a stretch of road.
std::vector<RoadSample> SampleRoad(const Road& road, const RoadGround& ground = {});
/// How far from its centre line a road changes anything.
float RoadReach(const Road& road);

/// Paints and grades one chunk with every road given (in order), `textures` being the list its texture ids index (new
/// names are added). True when the chunk changed.
/// A road's line, worked out once and reused for many chunks (ApplyRoads samples it itself without).
using RoadLines = std::function<const std::vector<RoadSample>&(const Road&)>;
/// `paint` false: heights only.
bool ApplyRoads(const std::vector<const Road*>& roads, AdtChunk& chunk, std::vector<std::string>& textures, const RoadGround& ground = {},
                bool paint = true, const RoadLines& lines = {});

/// The project's roads. Domain "editor.roads", change data { "id", "before": road | null, "after": road | null }.
/// A preview (the road being dragged) shows in place of its saved version until cleared or committed.
class RoadStore final : public Adapter
{
public:
    explicit RoadStore(ChangeStore& store) : m_store(store) {}
    const char* Domain() const override { return "editor.roads"; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }

    const Road* Find(uint32_t id) const;
    /// The road as last committed (ignoring a preview).
    const Road* Saved(uint32_t id) const { auto it = m_roads.find(id); return it == m_roads.end() ? nullptr : &it->second; }
    /// Roads of a map as they show (previews included), by id.
    std::vector<const Road*> OnMap(const std::string& map) const;
    uint32_t NextId() const;
    /// Every saved road (no previews), any map.
    std::vector<const Road*> All() const;

    /// A road change (null = none) for the caller to apply and commit (or batch).
    Change MakeChange(const Road* before, const Road* after, const std::string& label) const;
    /// MakeChange, applied and recorded.
    void Commit(const Road* before, const Road* after, const std::string& label);
    /// Shows `road` in place of its saved version (nullptr: back to the saved one).
    void Preview(const Road* road);
    bool Previewing() const { return m_preview.has_value(); }
    /// Forgets every road (project closed).
    void Clear() { m_roads.clear(); m_preview.reset(); ++m_version; }

    /// Chunk cells (grid x, z: map-wide, kChunkSize apart) of a map whose look changed: only where the road's line or
    /// settings differ, so moving one point of a long road redraws the stretch around it.
    using Cells = std::set<std::pair<int, int>>;
    std::function<void(const std::string& map, const Cells& cells)> onChanged;
    /// Bumped by every change and preview (for caches of the roads' lines).
    uint64_t Version() const { return m_version; }

private:
    void Set(const Change& change, bool after);
    void Changed(const Road* a, const Road* b);
    ChangeStore& m_store;
    std::map<uint32_t, Road> m_roads;
    std::optional<Road> m_preview;
    uint64_t m_version = 0;
};

/// The chunk cells a road reaches, each with a hash of what shapes the road there (its line nearby and its settings):
/// two versions of a road differ exactly where these hashes differ.
std::map<std::pair<int, int>, uint64_t> RoadCellHashes(const Road& road);

/// Road engine checks: spline through its points, chunk graded and painted under it, untouched away from it.
bool RoadSelfTest();
