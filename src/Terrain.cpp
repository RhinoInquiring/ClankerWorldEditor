#include "Terrain.hpp"

#include "Mpq.hpp"
#include "Renderer.hpp"
#include "Blend.hpp"
#include "Models.hpp"
#include "Catalog.hpp"
#include "Loader.hpp"

#include <DirectXCollision.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

using namespace DirectX;
namespace fs = std::filesystem;

// Change payload: { "map": "Azeroth", "edits": [[tileX, tileY, chunk, vertex, before, after], ...],
//                  "layers": [[tileX, tileY, chunk, beforeState, afterState], ...],    (optional)
//                  "holes":  [[tileX, tileY, chunk, before, after], ...],                (optional)
//                  "areas":  [[tileX, tileY, chunk, before, after], ...],                (optional; MCNK area id)
//                  "colors": [[tileX, tileY, chunk, before, after], ...],                (optional; MCCV, base64 145 x BGRA)
//                  "objects": [[tileX, tileY, "m2" | "wmo", before, after], ...] }     (optional; null before = added,
//                                                                                        null after = deleted; the older
//                                                                                        4-element form is an addition)

namespace
{
    /// Position of MCVT vertex j inside its chunk, in units of kUnitSize.
    void VertexXZ(size_t j, float& x, float& z)
    {
        const size_t row = j / 17, col = j % 17;
        const bool inner = col >= 9;
        x = inner ? float(col - 9) + 0.5f : float(col);
        z = inner ? float(row) + 0.5f : float(row);
    }

    const char* ModeName(Brush::Mode m)
    {
        switch (m)
        {
        case Brush::Mode::Raise: return "Raise";
        case Brush::Mode::Lower: return "Lower";
        case Brush::Mode::Flatten: return "Flatten";
        case Brush::Mode::Smooth: return "Smooth";
        }
        return "Edit";
    }

    int GridIndex(float base) { return int(std::lround(base / kChunkSize)); }

    nlohmann::json Vec3(const float v[3]) { return { v[0], v[1], v[2] }; }
    void Vec3(const nlohmann::json& j, float v[3]) { for (int i = 0; i < 3; ++i) v[i] = j[size_t(i)]; }

    nlohmann::json ToJson(const DoodadPlacement& p)
    {
        return { { "model", p.model }, { "pos", Vec3(p.pos) }, { "rot", Vec3(p.rot) }, { "scale", p.scale }, { "uid", p.uniqueId }, { "flags", p.flags } };
    }
    nlohmann::json ToJson(const WmoPlacement& p)
    {
        return { { "model", p.model }, { "pos", Vec3(p.pos) }, { "rot", Vec3(p.rot) }, { "min", Vec3(p.extMin) }, { "max", Vec3(p.extMax) },
                 { "uid", p.uniqueId }, { "set", p.doodadSet }, { "flags", p.flags }, { "nameSet", p.nameSet } };
    }
    DoodadPlacement DoodadFrom(const nlohmann::json& j)
    {
        DoodadPlacement p{ j.at("model") };
        Vec3(j.at("pos"), p.pos);
        Vec3(j.at("rot"), p.rot);
        p.scale = j.at("scale");
        p.uniqueId = j.at("uid");
        p.flags = j.value("flags", 0);
        return p;
    }
    WmoPlacement WmoFrom(const nlohmann::json& j)
    {
        WmoPlacement p{ j.at("model") };
        Vec3(j.at("pos"), p.pos);
        Vec3(j.at("rot"), p.rot);
        Vec3(j.at("min"), p.extMin);
        Vec3(j.at("max"), p.extMax);
        p.uniqueId = j.at("uid");
        p.doodadSet = j.value("set", 0);
        p.flags = j.value("flags", 0);
        p.nameSet = j.value("nameSet", 0);
        return p;
    }

    /// Unique ids for objects the editor adds start here, far above the ids in Blizzard's 3.3.5 maps.
    constexpr uint32_t kEditorUniqueIdBase = 200'000'000;

    /// One "objects" entry applied forwards (after) or backwards to a tile's lists; objects keep their list position.
    void ApplyObjectEntry(Adt& adt, const nlohmann::json& e, bool after)
    {
        nlohmann::json from, to;
        if (e.size() == 4) { (after ? to : from) = e[3]; }
        else { from = e[after ? 3 : 4]; to = e[after ? 4 : 3]; }
        const uint32_t uid = (to.is_null() ? from : to).at("uid");
        auto apply = [&](auto& list, auto make) {
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& p) { return p.uniqueId == uid; });
            if (to.is_null()) { if (it != list.end()) list.erase(it); }
            else if (it != list.end()) *it = make(to);
            else list.push_back(make(to));
        };
        if (e[2] == "wmo") apply(adt.wmos, WmoFrom);
        else apply(adt.doodads, DoodadFrom);
    }

    uint32_t EntryUid(const nlohmann::json& e)
    {
        const nlohmann::json& p = e.size() == 4 || !e[3].is_null() ? e[3] : e[4];
        return p.at("uid");
    }
}

// ---------------------------------------------------------------------------------------------- tiles

bool TerrainAdapter::SetMap(const std::string& directory, std::string& error)
{
    if (directory == m_map) return true;
    const auto wdt = m_mpq.Read("World\\Maps\\" + directory + "\\" + directory + ".wdt");
    if (!wdt) { error = "Map not found: " + directory; return false; }
    Unload();
    m_map = directory;
    m_bigAlpha = WdtBigAlpha(*wdt);
    m_vertexColors = WdtVertexColors(*wdt);
    m_present = WdtTiles(*wdt);
    m_globalWmo = WdtGlobalWmo(*wdt);
    return true;
}

void TerrainAdapter::Unload()
{
    m_tiles.clear();
    m_renderer.Clear();
    m_map.clear();
    m_present.clear();
    m_globalWmo.reset();
    m_stroking = false;
    m_stroke.clear();
    m_previewed.clear();
}

LoadedTile LoadedTile::Make(int x, int y, std::vector<uint8_t> bytes, Adt adt)
{
    LoadedTile tile{ x, y, std::move(bytes), std::move(adt) };
    tile.byGrid.fill(-1);
    for (size_t i = 0; i < tile.adt.chunks.size(); ++i)
    {
        const AdtChunk& c = tile.adt.chunks[i];
        const int col = GridIndex(c.baseX) - x * 16, row = GridIndex(c.baseZ) - y * 16;
        if (col >= 0 && col < 16 && row >= 0 && row < 16) tile.byGrid[size_t(row * 16 + col)] = int16_t(i);
    }
    return tile;
}

std::map<int, size_t> TerrainAdapter::EditHashes(const std::vector<Change>& done, const std::string& map)
{
    std::map<int, size_t> out;
    for (const ChangeStore::Part& c : ChangeStore::Parts(done))
    {
        if (c.domain != "terrain.heights" || c.data.value("map", std::string()) != map) continue;
        for (const char* field : { "edits", "layers", "holes", "areas", "colors", "liquids", "objects", "tiles" })
            for (const auto& e : c.data.value(field, nlohmann::json::array()))
            {
                size_t& h = out[TileKey(e[0], e[1])];
                h = h * 1000003u ^ std::hash<std::string>{}(e.dump());
            }
    }
    return out;
}

bool TerrainAdapter::LoadNow(int x, int y, std::string& error)
{
    const int key = TileKey(x, y);
    if (m_tiles.count(key)) return true;
    if (size_t(key) >= m_present.size() || !m_present[size_t(key)]) { error = "the map has no tile there"; return false; }
    TileStats stats;
    return LoadTile(x, y, stats, error);
}

bool TerrainAdapter::LoadTile(int x, int y, TileStats& stats, std::string& error)
{
    const std::string name = "World\\Maps\\" + m_map + "\\" + m_map + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt";
    auto bytes = m_mpq.Read(name);
    if (!bytes) { error = "Missing " + name; return false; }
    auto adt = ParseAdt(*bytes, m_bigAlpha);
    if (!adt) { error = "Could not parse " + name + " (not a 3.3.5 monolithic ADT?)"; return false; }
    stats = FinishTile(x, y, std::move(*bytes), std::move(*adt));
    return true;
}

TileStats TerrainAdapter::FinishTile(int x, int y, std::vector<uint8_t> bytes, Adt adt)
{
    LoadedTile tile = LoadedTile::Make(x, y, std::move(bytes), std::move(adt));
    if (ReplayEdits(tile, m_map, m_store.Done())) m_objectsChanged.insert(tile.Key());   // the project's edits, in order

    const TileStats stats = m_renderer.LoadTile(tile.Key(), tile.adt, m_mpq);
    tile.maxHeight = stats.maxHeight;
    m_tiles[tile.Key()] = std::move(tile);
    // Roads over this tile were drawn before its ground was known (and the roads beside it follow that ground too):
    // draw the road cells on it and around it again.
    if (m_roads)
    {
        RoadStore::Cells cells;
        for (const Road* r : m_roads->OnMap(m_map))
            for (const auto& [cell, hash] : RoadCellHashes(*r))
                if (cell.first >= x * 16 - 2 && cell.first < x * 16 + 18 && cell.second >= y * 16 - 2 && cell.second < y * 16 + 18) cells.insert(cell);
        if (!cells.empty()) RefreshCells(m_map, cells);
    }
    return stats;
}

std::vector<std::pair<int, TileStats>> TerrainAdapter::Stream(float x, float z, int radius, std::string& error, float budgetMs)
{
    std::vector<std::pair<int, TileStats>> loaded;
    if (m_map.empty()) return loaded;
    const int cx = int(std::floor(x / kTileSize)), cz = int(std::floor(z / kTileSize));

    for (auto it = m_tiles.begin(); it != m_tiles.end();)
    {
        const bool distant = std::abs(it->second.x - cx) > radius + 1 || std::abs(it->second.y - cz) > radius + 1;
        if (distant && !m_stroking && !m_painting && !m_pinned.count(it->first))
        {
            m_renderer.UnloadTile(it->first);
            it = m_tiles.erase(it);
        }
        else
            ++it;
    }

    // Missing tiles, pinned ones first, then nearest first.
    std::vector<std::pair<int, int>> wanted;   // priority, key
    auto missing = [&](int key) { return key >= 0 && key < 4096 && m_present[size_t(key)] && !m_tiles.count(key); };
    for (int key : m_pinned)
        if (missing(key)) wanted.push_back({ -1, key });
    for (int ty = cz - radius; ty <= cz + radius; ++ty)
        for (int tx = cx - radius; tx <= cx + radius; ++tx)
            if (tx >= 0 && tx <= 63 && ty >= 0 && ty <= 63 && missing(TileKey(tx, ty)) && !m_pinned.count(TileKey(tx, ty)))
                wanted.push_back({ (tx - cx) * (tx - cx) + (ty - cz) * (ty - cz), TileKey(tx, ty) });
    std::stable_sort(wanted.begin(), wanted.end());

    if (!m_loader)
    {
        if (wanted.empty()) return loaded;
        const int key = wanted.front().second;
        TileStats stats;
        if (LoadTile(key % 64, key / 64, stats, error)) loaded.push_back({ key, stats });
        else m_present[size_t(key)] = false;   // do not retry a broken tile every frame
        return loaded;
    }

    std::vector<int> keys;
    for (const auto& w : wanted) keys.push_back(w.second);
    m_loader->Want(m_map, m_bigAlpha, std::move(keys));
    const auto start = std::chrono::steady_clock::now();
    do
    {
        auto tile = m_loader->TakeTile();
        if (!tile) break;
        const int tx = tile->key % 64, ty = tile->key / 64;
        if (m_tiles.count(tile->key)) continue;
        if (!tile->adt)
        {
            error = "Could not load " + m_map + "_" + std::to_string(tx) + "_" + std::to_string(ty) + ".adt";
            m_present[size_t(tile->key)] = false;
            continue;
        }
        loaded.push_back({ tile->key, FinishTile(tx, ty, std::move(tile->bytes), std::move(*tile->adt)) });
    } while (std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count() < budgetMs);
    return loaded;
}

size_t TerrainAdapter::MissingTiles(const std::set<int>& keys) const
{
    size_t missing = 0;
    for (int key : keys)
        if (key >= 0 && key < 4096 && size_t(key) < m_present.size() && m_present[size_t(key)] && !m_tiles.count(key)) ++missing;
    return missing;
}

const AdtChunk* TerrainAdapter::Chunk(ChunkRef ref) const
{
    auto it = m_tiles.find(ref.tile);
    if (it == m_tiles.end() || ref.chunk < 0 || size_t(ref.chunk) >= it->second.adt.chunks.size()) return nullptr;
    return &it->second.adt.chunks[size_t(ref.chunk)];
}

std::pair<int, int> TerrainAdapter::GridOf(ChunkRef ref) const
{
    const AdtChunk* c = Chunk(ref);
    return c ? std::pair{ GridIndex(c->baseX), GridIndex(c->baseZ) } : std::pair{ -1, -1 };
}

std::optional<ChunkRef> TerrainAdapter::ChunkAtGrid(int gx, int gz) const
{
    if (gx < 0 || gz < 0 || gx >= 1024 || gz >= 1024) return std::nullopt;
    auto it = m_tiles.find(TileKey(gx / 16, gz / 16));
    if (it == m_tiles.end()) return std::nullopt;
    const int16_t i = it->second.byGrid[size_t((gz % 16) * 16 + gx % 16)];
    return i < 0 ? std::nullopt : std::optional<ChunkRef>(ChunkRef{ it->first, i });
}

namespace
{
    /// Height inside a chunk: bilinear over the cell's four outer corners, close enough for overlays and roads.
    float ChunkHeightAt(const AdtChunk& c, float x, float z)
    {
        const float lx = std::clamp((x - c.baseX) / kUnitSize, 0.0f, 7.999f), lz = std::clamp((z - c.baseZ) / kUnitSize, 0.0f, 7.999f);
        const size_t col = size_t(lx), row = size_t(lz);
        const float fx = lx - col, fz = lz - row;
        const float tl = c.heights[row * 17 + col], tr = c.heights[row * 17 + col + 1];
        const float bl = c.heights[(row + 1) * 17 + col], br = c.heights[(row + 1) * 17 + col + 1];
        return c.baseY + (tl * (1 - fx) + tr * fx) * (1 - fz) + (bl * (1 - fx) + br * fx) * fz;
    }

    /// Height at a point of tile (tx, ty) parsed on its own (chunks found by their place in the tile).
    std::optional<float> TileHeightAt(const Adt& adt, int tx, int ty, float x, float z)
    {
        const int col = int(std::floor((x - tx * kTileSize) / kChunkSize)), row = int(std::floor((z - ty * kTileSize) / kChunkSize));
        if (col < 0 || row < 0 || col > 15 || row > 15) return std::nullopt;
        const size_t i = size_t(row * 16 + col);
        if (i < adt.chunks.size() && int(adt.chunks[i].indexX) == col && int(adt.chunks[i].indexY) == row) return ChunkHeightAt(adt.chunks[i], x, z);
        for (const AdtChunk& c : adt.chunks)   // files out of order
            if (int(c.indexX) == col && int(c.indexY) == row) return ChunkHeightAt(c, x, z);
        return std::nullopt;
    }
}

std::optional<float> TerrainAdapter::HeightAt(float x, float z) const
{
    const auto ref = ChunkAtGrid(int(std::floor(x / kChunkSize)), int(std::floor(z / kChunkSize)));
    if (!ref) return std::nullopt;
    return ChunkHeightAt(*Chunk(*ref), x, z);
}

std::optional<TerrainHit> TerrainAdapter::PickIn(const std::map<int, LoadedTile>& tiles, FXMVECTOR origin, FXMVECTOR dir, bool throughHoles)
{
    float best = 1e30f;
    std::optional<TerrainHit> hit;
    for (const auto& [key, tile] : tiles)
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            const AdtChunk& c = tile.adt.chunks[ci];
            const auto [lo, hi] = std::minmax_element(c.heights.begin(), c.heights.end());
            BoundingBox box;
            BoundingBox::CreateFromPoints(box, XMVectorSet(c.baseX, c.baseY + *lo - 1, c.baseZ, 0),
                                          XMVectorSet(c.baseX + kChunkSize, c.baseY + *hi + 1, c.baseZ + kChunkSize, 0));
            float boxT = 0;
            if (!box.Intersects(origin, dir, boxT) || boxT > best) continue;

            auto vpos = [&](size_t j) {
                float x, z;
                VertexXZ(j, x, z);
                return XMVectorSet(c.baseX + x * kUnitSize, c.baseY + c.heights[j], c.baseZ + z * kUnitSize, 0);
            };
            for (size_t r = 0; r < 8; ++r)
                for (size_t col = 0; col < 8; ++col)
                {
                    if (!throughHoles && (c.holes & (1u << ((r / 2) * 4 + col / 2)))) continue;
                    const size_t ring[4] = { r * 17 + col, r * 17 + col + 1, (r + 1) * 17 + col + 1, (r + 1) * 17 + col };
                    const XMVECTOR m = vpos(r * 17 + 9 + col);
                    for (int k = 0; k < 4; ++k)
                    {
                        float t = 0;
                        if (TriangleTests::Intersects(origin, dir, vpos(ring[k]), vpos(ring[(k + 1) % 4]), m, t) && t < best)
                        {
                            best = t;
                            XMFLOAT3 p;
                            XMStoreFloat3(&p, XMVectorAdd(origin, XMVectorScale(dir, t)));
                            hit = TerrainHit{ p, ChunkRef{ key, int(ci) } };
                        }
                    }
                }
        }
    return hit;
}

// ---------------------------------------------------------------------------------------------- edits

nlohmann::json TerrainAdapter::LayerState(const LoadedTile& tile, const AdtChunk& c)
{
    nlohmann::json names = nlohmann::json::array(), flags = nlohmann::json::array(), effects = nlohmann::json::array();
    for (uint32_t l = 0; l < c.layerCount && l < 4; ++l)
    {
        names.push_back(c.textureIds[l] < tile.adt.textures.size() ? tile.adt.textures[c.textureIds[l]] : std::string());
        flags.push_back(c.layerFlags[l]);
        effects.push_back(c.effectIds[l]);
    }
    std::vector<uint8_t> rgb(64 * 64 * 3);
    for (size_t i = 0; i < 4096; ++i)
        for (size_t ch = 0; ch < 3; ++ch) rgb[i * 3 + ch] = c.alpha.size() == 4096 * 4 ? c.alpha[i * 4 + ch] : 0;
    return { { "names", names }, { "flags", flags }, { "effects", effects }, { "alpha", Base64Encode(rgb.data(), rgb.size()) } };
}

void TerrainAdapter::SetLayerState(LoadedTile& tile, AdtChunk& c, const nlohmann::json& s)
{
    const auto& names = s.at("names");
    c.layerCount = uint32_t(std::min<size_t>(names.size(), 4));
    for (uint32_t l = 0; l < c.layerCount; ++l)
    {
        const std::string name = names[l];
        auto it = std::find(tile.adt.textures.begin(), tile.adt.textures.end(), name);
        if (it == tile.adt.textures.end()) it = tile.adt.textures.insert(tile.adt.textures.end(), name);
        c.textureIds[l] = uint32_t(it - tile.adt.textures.begin());
        c.layerFlags[l] = s.at("flags")[l];
        c.effectIds[l] = s.at("effects")[l];
    }
    const std::vector<uint8_t> rgb = Base64Decode(s.at("alpha").get<std::string>());
    c.alpha.assign(4096 * 4, 0);
    for (size_t i = 0; i < 4096 && i * 3 + 2 < rgb.size(); ++i)
        for (size_t ch = 0; ch < 3; ++ch) c.alpha[i * 4 + ch] = rgb[i * 3 + ch];
}

namespace
{
    bool LiquidOf(const AdtLiquid& l, const AdtChunk& c)
    {
        return std::fabs(l.cornerX - c.baseX) < 1.0f && std::fabs(l.cornerZ - c.baseZ) < 1.0f;
    }
}

nlohmann::json TerrainAdapter::LiquidState(const Adt& adt, const AdtChunk& c)
{
    nlohmann::json out = nlohmann::json::array();
    for (const AdtLiquid& l : adt.liquids)
    {
        if (!LiquidOf(l, c)) continue;
        std::vector<uint8_t> exists(l.exists.begin(), l.exists.end());
        out.push_back({ { "type", l.type }, { "format", l.format }, { "x", l.x }, { "y", l.y }, { "w", l.w }, { "h", l.h }, { "heights", l.heights },
                        { "exists", Base64Encode(exists.data(), exists.size()) }, { "extra", Base64Encode(l.extra.data(), l.extra.size()) },
                        { "fishable", l.fishable }, { "deep", l.deep } });
    }
    return out;
}

void TerrainAdapter::SetLiquidState(Adt& adt, const AdtChunk& c, const nlohmann::json& state)
{
    std::erase_if(adt.liquids, [&](const AdtLiquid& l) { return LiquidOf(l, c); });
    for (const auto& j : state)
    {
        AdtLiquid l;
        l.type = j.at("type");
        l.format = j.at("format");
        l.x = j.at("x"); l.y = j.at("y"); l.w = j.at("w"); l.h = j.at("h");
        l.cornerX = c.baseX;
        l.cornerZ = c.baseZ;
        l.heights = j.at("heights").get<std::vector<float>>();
        if (!l.w || !l.h || l.x + l.w > 8 || l.y + l.h > 8 || l.heights.size() != size_t(l.w + 1) * (l.h + 1)) continue;
        for (uint8_t b : Base64Decode(j.value("exists", std::string()))) l.exists.push_back(b != 0);
        l.exists.resize(size_t(l.w) * l.h, true);
        l.extra = Base64Decode(j.value("extra", std::string()));
        l.fishable = j.value("fishable", ~0ull);
        l.deep = j.value("deep", 0ull);
        adt.liquids.push_back(std::move(l));
    }
}

nlohmann::json TerrainAdapter::RotateLiquidState(const nlohmann::json& state)
{
    // Chunk vertex (row, col) goes to (col, 8 - row) and cell (row, col) to (col, 7 - row), as for heights. An
    // instance's rows become its columns: it starts at x' = 8 - y - h, y' = x and is h wide, w deep.
    nlohmann::json out = nlohmann::json::array();
    for (nlohmann::json j : state)
    {
        const int x = j.at("x"), y = j.at("y"), w = j.at("w"), h = j.at("h");
        auto turn = [](const std::vector<uint8_t>& in, int cols, int rows, size_t size) {   // rows x cols elements of `size` bytes
            std::vector<uint8_t> o(in.size());
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    for (size_t b = 0; b < size; ++b)
                        if ((size_t(r * cols + c) + 1) * size <= in.size())
                            o[size_t(c * rows + (rows - 1 - r)) * size + b] = in[size_t(r * cols + c) * size + b];
            return o;
        };
        const std::vector<float> hs = j.at("heights").get<std::vector<float>>();
        std::vector<uint8_t> hb(hs.size() * 4);
        std::memcpy(hb.data(), hs.data(), hb.size());
        hb = turn(hb, w + 1, h + 1, 4);
        std::vector<float> turned(hs.size());
        std::memcpy(turned.data(), hb.data(), hb.size());
        j["heights"] = turned;
        const std::vector<uint8_t> exists = turn(Base64Decode(j.value("exists", std::string())), w, h, 1);
        j["exists"] = Base64Encode(exists.data(), exists.size());
        // Extra: per-vertex arrays one after another (uv of 4 bytes, depth of 1).
        const uint16_t format = j.at("format");
        const std::vector<size_t> parts = format == 1 ? std::vector<size_t>{ 4 } : format == 3 ? std::vector<size_t>{ 4, 1 } : std::vector<size_t>{ 1 };
        const std::vector<uint8_t> extra = Base64Decode(j.value("extra", std::string()));
        std::vector<uint8_t> turnedExtra;
        const size_t verts = size_t(w + 1) * (h + 1);
        size_t at = 0;
        for (size_t part : parts)
        {
            if (at + verts * part > extra.size()) break;
            const std::vector<uint8_t> seg(extra.begin() + std::ptrdiff_t(at), extra.begin() + std::ptrdiff_t(at + verts * part));
            const std::vector<uint8_t> t = turn(seg, w + 1, h + 1, part);
            turnedExtra.insert(turnedExtra.end(), t.begin(), t.end());
            at += verts * part;
        }
        j["extra"] = Base64Encode(turnedExtra.data(), turnedExtra.size());
        for (const char* mask : { "fishable", "deep" })   // 8 x 8 cell bits, row-major
        {
            const uint64_t m = j.value(mask, 0ull);
            uint64_t t = 0;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c)
                    if (m >> (r * 8 + c) & 1) t |= 1ull << (c * 8 + (7 - r));
            j[mask] = t;
        }
        j["x"] = 8 - y - h;
        j["y"] = x;
        j["w"] = h;
        j["h"] = w;
        out.push_back(std::move(j));
    }
    return out;
}

bool TerrainAdapter::ReplayEdits(LoadedTile& tile, const std::string& map, const std::vector<Change>& done)
{
    const int x = tile.x, y = tile.y;
    bool objects = false;
    for (const ChangeStore::Part& c : ChangeStore::Parts(done))
    {
        if (c.domain != "terrain.heights" || c.data.at("map").get<std::string>() != map) continue;
        for (const auto& e : c.data.at("edits"))
            if (e[0] == x && e[1] == y)
            {
                const size_t ci = e[2], vi = e[3];
                if (ci < tile.adt.chunks.size() && vi < 145) tile.adt.chunks[ci].heights[vi] = e[5];
            }
        for (const auto& e : c.data.value("layers", nlohmann::json::array()))
            if (e[0] == x && e[1] == y && size_t(e[2]) < tile.adt.chunks.size())
                SetLayerState(tile, tile.adt.chunks[size_t(e[2])], e[4]);
        for (const auto& e : c.data.value("holes", nlohmann::json::array()))
            if (e[0] == x && e[1] == y && size_t(e[2]) < tile.adt.chunks.size()) tile.adt.chunks[size_t(e[2])].holes = e[4];
        for (const auto& e : c.data.value("areas", nlohmann::json::array()))
            if (e[0] == x && e[1] == y && size_t(e[2]) < tile.adt.chunks.size()) tile.adt.chunks[size_t(e[2])].areaId = e[4];
        for (const auto& e : c.data.value("colors", nlohmann::json::array()))
            if (e[0] == x && e[1] == y && size_t(e[2]) < tile.adt.chunks.size()) tile.adt.chunks[size_t(e[2])].colors = Base64Decode(e[4].get<std::string>());
        for (const auto& e : c.data.value("liquids", nlohmann::json::array()))
            if (e[0] == x && e[1] == y && size_t(e[2]) < tile.adt.chunks.size()) SetLiquidState(tile.adt, tile.adt.chunks[size_t(e[2])], e[4]);
        for (const auto& e : c.data.value("objects", nlohmann::json::array()))
            if (e[0] == x && e[1] == y)
            {
                ApplyObjectEntry(tile.adt, e, true);
                objects = true;
            }
    }
    return objects;
}

void TerrainAdapter::RefreshTextures(int key, size_t chunk)
{
    const LoadedTile& tile = m_tiles.at(key);
    m_renderer.UpdateChunkTextures(key, chunk, tile.adt.chunks[chunk], tile.adt.textures, m_mpq);
}

void TerrainAdapter::Set(const Change& change, bool after)
{
    for (const auto& e : change.data.value("tiles", nlohmann::json::array()))   // added tiles: the overlay, whatever map is open
        SetTile(change.data.at("map"), e[0], e[1], e[2], after);
    if (const auto it = change.data.find("global"); it != change.data.end() && it->is_array() && it->size() == 2)
        SetGlobalWmo(change.data.at("map"), WmoFrom((*it)[after ? 1 : 0]));
    if (change.data.at("map").get<std::string>() != m_map) return;   // other maps pick it up when loaded
    std::set<std::pair<int, int>> touched;
    for (const auto& e : change.data.at("edits"))
    {
        auto it = m_tiles.find(TileKey(e[0], e[1]));
        const size_t ci = e[2], vi = e[3];
        if (it == m_tiles.end() || ci >= it->second.adt.chunks.size() || vi >= 145) continue;
        it->second.adt.chunks[ci].heights[vi] = e[after ? 5 : 4];
        touched.insert({ it->first, int(ci) });
    }
    for (const auto& [key, ci] : touched) m_renderer.UpdateChunk(key, size_t(ci), m_tiles.at(key).adt.chunks[size_t(ci)]);

    for (const auto& e : change.data.value("layers", nlohmann::json::array()))
    {
        auto it = m_tiles.find(TileKey(e[0], e[1]));
        const size_t ci = e[2];
        if (it == m_tiles.end() || ci >= it->second.adt.chunks.size()) continue;
        SetLayerState(it->second, it->second.adt.chunks[ci], e[after ? 4 : 3]);
        RefreshTextures(it->first, ci);
    }

    for (const auto& e : change.data.value("holes", nlohmann::json::array()))
    {
        auto it = m_tiles.find(TileKey(e[0], e[1]));
        const size_t ci = e[2];
        if (it == m_tiles.end() || ci >= it->second.adt.chunks.size()) continue;
        it->second.adt.chunks[ci].holes = e[after ? 4 : 3];
        m_renderer.SetChunkHoles(it->first, ci, it->second.adt.chunks[ci].holes);
    }

    for (const auto& e : change.data.value("areas", nlohmann::json::array()))
    {
        auto it = m_tiles.find(TileKey(e[0], e[1]));
        if (it != m_tiles.end() && size_t(e[2]) < it->second.adt.chunks.size()) it->second.adt.chunks[size_t(e[2])].areaId = e[after ? 4 : 3];
    }

    for (const auto& e : change.data.value("colors", nlohmann::json::array()))
    {
        auto it = m_tiles.find(TileKey(e[0], e[1]));
        const size_t ci = e[2];
        if (it == m_tiles.end() || ci >= it->second.adt.chunks.size()) continue;
        it->second.adt.chunks[ci].colors = Base64Decode(e[after ? 4 : 3].get<std::string>());
        m_renderer.UpdateChunk(it->first, ci, it->second.adt.chunks[ci]);
    }

    std::set<int> water;
    for (const auto& e : change.data.value("liquids", nlohmann::json::array()))
    {
        auto it = m_tiles.find(TileKey(e[0], e[1]));
        if (it == m_tiles.end() || size_t(e[2]) >= it->second.adt.chunks.size()) continue;
        SetLiquidState(it->second.adt, it->second.adt.chunks[size_t(e[2])], e[after ? 4 : 3]);
        water.insert(it->first);
    }
    for (int key : water) m_renderer.UpdateWater(key, m_tiles.at(key).adt.liquids, m_mpq);

    const auto objects = change.data.value("objects", nlohmann::json::array());
    for (auto& [key, tile] : m_tiles) SetObjects(tile, objects, after);
}

void TerrainAdapter::SetObjects(LoadedTile& tile, const nlohmann::json& objects, bool after)
{
    // Undo walks the entries backwards so an object edited twice in one change ends where it started.
    for (size_t k = 0; k < objects.size(); ++k)
    {
        const auto& e = objects[after ? k : objects.size() - 1 - k];
        if (e[0] != tile.x || e[1] != tile.y) continue;
        ApplyObjectEntry(tile.adt, e, after);
        m_objectsChanged.insert(tile.Key());
    }
}

std::optional<DoodadPlacement> TerrainAdapter::FindDoodad(uint32_t uid) const
{
    for (const auto& [key, tile] : m_tiles)
        for (const auto& d : tile.adt.doodads)
            if (d.uniqueId == uid) return d;
    return std::nullopt;
}

std::optional<WmoPlacement> TerrainAdapter::FindWmo(uint32_t uid) const
{
    if (m_globalWmo && m_globalWmo->uniqueId == uid) return m_globalWmo;
    for (const auto& [key, tile] : m_tiles)
        for (const auto& w : tile.adt.wmos)
            if (w.uniqueId == uid) return w;
    return std::nullopt;
}

void TerrainAdapter::FitWmoExtents(WmoPlacement& w) const
{
    auto [it, added] = m_wmoBounds.try_emplace(w.model);
    if (added)
    {
        uint32_t groups = 0;
        std::array<float, 6> b{};
        if (auto root = m_mpq.Read(w.model); root && WmoRootInfo(*root, groups, b.data())) it->second = b;
    }
    if (!it->second) return;
    const auto& b = *it->second;
    const XMMATRIX m = PlacementMatrix(w.pos, w.rot, 1.0f);
    XMFLOAT3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < 8; ++i)
    {
        const XMFLOAT3 c = FromWowAxes(i & 1 ? b[3] : b[0], i & 2 ? b[4] : b[1], i & 4 ? b[5] : b[2]);
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&c), m));
        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
    }
    w.extMin[0] = lo.x; w.extMin[1] = lo.y; w.extMin[2] = lo.z;
    w.extMax[0] = hi.x; w.extMax[1] = hi.y; w.extMax[2] = hi.z;
}

std::set<ObjectRef> TerrainAdapter::DoodadsInside(const std::set<ObjectRef>& objects) const
{
    std::set<ObjectRef> inside;
    for (const ObjectRef& r : objects)
    {
        if (!r.wmo) continue;
        WmoPlacement w;
        if (auto found = FindWmo(r.uid)) w = *found;
        else continue;
        FitWmoExtents(w);   // reads the root's bounds into m_wmoBounds
        const auto& b = m_wmoBounds[w.model];
        if (!b) continue;
        // The root's box in the WMO's own frame (editor axes), a little larger so furniture on the edge counts.
        const XMFLOAT3 c0 = FromWowAxes((*b)[0], (*b)[1], (*b)[2]), c1 = FromWowAxes((*b)[3], (*b)[4], (*b)[5]);
        const XMFLOAT3 lo{ std::min(c0.x, c1.x) - 0.5f, std::min(c0.y, c1.y) - 0.5f, std::min(c0.z, c1.z) - 0.5f };
        const XMFLOAT3 hi{ std::max(c0.x, c1.x) + 0.5f, std::max(c0.y, c1.y) + 0.5f, std::max(c0.z, c1.z) + 0.5f };
        const XMMATRIX toLocal = XMMatrixInverse(nullptr, PlacementMatrix(w.pos, w.rot, 1.0f));
        for (const auto& [key, tile] : m_tiles)
            for (const DoodadPlacement& d : tile.adt.doodads)
            {
                if (objects.count({ false, d.uniqueId })) continue;
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(d.pos[0], d.pos[1], d.pos[2], 1), toLocal));
                if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z) inside.insert({ false, d.uniqueId });
            }
    }
    return inside;
}

void TerrainAdapter::BeginObjectEdit(const std::set<ObjectRef>& objects)
{
    CancelObjectEdit();
    if (m_globalWmo && objects.count({ true, m_globalWmo->uniqueId })) m_objectEdit.push_back({ kGlobalWmoKey, true, {}, *m_globalWmo });
    for (const auto& [key, tile] : m_tiles)
    {
        for (const auto& d : tile.adt.doodads)
            if (objects.count({ false, d.uniqueId })) m_objectEdit.push_back({ key, false, d, {} });
        for (const auto& w : tile.adt.wmos)
            if (objects.count({ true, w.uniqueId })) m_objectEdit.push_back({ key, true, {}, w });
    }
}

void TerrainAdapter::PreviewObjectEdit(const std::function<void(DoodadPlacement&)>& doodad, const std::function<void(WmoPlacement&)>& wmo)
{
    for (const ObjectSnap& s : m_objectEdit)
    {
        if (s.tile == kGlobalWmoKey && m_globalWmo)   // shown at once; written to the WDT when the edit ends
        {
            WmoPlacement w = s.wmoPlacement;
            wmo(w);
            FitWmoExtents(w);
            m_globalWmo = w;
            m_objectsChanged.insert(kGlobalWmoKey);
            continue;
        }
        auto it = m_tiles.find(s.tile);
        if (it == m_tiles.end()) continue;
        nlohmann::json after;
        if (s.wmo)
        {
            WmoPlacement w = s.wmoPlacement;
            wmo(w);
            FitWmoExtents(w);
            after = ToJson(w);
        }
        else
        {
            DoodadPlacement d = s.doodad;
            doodad(d);
            after = ToJson(d);
        }
        ApplyObjectEntry(it->second.adt, { it->second.x, it->second.y, s.wmo ? "wmo" : "m2", nullptr, after }, true);
        m_objectsChanged.insert(s.tile);
    }
}

std::optional<Change> TerrainAdapter::EndObjectEdit(const std::string& label)
{
    nlohmann::json objects = nlohmann::json::array(), global;
    for (const ObjectSnap& s : m_objectEdit)
    {
        if (s.tile == kGlobalWmoKey)
        {
            if (m_globalWmo && ToJson(*m_globalWmo) != ToJson(s.wmoPlacement)) global = { ToJson(s.wmoPlacement), ToJson(*m_globalWmo) };
            continue;
        }
        auto it = m_tiles.find(s.tile);
        if (it == m_tiles.end()) continue;
        const nlohmann::json before = s.wmo ? ToJson(s.wmoPlacement) : ToJson(s.doodad);
        const uint32_t uid = s.wmo ? s.wmoPlacement.uniqueId : s.doodad.uniqueId;
        std::optional<nlohmann::json> now;
        if (s.wmo) { for (const auto& w : it->second.adt.wmos) if (w.uniqueId == uid) now = ToJson(w); }
        else { for (const auto& d : it->second.adt.doodads) if (d.uniqueId == uid) now = ToJson(d); }
        if (now && *now != before) objects.push_back({ it->second.x, it->second.y, s.wmo ? "wmo" : "m2", before, *now });
    }
    m_objectEdit.clear();
    if (objects.empty() && global.is_null()) return std::nullopt;
    Change c = MakeChange(Edits{}, nlohmann::json::array(), label, nlohmann::json::array(), objects);
    if (!global.is_null())
    {
        c.data["global"] = global;   // the WMO of a WMO-only map: [before, after], kept in its WDT
        c.target = m_map + " WMO";
        SetGlobalWmo(m_map, WmoFrom(global[1]));
    }
    return c;
}

void TerrainAdapter::CancelObjectEdit()
{
    for (const ObjectSnap& s : m_objectEdit)
        if (s.tile == kGlobalWmoKey)
        {
            m_globalWmo = s.wmoPlacement;
            m_objectsChanged.insert(kGlobalWmoKey);
        }
        else if (auto it = m_tiles.find(s.tile); it != m_tiles.end())
        {
            const nlohmann::json before = s.wmo ? ToJson(s.wmoPlacement) : ToJson(s.doodad);
            ApplyObjectEntry(it->second.adt, { it->second.x, it->second.y, s.wmo ? "wmo" : "m2", nullptr, before }, true);
            m_objectsChanged.insert(s.tile);
        }
    m_objectEdit.clear();
}

nlohmann::json TerrainAdapter::AddObjects(std::vector<DoodadPlacement> doodads, std::vector<WmoPlacement> wmos, std::vector<ObjectRef>* placed)
{
    // Objects go to the tile under their position, each with a fresh unique id.
    nlohmann::json objects = nlohmann::json::array();
    uint32_t uid = NextUniqueId();
    auto add = [&](auto& placement, const char* kind, bool wmo) {
        const int tx = int(std::floor(placement.pos[0] / kTileSize)), ty = int(std::floor(placement.pos[2] / kTileSize));
        if (!m_tiles.count(TileKey(tx, ty))) return;
        placement.uniqueId = uid++;
        objects.push_back({ tx, ty, kind, ToJson(placement) });
        if (placed) placed->push_back({ wmo, placement.uniqueId });
    };
    for (auto& d : doodads) add(d, "m2", false);
    for (auto& w : wmos)
    {
        FitWmoExtents(w);
        add(w, "wmo", true);
    }
    for (auto& [key, tile] : m_tiles) SetObjects(tile, objects, true);
    return objects;
}

std::optional<Change> TerrainAdapter::PlaceObjects(const std::vector<DoodadPlacement>& doodads, const std::vector<WmoPlacement>& wmos,
                                                   const std::string& label, std::vector<ObjectRef>* placed)
{
    const nlohmann::json objects = AddObjects(doodads, wmos, placed);
    if (objects.empty()) return std::nullopt;
    return MakeChange(Edits{}, nlohmann::json::array(), label, nlohmann::json::array(), objects);
}

std::optional<Change> TerrainAdapter::DeleteObjects(const std::set<ObjectRef>& refs)
{
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& [key, tile] : m_tiles)
    {
        for (const auto& d : tile.adt.doodads)
            if (refs.count({ false, d.uniqueId })) objects.push_back({ tile.x, tile.y, "m2", ToJson(d), nullptr });
        for (const auto& w : tile.adt.wmos)
            if (refs.count({ true, w.uniqueId })) objects.push_back({ tile.x, tile.y, "wmo", ToJson(w), nullptr });
    }
    if (objects.empty()) return std::nullopt;
    for (auto& [key, tile] : m_tiles) SetObjects(tile, objects, true);
    return MakeChange(Edits{}, nlohmann::json::array(), "Delete " + std::to_string(refs.size()) + " object(s)", nlohmann::json::array(), objects);
}

uint32_t TerrainAdapter::NextUniqueId() const
{
    uint32_t next = kEditorUniqueIdBase;
    for (const ChangeStore::Part& c : ChangeStore::Parts(m_store.Done()))
        if (c.domain == Domain())
        {
            for (const auto& e : c.data.value("objects", nlohmann::json::array())) next = std::max<uint32_t>(next, EntryUid(e) + 1);
            for (const auto& e : c.data.value("tiles", nlohmann::json::array())) next = std::max<uint32_t>(next, uint32_t(e[3]) + 1);
        }
    // ponytail: ids only need to avoid each other and Blizzard's; scan every ADT of the map if a custom map uses this range.
    return next;
}

Change TerrainAdapter::MakeChange(const Edits& edits, const nlohmann::json& layers, const std::string& label, const nlohmann::json& holes,
                                  const nlohmann::json& objects, const nlohmann::json& areas, const nlohmann::json& liquids, const nlohmann::json& colors) const
{
    nlohmann::json list = nlohmann::json::array();
    std::set<int> tiles;
    for (const auto& [key, values] : edits)
    {
        const auto [tileKey, ci, vi] = key;
        if (values.first == values.second) continue;
        list.push_back({ tileKey % 64, tileKey / 64, ci, vi, values.first, values.second });
        tiles.insert(tileKey);
    }
    for (const auto& e : layers) tiles.insert(TileKey(e[0], e[1]));
    for (const auto& e : holes) tiles.insert(TileKey(e[0], e[1]));
    for (const auto& e : objects) tiles.insert(TileKey(e[0], e[1]));
    for (const auto& e : areas) tiles.insert(TileKey(e[0], e[1]));
    for (const auto& e : liquids) tiles.insert(TileKey(e[0], e[1]));
    for (const auto& e : colors) tiles.insert(TileKey(e[0], e[1]));
    Change c;
    c.domain = Domain();
    c.label = label;
    c.target = m_map + (tiles.size() == 1 ? " " + std::to_string(*tiles.begin() % 64) + "_" + std::to_string(*tiles.begin() / 64)
                                          : " (" + std::to_string(tiles.size()) + " tiles)");
    c.data = { { "map", m_map }, { "edits", std::move(list) } };
    if (!layers.empty()) c.data["layers"] = layers;
    if (!holes.empty()) c.data["holes"] = holes;
    if (!objects.empty()) c.data["objects"] = objects;
    if (!areas.empty()) c.data["areas"] = areas;
    if (!liquids.empty()) c.data["liquids"] = liquids;
    if (!colors.empty()) c.data["colors"] = colors;
    return c;
}

void TerrainAdapter::PaintStep(const XMFLOAT3& center, const PaintBrush& brush, const std::string& texture, bool erase, float dt)
{
    if (!m_painting || texture.empty()) return;
    const float r = std::max(brush.radius, 0.5f);
    const float inner = r * std::clamp(brush.hardness, 0.0f, 0.99f);
    const float rate = std::clamp(brush.pressure, 0.01f, 1.0f) * 4.0f * dt;   // full coverage in about a quarter second at pressure 1
    const std::string lower = Catalog::Normalize(texture);

    // Ground effect (grass, pebbles) of this texture elsewhere on the map, so a new layer keeps its detail doodads.
    // Only one this client knows: tiles pasted from another client can carry ids that crash it.
    uint32_t effect = 0;
    for (const auto& [key, tile] : m_tiles)
        for (const AdtChunk& c : tile.adt.chunks)
            for (uint32_t l = 0; l < c.layerCount && !effect; ++l)
                if (c.textureIds[l] < tile.adt.textures.size() && Catalog::Normalize(tile.adt.textures[c.textureIds[l]]) == lower &&
                    KnownEffect(c.effectIds[l]))
                    effect = c.effectIds[l];

    for (auto& [key, tile] : m_tiles)
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            AdtChunk& c = tile.adt.chunks[ci];
            if (center.x + r < c.baseX || center.x - r > c.baseX + kChunkSize || center.z + r < c.baseZ || center.z - r > c.baseZ + kChunkSize) continue;

            // Is any texel of this chunk inside the brush?
            const float nx = std::clamp(center.x, c.baseX, c.baseX + kChunkSize) - center.x, nz = std::clamp(center.z, c.baseZ, c.baseZ + kChunkSize) - center.z;
            if (nx * nx + nz * nz > r * r) continue;

            if (!m_paintBefore.count({ key, int(ci) })) m_paintBefore[{ key, int(ci) }] = LayerState(tile, c);
            if (c.alpha.size() != 4096 * 4) c.alpha.assign(4096 * 4, 0);

            // Which layer holds the texture; add it, or replace the weakest layer when all four are used.
            uint32_t n = std::min<uint32_t>(c.layerCount, 4);
            int target = -1;
            for (uint32_t l = 0; l < n; ++l)
                if (c.textureIds[l] < tile.adt.textures.size() && Catalog::Normalize(tile.adt.textures[c.textureIds[l]]) == lower) target = int(l);
            if (target < 0)
            {
                if (erase) continue;   // nothing of it here to remove
                auto it = std::find(tile.adt.textures.begin(), tile.adt.textures.end(), texture);
                if (it == tile.adt.textures.end()) it = tile.adt.textures.insert(tile.adt.textures.end(), texture);
                const uint32_t textureId = uint32_t(it - tile.adt.textures.begin());
                if (n < 4)
                    target = int(n++);
                else
                {
                    std::array<double, 4> totals{};
                    for (size_t i = 0; i < 4096; ++i)
                    {
                        const auto w = LayerWeights(c.alpha[i * 4] / 255.0f, c.alpha[i * 4 + 1] / 255.0f, c.alpha[i * 4 + 2] / 255.0f);
                        for (int l = 0; l < 4; ++l) totals[size_t(l)] += w[size_t(l)];
                    }
                    target = int(std::min_element(totals.begin(), totals.end()) - totals.begin());
                    // The replaced texture's share goes to the others; the new one starts from nothing.
                    for (size_t i = 0; i < 4096; ++i)
                    {
                        auto w = LayerWeights(c.alpha[i * 4] / 255.0f, c.alpha[i * 4 + 1] / 255.0f, c.alpha[i * 4 + 2] / 255.0f);
                        w[size_t(target)] = 0;
                        float sum = w[0] + w[1] + w[2] + w[3];
                        if (sum <= 1e-6f) { w = { 0, 0, 0, 0 }; w[target == 0 ? 1 : 0] = 1; sum = 1; }
                        for (float& v : w) v /= sum;
                        const auto a = LayerAlphas(w);
                        for (int ch = 0; ch < 3; ++ch) c.alpha[i * 4 + size_t(ch)] = uint8_t(std::lround(a[size_t(ch)] * 255.0f));
                    }
                }
                c.textureIds[size_t(target)] = textureId;
                c.layerFlags[size_t(target)] = 0;
                c.effectIds[size_t(target)] = effect;
                if (target > 0)
                    for (size_t i = 0; i < 4096; ++i) c.alpha[i * 4 + size_t(target - 1)] = 0;
                c.layerCount = n;
            }

            // Texel by texel: move the target's share up or down by the brush strength there, keep the sum at 1.
            for (int ty = 0; ty < 64; ++ty)
                for (int tx = 0; tx < 64; ++tx)
                {
                    const float x = c.baseX + (tx + 0.5f) * kChunkSize / 64, z = c.baseZ + (ty + 0.5f) * kChunkSize / 64;
                    const float d = std::sqrt((x - center.x) * (x - center.x) + (z - center.z) * (z - center.z));
                    if (d >= r) continue;
                    float f = d <= inner ? 1.0f : 1.0f - (d - inner) / (r - inner);
                    f = f * f * (3 - 2 * f);
                    const size_t i = size_t(ty * 64 + tx);
                    auto w = LayerWeights(n > 1 ? c.alpha[i * 4] / 255.0f : 0, n > 2 ? c.alpha[i * 4 + 1] / 255.0f : 0, n > 3 ? c.alpha[i * 4 + 2] / 255.0f : 0);
                    const float amount = rate * f;
                    float& t = w[size_t(target)];
                    if (erase)
                    {
                        const float removed = std::min(t, amount);
                        t -= removed;
                        float others = 0;
                        for (int l = 0; l < int(n); ++l) others += l == target ? 0 : w[size_t(l)];
                        if (others > 1e-6f)
                        {
                            for (int l = 0; l < int(n); ++l)
                                if (l != target) w[size_t(l)] += removed * w[size_t(l)] / others;
                        }
                        else if (n > 1)
                            w[target == 0 ? 1 : 0] += removed;   // only this texture here: the base layer (or the next) comes back
                        else
                            t += removed;                        // the chunk has no other texture to show
                    }
                    else
                    {
                        const float added = std::min(1.0f - t, amount);
                        for (int l = 0; l < 4; ++l)
                            if (l != target) w[size_t(l)] *= (1.0f - t - added) / std::max(1.0f - t, 1e-6f);
                        t += added;
                    }
                    const auto a = LayerAlphas(w);
                    for (int ch = 0; ch < 3; ++ch) c.alpha[i * 4 + size_t(ch)] = uint8_t(std::lround(a[size_t(ch)] * 255.0f));
                }
            RefreshTextures(key, ci);
        }
}

std::optional<Change> TerrainAdapter::EndPaint(const std::string& label)
{
    m_painting = false;
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& [key, before] : m_paintBefore)
    {
        auto it = m_tiles.find(key.first);
        if (it == m_tiles.end()) continue;
        nlohmann::json after = LayerState(it->second, it->second.adt.chunks[size_t(key.second)]);
        if (after != before) layers.push_back({ it->second.x, it->second.y, key.second, before, std::move(after) });
    }
    m_paintBefore.clear();
    if (layers.empty()) return std::nullopt;
    return MakeChange(Edits{}, layers, label);
}

std::optional<std::string> TerrainAdapter::TextureAt(float x, float z) const
{
    const auto ref = ChunkAtGrid(int(std::floor(x / kChunkSize)), int(std::floor(z / kChunkSize)));
    if (!ref) return std::nullopt;
    const LoadedTile& tile = m_tiles.at(ref->tile);
    const AdtChunk& c = *Chunk(*ref);
    if (c.layerCount == 0 || c.alpha.size() != 4096 * 4) return std::nullopt;
    const int tx = std::clamp(int((x - c.baseX) / kChunkSize * 64), 0, 63), tz = std::clamp(int((z - c.baseZ) / kChunkSize * 64), 0, 63);
    const size_t i = size_t(tz * 64 + tx);
    const auto w = LayerWeights(c.layerCount > 1 ? c.alpha[i * 4] / 255.0f : 0, c.layerCount > 2 ? c.alpha[i * 4 + 1] / 255.0f : 0,
                                c.layerCount > 3 ? c.alpha[i * 4 + 2] / 255.0f : 0);
    const size_t best = size_t(std::max_element(w.begin(), w.begin() + std::min<uint32_t>(c.layerCount, 4)) - w.begin());
    if (c.textureIds[best] >= tile.adt.textures.size()) return std::nullopt;
    return tile.adt.textures[c.textureIds[best]];
}

void TerrainAdapter::ShadeStep(const XMFLOAT3& center, const PaintBrush& brush, const std::array<uint8_t, 3>& rgb, bool erase, float dt)
{
    if (!m_shading) return;
    const float r = std::max(brush.radius, 0.5f);
    const float inner = r * std::clamp(brush.hardness, 0.0f, 0.99f);
    const float rate = std::clamp(brush.pressure, 0.01f, 1.0f) * 4.0f * dt;   // as Paint: full colour in about a quarter second
    const uint8_t target[3] = { erase ? uint8_t(0x7F) : rgb[2], erase ? uint8_t(0x7F) : rgb[1], erase ? uint8_t(0x7F) : rgb[0] };   // B, G, R
    for (auto& [key, tile] : m_tiles)
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            AdtChunk& c = tile.adt.chunks[ci];
            if (center.x + r < c.baseX || center.x - r > c.baseX + kChunkSize || center.z + r < c.baseZ || center.z - r > c.baseZ + kChunkSize) continue;
            bool touched = false;
            for (size_t j = 0; j < 145; ++j)
            {
                float vx, vz;
                VertexXZ(j, vx, vz);
                const float x = c.baseX + vx * kUnitSize - center.x, z = c.baseZ + vz * kUnitSize - center.z;
                const float d = std::sqrt(x * x + z * z);
                if (d >= r) continue;
                if (!touched)
                {
                    m_shadeBefore.try_emplace({ key, int(ci) }, c.colors);
                    if (c.colors.size() < 145 * 4) c.colors.assign(145 * 4, 0x7F);
                    touched = true;
                }
                float f = d <= inner ? 1.0f : 1.0f - (d - inner) / (r - inner);
                f = std::min(1.0f, rate * f * f * (3 - 2 * f));
                for (size_t k = 0; k < 3; ++k)
                {
                    uint8_t& v = c.colors[j * 4 + k];
                    const float step = (float(target[k]) - float(v)) * f;
                    // At least one step a frame, or slow strokes stall a few values short of the colour.
                    v = uint8_t(std::clamp(int(v) + (std::abs(step) < 1 && target[k] != v ? (target[k] > v ? 1 : -1) : int(std::lround(step))), 0, 255));
                }
            }
            if (touched) m_renderer.UpdateChunk(key, ci, c);
        }
}

std::optional<Change> TerrainAdapter::EndShade()
{
    m_shading = false;
    nlohmann::json colors = nlohmann::json::array();
    for (const auto& [key, before] : m_shadeBefore)
    {
        auto it = m_tiles.find(key.first);
        if (it == m_tiles.end()) continue;
        const std::vector<uint8_t>& after = it->second.adt.chunks[size_t(key.second)].colors;
        if (after != before)
            colors.push_back({ it->second.x, it->second.y, key.second, Base64Encode(before.data(), before.size()), Base64Encode(after.data(), after.size()) });
    }
    m_shadeBefore.clear();
    if (colors.empty()) return std::nullopt;
    return MakeChange(Edits{}, nlohmann::json::array(), "Shade " + std::to_string(colors.size()) + " chunk(s)", nlohmann::json::array(),
                      nlohmann::json::array(), nlohmann::json::array(), nlohmann::json::array(), colors);
}

std::optional<std::array<uint8_t, 3>> TerrainAdapter::ShadeAt(float x, float z) const
{
    const auto ref = ChunkAtGrid(int(std::floor(x / kChunkSize)), int(std::floor(z / kChunkSize)));
    if (!ref) return std::nullopt;
    const AdtChunk& c = *Chunk(*ref);
    if (c.colors.size() < 145 * 4) return std::nullopt;
    size_t best = 0;
    float bestD = 1e9f;
    for (size_t j = 0; j < 145; ++j)
    {
        float vx, vz;
        VertexXZ(j, vx, vz);
        const float dx = c.baseX + vx * kUnitSize - x, dz = c.baseZ + vz * kUnitSize - z, d = dx * dx + dz * dz;
        if (d < bestD) { bestD = d; best = j; }
    }
    return std::array<uint8_t, 3>{ c.colors[best * 4 + 2], c.colors[best * 4 + 1], c.colors[best * 4] };
}

void TerrainAdapter::BeginStroke(const TerrainHit& at)
{
    m_stroking = true;
    m_stroke.clear();
    m_flattenHeight = at.pos.y;
}

void TerrainAdapter::StrokeStep(const XMFLOAT3& center, const Brush& brush, float dt)
{
    if (!m_stroking) return;
    const float r = std::max(brush.radius, 0.5f);
    auto inRange = [&](const AdtChunk& c) {
        return !(center.x + r < c.baseX || center.x - r > c.baseX + kChunkSize || center.z + r < c.baseZ || center.z - r > c.baseZ + kChunkSize);
    };

    // Smooth targets the average height under the brush, measured before this step moves anything.
    float smoothTarget = 0;
    if (brush.mode == Brush::Mode::Smooth)
    {
        float sum = 0, weight = 0;
        for (const auto& [key, tile] : m_tiles)
            for (const AdtChunk& c : tile.adt.chunks)
            {
                if (!inRange(c)) continue;
                for (size_t j = 0; j < 145; ++j)
                {
                    float x, z;
                    VertexXZ(j, x, z);
                    const float dx = c.baseX + x * kUnitSize - center.x, dz = c.baseZ + z * kUnitSize - center.z;
                    if (dx * dx + dz * dz > r * r) continue;
                    sum += c.baseY + c.heights[j];
                    weight += 1;
                }
            }
        if (weight == 0) return;
        smoothTarget = sum / weight;
    }

    for (auto& [key, tile] : m_tiles)
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            AdtChunk& c = tile.adt.chunks[ci];
            if (!inRange(c)) continue;
            bool changed = false;
            for (size_t j = 0; j < 145; ++j)
            {
                float x, z;
                VertexXZ(j, x, z);
                const float dx = c.baseX + x * kUnitSize - center.x, dz = c.baseZ + z * kUnitSize - center.z;
                const float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > r) continue;
                const float t = 1.0f - dist / r;
                const float falloff = t * t * (3 - 2 * t);   // smoothstep: soft rim, full at the centre

                float& h = c.heights[j];
                auto& entry = m_stroke.try_emplace({ key, int(ci), int(j) }, std::pair{ h, h }).first->second;
                switch (brush.mode)
                {
                case Brush::Mode::Raise: h += brush.strength * falloff * dt; break;
                case Brush::Mode::Lower: h -= brush.strength * falloff * dt; break;
                case Brush::Mode::Flatten:
                case Brush::Mode::Smooth:
                {
                    const float target = (brush.mode == Brush::Mode::Flatten ? m_flattenHeight : smoothTarget) - c.baseY;
                    h += (target - h) * std::min(1.0f, brush.strength * 0.1f * falloff * dt);
                    break;
                }
                }
                entry.second = h;
                changed = true;
            }
            if (changed) m_renderer.UpdateChunk(key, ci, c);
        }
}

std::optional<Change> TerrainAdapter::EndStroke(const Brush& brush)
{
    m_stroking = false;
    if (m_stroke.empty()) return std::nullopt;
    Change c = MakeChange(m_stroke, nlohmann::json::array(), std::string(ModeName(brush.mode)) + " terrain");
    m_stroke.clear();
    if (c.data["edits"].empty()) return std::nullopt;
    c.label += ", " + std::to_string(c.data["edits"].size()) + " vertices";
    return c;
}

std::vector<std::pair<ChunkRef, int>> TerrainAdapter::HoleCellsAt(const XMFLOAT3& p, float radius) const
{
    std::vector<std::pair<ChunkRef, int>> cells;
    const float cell = kChunkSize / 4;
    const int x0 = int(std::floor((p.x - radius) / kChunkSize)), x1 = int(std::floor((p.x + radius) / kChunkSize));
    const int z0 = int(std::floor((p.z - radius) / kChunkSize)), z1 = int(std::floor((p.z + radius) / kChunkSize));
    for (int gz = z0; gz <= z1; ++gz)
        for (int gx = x0; gx <= x1; ++gx)
        {
            const auto ref = ChunkAtGrid(gx, gz);
            if (!ref) continue;
            const AdtChunk& c = *Chunk(*ref);
            for (int hr = 0; hr < 4; ++hr)
                for (int hc = 0; hc < 4; ++hc)
                {
                    const float minX = c.baseX + hc * cell, minZ = c.baseZ + hr * cell;
                    const bool under = p.x >= minX && p.x < minX + cell && p.z >= minZ && p.z < minZ + cell;
                    const float dx = minX + cell / 2 - p.x, dz = minZ + cell / 2 - p.z;
                    if (under || dx * dx + dz * dz <= radius * radius) cells.push_back({ *ref, hr * 4 + hc });
                }
        }
    return cells;
}

void TerrainAdapter::HoleStep(const XMFLOAT3& center, float radius, bool cut)
{
    if (!m_holing) return;
    for (const auto& [ref, bit] : HoleCellsAt(center, radius))
    {
        AdtChunk& c = m_tiles.at(ref.tile).adt.chunks[size_t(ref.chunk)];
        auto& entry = m_holeStroke.try_emplace({ ref.tile, ref.chunk }, std::pair{ c.holes, c.holes }).first->second;
        const uint16_t next = cut ? uint16_t(c.holes | (1u << bit)) : uint16_t(c.holes & ~(1u << bit));
        if (next == c.holes) continue;
        c.holes = next;
        entry.second = next;
        m_renderer.SetChunkHoles(ref.tile, size_t(ref.chunk), next);
    }
}

std::optional<Change> TerrainAdapter::EndHoles()
{
    m_holing = false;
    nlohmann::json holes = nlohmann::json::array();
    int cut = 0, filled = 0;
    for (const auto& [key, values] : m_holeStroke)
    {
        if (values.first == values.second) continue;
        const LoadedTile& tile = m_tiles.at(key.first);
        holes.push_back({ tile.x, tile.y, key.second, values.first, values.second });
        cut += std::popcount(uint16_t(values.second & ~values.first));
        filled += std::popcount(uint16_t(values.first & ~values.second));
    }
    m_holeStroke.clear();
    if (holes.empty()) return std::nullopt;
    const std::string label = cut && filled ? "Edit holes, " + std::to_string(cut + filled) + " cells"
                            : cut ? "Cut holes, " + std::to_string(cut) + " cells" : "Fill holes, " + std::to_string(filled) + " cells";
    return MakeChange(Edits{}, nlohmann::json::array(), label, holes);
}

std::vector<ChunkRef> TerrainAdapter::ChunksAt(const XMFLOAT3& p, float radius) const
{
    std::vector<ChunkRef> out;
    const int x0 = int(std::floor((p.x - radius) / kChunkSize)), x1 = int(std::floor((p.x + radius) / kChunkSize));
    const int z0 = int(std::floor((p.z - radius) / kChunkSize)), z1 = int(std::floor((p.z + radius) / kChunkSize));
    const int ux = int(std::floor(p.x / kChunkSize)), uz = int(std::floor(p.z / kChunkSize));
    for (int gz = z0; gz <= z1; ++gz)
        for (int gx = x0; gx <= x1; ++gx)
        {
            const float dx = (gx + 0.5f) * kChunkSize - p.x, dz = (gz + 0.5f) * kChunkSize - p.z;
            if (gx != ux || gz != uz)
                if (dx * dx + dz * dz > radius * radius) continue;
            if (const auto ref = ChunkAtGrid(gx, gz)) out.push_back(*ref);
        }
    return out;
}

void TerrainAdapter::AreaStep(const XMFLOAT3& center, float radius, uint32_t area)
{
    if (!m_areaing) return;
    for (const ChunkRef& ref : ChunksAt(center, radius))
    {
        AdtChunk& c = m_tiles.at(ref.tile).adt.chunks[size_t(ref.chunk)];
        m_areaStroke.try_emplace({ ref.tile, ref.chunk }, std::pair{ c.areaId, c.areaId }).first->second.second = area;
        c.areaId = area;
    }
}

std::optional<Change> TerrainAdapter::EndAreas(const std::string& label)
{
    m_areaing = false;
    nlohmann::json areas = nlohmann::json::array();
    for (const auto& [key, values] : m_areaStroke)
        if (values.first != values.second)
        {
            const LoadedTile& tile = m_tiles.at(key.first);
            areas.push_back({ tile.x, tile.y, key.second, values.first, values.second });
        }
    m_areaStroke.clear();
    if (areas.empty()) return std::nullopt;
    return MakeChange(Edits{}, nlohmann::json::array(), label + ", " + std::to_string(areas.size()) + " chunk(s)", nlohmann::json::array(),
                      nlohmann::json::array(), areas);
}

TerrainClipboard TerrainAdapter::Copy(const std::set<ChunkRef>& chunks) const
{
    std::set<std::pair<int, int>> cells;
    for (ChunkRef ref : chunks)
        if (Chunk(ref)) cells.insert(GridOf(ref));
    return CopyFrom(m_tiles, cells);
}

TerrainClipboard TerrainAdapter::CopyFrom(const std::map<int, LoadedTile>& tiles, const std::set<std::pair<int, int>>& cells)
{
    TerrainClipboard clip;
    int ox = 1 << 30, oz = 1 << 30;
    for (const auto& [gx, gz] : cells) { ox = std::min(ox, gx); oz = std::min(oz, gz); }
    clip.originX = ox;
    clip.originZ = oz;
    for (const auto& [gx, gz] : cells)
    {
        auto it = tiles.find(TileKey(gx / 16, gz / 16));
        if (it == tiles.end() || gx < 0 || gz < 0) continue;
        const int16_t index = it->second.byGrid[size_t((gz % 16) * 16 + gx % 16)];
        if (index < 0) continue;
        const AdtChunk& c = it->second.adt.chunks[size_t(index)];
        TerrainClipboard::Entry e{ gx - ox, gz - oz };
        for (size_t j = 0; j < 145; ++j) e.heights[j] = c.baseY + c.heights[j];
        e.layers = LayerState(it->second, c);
        e.holes = c.holes;
        e.liquids = LiquidState(it->second.adt, c);
        e.colors = c.colors;
        clip.chunks.push_back(std::move(e));
    }

    // Objects whose position lies on a copied chunk; an object listed by several tiles is taken once.
    auto onCopy = [&](const float pos[3]) { return cells.count({ int(std::floor(pos[0] / kChunkSize)), int(std::floor(pos[2] / kChunkSize)) }) != 0; };
    const float originX = ox * kChunkSize, originZ = oz * kChunkSize;
    std::set<uint32_t> seenDoodads, seenWmos;
    for (const auto& [key, tile] : tiles)
    {
        for (DoodadPlacement d : tile.adt.doodads)
            if (onCopy(d.pos) && seenDoodads.insert(d.uniqueId).second)
            {
                d.pos[0] -= originX;
                d.pos[2] -= originZ;
                clip.doodads.push_back(std::move(d));
            }
        for (WmoPlacement w : tile.adt.wmos)
            if (onCopy(w.pos) && seenWmos.insert(w.uniqueId).second)
            {
                w.pos[0] -= originX; w.extMin[0] -= originX; w.extMax[0] -= originX;
                w.pos[2] -= originZ; w.extMin[2] -= originZ; w.extMax[2] -= originZ;
                clip.wmos.push_back(std::move(w));
            }
    }
    return clip;
}

nlohmann::json TerrainClipboard::ToJson() const
{
    nlohmann::json chunksJson = nlohmann::json::array(), doodadsJson = nlohmann::json::array(), wmosJson = nlohmann::json::array();
    for (const Entry& e : chunks)
    {
        chunksJson.push_back({ { "dx", e.dx }, { "dz", e.dz },
                               { "heights", Base64Encode(reinterpret_cast<const uint8_t*>(e.heights.data()), e.heights.size() * sizeof(float)) },
                               { "layers", e.layers }, { "holes", e.holes }, { "liquids", e.liquids } });
        if (!e.colors.empty()) chunksJson.back()["colors"] = Base64Encode(e.colors.data(), e.colors.size());
    }
    for (const auto& d : doodads) doodadsJson.push_back(::ToJson(d));
    for (const auto& w : wmos) wmosJson.push_back(::ToJson(w));
    return { { "origin", { originX, originZ } }, { "chunks", chunksJson }, { "doodads", doodadsJson }, { "wmos", wmosJson }, { "pois", pois } };
}

TerrainClipboard TerrainClipboard::FromJson(const nlohmann::json& j)
{
    TerrainClipboard clip;
    clip.originX = j.at("origin")[0];
    clip.originZ = j.at("origin")[1];
    for (const auto& c : j.at("chunks"))
    {
        Entry e{ c.at("dx"), c.at("dz") };
        const std::vector<uint8_t> bytes = Base64Decode(c.at("heights").get<std::string>());
        std::memcpy(e.heights.data(), bytes.data(), std::min(bytes.size(), e.heights.size() * sizeof(float)));
        e.layers = c.value("layers", nlohmann::json());
        e.holes = c.value("holes", 0);
        e.liquids = c.value("liquids", nlohmann::json());   // blueprints saved before water was copied: leave water alone
        if (c.contains("colors")) e.colors = Base64Decode(c.at("colors").get<std::string>());
        clip.chunks.push_back(std::move(e));
    }
    for (const auto& d : j.value("doodads", nlohmann::json::array())) clip.doodads.push_back(DoodadFrom(d));
    for (const auto& w : j.value("wmos", nlohmann::json::array())) clip.wmos.push_back(WmoFrom(w));
    clip.pois = j.value("pois", nlohmann::json::array());
    return clip;
}

Adt TerrainClipboard::ToAdt() const
{
    LoadedTile scratch;   // collects the texture names the layers use
    for (const Entry& e : chunks)
    {
        AdtChunk c;
        c.baseX = (originX + e.dx) * kChunkSize;
        c.baseZ = (originZ + e.dz) * kChunkSize;
        c.baseY = *std::min_element(e.heights.begin(), e.heights.end());
        c.indexX = uint32_t((originX + e.dx) % 16);
        c.indexY = uint32_t((originZ + e.dz) % 16);
        for (size_t k = 0; k < 145; ++k) c.heights[k] = e.heights[k] - c.baseY;
        c.holes = e.holes;
        c.colors = e.colors;
        if (!e.layers.is_null()) TerrainAdapter::SetLayerState(scratch, c, e.layers);
        else c.alpha.assign(64 * 64 * 4, 0);
        if (!e.liquids.is_null()) TerrainAdapter::SetLiquidState(scratch.adt, c, e.liquids);
        scratch.adt.chunks.push_back(std::move(c));
    }
    for (DoodadPlacement d : doodads)
    {
        d.pos[0] += originX * kChunkSize;
        d.pos[2] += originZ * kChunkSize;
        scratch.adt.doodads.push_back(std::move(d));
    }
    for (WmoPlacement w : wmos)
    {
        w.pos[0] += originX * kChunkSize; w.extMin[0] += originX * kChunkSize; w.extMax[0] += originX * kChunkSize;
        w.pos[2] += originZ * kChunkSize; w.extMin[2] += originZ * kChunkSize; w.extMax[2] += originZ * kChunkSize;
        scratch.adt.wmos.push_back(std::move(w));
    }
    return std::move(scratch.adt);
}

float TerrainClipboard::MinHeight() const
{
    float lo = 1e9f;
    for (const auto& e : chunks)
        for (float h : e.heights) lo = std::min(lo, h);
    return chunks.empty() ? 0.0f : lo;
}

float TerrainClipboard::MeanHeight() const
{
    double sum = 0;
    for (const auto& e : chunks)
        for (float h : e.heights) sum += h;
    return chunks.empty() ? 0.0f : float(sum / (chunks.size() * 145.0));
}

void TerrainClipboard::RotateClockwise()
{
    // Grid cell (col, row) goes to (last - row, col); the same turn for chunks, vertices and alpha texels.
    const int depth = Depth();
    for (auto& e : chunks)
    {
        const int dx = e.dx, dz = e.dz;
        e.dx = depth - 1 - dz;
        e.dz = dx;

        // Vertex j goes to turned(j): the same for heights and vertex colours.
        auto turned = [](size_t j) {
            const int r = int(j / 17), c = int(j % 17);
            return c < 9 ? size_t(c * 17 + (8 - r)) : size_t((c - 9) * 17 + 9 + (7 - r));
        };
        std::array<float, 145> h{};
        for (size_t j = 0; j < 145; ++j) h[turned(j)] = e.heights[j];
        e.heights = h;
        if (e.colors.size() >= 145 * 4)
        {
            std::vector<uint8_t> col(145 * 4);
            for (size_t j = 0; j < 145; ++j) std::memcpy(&col[turned(j) * 4], &e.colors[j * 4], 4);
            e.colors = std::move(col);
        }

        uint16_t holes = 0;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (e.holes & (1u << (r * 4 + c))) holes |= uint16_t(1u << (c * 4 + (3 - r)));
        e.holes = holes;
        if (!e.liquids.is_null()) e.liquids = TerrainAdapter::RotateLiquidState(e.liquids);

        if (e.layers.is_null()) continue;
        const std::vector<uint8_t> in = Base64Decode(e.layers.at("alpha").get<std::string>());
        if (in.size() < 64 * 64 * 3) continue;
        std::vector<uint8_t> out(64 * 64 * 3);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                for (int ch = 0; ch < 3; ++ch) out[size_t((x * 64 + (63 - y)) * 3 + ch)] = in[size_t((y * 64 + x) * 3 + ch)];
        e.layers["alpha"] = Base64Encode(out.data(), out.size());
    }

    // Objects: (x, z) -> (span - z, x) in yards, a -90 degree turn about y, so the yaw drops by 90.
    const float span = depth * kChunkSize;
    for (auto& d : doodads)
    {
        const float x = d.pos[0];
        d.pos[0] = span - d.pos[2];
        d.pos[2] = x;
        d.rot[1] = std::fmod(d.rot[1] - 90.0f + 360.0f, 360.0f);
    }
    for (auto& w : wmos)
    {
        const float x = w.pos[0], minX = w.extMin[0], maxX = w.extMax[0];
        w.pos[0] = span - w.pos[2];
        w.pos[2] = x;
        w.extMin[0] = span - w.extMax[2];
        w.extMax[0] = span - w.extMin[2];
        w.extMin[2] = minX;
        w.extMax[2] = maxX;
        w.rot[1] = std::fmod(w.rot[1] - 90.0f + 360.0f, 360.0f);
    }
    for (auto& p : pois)
    {
        auto& pos = p.at("pos");
        const float x = pos[0];
        pos[0] = span - pos[2].get<float>();
        pos[2] = x;
    }
}

std::vector<ChunkRef> TerrainAdapter::Footprint(const TerrainClipboard& clip, int gx, int gz) const
{
    std::vector<ChunkRef> refs;
    for (const auto& e : clip.chunks)
        if (auto ref = ChunkAtGrid(gx + e.dx, gz + e.dz)) refs.push_back(*ref);
    return refs;
}

std::optional<float> TerrainAdapter::FootprintMin(const TerrainClipboard& clip, int gx, int gz) const
{
    std::optional<float> lo;
    for (ChunkRef ref : Footprint(clip, gx, gz))
    {
        const AdtChunk& c = *Chunk(ref);
        for (float h : c.heights) lo = std::min(lo.value_or(1e9f), c.baseY + h);
    }
    return lo;
}

std::optional<float> TerrainAdapter::FootprintMean(const TerrainClipboard& clip, int gx, int gz) const
{
    double sum = 0;
    size_t count = 0;
    for (ChunkRef ref : Footprint(clip, gx, gz))
    {
        const AdtChunk& c = *Chunk(ref);
        for (float h : c.heights) sum += c.baseY + h;
        count += 145;
    }
    return count ? std::optional<float>(float(sum / count)) : std::nullopt;
}

// ---------------------------------------------------------------------------------------------- paste plans

namespace
{
    struct DecodedLayers
    {
        std::vector<std::string> names;
        std::vector<uint32_t> flags, effects;
        std::vector<uint8_t> alpha;   // 64x64x3
    };

    DecodedLayers Decode(const nlohmann::json& s)
    {
        DecodedLayers d;
        for (const auto& n : s.at("names")) d.names.push_back(n);
        for (const auto& f : s.at("flags")) d.flags.push_back(f);
        for (const auto& e : s.at("effects")) d.effects.push_back(e);
        d.alpha = Base64Decode(s.at("alpha").get<std::string>());
        d.alpha.resize(64 * 64 * 3, 0);
        return d;
    }

    /// Per-texel weight of each texture of a layer set.
    /// Per-texel weights by texture, keyed by normalised name (one texture can be spelled "Tileset\X.blp" in one map
    /// and "tileset/x.blp" in another); `spelling` keeps the first spelling seen.
    void AddWeights(const DecodedLayers& d, const float* t, bool pasteSide, std::map<std::string, std::vector<float>>& out,
                    std::map<std::string, std::string>& spelling)
    {
        for (size_t l = 0; l < d.names.size() && l < 4; ++l)
        {
            const std::string key = Catalog::Normalize(d.names[l]);
            spelling.try_emplace(key, d.names[l]);
            auto& w = out[key];
            if (w.empty()) w.assign(4096, 0.0f);
            for (size_t i = 0; i < 4096; ++i)
            {
                const auto lw = LayerWeights(d.names.size() > 1 ? d.alpha[i * 3] / 255.0f : 0.0f, d.names.size() > 2 ? d.alpha[i * 3 + 1] / 255.0f : 0.0f,
                                             d.names.size() > 3 ? d.alpha[i * 3 + 2] / 255.0f : 0.0f);
                w[i] += lw[l] * (pasteSide ? t[i] : 1.0f - t[i]);
            }
        }
    }

    /// Crossfades two layer sets texel by texel (t = 0 ground, 1 paste) and keeps the four strongest textures.
    nlohmann::json BlendLayers(const nlohmann::json& groundState, const nlohmann::json& pasteState, const float* t)
    {
        const DecodedLayers ground = Decode(groundState), paste = Decode(pasteState);
        std::map<std::string, std::vector<float>> weights;
        std::map<std::string, std::string> spelling;   // the ground's spelling wins
        AddWeights(ground, t, false, weights, spelling);
        AddWeights(paste, t, true, weights, spelling);

        std::vector<std::pair<float, std::string>> totals;
        for (const auto& [name, w] : weights)
        {
            float sum = 0;
            for (float v : w) sum += v;
            totals.push_back({ sum, name });
        }
        std::sort(totals.begin(), totals.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        if (totals.size() > 4) totals.resize(4);   // a chunk holds four layers; the weakest give way

        nlohmann::json names = nlohmann::json::array(), flags = nlohmann::json::array(), effects = nlohmann::json::array();
        for (const auto& [sum, name] : totals)
        {
            names.push_back(spelling.at(name));
            auto from = [&](const DecodedLayers& d, uint32_t& flag, uint32_t& effect) {
                for (size_t l = 0; l < d.names.size(); ++l)
                    if (Catalog::Normalize(d.names[l]) == name) { flag = d.flags[l]; effect = d.effects[l]; return true; }
                return false;
            };
            uint32_t flag = 0, effect = 0;
            if (!from(paste, flag, effect)) from(ground, flag, effect);
            flags.push_back(flag);
            effects.push_back(effect);
        }

        std::vector<uint8_t> rgb(64 * 64 * 3, 0);
        for (size_t i = 0; i < 4096; ++i)
        {
            std::array<float, 4> w{};
            float sum = 0;
            for (size_t k = 0; k < totals.size(); ++k) sum += w[k] = weights[totals[k].second][i];
            if (sum <= 1e-6f) { w = { 1, 0, 0, 0 }; sum = 1; }
            for (float& v : w) v /= sum;
            const auto a = LayerAlphas(w);
            for (size_t ch = 0; ch < 3; ++ch) rgb[i * 3 + ch] = uint8_t(std::lround(a[ch] * 255.0f));
        }
        return { { "names", names }, { "flags", flags }, { "effects", effects }, { "alpha", Base64Encode(rgb.data(), rgb.size()) } };
    }
}

PastePlan TerrainAdapter::PlanPaste(const TerrainClipboard& clip, int gx, int gz, float offset, const PasteOptions& o) const
{
    PastePlan plan;
    auto pastedHeight = [&](const TerrainClipboard::Entry& e, size_t j) {
        const size_t row = j / 17, col = j % 17;
        const bool inner = col >= 9;
        const float x = float(e.dx * 8) + (inner ? float(col - 9) + 0.5f : float(col));
        const float z = float(e.dz * 8) + (inner ? float(row) + 0.5f : float(row));
        return e.heights[j] + offset + o.slopeX * x + o.slopeZ * z;
    };
    // Water: the copy's instances replace the chunk's (none removes its water), raised with the paste.
    auto pastedWater = [&](const TerrainClipboard::Entry& e) {
        if (!o.water || e.liquids.is_null()) return nlohmann::json();
        const float lift = offset + o.slopeX * float(e.dx * 8 + 4) + o.slopeZ * float(e.dz * 8 + 4);
        nlohmann::json w = e.liquids;
        for (auto& l : w)
            for (auto& h : l.at("heights")) h = h.get<float>() + lift;
        return w;
    };
    std::map<std::pair<int, int>, const TerrainClipboard::Entry*> pasted;   // grid cell -> clipboard chunk
    for (const auto& e : clip.chunks)
        if (auto ref = ChunkAtGrid(gx + e.dx, gz + e.dz))
        {
            pasted[{ gx + e.dx, gz + e.dz }] = &e;
            plan.footprint.push_back(*ref);
        }
    if (pasted.empty()) return plan;

    if (o.objects)
    {
        // Objects keep their place on the copy: moved with it and lifted by the same offset and tilt.
        const float originX = gx * kChunkSize, originZ = gz * kChunkSize;
        auto lift = [&](const float rel[3]) { return offset + o.slopeX * rel[0] / kUnitSize + o.slopeZ * rel[2] / kUnitSize; };
        auto onPaste = [&](float x, float z) { return pasted.count({ int(std::floor(x / kChunkSize)), int(std::floor(z / kChunkSize)) }) != 0; };
        for (DoodadPlacement d : clip.doodads)
        {
            const float dy = lift(d.pos);
            d.pos[0] += originX; d.pos[1] += dy; d.pos[2] += originZ;
            d.uniqueId = 0;
            if (onPaste(d.pos[0], d.pos[2])) plan.doodads.push_back(std::move(d));
        }
        for (WmoPlacement w : clip.wmos)
        {
            const float dy = lift(w.pos);
            w.pos[0] += originX; w.extMin[0] += originX; w.extMax[0] += originX;
            w.pos[2] += originZ; w.extMin[2] += originZ; w.extMax[2] += originZ;
            w.pos[1] += dy; w.extMin[1] += dy; w.extMax[1] += dy;
            w.uniqueId = 0;
            // A building comes along when its origin or its bounds touch the paste (a compared cave whose origin is off to one side).
            bool reaches = onPaste(w.pos[0], w.pos[2]);
            for (const auto& [cell, e] : pasted)
                reaches = reaches || (w.extMax[0] >= cell.first * kChunkSize && w.extMin[0] < (cell.first + 1) * kChunkSize &&
                                      w.extMax[2] >= cell.second * kChunkSize && w.extMin[2] < (cell.second + 1) * kChunkSize);
            if (reaches) plan.wmos.push_back(std::move(w));
        }
        for (nlohmann::json p : clip.pois)
        {
            float rel[3] = { p.at("pos")[0], p.at("pos")[1], p.at("pos")[2] };
            const float dy = p.value("ground", false) ? 0.0f : lift(rel);
            p["pos"] = { rel[0] + originX, rel[1] + dy, rel[2] + originZ };
            if (onPaste(rel[0] + originX, rel[2] + originZ)) plan.pois.push_back(std::move(p));
        }
    }

    if (!o.blend)
    {
        for (const auto& [cell, e] : pasted)
        {
            const ChunkRef ref = *ChunkAtGrid(cell.first, cell.second);
            const AdtChunk& c = *Chunk(ref);
            PastePlan::Chunk pc{ ref };
            if (o.heights)
            {
                pc.setHeights = true;
                for (size_t j = 0; j < 145; ++j) pc.heights[j] = pastedHeight(*e, j) - c.baseY;
            }
            if (o.textures && !e->layers.is_null()) pc.layers = e->layers;
            if (o.textures && m_vertexColors) pc.colors = e->colors;
            if (o.holes) pc.holes = e->holes;
            pc.liquids = pastedWater(*e);
            plan.chunks.push_back(std::move(pc));
        }
        if (!o.heights) return plan;
        // Even a hard paste must close: every chunk keeps its own copy of the vertices on its edges, so a ground
        // chunk next to the paste takes the pasted height on the ones it shares with it (a steep step), and its
        // inner vertices follow their cell corners. Left alone, the client shows a gap down to the fog.
        std::map<std::pair<int, int>, float> edge;   // outer-vertex node (grid cell * 8 + col/row) -> pasted height
        for (const auto& [cell, e] : pasted)
            for (int row = 0; row <= 8; ++row)
                for (int col = 0; col <= 8; ++col) edge[{ cell.first * 8 + col, cell.second * 8 + row }] = pastedHeight(*e, size_t(row * 17 + col));
        std::set<std::pair<int, int>> around;
        for (const auto& [cell, e] : pasted)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if (!pasted.count({ cell.first + dx, cell.second + dz })) around.insert({ cell.first + dx, cell.second + dz });
        for (const auto& [cx, cz] : around)
        {
            const auto ref = ChunkAtGrid(cx, cz);
            if (!ref) continue;   // ponytail: an unloaded neighbour tile stays open; Problems reports the crack
            const AdtChunk& c = *Chunk(*ref);
            PastePlan::Chunk pc{ *ref };
            float corr[9][9];
            bool changed = false;
            for (int row = 0; row <= 8; ++row)
                for (int col = 0; col <= 8; ++col)
                {
                    const size_t j = size_t(row * 17 + col);
                    const auto it = edge.find({ cx * 8 + col, cz * 8 + row });
                    const float own = c.baseY + c.heights[j], value = it != edge.end() ? it->second : own;
                    corr[row][col] = value - own;
                    pc.heights[j] = value - c.baseY;
                    changed |= std::fabs(corr[row][col]) > 1e-3f;
                }
            if (!changed) continue;
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 8; ++col)
                {
                    const size_t j = size_t(row * 17 + 9 + col);
                    pc.heights[j] = c.heights[j] + (corr[row][col] + corr[row][col + 1] + corr[row + 1][col] + corr[row + 1][col + 1]) / 4;
                }
            pc.setHeights = true;
            plan.chunks.push_back(std::move(pc));
        }
        return plan;
    }

    // Region: the paste's bounding cells plus a band of chunks on every side, as one grid of outer vertices.
    int minX = 1 << 30, minZ = 1 << 30, maxX = -1, maxZ = -1;
    for (const auto& [cell, e] : pasted)
    {
        minX = std::min(minX, cell.first);  maxX = std::max(maxX, cell.first);
        minZ = std::min(minZ, cell.second); maxZ = std::max(maxZ, cell.second);
    }
    const int band = o.widthYards > 0 ? int(std::ceil(o.widthYards / kChunkSize)) + 1 : 4;
    const int rx0 = minX - band, rz0 = minZ - band, rw = maxX - minX + 1 + 2 * band, rd = maxZ - minZ + 1 + 2 * band;

    BlendGrid g;
    g.width = rw * 8 + 1;
    g.height = rd * 8 + 1;
    const size_t nodes = size_t(g.width) * g.height;
    g.ground.assign(nodes, 0);
    g.pasted.assign(nodes, 0);
    g.inside.assign(nodes, 0);
    g.exists.assign(nodes, 0);
    for (int cz = rz0; cz < rz0 + rd; ++cz)
        for (int cx = rx0; cx < rx0 + rw; ++cx)
        {
            const auto ref = ChunkAtGrid(cx, cz);
            if (!ref) continue;
            const AdtChunk& c = *Chunk(*ref);
            auto it = pasted.find({ cx, cz });
            for (int row = 0; row <= 8; ++row)
                for (int col = 0; col <= 8; ++col)
                {
                    const size_t i = g.At((cx - rx0) * 8 + col, (cz - rz0) * 8 + row), j = size_t(row * 17 + col);
                    g.exists[i] = 1;
                    g.ground[i] = c.baseY + c.heights[j];
                    if (it != pasted.end())
                    {
                        g.inside[i] = 1;
                        g.pasted[i] = pastedHeight(*it->second, j);
                    }
                }
        }

    plan.widthYards = o.widthYards > 0 ? o.widthYards : AutoBlendWidth(g);
    const float wNodes = plan.widthYards / kUnitSize;
    const BlendResult r = BlendHeights(g, wNodes);

    for (int cz = rz0; cz < rz0 + rd; ++cz)
        for (int cx = rx0; cx < rx0 + rw; ++cx)
        {
            const auto ref = ChunkAtGrid(cx, cz);
            if (!ref) continue;
            const AdtChunk& c = *Chunk(*ref);
            auto it = pasted.find({ cx, cz });
            const bool inside = it != pasted.end();
            PastePlan::Chunk pc{ *ref };
            auto node = [&](int row, int col) { return g.At((cx - rx0) * 8 + col, (cz - rz0) * 8 + row); };

            if (o.heights)
            {
                // Outer vertices straight from the blend; inner vertices keep their own detail plus the
                // average correction of their cell's four corners, each measured against this chunk's own
                // vertex (a corner shared with the paste must not hand its pasted height to a ground chunk).
                auto own = [&](size_t j) { return inside ? pastedHeight(*it->second, j) : c.baseY + c.heights[j]; };
                float corr[9][9];
                bool changed = false;
                for (int row = 0; row <= 8; ++row)
                    for (int col = 0; col <= 8; ++col)
                    {
                        const size_t i = node(row, col), j = size_t(row * 17 + col);
                        const float value = inside ? own(j) : r.heights[i];   // the paste is kept exactly
                        corr[row][col] = value - own(j);
                        pc.heights[j] = value - c.baseY;
                    }
                for (int row = 0; row < 8; ++row)
                    for (int col = 0; col < 8; ++col)
                    {
                        const size_t j = size_t(row * 17 + 9 + col);
                        const float avg = (corr[row][col] + corr[row][col + 1] + corr[row + 1][col] + corr[row + 1][col + 1]) / 4;
                        pc.heights[j] = own(j) + avg - c.baseY;
                    }
                for (size_t j = 0; j < 145; ++j) changed |= std::fabs(pc.heights[j] - c.heights[j]) > 1e-3f;
                pc.setHeights = changed;
            }

            if (o.textures && inside && !it->second->layers.is_null())
            {
                // Texel t from the distance to the paste's edge: ground textures at the edge, the paste's inside.
                std::vector<float> t(4096);
                float tMin = 1.0f;
                for (int ty = 0; ty < 64; ++ty)
                    for (int tx = 0; tx < 64; ++tx)
                    {
                        const float nx = (tx + 0.5f) / 8.0f, nz = (ty + 0.5f) / 8.0f;
                        const int x0 = std::min(int(nx), 7), z0 = std::min(int(nz), 7);
                        const float fx = nx - x0, fz = nz - z0;
                        const float d = (r.insideDist[node(z0, x0)] * (1 - fx) + r.insideDist[node(z0, x0 + 1)] * fx) * (1 - fz) +
                                        (r.insideDist[node(z0 + 1, x0)] * (1 - fx) + r.insideDist[node(z0 + 1, x0 + 1)] * fx) * fz;
                        const float v = std::clamp((d - 0.5f) / std::min(wNodes, 4.0f), 0.0f, 1.0f);   // ~17 yd fade inside the edge
                        t[size_t(ty * 64 + tx)] = v * v * (3 - 2 * v);
                        tMin = std::min(tMin, t[size_t(ty * 64 + tx)]);
                    }
                pc.layers = tMin >= 1.0f ? it->second->layers : BlendLayers(LayerState(m_tiles.at(ref->tile), c), it->second->layers, t.data());
            }
            if (o.textures && inside && m_vertexColors && it->second->colors.size() >= 145 * 4)
            {
                // Vertex shading fades in over the same distance as the textures, from the ground's own colours.
                auto fade = [&](int row, int col) {
                    const float v = std::clamp((r.insideDist[node(row, col)] - 0.5f) / std::min(wNodes, 4.0f), 0.0f, 1.0f);
                    return v * v * (3 - 2 * v);
                };
                pc.colors = it->second->colors;
                for (size_t j = 0; j < 145; ++j)
                {
                    const int row = int(j / 17), col = int(j % 17);
                    const float t = col < 9 ? fade(row, col)
                                            : (fade(row, col - 9) + fade(row, col - 8) + fade(row + 1, col - 9) + fade(row + 1, col - 8)) / 4;
                    for (size_t k = 0; k < 4; ++k)
                    {
                        const float ground = c.colors.size() >= 145 * 4 ? c.colors[j * 4 + k] : 0x7F;
                        pc.colors[j * 4 + k] = uint8_t(std::lround(ground + (pc.colors[j * 4 + k] - ground) * t));
                    }
                }
            }
            if (o.holes && inside) pc.holes = it->second->holes;
            if (inside) pc.liquids = pastedWater(*it->second);
            if (pc.setHeights || !pc.layers.is_null() || pc.holes || !pc.liquids.is_null() || !pc.colors.empty()) plan.chunks.push_back(std::move(pc));
        }
    return plan;
}

std::optional<Change> TerrainAdapter::ApplyPlan(const PastePlan& plan, const std::string& label)
{
    PreviewPlan(nullptr);
    Edits edits;
    nlohmann::json layers = nlohmann::json::array(), holes = nlohmann::json::array(), liquids = nlohmann::json::array(), colors = nlohmann::json::array();
    std::set<std::pair<int, int>> touched;
    std::set<int> water;
    for (const auto& pc : plan.chunks)
    {
        auto it = m_tiles.find(pc.ref.tile);
        if (it == m_tiles.end() || size_t(pc.ref.chunk) >= it->second.adt.chunks.size()) continue;
        LoadedTile& tile = it->second;
        AdtChunk& c = tile.adt.chunks[size_t(pc.ref.chunk)];
        if (pc.setHeights)
            for (size_t j = 0; j < 145; ++j)
            {
                edits[{ pc.ref.tile, pc.ref.chunk, int(j) }] = { c.heights[j], pc.heights[j] };
                c.heights[j] = pc.heights[j];
            }
        if (!pc.layers.is_null())
        {
            nlohmann::json before = LayerState(tile, c);
            SetLayerState(tile, c, pc.layers);
            layers.push_back({ tile.x, tile.y, pc.ref.chunk, std::move(before), pc.layers });
            RefreshTextures(pc.ref.tile, size_t(pc.ref.chunk));
        }
        if (pc.holes && *pc.holes != c.holes)
        {
            holes.push_back({ tile.x, tile.y, pc.ref.chunk, c.holes, *pc.holes });
            c.holes = *pc.holes;
            m_renderer.SetChunkHoles(pc.ref.tile, size_t(pc.ref.chunk), c.holes);
        }
        if (!pc.colors.empty() && pc.colors != c.colors)
        {
            colors.push_back({ tile.x, tile.y, pc.ref.chunk, Base64Encode(c.colors.data(), c.colors.size()), Base64Encode(pc.colors.data(), pc.colors.size()) });
            c.colors = pc.colors;
        }
        if (!pc.liquids.is_null())
            if (nlohmann::json before = LiquidState(tile.adt, c); before != pc.liquids)
            {
                SetLiquidState(tile.adt, c, pc.liquids);
                liquids.push_back({ tile.x, tile.y, pc.ref.chunk, std::move(before), pc.liquids });
                water.insert(pc.ref.tile);
            }
        touched.insert({ pc.ref.tile, pc.ref.chunk });
    }
    for (int key : water) m_renderer.UpdateWater(key, m_tiles.at(key).adt.liquids, m_mpq);
    for (const auto& [key, ci] : touched) m_renderer.UpdateChunk(key, size_t(ci), m_tiles.at(key).adt.chunks[size_t(ci)]);

    const nlohmann::json objects = AddObjects(plan.doodads, plan.wmos, nullptr);

    if (touched.empty() && objects.empty()) return std::nullopt;
    Change c = MakeChange(edits, layers, label, holes, objects, nlohmann::json::array(), liquids, colors);
    if (c.data["edits"].empty() && !c.data.contains("layers") && !c.data.contains("holes") && !c.data.contains("objects") &&
        !c.data.contains("liquids") && !c.data.contains("colors"))
        return std::nullopt;
    return c;
}

/// The chunk as the plan would leave it, with its textures listed in `textures`.
static AdtChunk PlannedChunk(const LoadedTile& tile, const PastePlan::Chunk& pc, LoadedTile& scratch)
{
    AdtChunk c = tile.adt.chunks[size_t(pc.ref.chunk)];
    if (pc.setHeights) c.heights = pc.heights;
    if (pc.holes) c.holes = *pc.holes;
    if (!pc.colors.empty()) c.colors = pc.colors;
    TerrainAdapter::SetLayerState(scratch, c, pc.layers.is_null() ? TerrainAdapter::LayerState(tile, tile.adt.chunks[size_t(pc.ref.chunk)]) : pc.layers);
    return c;
}

void TerrainAdapter::PreviewPlan(const PastePlan* plan)
{
    for (ChunkRef ref : m_previewed)   // put back what the last preview changed
        if (auto it = m_tiles.find(ref.tile); it != m_tiles.end())
        {
            m_renderer.UpdateChunk(ref.tile, size_t(ref.chunk), it->second.adt.chunks[size_t(ref.chunk)]);
            m_renderer.SetChunkHoles(ref.tile, size_t(ref.chunk), it->second.adt.chunks[size_t(ref.chunk)].holes);
            RefreshTextures(ref.tile, size_t(ref.chunk));
        }
    m_previewed.clear();
    for (int key : m_previewedWater)
        if (auto it = m_tiles.find(key); it != m_tiles.end()) m_renderer.UpdateWater(key, it->second.adt.liquids, m_mpq);
    m_previewedWater.clear();
    if (!plan) return;
    std::map<int, Adt> water;   // tiles whose water the plan changes, with their liquids as it leaves them
    for (const auto& pc : plan->chunks)
        if (!pc.liquids.is_null())
            if (auto it = m_tiles.find(pc.ref.tile); it != m_tiles.end())
            {
                auto [w, added] = water.try_emplace(pc.ref.tile);
                if (added) w->second.liquids = it->second.adt.liquids;
                SetLiquidState(w->second, it->second.adt.chunks[size_t(pc.ref.chunk)], pc.liquids);
            }
    for (const auto& [key, adt] : water)
    {
        m_renderer.UpdateWater(key, adt.liquids, m_mpq);
        m_previewedWater.insert(key);
    }
    for (const auto& pc : plan->chunks)
    {
        auto it = m_tiles.find(pc.ref.tile);
        if (it == m_tiles.end()) continue;
        LoadedTile scratch;
        scratch.adt.textures = it->second.adt.textures;
        const AdtChunk c = PlannedChunk(it->second, pc, scratch);
        m_renderer.UpdateChunk(pc.ref.tile, size_t(pc.ref.chunk), c);
        m_renderer.SetChunkHoles(pc.ref.tile, size_t(pc.ref.chunk), c.holes);
        m_renderer.UpdateChunkTextures(pc.ref.tile, size_t(pc.ref.chunk), c, scratch.adt.textures, m_mpq);
        m_previewed.push_back(pc.ref);
    }
}

std::optional<std::array<float, 3>> TerrainAdapter::FitSlope(const TerrainClipboard& clip, int gx, int gz) const
{
    // Least squares for r = a + b x + c z, with r = ground - copy at each outer vertex.
    double s[3][3] = {}, v[3] = {};
    size_t count = 0;
    for (const auto& e : clip.chunks)
    {
        const auto ref = ChunkAtGrid(gx + e.dx, gz + e.dz);
        if (!ref) continue;
        const AdtChunk& c = *Chunk(*ref);
        for (int row = 0; row <= 8; ++row)
            for (int col = 0; col <= 8; ++col)
            {
                const size_t j = size_t(row * 17 + col);
                const double x = e.dx * 8 + col, z = e.dz * 8 + row, r = c.baseY + c.heights[j] - e.heights[j];
                const double basis[3] = { 1, x, z };
                for (int a = 0; a < 3; ++a)
                {
                    v[a] += basis[a] * r;
                    for (int b2 = 0; b2 < 3; ++b2) s[a][b2] += basis[a] * basis[b2];
                }
                ++count;
            }
    }
    if (count < 3) return std::nullopt;
    // Solve the 3x3 system by Cramer's rule.
    auto det = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double d = det(s);
    if (std::fabs(d) < 1e-9) return std::nullopt;
    std::array<float, 3> out{};
    for (int k = 0; k < 3; ++k)
    {
        double m[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b2 = 0; b2 < 3; ++b2) m[a][b2] = b2 == k ? v[a] : s[a][b2];
        out[size_t(k)] = float(det(m) / d);
    }
    return out;
}

Adt TerrainAdapter::BuildGhost(const PastePlan& plan) const
{
    LoadedTile ghost;
    for (const auto& pc : plan.chunks)
        if (auto it = m_tiles.find(pc.ref.tile); it != m_tiles.end())
            ghost.adt.chunks.push_back(PlannedChunk(it->second, pc, ghost));
    ghost.adt.doodads = plan.doodads;
    ghost.adt.wmos = plan.wmos;
    return std::move(ghost.adt);
}

std::optional<Change> TerrainAdapter::Paste(const TerrainClipboard& clip, int gx, int gz, float offset, bool heights, bool textures)
{
    PasteOptions o;
    o.heights = heights;
    o.textures = textures;
    o.blend = false;
    o.objects = false;
    return ApplyPlan(PlanPaste(clip, gx, gz, offset, o), "Paste " + std::to_string(clip.chunks.size()) + " chunk(s)");
}

std::optional<Change> TerrainAdapter::RotateInPlace(const std::set<ChunkRef>& chunks, std::vector<ChunkRef>* footprint)
{
    if (chunks.empty()) return std::nullopt;
    TerrainClipboard clip = Copy(chunks);
    int minX = 1 << 30, minZ = 1 << 30;
    for (ChunkRef r : chunks)
    {
        const auto [gx, gz] = GridOf(r);
        minX = std::min(minX, gx);
        minZ = std::min(minZ, gz);
    }
    const int cx = minX + (clip.Width() - 1) / 2, cz = minZ + (clip.Depth() - 1) / 2;
    clip.RotateClockwise();
    const int ax = cx - (clip.Width() - 1) / 2, az = cz - (clip.Depth() - 1) / 2;

    // Where each object ends up: the turned clipboard's spot, anchored like the terrain.
    std::map<std::pair<bool, uint32_t>, std::pair<std::array<float, 3>, std::array<float, 3>>> turned;   // (wmo, uid) -> pos, rot
    const float originX = ax * kChunkSize, originZ = az * kChunkSize;
    for (const auto& d : clip.doodads) turned[{ false, d.uniqueId }] = { { d.pos[0] + originX, d.pos[1], d.pos[2] + originZ }, { d.rot[0], d.rot[1], d.rot[2] } };
    for (const auto& w : clip.wmos) turned[{ true, w.uniqueId }] = { { w.pos[0] + originX, w.pos[1], w.pos[2] + originZ }, { w.rot[0], w.rot[1], w.rot[2] } };
    auto place = [&](bool wmo, uint32_t uid, float* pos, float* rot) {
        auto it = turned.find({ wmo, uid });
        if (it == turned.end()) return;
        std::copy(it->second.first.begin(), it->second.first.end(), pos);
        std::copy(it->second.second.begin(), it->second.second.end(), rot);
    };

    auto change = Paste(clip, ax, az, 0.0f, true, true);
    if (!change) return std::nullopt;
    if (!turned.empty())
    {
        std::set<ObjectRef> objects;
        for (const auto& [key, value] : turned) objects.insert({ key.first, key.second });
        BeginObjectEdit(objects);
        PreviewObjectEdit([&](DoodadPlacement& d) { place(false, d.uniqueId, d.pos, d.rot); }, [&](WmoPlacement& w) { place(true, w.uniqueId, w.pos, w.rot); });
        if (auto moved = EndObjectEdit({}))   // folded into the terrain change: one undo step
        {
            nlohmann::json& list = change->data["objects"];
            if (!list.is_array()) list = nlohmann::json::array();
            for (auto& e : moved->data.at("objects")) list.push_back(std::move(e));
        }
    }
    change->label = "Rotate " + std::to_string(clip.chunks.size()) + " chunk(s) 90 degrees" +
                    (turned.empty() ? "" : " with " + std::to_string(turned.size()) + " object(s)");
    if (footprint) *footprint = Footprint(clip, ax, az);
    return change;
}

void TerrainAdapter::SetTile(const std::string& map, int x, int y, const std::string& stash, bool present)
{
    const std::string base = "World\\Maps\\" + map + "\\" + map;
    const fs::path adtPath = m_mpq.OverlayPath(base + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt");
    const fs::path wdtPath = m_mpq.OverlayPath(base + ".wdt");
    if (adtPath.empty() || m_projectDir.empty()) return;
    const size_t key = size_t(TileKey(x, y));
    std::error_code ec;
    if (present)
    {
        fs::create_directories(adtPath.parent_path(), ec);
        fs::copy_file(m_projectDir / "tiles" / stash, adtPath, fs::copy_options::overwrite_existing, ec);
    }
    else if (map == m_map)   // out of view before its file goes
    {
        if (key < m_present.size()) m_present[key] = false;
        if (m_tiles.erase(int(key)))
        {
            m_renderer.UnloadTile(int(key));
            m_objectsChanged.insert(int(key));
        }
    }
    if (const auto wdt = m_mpq.Read(base + ".wdt"))
        if (const auto patched = WdtSetTile(*wdt, x, y, present); !patched.empty())
        {
            fs::create_directories(wdtPath.parent_path(), ec);
            std::ofstream(wdtPath, std::ios::binary).write(reinterpret_cast<const char*>(patched.data()), std::streamsize(patched.size()));
        }
    // The low-detail WDL: the tile's heights from its own file (cleared again when it goes).
    if (const auto wdl = m_mpq.Read(base + ".wdl"))
    {
        std::optional<Adt> adt;
        if (present)
            if (std::ifstream f(m_projectDir / "tiles" / stash, std::ios::binary); f)
                adt = ParseAdt(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), m_bigAlpha);
        if (const auto patched = WdlSetTile(*wdl, x, y, adt ? &*adt : nullptr); !patched.empty())
        {
            const fs::path wdlPath = m_mpq.OverlayPath(base + ".wdl");
            fs::create_directories(wdlPath.parent_path(), ec);
            std::ofstream(wdlPath, std::ios::binary).write(reinterpret_cast<const char*>(patched.data()), std::streamsize(patched.size()));
            if (map == m_map) m_farChanged = true;
        }
    }
    if (!present) fs::remove(adtPath, ec);
    else if (map == m_map && key < m_present.size()) m_present[key] = true;   // streams in like any other tile
}

void TerrainAdapter::RebuildOverlay()
{
    if (m_projectDir.empty()) return;
    std::error_code ec;
    fs::remove_all(m_projectDir / "overlay", ec);   // the overlay is the applied changes, nothing else
    for (const ChangeStore::Part& c : ChangeStore::Parts(m_store.Done()))
        if (c.domain == Domain())
        {
            for (const auto& e : c.data.value("tiles", nlohmann::json::array())) SetTile(c.data.at("map"), e[0], e[1], e[2], true);
            if (const auto it = c.data.find("global"); it != c.data.end() && it->is_array() && it->size() == 2) SetGlobalWmo(c.data.at("map"), WmoFrom((*it)[1]));
        }
}

void TerrainAdapter::SetGlobalWmo(const std::string& map, const WmoPlacement& p)
{
    const std::string name = "World\\Maps\\" + map + "\\" + map + ".wdt";
    if (const auto wdt = m_mpq.Read(name))
        if (const auto patched = WdtSetGlobalWmo(*wdt, p); !patched.empty())
            if (const fs::path path = m_mpq.OverlayPath(name); !path.empty())
            {
                std::error_code ec;
                fs::create_directories(path.parent_path(), ec);
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(patched.data()), std::streamsize(patched.size()));
            }
    if (map == m_map)
    {
        m_globalWmo = p;
        m_objectsChanged.insert(kGlobalWmoKey);
    }
}

std::optional<TerrainAdapter::NewTile> TerrainAdapter::ReadNewTile(const MpqChain& chain, const std::string& map, int x, int y)
{
    auto adt = chain.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt");
    if (!adt) return std::nullopt;
    NewTile t{ x, y, std::move(*adt) };
    if (const auto trs = chain.Read("textures\\Minimap\\md5translate.trs"))
        if (const auto file = TrsLookup(*trs, map, x, y))
            if (auto blp = chain.Read("textures\\Minimap\\" + *file)) t.minimap = std::move(*blp);
    return t;
}

std::optional<Change> TerrainAdapter::AddTiles(const std::vector<NewTile>& tiles, bool otherBigAlpha, std::string& error)
{
    if (m_map.empty() || m_projectDir.empty()) { error = "no map open"; return std::nullopt; }
    auto lower = [](std::string s) { for (char& c : s) c = c == '/' ? '\\' : char(std::tolower((unsigned char)c)); return s; };

    // Objects already listed by the map's tiles around these: the same placement under the same id is one object
    // (a building over a tile border) and keeps its id; every other id is made fresh, shared across the new tiles.
    std::set<int> adding;
    for (const NewTile& t : tiles) adding.insert(TileKey(t.x, t.y));
    std::map<uint32_t, std::pair<std::string, std::array<float, 3>>> nearby;
    for (int key : adding)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int nx = key % 64 + dx, ny = key / 64 + dy, n = TileKey(nx, ny);
                if (nx < 0 || ny < 0 || nx > 63 || ny > 63 || adding.count(n) || size_t(n) >= m_present.size() || !m_present[size_t(n)]) continue;
                const auto adt = EditedHeights(m_map, nx, ny);
                if (!adt) continue;
                for (const auto& d : adt->doodads) nearby[d.uniqueId] = { lower(d.model), { d.pos[0], d.pos[1], d.pos[2] } };
                for (const auto& w : adt->wmos) nearby[w.uniqueId] = { lower(w.model), { w.pos[0], w.pos[1], w.pos[2] } };
            }
    uint32_t next = NextUniqueId();
    std::map<uint32_t, uint32_t> fresh;
    auto idFor = [&](uint32_t uid, const std::string& model, const float pos[3]) {
        if (const auto it = nearby.find(uid); it != nearby.end() && it->second.first == lower(model) &&
            std::fabs(it->second.second[0] - pos[0]) < 0.5f && std::fabs(it->second.second[1] - pos[1]) < 0.5f && std::fabs(it->second.second[2] - pos[2]) < 0.5f)
            return uid;
        auto [it, added] = fresh.try_emplace(uid, next);
        if (added) ++next;
        return it->second;
    };

    nlohmann::json entries = nlohmann::json::array();
    std::error_code ec;
    fs::create_directories(m_projectDir / "tiles", ec);
    for (const NewTile& t : tiles)
    {
        const int x = t.x, y = t.y;
        const std::vector<uint8_t>& bytes = t.adt;
        const std::string name = m_map + "_" + std::to_string(x) + "_" + std::to_string(y);
        if (size_t(TileKey(x, y)) < m_present.size() && m_present[size_t(TileKey(x, y))]) { error += name + ": the map has it already; "; continue; }
        auto adt = ParseAdt(bytes, otherBigAlpha);
        if (!adt) { error += name + ": does not parse; "; continue; }
        std::vector<uint32_t> doodadIds, wmoIds;
        for (const auto& d : adt->doodads) doodadIds.push_back(idFor(d.uniqueId, d.model, d.pos));
        for (const auto& w : adt->wmos) wmoIds.push_back(idFor(w.uniqueId, w.model, w.pos));
        // Ground effects this client has no row for crash it near the player: those layers get none.
        bool unknownEffects = false;
        for (AdtChunk& c : adt->chunks)
            for (uint32_t l = 0; l < c.layerCount && l < 4; ++l)
                if (!KnownEffect(c.effectIds[l])) { c.effectIds[l] = 0; unknownEffects = true; ++m_effectsDropped; }
        // Every chunk's layers written again, in this map's alpha format (objects untouched here, so their chunk
        // references stay); then the fresh ids go into the placements in place.
        std::set<size_t> all;
        for (size_t ci = 0; ci < adt->chunks.size(); ++ci) all.insert(ci);
        std::vector<uint8_t> out = RewriteAdt(bytes, *adt, all, m_bigAlpha);
        if (out.empty() && otherBigAlpha == m_bigAlpha && !unknownEffects) out = bytes;   // a layout RewriteAdt will not rebuild, nothing to convert
        if (out.empty()) { error += name + ": its chunk layout cannot be rewritten; "; continue; }
        if (!SetUniqueIds(out, doodadIds, wmoIds)) { error += name + ": object lists do not match the file; "; continue; }
        if (const auto issues = ValidateAdt(out, m_bigAlpha); !issues.empty()) { error += name + ": " + issues.front() + "; "; continue; }
        const std::string stash = name + ".adt";
        std::ofstream f(m_projectDir / "tiles" / stash, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        if (!f) { error += name + ": cannot write the project copy; "; continue; }
        std::string minimap;   // kept beside it; export names it in md5translate.trs
        if (!t.minimap.empty())
        {
            minimap = name + ".minimap.blp";
            std::ofstream(m_projectDir / "tiles" / minimap, std::ios::binary)
                .write(reinterpret_cast<const char*>(t.minimap.data()), std::streamsize(t.minimap.size()));
        }
        entries.push_back({ x, y, stash, next - 1, minimap });
    }
    if (entries.empty()) return std::nullopt;
    for (const auto& e : entries) SetTile(m_map, e[0], e[1], e[2], true);
    Change c;
    c.domain = Domain();
    c.label = "Add " + std::to_string(entries.size()) + " tile(s)";
    c.target = m_map + (entries.size() == 1 ? " " + std::to_string(int(entries[0][0])) + "_" + std::to_string(int(entries[0][1])) : "");
    c.data = { { "map", m_map }, { "edits", nlohmann::json::array() }, { "tiles", std::move(entries) } };
    return c;
}

std::set<int> TerrainAdapter::EditedTiles(const std::string& map) const
{
    std::set<int> tiles;
    for (const ChangeStore::Part& c : ChangeStore::Parts(m_store.Done()))
        if (c.domain == Domain() && c.data.at("map").get<std::string>() == map)
        {
            for (const auto& e : c.data.at("edits")) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("holes", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("areas", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("colors", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("layers", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("objects", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("liquids", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
            for (const auto& e : c.data.value("tiles", nlohmann::json::array())) tiles.insert(TileKey(e[0], e[1]));
        }
    return tiles;
}

std::optional<Adt> TerrainAdapter::EditedHeights(const std::string& map, int x, int y) const
{
    if (x < 0 || y < 0 || x > 63 || y > 63) return std::nullopt;
    const auto bytes = m_mpq.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt");
    auto adt = bytes ? ParseAdt(*bytes, false) : std::nullopt;   // alpha unused here
    if (!adt) return adt;
    for (const ChangeStore::Part& c : ChangeStore::Parts(m_store.Done()))
        if (c.domain == Domain() && c.data.at("map") == map)
            for (const auto& e : c.data.at("edits"))
                if (e[0] == x && e[1] == y && size_t(e[2]) < adt->chunks.size() && size_t(e[3]) < 145) adt->chunks[size_t(e[2])].heights[size_t(e[3])] = e[5];
    return adt;
}

bool TerrainAdapter::KnownEffect(uint32_t id) const
{
    if (!m_groundEffects)
    {
        m_groundEffects.emplace();
        if (const auto dbc = m_mpq.Read("DBFilesClient\\GroundEffectTexture.dbc")) *m_groundEffects = DbcIds(*dbc);
    }
    return id == 0 || m_groundEffects->empty() || m_groundEffects->count(id) != 0;
}

void TerrainAdapter::FindCracks(std::vector<Problem>& problems) const
{
    // Final heights of the edited tiles: the client's tile plus the last value written to each vertex.
    std::map<std::pair<std::string, int>, std::map<std::pair<size_t, size_t>, float>> edits;
    std::set<std::pair<std::string, int>> whole;
    for (const ChangeStore::Part& c : ChangeStore::Parts(m_store.Done()))
        if (c.domain == Domain())
        {
            for (const auto& e : c.data.at("edits")) edits[{ c.data.at("map"), TileKey(e[0], e[1]) }][{ size_t(e[2]), size_t(e[3]) }] = e[5];
            for (const auto& e : c.data.value("tiles", nlohmann::json::array())) whole.insert({ c.data.at("map"), TileKey(e[0], e[1]) });
        }
    for (const auto& id : whole) edits[id];   // an added tile: all its chunks
    std::map<std::pair<std::string, int>, std::optional<Adt>> tiles;
    auto tile = [&](const std::string& map, int x, int y) -> const Adt* {
        if (x < 0 || y < 0 || x > 63 || y > 63) return nullptr;
        auto [it, added] = tiles.try_emplace({ map, TileKey(x, y) });
        if (added) it->second = EditedHeights(map, x, y);
        return it->second ? &*it->second : nullptr;
    };
    // Outer vertex (row, col 0..8) of the chunk at global chunk cell (gx, gy), absolute height.
    auto vertex = [&](const std::string& map, int gx, int gy, int row, int col) -> std::optional<float> {
        const Adt* a = tile(map, gx / 16, gy / 16);
        if (!a) return std::nullopt;
        for (const AdtChunk& c : a->chunks)
            if (int(c.indexX) == gx % 16 && int(c.indexY) == gy % 16) return c.baseY + c.heights[size_t(row * 17 + col)];
        return std::nullopt;
    };
    for (const auto& [id, vertices] : edits)
    {
        const auto& [map, key] = id;
        const Adt* a = tile(map, key % 64, key / 64);
        if (!a) continue;
        std::set<size_t> chunks;
        for (const auto& [cv, value] : vertices) chunks.insert(cv.first);
        if (whole.count(id))
            for (size_t ci = 0; ci < a->chunks.size(); ++ci) chunks.insert(ci);
        size_t bad = 0;
        float worst = 0;
        for (size_t ci : chunks)
        {
            if (ci >= a->chunks.size()) continue;
            const AdtChunk& c = a->chunks[ci];
            const int gx = (key % 64) * 16 + int(c.indexX), gy = (key / 64) * 16 + int(c.indexY);
            // Each edge against the neighbour across it: right/left share columns 8/0, down/up rows 8/0.
            const struct { int dx, dy; } sides[] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const auto& s : sides)
            {
                float gap = 0;
                for (int k = 0; k <= 8; ++k)
                {
                    const int row = s.dy ? (s.dy > 0 ? 8 : 0) : k, col = s.dx ? (s.dx > 0 ? 8 : 0) : k;
                    const int nRow = s.dy ? 8 - row : row, nCol = s.dx ? 8 - col : col;
                    const auto other = vertex(map, gx + s.dx, gy + s.dy, nRow, nCol);
                    if (other) gap = std::max(gap, std::fabs(c.baseY + c.heights[size_t(row * 17 + col)] - *other));
                }
                if (gap > 0.01f) { ++bad; worst = std::max(worst, gap); }
            }
        }
        if (bad)
        {
            char text[200];
            snprintf(text, sizeof text, "%zu chunk edge(s) do not meet their neighbour (gap up to %.2f yd); the client shows a crack there", bad, worst);
            problems.push_back({ Problem::Severity::Error, "Terrain", text, map, key % 64, key / 64 });
        }
    }
}

size_t TerrainAdapter::Export(const fs::path& outDir, std::string& error, std::vector<fs::path>* files, std::vector<Problem>* problems) const
{
    // Net result per tile: the last state written to each vertex / chunk wins.
    struct TileEdits
    {
        std::map<std::pair<size_t, size_t>, float> heights;
        std::map<size_t, nlohmann::json> layers;
        std::map<size_t, uint16_t> holes;
        std::map<size_t, uint32_t> areas;
        std::map<size_t, std::string> colors;
        std::map<size_t, nlohmann::json> liquids;
        nlohmann::json objects = nlohmann::json::array();
    };
    std::map<std::pair<std::string, int>, TileEdits> tiles;
    std::set<std::string> addedMaps;   // maps that gained tiles: their WDT and WDL are exported too
    std::map<std::pair<std::string, int>, fs::path> minimaps;   // tile -> its minimap image (added tiles: the other version's)
    for (const ChangeStore::Part& c : ChangeStore::Parts(m_store.Done()))
    {
        if (c.domain != Domain()) continue;
        const std::string map = c.data.at("map");
        for (const auto& e : c.data.at("edits")) tiles[{ map, TileKey(e[0], e[1]) }].heights[{ size_t(e[2]), size_t(e[3]) }] = e[5];
        for (const auto& e : c.data.value("layers", nlohmann::json::array())) tiles[{ map, TileKey(e[0], e[1]) }].layers[size_t(e[2])] = e[4];
        for (const auto& e : c.data.value("holes", nlohmann::json::array())) tiles[{ map, TileKey(e[0], e[1]) }].holes[size_t(e[2])] = e[4];
        for (const auto& e : c.data.value("areas", nlohmann::json::array())) tiles[{ map, TileKey(e[0], e[1]) }].areas[size_t(e[2])] = e[4];
        for (const auto& e : c.data.value("colors", nlohmann::json::array())) tiles[{ map, TileKey(e[0], e[1]) }].colors[size_t(e[2])] = e[4];
        for (const auto& e : c.data.value("objects", nlohmann::json::array())) tiles[{ map, TileKey(e[0], e[1]) }].objects.push_back(e);
        for (const auto& e : c.data.value("liquids", nlohmann::json::array())) tiles[{ map, TileKey(e[0], e[1]) }].liquids[size_t(e[2])] = e[4];
        if (c.data.contains("global")) addedMaps.insert(map);   // its WDT carries the moved or replaced WMO
        for (const auto& e : c.data.value("tiles", nlohmann::json::array()))
        {
            tiles[{ map, TileKey(e[0], e[1]) }];   // read through the overlay: the added tile as converted
            addedMaps.insert(map);
            if (e.size() > 4 && !e[4].get<std::string>().empty()) minimaps[{ map, TileKey(e[0], e[1]) }] = m_projectDir / "tiles" / e[4].get<std::string>();
        }
    }

    // Final heights of any tile (edited or not), for normals that look across tile borders.
    // Roads: written as the heights and textures they leave on the terrain as edited (the roads themselves are editor-only).
    std::map<std::string, std::vector<const Road*>> roadsByMap;
    if (m_roads)
        for (const Road* r : m_roads->All()) roadsByMap[r->map].push_back(r);
    // The ground the roads follow: the terrain as edited, without roads, any tile.
    std::map<std::pair<std::string, int>, std::optional<Adt>> plainTiles;
    auto groundOf = [&](const std::string& map) -> RoadGround {
        return [&, map](float x, float z) -> std::optional<float> {
            const int tx = int(std::floor(x / kTileSize)), ty = int(std::floor(z / kTileSize));
            if (tx < 0 || ty < 0 || tx > 63 || ty > 63) return std::nullopt;
            auto [it, added] = plainTiles.try_emplace({ map, TileKey(tx, ty) });
            if (added) it->second = EditedHeights(map, tx, ty);
            return it->second ? TileHeightAt(*it->second, tx, ty, x, z) : std::nullopt;
        };
    };
    for (const auto& [map, roads] : roadsByMap)
    {
        const RoadGround ground = groundOf(map);
        std::set<int> keys;
        for (const Road* r : roads)
        {
            const float slack = RoadReach(*r);
            for (const RoadSample& p : SampleRoad(*r))   // every yard of the line: long stretches cross tiles between points
                for (int tz = int(std::floor((p.pos.z - slack) / kTileSize)); tz <= int(std::floor((p.pos.z + slack) / kTileSize)); ++tz)
                    for (int tx = int(std::floor((p.pos.x - slack) / kTileSize)); tx <= int(std::floor((p.pos.x + slack) / kTileSize)); ++tx)
                        if (tx >= 0 && tz >= 0 && tx < 64 && tz < 64) keys.insert(TileKey(tx, tz));
        }
        const auto wdt = m_mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
        for (int key : keys)
        {
            const auto bytes = m_mpq.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt");
            auto adt = bytes && wdt ? ParseAdt(*bytes, WdtBigAlpha(*wdt)) : std::nullopt;
            if (!adt) continue;
            LoadedTile plain{ key % 64, key / 64, {}, std::move(*adt) };
            ReplayEdits(plain, map, m_store.Done());
            for (size_t ci = 0; ci < plain.adt.chunks.size(); ++ci)
            {
                AdtChunk shown = plain.adt.chunks[ci];
                LoadedTile textures;
                textures.adt.textures = plain.adt.textures;
                if (!ApplyRoads(roads, shown, textures.adt.textures, ground)) continue;
                TileEdits& edits = tiles[{ map, key }];
                for (size_t j = 0; j < 145; ++j)
                    if (shown.heights[j] != plain.adt.chunks[ci].heights[j]) edits.heights[{ ci, j }] = shown.heights[j];
                if (nlohmann::json after = LayerState(textures, shown); after != LayerState(plain, plain.adt.chunks[ci])) edits.layers[ci] = std::move(after);
            }
        }
    }

    std::map<std::pair<std::string, int>, std::optional<Adt>> finals;
    auto heightsOf = [&](const std::string& map, int x, int y) -> const Adt* {
        if (x < 0 || y < 0 || x > 63 || y > 63) return nullptr;
        auto [it, added] = finals.try_emplace({ map, TileKey(x, y) });
        if (added)
        {
            it->second = EditedHeights(map, x, y);
            if (auto r = roadsByMap.find(map); it->second && r != roadsByMap.end())   // neighbours' edges as the roads leave them
                for (AdtChunk& c : it->second->chunks)
                {
                    std::vector<std::string> unused;
                    ApplyRoads(r->second, c, unused, groundOf(map));
                }
        }
        return it->second ? &*it->second : nullptr;
    };
    auto chunkAt = [](const Adt& a, int ix, int iy) -> const AdtChunk* {
        for (const AdtChunk& c : a.chunks)
            if (int(c.indexX) == ix && int(c.indexY) == iy) return &c;
        return nullptr;
    };
    // A reshaped chunk on a tile border changes the lighting of the chunks across it: relight those (and export
    // their tile, even unedited), or the client shows a shading line along the border.
    std::map<std::pair<std::string, int>, std::set<std::pair<int, int>>> relightAcross;   // tile -> chunk cells
    for (const auto& [id, edits] : std::map(tiles))
    {
        const auto& [map, key] = id;
        const Adt* a = edits.heights.empty() ? nullptr : heightsOf(map, key % 64, key / 64);
        if (!a) continue;
        std::set<size_t> shaped;
        for (const auto& [cv, value] : edits.heights) shaped.insert(cv.first);
        for (size_t ci : shaped)
        {
            if (ci >= a->chunks.size()) continue;
            const int gx = (key % 64) * 16 + int(a->chunks[ci].indexX), gz = (key / 64) * 16 + int(a->chunks[ci].indexY);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = gx + dx, nz = gz + dz, nkey = TileKey(nx / 16, nz / 16);
                    if (nx < 0 || nz < 0 || nx >= 1024 || nz >= 1024 || nkey == key || !heightsOf(map, nx / 16, nz / 16)) continue;
                    relightAcross[{ map, nkey }].insert({ nx % 16, nz % 16 });
                    tiles[{ map, nkey }];   // exported for its new edge lighting
                }
        }
    }

    size_t written = 0;
    for (const auto& [id, edits] : tiles)
    {
        const auto& [map, key] = id;
        const std::string name = map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64);
        const std::string rel = "World\\Maps\\" + map + "\\" + name + ".adt";
        auto bytes = m_mpq.Read(rel);
        const auto wdt = m_mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
        const bool bigAlpha = wdt && WdtBigAlpha(*wdt);
        auto adt = bytes && wdt ? ParseAdt(*bytes, bigAlpha) : std::nullopt;
        // A failed tile stops the export, or with a problem list is recorded there and skipped.
        auto fail = [&](const std::string& message) {
            if (!problems) { error = message; return true; }
            problems->push_back({ Problem::Severity::Error, "Terrain", message, map, key % 64, key / 64 });
            return false;
        };
        if (!adt) { if (fail("Cannot read " + rel + " for export")) return written; continue; }
        const size_t droppedBefore = m_effectsDropped;

        std::set<size_t> reshaped;
        for (const auto& [cv, value] : edits.heights)
        {
            if (cv.first >= adt->chunks.size()) continue;
            auto& chunk = adt->chunks[cv.first];
            if (chunk.mcvtOffset) std::memcpy(bytes->data() + chunk.mcvtOffset + cv.second * 4, &value, 4);
            chunk.heights[cv.second] = value;
            reshaped.insert(cv.first);
        }

        // Normals (MCNR) for every reshaped chunk and its neighbours, whose edge normals depend on it; the client
        // lights terrain with these. Stored as (-z, -x, +y) of the editor's normal, in 1/127 steps (measured
        // against Blizzard's own tiles). Chunks across a tile border are relit too (relightAcross).
        std::set<size_t> relight;
        for (size_t ci : reshaped)
            for (size_t other = 0; other < adt->chunks.size(); ++other)
                if (std::abs(adt->chunks[other].baseX - adt->chunks[ci].baseX) < kChunkSize * 1.5f &&
                    std::abs(adt->chunks[other].baseZ - adt->chunks[ci].baseZ) < kChunkSize * 1.5f)
                    relight.insert(other);
        if (auto across = relightAcross.find(id); across != relightAcross.end())
            for (size_t ci = 0; ci < adt->chunks.size(); ++ci)
                if (across->second.count({ int(adt->chunks[ci].indexX), int(adt->chunks[ci].indexY) })) relight.insert(ci);
        // Heights just across the border, from the neighbouring tiles as edited.
        const int tx = key % 64, ty = key / 64;
        auto beyond = [&](int gx, int gz) -> std::optional<float> {
            const int vx = tx * 128 + gx, vz = ty * 128 + gz;   // map-wide vertex grid
            if (vx < 0 || vz < 0) return std::nullopt;
            const Adt* n = heightsOf(map, vx / 128, vz / 128);
            const AdtChunk* c = n ? chunkAt(*n, (vx % 128) / 8, (vz % 128) / 8) : nullptr;
            if (!c) return std::nullopt;
            return c->baseY + c->heights[size_t(((vz % 128) % 8) * 17 + (vx % 128) % 8)];
        };
        for (size_t ci : relight)
        {
            const AdtChunk& chunk = adt->chunks[ci];
            if (!chunk.mcnrOffset) continue;
            const auto normals = ChunkNormals(*adt, ci, beyond);
            for (size_t j = 0; j < 145; ++j)
            {
                const float v[3] = { -normals[j][2], -normals[j][0], normals[j][1] };
                for (size_t k = 0; k < 3; ++k) (*bytes)[chunk.mcnrOffset + j * 3 + k] = uint8_t(int8_t(std::lround(std::clamp(v[k], -1.0f, 1.0f) * 127.0f)));
            }
        }

        for (const auto& [ci, mask] : edits.holes)   // MCNK header: holes at +0x3C of the header (after the 8-byte chunk header)
            if (ci < adt->chunks.size() && adt->chunks[ci].mcnkOffset) std::memcpy(bytes->data() + adt->chunks[ci].mcnkOffset + 8 + 0x3C, &mask, 2);
        for (const auto& [ci, area] : edits.areas)   // MCNK header: area id at +0x34
            if (ci < adt->chunks.size() && adt->chunks[ci].mcnkOffset)
            {
                std::memcpy(bytes->data() + adt->chunks[ci].mcnkOffset + 8 + 0x34, &area, 4);
                adt->chunks[ci].areaId = area;
            }
        for (const auto& [ci, text] : edits.colors)   // MCCV in place (before a rewrite moves the sub-chunks)
        {
            const std::vector<uint8_t> colors = Base64Decode(text);
            if (ci >= adt->chunks.size() || colors.size() < 145 * 4) continue;
            if (!adt->chunks[ci].mccvOffset)
            {
                if (fail(rel + ": chunk " + std::to_string(ci) + " has no MCCV to hold its shading; not written")) return written;
                continue;
            }
            std::memcpy(bytes->data() + adt->chunks[ci].mccvOffset, colors.data(), 145 * 4);
            adt->chunks[ci].colors = colors;
        }

        if (!edits.layers.empty() || !edits.objects.empty() || !edits.liquids.empty())
        {
            LoadedTile tile{ key % 64, key / 64, {}, std::move(*adt) };
            std::set<size_t> changed;
            for (const auto& [ci, state] : edits.layers)
                if (ci < tile.adt.chunks.size())
                {
                    SetLayerState(tile, tile.adt.chunks[ci], state);
                    changed.insert(ci);
                }
            for (const auto& e : edits.objects) ApplyObjectEntry(tile.adt, e, true);
            for (const auto& [ci, state] : edits.liquids)
                if (ci < tile.adt.chunks.size()) SetLiquidState(tile.adt, tile.adt.chunks[ci], state);
            // Ground effects the client has no GroundEffectTexture row for crash it when the player comes near
            // (other clients' maps can carry ids, or 65535, this client lacks): those layers get none.
            for (size_t ci : changed)
                for (uint32_t l = 0; l < tile.adt.chunks[ci].layerCount; ++l)
                {
                    uint32_t& effect = tile.adt.chunks[ci].effectIds[l];
                    if (!KnownEffect(effect)) { effect = 0; ++m_effectsDropped; }
                }
            auto rewritten = RewriteAdt(*bytes, tile.adt, changed, bigAlpha);
            if (rewritten.empty())
            {
                if (fail(rel + " cannot be rewritten (missing MHDR/MCIN/object chunks or an MCNK layout the editor does not know); texture and object changes not written")) return written;
                continue;
            }
            *bytes = std::move(rewritten);
        }

        // Never hand the client a tile it would trip over.
        if (const auto issues = ValidateAdt(*bytes, bigAlpha); !issues.empty())
        {
            if (fail(rel + " failed the structure check, not written: " + issues.front())) return written;
            continue;
        }
        if (problems && m_effectsDropped > droppedBefore)
            problems->push_back({ Problem::Severity::Warning, "Terrain",
                                  std::to_string(m_effectsDropped - droppedBefore) + " texture layer(s) carry ground effects this client lacks "
                                  "(pasted from another map); exported without them", map, key % 64, key / 64 });

        const fs::path path = outDir / "World" / "Maps" / map / (name + ".adt");
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
        if (!f) { error = "Cannot write " + path.string(); return written; }
        ++written;
        if (files) files->push_back(path);
        // A minimap the editor rendered of the tile as edited (App::RenderMinimaps) replaces the client's picture.
        if (const fs::path rendered = m_projectDir / "minimaps" / (name + ".blp"); !m_projectDir.empty() && fs::exists(rendered, ec))
            minimaps[id] = rendered;
    }
    auto writeFile = [&](const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) { error = "Cannot write " + path.string(); return false; }
        ++written;
        if (files) files->push_back(path);
        return true;
    };
    // The WDT listing the added tiles (the client loads only tiles it lists) and the WDL with their far heights.
    for (const std::string& map : addedMaps)
        for (const char* ext : { ".wdt", ".wdl" })
            if (const auto bytes = m_mpq.Read("World\\Maps\\" + map + "\\" + map + ext))
                if (!writeFile(outDir / "World" / "Maps" / map / (map + ext), *bytes)) return written;
    // Minimaps of the added tiles: their images under names of our own, and the index naming them (the client reads
    // one md5translate.trs, so the whole index as the client has it plus these lines).
    if (!minimaps.empty())
    {
        const std::string trsName = "textures\\Minimap\\md5translate.trs";
        std::vector<uint8_t> trs = m_mpq.Read(trsName).value_or(std::vector<uint8_t>{});
        for (const auto& [id, image] : minimaps)
        {
            const auto& [map, key] = id;
            std::ifstream f(image, std::ios::binary);
            const std::vector<uint8_t> blp((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (blp.empty()) continue;
            const std::string file = "wwe_" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".blp";
            if (!writeFile(outDir / "textures" / "Minimap" / file, blp)) return written;
            trs = TrsSet(trs, map, key % 64, key / 64, file);
        }
        if (!writeFile(outDir / "textures" / "Minimap" / "md5translate.trs", trs)) return written;
    }
    return written;
}

void TerrainAdapter::SetRoads(const RoadStore* roads)
{
    m_roads = roads;
    m_renderer.SetChunkView([this](int, size_t, const AdtChunk& c, const std::vector<std::string>& textures, bool paint) -> std::optional<Renderer::ShownChunk> {
        if (!m_roads) return std::nullopt;
        const std::vector<const Road*> roads = m_roads->OnMap(m_map);
        if (roads.empty()) return std::nullopt;
        Renderer::ShownChunk shown{ c, textures };
        if (!ApplyRoads(roads, shown.chunk, shown.textures, {}, paint, [this](const Road& r) -> const std::vector<RoadSample>& { return RoadLine(r); }))
            return std::nullopt;
        return shown;
    });
}

const std::vector<RoadSample>& TerrainAdapter::RoadLine(const Road& road) const
{
    // Valid while the roads, the edits and the loaded ground stay as they are.
    // ponytail: a sculpt stroke reshapes the ground before it commits; roads following it catch up when it does.
    const auto key = std::tuple{ m_roads ? m_roads->Version() : 0, m_store.Revision(), m_tiles.size() };
    if (key != m_roadLinesKey)
    {
        m_roadLines.clear();
        m_roadLinesKey = key;
    }
    auto [it, added] = m_roadLines.try_emplace(road.id);
    if (added) it->second = SampleRoad(road, [this](float x, float z) { return HeightAt(x, z); });
    return it->second;
}

void TerrainAdapter::RefreshCells(const std::string& map, const RoadStore::Cells& cells)
{
    if (map != m_map) return;
    for (const auto& [gx, gz] : cells)
        if (const auto ref = ChunkAtGrid(gx, gz))
        {
            m_renderer.UpdateChunk(ref->tile, size_t(ref->chunk), *Chunk(*ref));
            RefreshTextures(ref->tile, size_t(ref->chunk));
        }
}

std::optional<float> TerrainAdapter::ShownHeightAt(float x, float z) const
{
    const auto ref = ChunkAtGrid(int(std::floor(x / kChunkSize)), int(std::floor(z / kChunkSize)));
    if (!ref || !m_roads) return HeightAt(x, z);
    AdtChunk c = *Chunk(*ref);
    std::vector<std::string> unused;
    if (!ApplyRoads(m_roads->OnMap(m_map), c, unused, {}, false, [this](const Road& r) -> const std::vector<RoadSample>& { return RoadLine(r); }))
        return HeightAt(x, z);
    return ChunkHeightAt(c, x, z);
}

std::optional<Change> TerrainAdapter::BakeRoad(const Road& road, const std::string& label)
{
    if (road.map != m_map || road.points.size() < 2) return std::nullopt;
    // Every tile the road crosses must be loaded to be written.
    const float reach = RoadReach(road);
    std::string error;
    std::set<int> keys;
    for (const RoadSample& p : SampleRoad(road))
        for (int tz = int(std::floor((p.pos.z - reach) / kTileSize)); tz <= int(std::floor((p.pos.z + reach) / kTileSize)); ++tz)
            for (int tx = int(std::floor((p.pos.x - reach) / kTileSize)); tx <= int(std::floor((p.pos.x + reach) / kTileSize)); ++tx)
                if (tx >= 0 && tz >= 0 && tx < 64 && tz < 64 && keys.insert(TileKey(tx, tz)).second) LoadNow(tx, tz, error);
    // Every chunk worked out from the ground as it is, then written (the road follows the ground: writing as we go
    // would feed baked heights back into its line).
    struct Baked { int key; size_t ci; AdtChunk chunk; std::vector<std::string> textures; };
    std::vector<Baked> baked;
    const RoadGround ground = [this](float x, float z) { return HeightAt(x, z); };
    for (int key : keys)
        if (auto it = m_tiles.find(key); it != m_tiles.end())
            for (size_t ci = 0; ci < it->second.adt.chunks.size(); ++ci)
            {
                Baked b{ key, ci, it->second.adt.chunks[ci], it->second.adt.textures };
                if (ApplyRoads({ &road }, b.chunk, b.textures, ground)) baked.push_back(std::move(b));
            }
    Edits edits;
    nlohmann::json layers = nlohmann::json::array();
    for (Baked& b : baked)
    {
        LoadedTile& tile = m_tiles.at(b.key);
        AdtChunk& c = tile.adt.chunks[b.ci];
        for (size_t j = 0; j < 145; ++j)
            if (b.chunk.heights[j] != c.heights[j]) edits[{ b.key, int(b.ci), int(j) }] = { c.heights[j], b.chunk.heights[j] };
        nlohmann::json before = LayerState(tile, c);
        for (const std::string& t : b.textures)   // the road's textures join the tile's list (ids stay valid: only appended)
            if (std::find(tile.adt.textures.begin(), tile.adt.textures.end(), t) == tile.adt.textures.end()) tile.adt.textures.push_back(t);
        // b.chunk's ids index b.textures; carry them over by name.
        nlohmann::json after = [&] { LoadedTile names; names.adt.textures = b.textures; return LayerState(names, b.chunk); }();
        c = b.chunk;
        SetLayerState(tile, c, after);
        if (after != before) layers.push_back({ tile.x, tile.y, int(b.ci), std::move(before), std::move(after) });
    }
    if (edits.empty() && layers.empty()) return std::nullopt;
    return MakeChange(edits, layers, label);
}

bool TerrainSelfTest()
{
    TerrainClipboard clip;
    for (int dx = 0; dx < 2; ++dx)
    {
        TerrainClipboard::Entry e{ dx, 0 };
        for (size_t j = 0; j < 145; ++j) e.heights[j] = float(dx * 1000 + int(j));
        std::vector<uint8_t> rgb(64 * 64 * 3);
        for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = uint8_t(i * 7 + dx);
        e.layers = { { "names", { "a.blp" } }, { "flags", { 0 } }, { "effects", { 0 } }, { "alpha", Base64Encode(rgb.data(), rgb.size()) } };
        e.holes = dx == 0 ? 0x0001 : 0x8000;   // top-left cell / bottom-right cell
        e.colors.resize(145 * 4);
        for (size_t j = 0; j < e.colors.size(); ++j) e.colors[j] = uint8_t(j / 4);   // vertex j coloured j
        clip.chunks.push_back(e);
    }
    const TerrainClipboard original = clip;

    clip.RotateClockwise();   // a 2x1 strip becomes 1x2; chunk (1,0) moves to (0,1)
    if (clip.Width() != 1 || clip.Depth() != 2 || clip.chunks[1].dx != 0 || clip.chunks[1].dz != 1) return false;
    if (clip.chunks[0].heights[144] != 8.0f) return false;          // top-right outer corner -> bottom-right
    if (clip.chunks[0].heights[9 + 7] != 9.0f) return false;   // inner top-left (row 0, col 0) -> top-right (row 0, col 7)
    if (clip.chunks[0].holes != 0x0008 || clip.chunks[1].holes != 0x1000) return false;   // holes turn too
    if (clip.chunks[0].colors[144 * 4] != 8 || clip.chunks[0].colors[(9 + 7) * 4] != 9) return false;   // colours turn with the vertices

    // Placement matrices decompose back to the same placement (the gizmo relies on it), gimbal lock included.
    {
        const float cases[][4] = { { 12, 33, -7, 1.0f }, { -80, 200, 45, 2.5f }, { 90, 10, 20, 0.5f }, { 0, 0, 0, 1.0f }, { 170, -95, 300, 1.2f } };
        for (const auto& k : cases)
        {
            const float pos[3] = { 100, 20, -50 }, rot[3] = { k[0], k[1], k[2] };
            const XMMATRIX m = PlacementMatrix(pos, rot, k[3]);
            float p2[3], r2[3], s2 = 0;
            DecomposePlacement(m, p2, r2, s2);
            const XMMATRIX back = PlacementMatrix(p2, r2, s2);
            for (int r = 0; r < 4; ++r)
                if (XMVectorGetX(XMVector4LengthEst(XMVectorSubtract(back.r[r], m.r[r]))) > 1e-2f) return false;
        }
    }

    // An object turns with the copy: its model matrix after the turn equals the old one followed by the turn.
    {
        TerrainClipboard objs;
        objs.chunks.push_back({ 0, 0 });
        objs.chunks.push_back({ 0, 1 });   // depth 2
        DoodadPlacement d{ "x.m2", { 10, 5, 20 }, { 12, 33, -7 }, 1.0f, 1 };
        objs.doodads.push_back(d);
        const XMMATRIX before = PlacementMatrix(d.pos, d.rot, 1.0f);
        objs.RotateClockwise();
        const XMMATRIX after = PlacementMatrix(objs.doodads[0].pos, objs.doodads[0].rot, 1.0f);
        const float span = 2 * kChunkSize;
        const XMMATRIX turn = XMMatrixRotationY(XMConvertToRadians(-90.0f)) * XMMatrixTranslation(span, 0, 0);   // (x, z) -> (span - z, x)
        const XMMATRIX expect = before * turn;
        for (int r = 0; r < 4; ++r)
            if (XMVectorGetX(XMVector4LengthEst(XMVectorSubtract(after.r[r], expect.r[r]))) > 1e-2f) return false;
        // A landmark turns the same way, its fields untouched, and survives the blueprint JSON.
        TerrainClipboard marks;
        marks.chunks = { { 0, 0 }, { 0, 1 } };   // depth 2, as objs before its turn
        marks.pois.push_back({ { "pos", { 10.0f, 5.0f, 20.0f } }, { "ground", true }, { "row", { { "Name_lang", "Town" } } } });
        marks.RotateClockwise();
        const TerrainClipboard back = TerrainClipboard::FromJson(marks.ToJson());
        const auto& pos = back.pois.at(0).at("pos");
        if (std::fabs(pos[0].get<float>() - (span - 20)) > 1e-3f || std::fabs(pos[2].get<float>() - 10) > 1e-3f || pos[1] != 5.0f ||
            back.pois[0]["row"]["Name_lang"] != "Town" || !back.pois[0]["ground"].get<bool>())
            return false;
    }

    for (int i = 0; i < 3; ++i) clip.RotateClockwise();
    for (size_t k = 0; k < clip.chunks.size(); ++k)
        if (clip.chunks[k].dx != original.chunks[k].dx || clip.chunks[k].dz != original.chunks[k].dz ||
            clip.chunks[k].heights != original.chunks[k].heights || clip.chunks[k].layers != original.chunks[k].layers ||
            clip.chunks[k].holes != original.chunks[k].holes || clip.chunks[k].colors != original.chunks[k].colors)
            return false;
    return true;
}
