// Zones: area ids painted on terrain chunks (MCNK) and the AreaTable rows they name (the Zones tool and its panel).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };

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
    if (!m_hover) return;
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
    ImGui::SeparatorText("Zones");
    if (const AdtChunk* c = m_hover ? m_terrain.Chunk(m_hover->chunk) : nullptr)
    {
        ImGui::TextColored(kQuiet, "Under cursor: %s", AreaLabel(c->areaId).c_str());
        if (const uint32_t zone = m_areas.ZoneOf(c->areaId); zone != c->areaId) ImGui::TextColored(kQuiet, "Zone: %s", AreaLabel(zone).c_str());
    }
    auto swatch = [](uint32_t id) {
        const XMFLOAT4 c = AreaColor(id, 1);
        ImGui::ColorButton("##swatch", { c.x, c.y, c.z, 1 }, ImGuiColorEditFlags_NoTooltip, { ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight() });
        ImGui::SameLine();
    };

    ImGui::SeparatorText("Paint");
    if (m_activeArea)
    {
        swatch(m_activeArea);
        ImGui::TextUnformatted(AreaLabel(m_activeArea).c_str());
    }
    else
        ImGui::TextColored(kQuiet, "No area picked yet.");
    ImGui::SetNextItemWidth(w - 90);
    ImGui::SliderFloat("Radius##areas", &m_areaRadius, 1.0f, 300.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
    ImGui::TextColored(kQuiet, "Drag: paint whole chunks (33 yd)\nAlt+click: pick the area under the cursor\nCtrl+wheel: radius\n"
                               "Inside WMOs the client uses WMOAreaTable,\nnot these ids.");

    const uint32_t map = CurrentMapId();
    std::vector<Area> areas = m_areas.OnMap(map);
    ImGui::SeparatorText(("Areas on this map (" + std::to_string(areas.size()) + ")").c_str());
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
    if (ImGui::BeginChild("##areas", { w, 220 }, ImGuiChildFlags_Borders))
        for (const Area& a : areas)
        {
            std::string name = a.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (!filter.empty() && name.find(filter) == std::string::npos && std::to_string(a.id).find(filter) == std::string::npos) continue;
            ImGui::PushID(int(a.id));
            ImGui::Indent(depth(a) * 12.0f + 1);
            swatch(a.id);
            const std::string label = a.name + "  " + std::to_string(a.id) + (m_project && m_project->Owns("area.id", a.id) ? "  (project)" : "");
            if (ImGui::Selectable(label.c_str(), a.id == m_activeArea)) m_activeArea = a.id;
            ImGui::Unindent(depth(a) * 12.0f + 1);
            ImGui::PopID();
        }
    ImGui::EndChild();

    ImGui::SeparatorText("New area");
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

    if (!m_activeArea) return;
    if (m_areaEditId != m_activeArea || m_areaEditVersion != m_areas.Version())
    {
        m_areaEdit = m_areas.Row(m_activeArea);
        m_areaEditId = m_activeArea;
        m_areaEditVersion = m_areas.Version();
    }
    if (!m_areaEdit.is_object()) return;
    ImGui::SeparatorText(("Edit " + AreaLabel(m_activeArea)).c_str());
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
