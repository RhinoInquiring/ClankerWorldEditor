// World map: a zone's map picture (WorldMapArea) and the pieces exploring reveals (WorldMapOverlay), rendered top-down
// from the terrain into the project's assets folder.
#include "App.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <fstream>

using namespace DirectX;
namespace fs = std::filesystem;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };

// The zone map shows 1002 x 668 pixels of a 1024 x 768 canvas cut into 4 x 3 tiles of 256.
constexpr int kViewW = 1002, kViewH = 668, kCanvasW = 1024, kCanvasH = 768;
constexpr uint32_t kNone = ~0u;

/// A WorldMapArea rectangle in editor axes: x0 / z0 the top-left corner of the view, spans in yards.
struct MapRect { float x0, z0, spanX, spanZ; };
MapRect RectOf(const nlohmann::json& row)
{
    // LocLeft / LocRight: WoW y (west is larger); LocTop / LocBottom: WoW x (north is larger). Editor x = zero - WoW y,
    // editor z = zero - WoW x, so the picture runs along +x to the right and +z downwards.
    const float left = row.value("LocLeft", 0.0f), right = row.value("LocRight", 0.0f), top = row.value("LocTop", 0.0f), bottom = row.value("LocBottom", 0.0f);
    return { kZeroPoint - left, kZeroPoint - top, left - right, top - bottom };
}

/// Letters and digits only: a folder or file name the client and the MPQ tools take as is.
std::string FileSafe(const std::string& name)
{
    std::string out;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    return out.empty() ? "Area" : out;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(f);
}

/// Pixels (w x h) of the canvas from (x, y), as RGBA; outside the canvas transparent.
std::vector<uint8_t> Crop(const std::vector<uint8_t>& canvas, int x, int y, int w, int h)
{
    std::vector<uint8_t> out(size_t(w) * h * 4, 0);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            if (x + c < kCanvasW && y + r < kCanvasH)
                std::memcpy(&out[(size_t(r) * w + c) * 4], &canvas[(size_t(y + r) * kCanvasW + x + c) * 4], 4);
    return out;
}
}

void App::QueueMapJob(uint32_t worldMap, bool base, std::vector<uint32_t> areas)
{
    const nlohmann::json& row = m_worldMaps.Row(worldMap);
    if (row.is_null()) return;
    const MapRect r = RectOf(row);
    MapJob job{ worldMap, base, std::move(areas), {} };
    // Every tile under the canvas (the parts past the view too: the canvas is drawn whole).
    const int tx0 = int(std::floor(r.x0 / kTileSize)), tx1 = int(std::floor((r.x0 + r.spanX * kCanvasW / kViewW) / kTileSize));
    const int tz0 = int(std::floor(r.z0 / kTileSize)), tz1 = int(std::floor((r.z0 + r.spanZ * kCanvasH / kViewH) / kTileSize));
    for (int tz = std::max(tz0, 0); tz <= std::min(tz1, 63); ++tz)
        for (int tx = std::max(tx0, 0); tx <= std::min(tx1, 63); ++tx)
            if (size_t(TileKey(tx, tz)) < m_terrain.Present().size() && m_terrain.Present()[size_t(TileKey(tx, tz))]) job.tiles.insert(TileKey(tx, tz));
    Log("World map %s: loading %zu tile(s) to render.", row.value("AreaName", "").c_str(), job.tiles.size());
    m_mapJob = std::move(job);
}

void App::RunMapJob()
{
    if (!m_mapJob || m_terrain.MissingTiles(m_mapJob->tiles)) return;
    for (int key : m_mapJob->tiles)
        if (!m_models.HasTile(key)) return;
    const MapJob job = *std::exchange(m_mapJob, std::nullopt);
    const nlohmann::json row = m_worldMaps.Row(job.worldMap);
    if (row.is_null() || !m_project) return;
    const MapRect r = RectOf(row);
    const std::string folder = row.value("AreaName", "");
    const fs::path dir = m_project->AssetsDir() / "Interface" / "WorldMap" / folder;

    float top = 0;
    for (int key : job.tiles)
        if (auto it = m_terrain.Tiles().find(key); it != m_terrain.Tiles().end()) top = std::max(top, it->second.maxHeight);
    const std::vector<uint8_t> canvas = RenderOrtho(r.x0, r.z0, r.spanX * kCanvasW / kViewW, r.spanZ * kCanvasH / kViewH, top + 50, kCanvasW, kCanvasH, 0);
    if (canvas.empty()) { Log("World map %s: rendering failed.", folder.c_str()); return; }

    // The area under every pixel (kNone: no terrain there), one lookup per chunk.
    std::vector<uint32_t> areaAt(size_t(kCanvasW) * kCanvasH, kNone);
    std::map<std::pair<int, int>, uint32_t> byCell;
    for (int py = 0; py < kCanvasH; ++py)
        for (int px = 0; px < kCanvasW; ++px)
        {
            const float x = r.x0 + (px + 0.5f) * r.spanX / kViewW, z = r.z0 + (py + 0.5f) * r.spanZ / kViewH;
            const std::pair<int, int> cell{ int(std::floor(x / kChunkSize)), int(std::floor(z / kChunkSize)) };
            auto [it, fresh] = byCell.try_emplace(cell, kNone);
            if (fresh)
                if (const auto ref = m_terrain.ChunkAtGrid(cell.first, cell.second)) it->second = m_terrain.Chunk(*ref)->areaId;
            areaAt[size_t(py) * kCanvasW + px] = it->second;
        }

    size_t files = 0;
    if (job.base)
    {
        // The unexplored look: the terrain faded into parchment; exploring lays the full-colour overlays on top.
        std::vector<uint8_t> faded(canvas.size());
        const float parchment[3] = { 222, 205, 165 };
        for (size_t p = 0; p < areaAt.size(); ++p)
        {
            const uint8_t* c = &canvas[p * 4];
            const float grey = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
            for (int k = 0; k < 3; ++k)
                faded[p * 4 + k] = areaAt[p] == kNone ? uint8_t(parchment[k]) : uint8_t(std::clamp(parchment[k] * 0.55f + (c[k] * 0.3f + grey * 0.7f) * 0.45f, 0.0f, 255.0f));
            faded[p * 4 + 3] = 255;
        }
        for (int i = 0; i < 12; ++i)
            files += WriteFile(dir / (folder + std::to_string(i + 1) + ".blp"), WriteBlp(256, 256, Crop(faded, (i % 4) * 256, (i / 4) * 256, 256, 256).data()));
    }

    const Project::IdRange range = m_project->Range("worldmapoverlay.id");
    const auto existing = m_mapOverlays.For(job.worldMap);
    std::vector<Change> parts;
    for (uint32_t area : job.areas)
    {
        // The area's pixels inside the view, feathered over a few pixels so chunk steps do not show.
        constexpr int kFeather = 3;
        int minX = kViewW, minY = kViewH, maxX = -1, maxY = -1;
        std::vector<float> mask(size_t(kViewW) * kViewH, 0);
        for (int py = 0; py < kViewH; ++py)
            for (int px = 0; px < kViewW; ++px)
                if (areaAt[size_t(py) * kCanvasW + px] == area)
                {
                    mask[size_t(py) * kViewW + px] = 1;
                    minX = std::min(minX, px); maxX = std::max(maxX, px); minY = std::min(minY, py); maxY = std::max(maxY, py);
                }
        if (maxX < 0) { Log("World map %s: %s has no chunks inside the map.", folder.c_str(), AreaLabel(area).c_str()); continue; }
        for (int pass = 0; pass < 2; ++pass)   // box blur along x, then y
        {
            std::vector<float> out(mask.size(), 0);
            for (int py = 0; py < kViewH; ++py)
                for (int px = 0; px < kViewW; ++px)
                {
                    float sum = 0;
                    for (int d = -kFeather; d <= kFeather; ++d)
                    {
                        const int qx = pass == 0 ? px + d : px, qy = pass == 0 ? py : py + d;
                        if (qx >= 0 && qy >= 0 && qx < kViewW && qy < kViewH) sum += mask[size_t(qy) * kViewW + qx];
                    }
                    out[size_t(py) * kViewW + px] = sum / (2 * kFeather + 1);
                }
            mask = std::move(out);
        }
        minX = std::max(minX - kFeather, 0); minY = std::max(minY - kFeather, 0);
        maxX = std::min(maxX + kFeather, kViewW - 1); maxY = std::min(maxY + kFeather, kViewH - 1);
        const int w = maxX - minX + 1, h = maxY - minY + 1;

        // Reuse the area's overlay row (and texture name) when it has one.
        uint32_t id = 0;
        nlohmann::json after;
        for (const auto& [oid, orow] : existing)
            if (orow.value("AreaID[0]", 0u) == area) { id = oid; after = orow; }
        if (!id)
        {
            id = m_mapOverlays.FreeId(range.first, range.last);
            if (!id) { Log("No free id in the project's worldmapoverlay.id range %u-%u.", range.first, range.last); break; }
            std::string name = FileSafe(m_areas.Find(area) ? m_areas.Find(area)->name : std::to_string(area));
            for (const auto& [oid, orow] : existing)
                if (orow.value("TextureName", "") == name) name += std::to_string(area);
            after = { { "ID", id }, { "MapAreaID", job.worldMap }, { "AreaID[0]", area }, { "AreaID[1]", 0u }, { "AreaID[2]", 0u }, { "AreaID[3]", 0u },
                      { "MapPointX", 0u }, { "MapPointY", 0u }, { "TextureName", name } };
        }
        after["TextureWidth"] = w; after["TextureHeight"] = h; after["OffsetX"] = minX; after["OffsetY"] = minY;
        after["HitRectTop"] = minY; after["HitRectLeft"] = minX; after["HitRectBottom"] = maxY + 1; after["HitRectRight"] = maxX + 1;

        // Tiles of 256 left to right, top to bottom; the last column / row is only as large as the power of two the
        // client expects there (WorldMapFrame.lua: 16, 32, ... up to the remainder).
        const std::string name = after.value("TextureName", "");
        const int cols = (w + 255) / 256, rows = (h + 255) / 256;
        auto fileSize = [](int pixels) { int s = 16; while (s < pixels) s *= 2; return s; };
        for (int j = 0; j < rows; ++j)
            for (int k = 0; k < cols; ++k)
            {
                const int fw = k < cols - 1 ? 256 : fileSize(w - 256 * k), fh = j < rows - 1 ? 256 : fileSize(h - 256 * j);
                std::vector<uint8_t> tile(size_t(fw) * fh * 4, 0);
                for (int y = 0; y < fh; ++y)
                    for (int x = 0; x < fw; ++x)
                    {
                        const int px = minX + k * 256 + x, py = minY + j * 256 + y;
                        if (px > maxX || py > maxY) continue;
                        uint8_t* t = &tile[(size_t(y) * fw + x) * 4];
                        std::memcpy(t, &canvas[(size_t(py) * kCanvasW + px) * 4], 3);
                        t[3] = uint8_t(std::lround(mask[size_t(py) * kViewW + px] * 255));
                    }
                files += WriteFile(dir / (name + std::to_string(j * cols + k + 1) + ".blp"), WriteBlp(uint32_t(fw), uint32_t(fh), tile.data()));
            }
        if (after == m_mapOverlays.Row(id)) continue;   // same rectangle as before: only the pictures changed
        Change c = m_mapOverlays.MakeChange(id, m_mapOverlays.Row(id), after, "Overlay " + AreaLabel(area));
        m_mapOverlays.Apply(c);
        parts.push_back(std::move(c));
    }
    m_store.Commit(std::move(parts), "World map overlays: " + folder);
    Log("World map %s: %zu picture file(s) in assets/Interface/WorldMap/%s.", folder.c_str(), files, folder.c_str());
}

void App::DrawWorldMapSection(float w)
{
    if (!m_activeArea || !m_project) return;
    const uint32_t map = CurrentMapId(), zone = m_areas.ZoneOf(m_activeArea);
    if (!Section("World map")) return;
    if (m_mapJob)
    {
        ImGui::Text("Rendering: %zu of %zu tile(s) still loading...", m_terrain.MissingTiles(m_mapJob->tiles), m_mapJob->tiles.size());
        if (ImGui::Button("Cancel", { w, 0 })) m_mapJob.reset();
        return;
    }
    // The zone and its sub-areas on this map: what gets an overlay.
    std::vector<uint32_t> zoneAreas;
    for (const Area& a : m_areas.OnMap(map))
        if (m_areas.ZoneOf(a.id) == zone) zoneAreas.push_back(a.id);

    const auto wm = m_worldMaps.ForZone(map, zone);
    if (!wm)
    {
        ImGui::PushTextWrapPos(w);
        ImGui::TextColored(kQuiet, "%s has no world map. Create one from the chunks of the zone and its sub-areas on the loaded tiles.",
                           AreaLabel(zone).c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Create world map", { w, 0 }))
        {
            float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
            for (const auto& [key, tile] : m_terrain.Tiles())
                for (const AdtChunk& c : tile.adt.chunks)
                    if (m_areas.ZoneOf(c.areaId) == zone)
                    {
                        x0 = std::min(x0, c.baseX); z0 = std::min(z0, c.baseZ);
                        x1 = std::max(x1, c.baseX + kChunkSize); z1 = std::max(z1, c.baseZ + kChunkSize);
                    }
            const Project::IdRange range = m_project->Range("worldmaparea.id");
            const uint32_t id = m_worldMaps.FreeId(range.first, range.last);
            if (x1 < x0) Log("No chunk of %s on the loaded tiles: paint it first.", AreaLabel(zone).c_str());
            else if (!id) Log("No free id in the project's worldmaparea.id range %u-%u.", range.first, range.last);
            else
            {
                // A margin, then the 3 : 2 shape of the map view.
                float spanX = (x1 - x0) * 1.1f, spanZ = (z1 - z0) * 1.1f;
                if (spanX < spanZ * 1.5f) spanX = spanZ * 1.5f; else spanZ = spanX / 1.5f;
                const float cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
                nlohmann::json row = { { "ID", id }, { "MapID", map }, { "AreaID", zone }, { "DisplayMapID", ~0u }, { "DefaultDungeonFloor", 0u },
                                       { "ParentWorldMapID", ~0u } };
                for (const auto& [sid, sibling] : m_worldMaps.Rows())   // the same as the map's other zones
                    if (sibling.value("MapID", 0u) == map && sibling.value("AreaID", 0u) != 0)
                    {
                        row["DisplayMapID"] = sibling["DisplayMapID"];
                        row["ParentWorldMapID"] = sibling["ParentWorldMapID"];
                        break;
                    }
                std::string folder = FileSafe(m_areas.Find(zone) ? m_areas.Find(zone)->name : "Zone");
                for (const auto& [sid, other] : m_worldMaps.Rows())
                    if (other.value("AreaName", "") == folder) { folder += std::to_string(id); break; }
                row["AreaName"] = folder;
                row["LocLeft"] = kZeroPoint - (cx - spanX / 2);
                row["LocRight"] = kZeroPoint - (cx + spanX / 2);
                row["LocTop"] = kZeroPoint - (cz - spanZ / 2);
                row["LocBottom"] = kZeroPoint - (cz + spanZ / 2);
                m_worldMaps.Commit(id, row, "World map for " + AreaLabel(zone));
                QueueMapJob(id, true, zoneAreas);
            }
        }
        return;
    }

    const nlohmann::json& row = m_worldMaps.Row(*wm);
    const MapRect r = RectOf(row);
    const auto overlays = m_mapOverlays.For(*wm);
    bool hasOverlay = false;
    for (const auto& [oid, o] : overlays)
        for (int k = 0; k < 4; ++k) hasOverlay |= o.value("AreaID[" + std::to_string(k) + "]", 0u) == m_activeArea;
    const bool own = m_project->Owns("worldmaparea.id", *wm);
    ImGui::TextColored(kQuiet, "%s (WorldMapArea %u%s)   %.0f x %.0f yd   %zu overlay(s)", row.value("AreaName", "").c_str(), *wm, own ? ", project" : "",
                       r.spanX, r.spanZ, overlays.size());
    if (own && ImGui::Button("Render the map and every overlay again", { w, 0 })) QueueMapJob(*wm, true, zoneAreas);
    if (ImGui::Button(((hasOverlay ? "Render overlay again: " : "Add to the world map: ") + AreaLabel(m_activeArea)).c_str(), { w, 0 }))
        QueueMapJob(*wm, false, { m_activeArea });
    ImGui::SetItemTooltip("A top-down render of the area's chunks, shown once a player has explored it.\n"
                          "Blizzard's zones keep their painted map underneath.");
    ImGui::TextColored(kQuiet, "Pictures: assets/Interface/WorldMap/%s.\nClient restart needed to see DBC changes.", row.value("AreaName", "").c_str());
}
