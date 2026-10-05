// Zones: area ids painted on terrain chunks (MCNK) and the AreaTable rows they name (the Zones tool and its panel).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };

/// A stable, well spread colour per area id.
XMFLOAT4 AreaColor(uint32_t id, float alpha)
{
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod(id * 0.6180339f, 1.0f), 0.65f, 1.0f, r, g, b);
    return { r, g, b, alpha };
}
}

std::string App::AreaLabel(uint32_t id) const
{
    if (id == 0) return "none (0)";
    const auto a = m_areas.Find(id);
    return a ? a->name + " (" + std::to_string(id) + ")" : std::to_string(id) + " (not in AreaTable)";
}

bool App::AreasPainted() const
{
    bool painted = false;
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        painted |= domain == m_terrain.Domain() && data.contains("areas");
    });
    return painted;
}

void App::ZonesViewport()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift)   // pick a building
    {
        m_objSel.clear();
        if (m_objHover && m_objHover->wmo) m_objSel.insert(*m_objHover);
        return;
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
    {
        if (io.KeyAlt)
        {
            if (const AdtChunk* c = m_terrain.Chunk(m_hover->chunk)) m_activeArea = c->areaId;
        }
        else if (!m_activeArea)
            Log("Pick an area first: Zones panel, or Alt+click the ground.");
        else
            m_terrain.BeginAreas();
    }
    if (m_terrain.AreaStroking() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.AreaStep(m_hover->pos, m_areaRadius, m_activeArea);
    if (m_terrain.AreaStroking() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        if (auto change = m_terrain.EndAreas("Paint " + AreaLabel(m_activeArea)))
        {
            Log("%s", change->label.c_str());
            m_store.Commit(std::move(*change));
        }
}

void App::BuildZoneOverlay(std::vector<LineVertex>& lines) const
{
    // Each chunk edge that meets a chunk of another area, drawn just inside the chunk in its area's colour:
    // every area gets a coloured rim along its border. Only near the camera (the far tiles would be noise).
    constexpr float kInset = 0.8f, kLift = 0.5f, kReach = 1200.0f;
    for (const auto& [key, tile] : m_terrain.Tiles())
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            const AdtChunk& c = tile.adt.chunks[ci];
            const float cx = c.baseX + kChunkSize / 2 - m_camera.pos.x, cz = c.baseZ + kChunkSize / 2 - m_camera.pos.z;
            if (cx * cx + cz * cz > kReach * kReach) continue;
            const auto [gx, gz] = m_terrain.GridOf({ key, int(ci) });
            const XMFLOAT4 color = AreaColor(c.areaId, c.areaId == m_activeArea ? 1.0f : 0.6f);
            auto p = [&](int row, int col) {
                const float ix = col == 0 ? kInset : col == 8 ? -kInset : 0, iz = row == 0 ? kInset : row == 8 ? -kInset : 0;
                return XMFLOAT3{ c.baseX + col * kUnitSize + ix, c.baseY + c.heights[size_t(row * 17 + col)] + kLift, c.baseZ + row * kUnitSize + iz };
            };
            // Neighbour across each edge: -z (row 0), +z (row 8), -x (col 0), +x (col 8).
            const int sides[4][3] = { { 0, -1, 0 }, { 0, 1, 8 }, { -1, 0, 0 }, { 1, 0, 8 } };
            for (int s = 0; s < 4; ++s)
            {
                const auto n = m_terrain.ChunkAtGrid(gx + sides[s][0], gz + sides[s][1]);
                const AdtChunk* nc = n ? m_terrain.Chunk(*n) : nullptr;
                if (!nc || nc->areaId == c.areaId) continue;
                const bool alongX = s < 2;   // edges at row 0 / 8 run along x
                for (int i = 0; i < 8; ++i)
                {
                    lines.push_back({ alongX ? p(sides[s][2], i) : p(i, sides[s][2]), color });
                    lines.push_back({ alongX ? p(sides[s][2], i + 1) : p(i + 1, sides[s][2]), color });
                }
            }
        }
    // The picked building, and the one Shift+click would pick.
    auto box = [&](const ObjectRef& r, XMFLOAT4 color) {
        XMFLOAT3 c[8];
        if (!m_models.Corners(r.wmo, r.uid, c)) return;
        const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& e : edges) { lines.push_back({ c[e[0]], color }); lines.push_back({ c[e[1]], color }); }
    };
    for (const ObjectRef& r : m_objSel)
        if (r.wmo) box(r, { 1, 0.75f, 0.2f, 1 });
    if (m_objHover && m_objHover->wmo && !m_objSel.count(*m_objHover)) box(*m_objHover, { 1, 1, 1, 0.6f });
    if (!m_hover || ImGui::GetIO().KeyShift) return;
    // The chunks the brush paints.
    for (const ChunkRef& ref : m_terrain.ChunksAt(m_hover->pos, m_areaRadius))
    {
        const AdtChunk* c = m_terrain.Chunk(ref);
        auto p = [&](int row, int col) { return XMFLOAT3{ c->baseX + col * kUnitSize, c->baseY + c->heights[size_t(row * 17 + col)] + 0.7f, c->baseZ + row * kUnitSize }; };
        const XMFLOAT4 white{ 1, 1, 1, 0.9f };
        for (int i = 0; i < 8; ++i)
        {
            lines.push_back({ p(0, i), white }); lines.push_back({ p(0, i + 1), white });
            lines.push_back({ p(8, i), white }); lines.push_back({ p(8, i + 1), white });
            lines.push_back({ p(i, 0), white }); lines.push_back({ p(i + 1, 0), white });
            lines.push_back({ p(i, 8), white }); lines.push_back({ p(i + 1, 8), white });
        }
    }
}

void App::DrawZonesPanel(float w)
{
    auto swatch = [](uint32_t id) {
        const XMFLOAT4 c = AreaColor(id, 1);
        ImGui::ColorButton("##swatch", { c.x, c.y, c.z, 1 }, ImGuiColorEditFlags_NoTooltip, { ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight() });
        ImGui::SameLine();
    };
    const uint32_t map = CurrentMapId();
    // Areas painted on the loaded terrain (whatever map their AreaTable row names: module copies of a continent reuse
    // its ids), with their zones so the tree is whole; or every AreaTable row of this map.
    std::map<uint32_t, size_t> painted;
    for (const auto& [key, tile] : m_terrain.Tiles())
        for (const AdtChunk& c : tile.adt.chunks) ++painted[c.areaId];
    std::vector<Area> areas;
    std::set<uint32_t> listed;
    auto add = [&](uint32_t id) {
        for (int depth = 0; depth < 8 && id && listed.insert(id).second; ++depth)
        {
            const auto a = m_areas.Find(id);
            areas.push_back(a ? *a : Area{ id, map, 0, 0, 0, 0, "(not in AreaTable)" });
            id = a ? a->parent : 0;
        }
    };
    for (const auto& [id, n] : painted) add(id);
    if (!m_areasLoadedOnly)
        for (const Area& a : m_areas.OnMap(map)) add(a.id);

    if (Section("Paint"))
    {
        if (m_activeArea)
        {
            swatch(m_activeArea);
            ImGui::TextUnformatted(AreaLabel(m_activeArea).c_str());
        }
        else
            ImGui::TextColored(kQuiet, "No area picked yet (Areas tab, or Alt+click the ground).");
        if (const AdtChunk* c = m_hover ? m_terrain.Chunk(m_hover->chunk) : nullptr)
        {
            ImGui::TextColored(kQuiet, "Under cursor: %s", AreaLabel(c->areaId).c_str());
            if (const uint32_t zone = m_areas.ZoneOf(c->areaId); zone != c->areaId) ImGui::TextColored(kQuiet, "Zone: %s", AreaLabel(zone).c_str());
        }
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Radius##areas", &m_areaRadius, 1.0f, 300.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
        ImGui::TextColored(kQuiet, "Drag: paint whole chunks (33 yd)\nAlt+click: pick the area under the cursor\nShift+click: pick a building (Building tab)\n"
                                   "Ctrl+wheel: radius");
    }

    if (Section(("Areas (" + std::to_string(areas.size()) + ")###areas").c_str()))
    {
        if (ImGui::RadioButton("On the loaded terrain", m_areasLoadedOnly)) m_areasLoadedOnly = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Whole map", !m_areasLoadedOnly)) m_areasLoadedOnly = false;
        ImGui::SetItemTooltip("Also every AreaTable row of this map, painted on the loaded tiles or not.");
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##areafilter", "Filter by name or id", &m_areaFilter);
        // Zones first, each followed by its sub-areas (parent chains are at most a few deep).
        auto depth = [&](const Area& a) { int d = 0; for (uint32_t p = a.parent; p && d < 4; ++d) { const auto up = m_areas.Find(p); p = up ? up->parent : 0; } return d; };
        std::sort(areas.begin(), areas.end(), [&](const Area& a, const Area& b) {
            const uint32_t za = m_areas.ZoneOf(a.id), zb = m_areas.ZoneOf(b.id);
            return za != zb ? za < zb : (a.id == za) != (b.id == zb) ? a.id == za : a.id < b.id;
        });
        std::string filter = m_areaFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (ImGui::BeginChild("##areas", { w, 0 }, ImGuiChildFlags_Borders))
            for (const Area& a : areas)
            {
                std::string name = a.name;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                if (!filter.empty() && name.find(filter) == std::string::npos && std::to_string(a.id).find(filter) == std::string::npos) continue;
                ImGui::PushID(int(a.id));
                ImGui::Indent(depth(a) * 12.0f + 1);
                swatch(a.id);
                const auto n = painted.find(a.id);
                const std::string label = a.name + "  " + std::to_string(a.id) + (n != painted.end() ? "   " + std::to_string(n->second) + " chunks" : "") +
                                          (m_project && m_project->Owns("area.id", a.id) ? "  (project)" : "");
                if (ImGui::Selectable(label.c_str(), a.id == m_activeArea)) m_activeArea = a.id;
                ImGui::Unindent(depth(a) * 12.0f + 1);
                ImGui::PopID();
            }
        ImGui::EndChild();
    }

    if (Section("New area"))
    {
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputText("Name##newarea", &m_newAreaName);
        ImGui::SetNextItemWidth(w - 90);
        const std::string parentLabel = m_newAreaParent ? AreaLabel(m_newAreaParent) : "none: a new zone";
        if (ImGui::BeginCombo("Inside", parentLabel.c_str()))
        {
            if (ImGui::Selectable("none: a new zone", m_newAreaParent == 0)) m_newAreaParent = 0;
            for (const Area& a : areas)
                if (a.parent == 0 && ImGui::Selectable((a.name + "  " + std::to_string(a.id)).c_str(), a.id == m_newAreaParent)) m_newAreaParent = a.id;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("A sub-area copies its zone's music, sounds and flags.");
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputScalar("Level##newarea", ImGuiDataType_U32, &m_newAreaLevel);
        ImGui::SetItemTooltip("ExplorationLevel: exploring it gives experience scaled to this level.");
        ImGui::BeginDisabled(!m_project || m_newAreaName.empty() || !map);
        if (ImGui::Button("Create and paint with it", { w, 0 }))
        {
            const Project::IdRange r = m_project->Range("area.id");
            if (const uint32_t id = m_areas.FreeId(r.first, r.last); !id)
                Log("No free area id in the project's range %u-%u (File > Project settings).", r.first, r.last);
            else if (nlohmann::json row = m_areas.NewRow(id, map, m_newAreaParent, m_newAreaName, m_newAreaLevel); row.value("AreaBit", 0u) == 0)
                Log("No free exploration bit (AreaBit) left below 4096.");
            else
            {
                m_areas.Commit(id, row, "Add area " + m_newAreaName);
                Log("Added area %s, AreaBit %u.", AreaLabel(id).c_str(), row.value("AreaBit", 0u));
                m_activeArea = id;
                m_newAreaName.clear();
            }
        }
        ImGui::EndDisabled();
    }

    if (m_activeArea && (m_areaEditId != m_activeArea || m_areaEditVersion != m_areas.Version()))
    {
        m_areaEdit = m_areas.Row(m_activeArea);
        m_areaEditId = m_activeArea;
        m_areaEditVersion = m_areas.Version();
    }
    if (m_activeArea && m_areaEdit.is_object() && Section("Edit"))
    {
        ImGui::TextUnformatted(AreaLabel(m_activeArea).c_str());
        std::string name = m_areaEdit.value("AreaName_lang", "");
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::InputText("Name##editarea", &name)) m_areaEdit["AreaName_lang"] = name;
        auto u32 = [&](const char* label, const char* field, ImGuiInputTextFlags flags = 0) {
            uint32_t v = m_areaEdit.value(field, 0u);
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::InputScalar(label, ImGuiDataType_U32, &v, nullptr, nullptr, flags & ImGuiInputTextFlags_CharsHexadecimal ? "%08X" : nullptr, flags))
                m_areaEdit[field] = v;
        };
        u32("Parent", "ParentAreaID");
        u32("Level##editarea", "ExplorationLevel");
        u32("Flags", "Flags", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SetItemTooltip("AreaTable flags (hex), e.g. 0x800 sanctuary, 0x8 capital. See the wiki's AreaTable page.");
        ImGui::TextColored(kQuiet, "AreaBit %u   music %u   ambience %u", m_areaEdit.value("AreaBit", 0u), m_areaEdit.value("ZoneMusic", 0u),
                           m_areaEdit.value("AmbienceID", 0u));
        const bool changed = m_areaEdit != m_areas.Row(m_activeArea);
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Apply", { (w - 8) / 2, 0 })) m_areas.Commit(m_activeArea, m_areaEdit, "Edit area " + name);
        ImGui::SameLine();
        if (ImGui::Button("Revert", { (w - 8) / 2, 0 })) m_areaEditVersion = ~0ull;
        ImGui::EndDisabled();
    }
    DrawBuildingAreas(w);
    DrawWorldMapSection(w);
}

void App::DrawBuildingAreas(float w)
{
    const ObjectRef* picked = m_objSel.size() == 1 && m_objSel.begin()->wmo ? &*m_objSel.begin() : nullptr;
    const uint32_t pickedUid = picked ? picked->uid : 0;   // a newly picked building brings its tab forward
    const bool select = pickedUid && pickedUid != m_buildingTabUid;
    m_buildingTabUid = pickedUid;
    if (!Section("Building", select)) return;
    const auto placement = picked ? m_terrain.FindWmo(picked->uid) : std::nullopt;
    if (!placement)
    {
        ImGui::TextColored(kQuiet, "Shift+click a building (WMO) to set the area\ninside it. Groups without a WMOAreaTable row\nuse the ground's area.");
        return;
    }
    auto [cached, fresh] = m_wmoKeys.try_emplace(placement->model);
    if (fresh) cached->second = ReadWmoAreaKeys(placement->model, [&](const std::string& name) { return m_mpq.Read(name); });
    if (!cached->second) { ImGui::TextColored(kWarn, "Cannot read %s", placement->model.c_str()); return; }
    const WmoAreaKeys& keys = *cached->second;
    const uint32_t nameSet = placement->nameSet;

    // WMOAreaTable rows are per model and name set, so every placement sharing both gets the same areas.
    std::set<uint32_t> sharing, nameSets;
    for (const auto& [key, tile] : m_terrain.Tiles())
        for (const WmoPlacement& p : tile.adt.wmos)
            if (p.model == placement->model)
            {
                nameSets.insert(p.nameSet);
                if (p.nameSet == nameSet) sharing.insert(p.uniqueId);
            }
    const size_t slash = placement->model.find_last_of("\\/");
    ImGui::TextUnformatted(placement->model.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
    ImGui::TextColored(kQuiet, "WMOID %u   name set %u   %zu group(s)", keys.wmoId, nameSet, keys.groups.size());
    if (sharing.size() > 1)
    {
        ImGui::PushTextWrapPos(w);
        ImGui::TextColored(kWarn, "%zu placements of this model use name set %u on the loaded tiles (maybe more elsewhere); "
                                  "the rows below apply to all of them.", sharing.size(), nameSet);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Give this one its own name set", { w, 0 }))
        {
            const uint32_t next = std::max(m_wmoAreas.MaxNameSet(keys.wmoId), *nameSets.rbegin()) + 1;
            if (next > 127)
                Log("No name set left for this model: AzerothCore reads name sets as 8-bit signed (at most 127).");
            else
            {
                m_terrain.BeginObjectEdit({ *picked });
                m_terrain.PreviewObjectEdit([](DoodadPlacement&) {}, [&](WmoPlacement& p) { p.nameSet = uint16_t(next); });
                if (auto change = m_terrain.EndObjectEdit("Name set " + std::to_string(next) + " for one placement"))
                {
                    m_store.Commit(std::move(*change));
                    Log("Name set %u: the server keeps the old one until its vmaps are extracted again from the exported tiles.", next);
                }
            }
        }
    }

    const auto rows = m_wmoAreas.For(keys.wmoId, nameSet);
    auto areaOf = [&](uint32_t group) -> std::string {
        const auto r = rows.find(group);
        if (r == rows.end()) return "ground";
        const uint32_t area = r->second.second.value("AreaTableID", 0u);
        const std::string name = r->second.second.value("AreaName_lang", "");
        return (area ? AreaLabel(area) : "ground") + (name.empty() ? "" : "  \"" + name + "\"");
    };
    // Rows for these groups point at the active area; the name override is cleared so the area's own name shows.
    auto assign = [&](const std::vector<uint32_t>& groups, const std::string& label) {
        const Project::IdRange range = m_project->Range("wmoarea.id");
        std::vector<Change> parts;
        for (uint32_t group : groups)
        {
            uint32_t id = 0;
            nlohmann::json after;
            if (const auto r = rows.find(group); r != rows.end())
                std::tie(id, after) = r->second;
            else if (id = m_wmoAreas.FreeId(range.first, range.last); id)
                after = m_wmoAreas.NewRow(id, keys.wmoId, nameSet, group, 0);
            else
            {
                Log("No free id in the project's wmoarea.id range %u-%u (File > Project settings).", range.first, range.last);
                break;
            }
            after["AreaTableID"] = m_activeArea;
            after["AreaName_lang"] = "";
            Change c = m_wmoAreas.MakeChange(id, m_wmoAreas.Row(id), after, label);
            m_wmoAreas.Apply(c);   // before the next FreeId, so each new row gets its own id
            parts.push_back(std::move(c));
        }
        m_store.Commit(std::move(parts), label);
    };

    ImGui::BeginDisabled(!m_activeArea || !m_project);
    if (ImGui::Button(("Whole building: " + (m_activeArea ? AreaLabel(m_activeArea) : std::string("pick an area"))).c_str(), { w, 0 }))
    {
        std::vector<uint32_t> groups;
        for (const auto& [id, name] : keys.groups) groups.push_back(id);
        if (rows.count(0xFFFFFFFFu)) groups.push_back(0xFFFFFFFFu);
        assign(groups, "Building " + std::to_string(keys.wmoId) + "/" + std::to_string(nameSet) + " -> " + AreaLabel(m_activeArea));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("One WMOAreaTable row per group (the server looks groups up one by one, never the -1 row).");
    if (ImGui::BeginTable("##groups", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, { w, 180 }))
    {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Area");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 36);
        for (const auto& [group, name] : keys.groups)
        {
            ImGui::PushID(int(group));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s  %u", name.empty() ? "(unnamed)" : name.c_str(), group);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(areaOf(group).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!m_activeArea || !m_project);
            if (ImGui::SmallButton("Set")) assign({ group }, "Group " + std::to_string(group) + " -> " + AreaLabel(m_activeArea));
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
