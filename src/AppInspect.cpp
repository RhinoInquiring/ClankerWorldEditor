// Inspector pages for what the current tool works on: a spawn (its database row and template), an object, an area.
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kAccent{ 0.30f, 0.62f, 1.00f, 1.00f };

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string Text(const nlohmann::json& v)
{
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "NULL";
    return v.dump();
}

/// Every field of a row as name / value lines; `filter` keeps the fields whose name contains it.
void RowTable(const char* id, const nlohmann::json& row, const std::string& filter)
{
    if (!row.is_object()) { ImGui::TextColored(kQuiet, "No row."); return; }
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    const std::string f = Lower(filter);
    for (const auto& [key, value] : row.items())
    {
        if (!f.empty() && Lower(key).find(f) == std::string::npos) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kQuiet, "%s", key.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Text(value).c_str());
    }
    ImGui::EndTable();
}

void Line(const char* key, const std::string& value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(kQuiet, "%s", key);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

std::string Floats(const float* v, int n, const char* format = "%.2f")
{
    std::string out;
    char buf[32];
    for (int i = 0; i < n; ++i) { snprintf(buf, sizeof buf, format, v[i]); out += (i ? ", " : "") + std::string(buf); }
    return out;
}
}

bool App::InspectSpawn()
{
    std::vector<const Spawn*> sel;
    for (const Spawn& s : m_spawnView)
        if (s.kind == m_spawnKind && m_spawnSel.count(s.guid)) sel.push_back(&s);
    if (sel.empty()) return false;
    const char* table = m_spawnKind == SpawnKind::Creature ? "creature" : "gameobject";
    if (sel.size() > 1)
    {
        ImGui::TextColored(kAccent, "%zu %s spawns selected", sel.size(), table);
        for (size_t i = 0; i < sel.size() && i < 30; ++i) ImGui::Text("%s  #%u   guid %u", sel[i]->name.c_str(), sel[i]->entry, sel[i]->guid);
        if (sel.size() > 30) ImGui::TextColored(kQuiet, "...");
        return true;
    }
    const Spawn& s = *sel.front();
    // Both rows read once per spawn and project change (the template from the database).
    const std::string key = std::string(table) + std::to_string(s.guid) + "|" + std::to_string(m_store.Revision());
    if (key != m_inspectKey)
    {
        m_inspectKey = key;
        const auto row = Spawns().Row(s.guid);
        m_inspectRow = row ? *row : nlohmann::json();
        m_inspectTemplate = nullptr;
        std::string error;
        if (m_db.Connected())
            if (const auto rows = m_db.QueryRows(std::string("SELECT * FROM ") + table + "_template WHERE entry = " + std::to_string(s.entry), error); rows && !rows->empty())
                m_inspectTemplate = (*rows)[0];
    }
    ImGui::TextColored(kAccent, "%s", s.name.c_str());
    ImGui::TextColored(kQuiet, "%s guid %u   entry %u", table, s.guid, s.entry);
    if (m_spawnKind == SpawnKind::Creature && m_inspectTemplate.is_object())
        ImGui::TextColored(kQuiet, "level %s-%s   faction %s   npcflag %s   rank %s", Text(m_inspectTemplate["minlevel"]).c_str(),
                           Text(m_inspectTemplate["maxlevel"]).c_str(), Text(m_inspectTemplate["faction"]).c_str(),
                           Text(m_inspectTemplate["npcflag"]).c_str(), Text(m_inspectTemplate["rank"]).c_str());
    const auto look = m_looks.SpawnLook(s);
    ImGui::TextColored(kQuiet, "display %u: %s", s.displayId, look ? look->look.model.c_str() : "(no model)");
    ImGui::TextColored(kQuiet, "zone %s   area %s", AreaLabel(s.zoneId).c_str(), AreaLabel(s.areaId).c_str());
    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##inspectfilter", "Filter fields", &filter);
    if (ImGui::CollapsingHeader((std::string("Spawn (") + table + ")").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) RowTable("##spawnrow", m_inspectRow, filter);
    if (ImGui::CollapsingHeader((std::string("Template (") + table + "_template)").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        RowTable("##templaterow", m_inspectTemplate, filter);
    ImGui::TextColored(kQuiet, "Edit facing, wander and respawn in Tools > Selected.");
    return true;
}

bool App::InspectObject()
{
    if (m_objSel.empty()) return false;
    if (m_objSel.size() > 1)
    {
        ImGui::TextColored(kAccent, "%zu objects selected", m_objSel.size());
        size_t shown = 0;
        for (const ObjectRef& r : m_objSel)
        {
            if (++shown > 30) { ImGui::TextColored(kQuiet, "..."); break; }
            const std::string name = r.wmo ? (m_terrain.FindWmo(r.uid) ? m_terrain.FindWmo(r.uid)->model : "?")
                                           : (m_terrain.FindDoodad(r.uid) ? m_terrain.FindDoodad(r.uid)->model : "?");
            ImGui::Text("%s   id %u", name.c_str(), r.uid);
        }
        return true;
    }
    const ObjectRef ref = *m_objSel.begin();
    std::vector<std::string> tiles;   // every loaded tile that lists it
    for (const auto& [key, tile] : m_terrain.Tiles())
    {
        const bool listed = ref.wmo ? std::any_of(tile.adt.wmos.begin(), tile.adt.wmos.end(), [&](const WmoPlacement& w) { return w.uniqueId == ref.uid; })
                                    : std::any_of(tile.adt.doodads.begin(), tile.adt.doodads.end(), [&](const DoodadPlacement& d) { return d.uniqueId == ref.uid; });
        if (listed) tiles.push_back(std::to_string(tile.x) + "_" + std::to_string(tile.y));
    }
    std::string tileList;
    for (const auto& t : tiles) tileList += (tileList.empty() ? "" : ", ") + t;
    char buf[64];
    if (ref.wmo)
    {
        const auto w = m_terrain.FindWmo(ref.uid);
        if (!w) return false;
        ImGui::TextColored(kAccent, "%s", w->model.c_str());
        if (!ImGui::BeginTable("##object", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return true;
        Line("Kind", "WMO (building)");
        Line("Unique id", std::to_string(w->uniqueId) + (w->uniqueId >= 200'000'000 ? "  (added in this project)" : ""));
        Line("Tiles", tileList);
        Line("Position", Floats(w->pos, 3));
        Line("Rotation", Floats(w->rot, 3, "%.1f"));
        const float size[3] = { w->extMax[0] - w->extMin[0], w->extMax[1] - w->extMin[1], w->extMax[2] - w->extMin[2] };
        Line("Size (box)", Floats(size, 3, "%.1f") + " yd");
        snprintf(buf, sizeof buf, "0x%04X", w->flags);
        Line("Flags", buf);
        Line("Doodad set", std::to_string(w->doodadSet));
        Line("Name set", std::to_string(w->nameSet));
        auto [cached, fresh] = m_wmoKeys.try_emplace(w->model);
        if (fresh) cached->second = ReadWmoAreaKeys(w->model, [&](const std::string& name) { return m_mpq.Read(name); });
        if (cached->second)
        {
            Line("WMOID", std::to_string(cached->second->wmoId));
            Line("Groups", std::to_string(cached->second->groups.size()));
            Line("WMOAreaTable rows", std::to_string(m_wmoAreas.For(cached->second->wmoId, w->nameSet).size()));
        }
        Line("Map doodads inside", std::to_string(m_terrain.DoodadsInside({ ref }).size()));
        ImGui::EndTable();
    }
    else
    {
        const auto d = m_terrain.FindDoodad(ref.uid);
        if (!d) return false;
        ImGui::TextColored(kAccent, "%s", d->model.c_str());
        if (!ImGui::BeginTable("##object", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return true;
        Line("Kind", "M2 (doodad)");
        Line("Unique id", std::to_string(d->uniqueId) + (d->uniqueId >= 200'000'000 ? "  (added in this project)" : ""));
        Line("Tiles", tileList);
        Line("Position", Floats(d->pos, 3));
        Line("Rotation", Floats(d->rot, 3, "%.1f"));
        snprintf(buf, sizeof buf, "%.3f", d->scale);
        Line("Scale", buf);
        snprintf(buf, sizeof buf, "0x%04X", d->flags);
        Line("Flags", buf);
        if (const auto cell = m_terrain.ChunkAtGrid(int(std::floor(d->pos[0] / kChunkSize)), int(std::floor(d->pos[2] / kChunkSize))))
            Line("Area", AreaLabel(m_terrain.Chunk(*cell)->areaId));
        ImGui::EndTable();
    }
    ImGui::TextColored(kQuiet, "Edit position, rotation and scale in the Object panel.");
    return true;
}

bool App::InspectArea()
{
    if (!m_activeArea) return false;
    const nlohmann::json& row = m_areas.Row(m_activeArea);
    if (!row.is_object()) return false;
    ImGui::TextColored(kAccent, "%s", AreaLabel(m_activeArea).c_str());
    const uint32_t zone = m_areas.ZoneOf(m_activeArea);
    ImGui::TextColored(kQuiet, "%s%s", zone == m_activeArea ? "a zone" : ("in " + AreaLabel(zone)).c_str(),
                       m_project && m_project->Owns("area.id", m_activeArea) ? "   (added in this project)" : "");
    size_t chunks = 0;
    for (const auto& [key, tile] : m_terrain.Tiles())
        for (const AdtChunk& c : tile.adt.chunks) chunks += c.areaId == m_activeArea;
    ImGui::TextColored(kQuiet, "%zu chunk(s) on the loaded tiles", chunks);
    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##inspectfilter", "Filter fields", &filter);
    if (ImGui::CollapsingHeader("AreaTable", ImGuiTreeNodeFlags_DefaultOpen)) RowTable("##arearow", row, filter);
    if (const auto wm = m_worldMaps.ForZone(CurrentMapId(), zone); wm && ImGui::CollapsingHeader("WorldMapArea (its zone's map)"))
        RowTable("##worldmaprow", m_worldMaps.Row(*wm), filter);
    return true;
}
