// Triggers: area triggers (AreaTrigger.dbc + the world's `areatrigger`), where they teleport players
// (areatrigger_teleport), and how instances are entered and left (Map.dbc corpse entrance, instance_template.parent).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
constexpr float kTwoPi = 6.2831853f;
constexpr uint32_t kNoMap = 0x80000000u;   // Map.dbc CorpseMapID -1 (and anything negative): no corpse entrance

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

XMFLOAT3 Centre(const Trigger& t) { return ServerToEditor(t.x, t.y, t.z); }

/// Top of the trigger (for its label), editor axes.
XMFLOAT3 Top(const Trigger& t)
{
    XMFLOAT3 p = Centre(t);
    p.y += (t.Sphere() ? t.radius : t.height / 2) + 1.5f;
    return p;
}

std::string Shape(const Trigger& t)
{
    char text[64];
    if (t.Sphere()) snprintf(text, sizeof text, "sphere %.0f yd", t.radius);
    else snprintf(text, sizeof text, "box %.0f x %.0f x %.0f", t.length, t.width, t.height);
    return text;
}

/// Server orientation of where the camera looks (0 = north, counter-clockwise).
float Facing(const XMFLOAT3& forward)
{
    const float o = std::atan2(-forward.x, -forward.z);
    return o < 0 ? o + kTwoPi : o;
}
}

// ---------------------------------------------------------------------------------------------- data

std::string App::MapLabel(uint32_t id) const
{
    for (const auto& m : m_maps)
        if (m.id == id) return m.name + " (" + std::to_string(id) + ")";
    return std::to_string(id) + " (not in Map.dbc)";
}

void App::FlyTo(uint32_t map, float x, float y, float z, bool ground)
{
    const XMFLOAT3 p = ServerToEditor(x, y, z);
    for (const auto& m : m_maps)
        if (m.id == map && m.directory != m_terrain.Map()) GoToTile(m.directory, int(p.x / kTileSize), int(p.z / kTileSize));
    m_focusTile.reset();   // the camera height is set here, not snapped to the tile
    m_flyGround.reset();
    m_camera.pos = { p.x, p.y + 40, p.z - 60 };
    m_camera.yaw = 0;
    m_camera.pitch = -0.55f;
    if (!ground) return;
    if (const auto h = m_terrain.HeightAt(p.x, p.z)) m_camera.pos.y = *h + 40;
    else m_flyGround = { p.x, p.z };   // taken once that ground streams in
}

const std::vector<Trigger>& App::TriggersOnMap() const
{
    const std::pair<uint32_t, uint64_t> key{ CurrentMapId(), m_triggers.Version() };
    if (key != m_triggerViewKey)
    {
        m_triggerViewKey = key;
        m_triggerView = m_terrain.Map().empty() ? std::vector<Trigger>{} : m_triggers.OnMap(key.first);
    }
    return m_triggerView;
}

const std::map<uint32_t, Teleport>& App::TeleportsView() const
{
    const bool db = m_db.Connected();
    if (m_teleportViewRevision == m_store.Revision() && m_teleportViewDb == db) return m_teleportView;
    m_teleportViewRevision = m_store.Revision();
    m_teleportViewDb = db;
    m_teleportView.clear();
    for (const auto& [id, rows] : m_teleports.All()) m_teleportView[id] = Teleport::FromRow(rows.front());
    return m_teleportView;
}

void App::CommitTrigger(uint32_t id, const std::optional<Trigger>& after, const std::optional<Teleport>& teleport, const std::string& label)
{
    if (!m_db.Connected())
    {
        Log("Triggers need the world database (File > Server setup): each one has an `areatrigger` row the server checks players against.");
        return;
    }
    std::vector<Change> parts;
    const nlohmann::json beforeDbc = m_triggers.Row(id);
    const nlohmann::json afterDbc = after ? after->ToDbcRow(beforeDbc.is_null() ? nlohmann::json::object() : beforeDbc) : nlohmann::json();
    if (afterDbc != beforeDbc)
    {
        // The server's row follows the client's: same id, same shape.
        Change c = m_triggers.MakeChange(id, beforeDbc, afterDbc, label);
        m_triggers.Apply(c);
        parts.push_back(std::move(c));
        Change s = m_triggerRows.MakeChange(id, m_triggerRows.Rows(id), after ? std::vector<nlohmann::json>{ after->ToServerRow() } : std::vector<nlohmann::json>{}, label);
        m_triggerRows.Apply(s);
        parts.push_back(std::move(s));
    }
    const std::vector<nlohmann::json> beforeTp = m_teleports.Rows(id);
    const std::vector<nlohmann::json> afterTp = teleport ? std::vector<nlohmann::json>{ teleport->ToRow() } : std::vector<nlohmann::json>{};
    const bool tpChanged = beforeTp.empty() != afterTp.empty() || (!afterTp.empty() && Teleport::FromRow(beforeTp.front()).ToRow() != afterTp.front());
    if (tpChanged)
    {
        Change t = m_teleports.MakeChange(id, beforeTp, afterTp, label);
        m_teleports.Apply(t);
        parts.push_back(std::move(t));
    }
    if (parts.empty()) return;
    for (const TableRowsAdapter* table : { &m_triggerRows, &m_teleports })
        if (!table->LastError().empty()) Log("%s", table->LastError().c_str());
    m_store.Commit(std::move(parts), label);
    if (!after) m_triggerSel = m_triggerSel == id ? 0 : m_triggerSel;
    if (afterDbc != beforeDbc)
        Log("%s: export and restart the client (AreaTrigger.dbc); worldserver reads trigger shapes only at start.", label.c_str());
    if (tpChanged && RunServerCommand(".reload areatrigger_teleport")) Log("%s: teleports reloaded on the server.", label.c_str());
}

// ---------------------------------------------------------------------------------------------- viewport

void App::TriggersViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    const ImGuiIO& io = ImGui::GetIO();
    const uint32_t map = CurrentMapId();
    m_triggerHover.reset();
    if (ImGui::IsItemHovered())
    {
        // The trigger the cursor ray enters first; one the camera is inside only when nothing else is hit.
        const float mx = (io.MousePos.x - origin.x) / size.x * 2 - 1, my = 1 - (io.MousePos.y - origin.y) / size.y * 2;
        const XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
        XMFLOAT3 p0, p1;
        XMStoreFloat3(&p0, XMVector3TransformCoord(XMVectorSet(mx, my, 0, 1), inv));
        XMStoreFloat3(&p1, XMVector3TransformCoord(XMVectorSet(mx, my, 1, 1), inv));
        XMFLOAT3 o, d;
        EditorToServer(p0, o.x, o.y, o.z);
        EditorToServer(p1, d.x, d.y, d.z);
        d = { d.x - o.x, d.y - o.y, d.z - o.z };
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        d = { d.x / len, d.y / len, d.z / len };
        float best = 1e30f;
        bool bestInside = true;
        for (const Trigger& t : TriggersOnMap())
            if (const auto hit = t.Hit(o, d))
            {
                const bool inside = *hit <= 0;
                if ((bestInside && !inside) || (inside == bestInside && *hit < best))
                {
                    best = *hit;
                    bestInside = inside;
                    m_triggerHover = t.id;
                }
            }
    }
    if (!ImGui::IsItemActivated() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    float gx = 0, gy = 0, gz = 0;
    if (m_hover) EditorToServer(m_hover->pos, gx, gy, gz);
    const TriggerPick pick = m_triggerPick == TriggerPick::None && io.KeyAlt && m_triggerSel ? TriggerPick::Move : m_triggerPick;
    if (pick == TriggerPick::None)
    {
        m_triggerSel = m_triggerHover.value_or(0);
        return;
    }
    if (!m_hover) return;
    if (pick != TriggerPick::Place || !io.KeyShift) m_triggerPick = TriggerPick::None;
    switch (pick)
    {
    case TriggerPick::Place:
    {
        const Project::IdRange r = m_project->Range("areatrigger.id");
        Trigger t = m_triggerShape;
        t.id = m_triggers.FreeId(r.first, r.last);
        if (!t.id) { Log("No free trigger id in the project's range %u-%u (File > Project settings).", r.first, r.last); return; }
        t.map = map;
        t.x = gx;
        t.y = gy;
        t.z = gz + (t.Sphere() ? 0 : t.height / 2);
        CommitTrigger(t.id, t, std::nullopt, "Add trigger " + std::to_string(t.id));
        if (!m_triggers.Find(t.id)) return;
        m_triggerSel = t.id;
        if (m_triggerNewTeleport && !io.KeyShift)
        {
            m_triggerPick = TriggerPick::Target;
            Log("Trigger %u placed. Now click where it sends players (another map: open it in Maps first; Esc: no teleport).", t.id);
        }
        break;
    }
    case TriggerPick::Move:
        if (auto t = m_triggers.Find(m_triggerSel))
        {
            t->x = gx;
            t->y = gy;
            t->z = gz + (t->Sphere() ? 0 : t->height / 2);
            const auto tp = TeleportsView().find(t->id);
            CommitTrigger(t->id, t, tp == TeleportsView().end() ? std::nullopt : std::optional<Teleport>(tp->second), "Move trigger " + std::to_string(t->id));
        }
        break;
    case TriggerPick::Target:
        if (const auto t = m_triggers.Find(m_triggerSel))
        {
            const auto old = TeleportsView().find(t->id);
            // The teleport as the panel has it (a name typed but not applied goes along), else as saved.
            Teleport tp = m_teleportEdit && m_triggerEditId == t->id ? *m_teleportEdit : old == TeleportsView().end() ? Teleport{ t->id } : old->second;
            tp.map = map;
            tp.x = gx;
            tp.y = gy;
            tp.z = gz + 0.5f;   // a little above the ground, as Blizzard's arrivals are
            XMFLOAT3 forward;
            XMStoreFloat3(&forward, m_camera.Forward());
            tp.o = Facing(forward);
            CommitTrigger(t->id, t, tp, "Teleport of trigger " + std::to_string(t->id));
        }
        break;
    case TriggerPick::Corpse:
    {
        const uint32_t target = m_entranceMap == ~0u ? map : m_entranceMap;
        nlohmann::json row = m_mapRows.Row(target);
        if (row.is_null()) { Log("Map %u has no Map.dbc row.", target); return; }
        row["CorpseMapID"] = map;
        row["Corpse[0]"] = gx;
        row["Corpse[1]"] = gy;
        m_mapRows.Commit(target, row, "Corpse entrance of " + MapLabel(target));
        Log("Corpse entrance of %s set: export and restart the client and worldserver (Map.dbc).", MapLabel(target).c_str());
        break;
    }
    case TriggerPick::None: break;
    }
}

void App::BuildTriggerOverlay(std::vector<LineVertex>& lines) const
{
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& c) { lines.push_back({ a, c }); lines.push_back({ b, c }); };
    auto server = [](float x, float y, float z) { return ServerToEditor(x, y, z); };
    const auto& teleports = TeleportsView();
    const uint32_t map = CurrentMapId();
    for (const Trigger& saved : TriggersOnMap())
    {
        // The selected one as the panel has it, so turning or resizing shows before Apply.
        const Trigger& t = saved.id == m_triggerSel && m_triggerEdit.id == saved.id && m_triggerEdit.map == saved.map ? m_triggerEdit : saved;
        const XMFLOAT3 c = Centre(t);
        const float dx = c.x - m_camera.pos.x, dz = c.z - m_camera.pos.z;
        const bool selected = t.id == m_triggerSel, hovered = m_triggerHover == t.id;
        if (!selected && dx * dx + dz * dz > 900.0f * 900.0f) continue;
        const auto tp = teleports.find(t.id);
        XMFLOAT4 color = tp != teleports.end() ? XMFLOAT4{ 0.8f, 0.45f, 1, 0.75f } : XMFLOAT4{ 1, 0.85f, 0.3f, 0.75f };
        if (selected) color = { 1, 1, 1, 1 };
        else if (hovered) color.w = 1;
        if (t.Sphere())
        {
            constexpr int kSeg = 40;
            for (int i = 0; i < kSeg; ++i)
            {
                const float a0 = kTwoPi * i / kSeg, a1 = kTwoPi * (i + 1) / kSeg, r = t.radius;
                const float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r, c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;
                line(server(t.x + c0, t.y + s0, t.z), server(t.x + c1, t.y + s1, t.z), color);
                line(server(t.x + c0, t.y, t.z + s0), server(t.x + c1, t.y, t.z + s1), color);
                line(server(t.x, t.y + c0, t.z + s0), server(t.x, t.y + c1, t.z + s1), color);
            }
        }
        else
        {
            const float cs = std::cos(t.yaw), sn = std::sin(t.yaw);
            XMFLOAT3 k[8];
            for (int i = 0; i < 8; ++i)
            {
                const float lx = (i & 1 ? 0.5f : -0.5f) * t.length, ly = (i & 2 ? 0.5f : -0.5f) * t.width, lz = (i & 4 ? 0.5f : -0.5f) * t.height;
                k[i] = server(t.x + lx * cs - ly * sn, t.y + lx * sn + ly * cs, t.z + lz);
            }
            const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (const auto& e : edges) line(k[e[0]], k[e[1]], color);
            // The face the turn points at (+length): a cross over it and an arrow out of it. The game itself fires from
            // any side; this shows which way Turn and Length run.
            line(k[1], k[7], color);
            line(k[3], k[5], color);
            const float reach = t.length / 2 + std::clamp(t.length * 0.5f, 2.0f, 8.0f), head = std::clamp(reach * 0.25f, 0.8f, 2.5f);
            const XMFLOAT3 tip = server(t.x + cs * reach, t.y + sn * reach, t.z);
            line(c, tip, color);
            line(tip, server(t.x + cs * (reach - head) - sn * head * 0.6f, t.y + sn * (reach - head) + cs * head * 0.6f, t.z), color);
            line(tip, server(t.x + cs * (reach - head) + sn * head * 0.6f, t.y + sn * (reach - head) - cs * head * 0.6f, t.z), color);
        }
        if (selected && tp != teleports.end() && tp->second.map == map)   // where it sends players, on this map
            line(c, server(tp->second.x, tp->second.y, tp->second.z), { 0.4f, 0.9f, 1, 0.8f });
    }
    // Arrival points on this map: a cross and the way players face.
    for (const auto& [id, tp] : teleports)
    {
        if (tp.map != map) continue;
        const XMFLOAT3 p = server(tp.x, tp.y, tp.z);
        const float dx = p.x - m_camera.pos.x, dz = p.z - m_camera.pos.z;
        if (id != m_triggerSel && dx * dx + dz * dz > 900.0f * 900.0f) continue;
        const XMFLOAT4 color = id == m_triggerSel ? XMFLOAT4{ 0.4f, 0.9f, 1, 1 } : XMFLOAT4{ 0.4f, 0.9f, 1, 0.6f };
        line(server(tp.x - 1.5f, tp.y, tp.z), server(tp.x + 1.5f, tp.y, tp.z), color);
        line(server(tp.x, tp.y - 1.5f, tp.z), server(tp.x, tp.y + 1.5f, tp.z), color);
        line(server(tp.x, tp.y, tp.z), server(tp.x, tp.y, tp.z + 3), color);
        line(server(tp.x, tp.y, tp.z + 0.2f), server(tp.x + std::cos(tp.o) * 4, tp.y + std::sin(tp.o) * 4, tp.z + 0.2f), color);
    }
    // The Entrance tab's corpse point, when it lies on this map.
    const uint32_t entrance = m_entranceMap == ~0u ? map : m_entranceMap;
    if (const nlohmann::json& row = m_mapRows.Row(entrance); !row.is_null() && row.value("CorpseMapID", kNoMap) == map)
    {
        const float x = row.value("Corpse[0]", 0.0f), y = row.value("Corpse[1]", 0.0f);
        const XMFLOAT3 e = server(x, y, 0);
        const float ground = m_terrain.HeightAt(e.x, e.z).value_or(0);
        const XMFLOAT4 color{ 1, 0.55f, 0.2f, 1 };
        line(server(x - 2, y - 2, ground), server(x + 2, y + 2, ground), color);
        line(server(x - 2, y + 2, ground), server(x + 2, y - 2, ground), color);
        line(server(x, y, ground), server(x, y, ground + 6), color);
    }
}

void App::DrawTriggerLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj) const
{
    const auto& teleports = TeleportsView();
    for (const Trigger& t : TriggersOnMap())
    {
        const XMFLOAT3 p = Top(t);
        const float dx = p.x - m_camera.pos.x, dy = p.y - m_camera.pos.y, dz = p.z - m_camera.pos.z;
        const bool selected = t.id == m_triggerSel, hovered = m_triggerHover == t.id;
        if (!selected && !hovered && dx * dx + dy * dy + dz * dz > 250.0f * 250.0f) continue;
        const auto at = ToScreen(viewProj, p, origin, size);
        if (!at) continue;
        std::string text = "#" + std::to_string(t.id);
        if (const auto tp = teleports.find(t.id); tp != teleports.end())
            text += " > " + MapLabel(tp->second.map) + (tp->second.name.empty() ? "" : "  " + tp->second.name);
        const ImVec2 s = ImGui::CalcTextSize(text.c_str());
        const ImU32 col = selected || hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 200, 255, 220);
        dl->AddText({ at->x - s.x / 2 + 1, at->y - s.y + 1 }, IM_COL32(0, 0, 0, 180), text.c_str());
        dl->AddText({ at->x - s.x / 2, at->y - s.y }, col, text.c_str());
    }
}

// ---------------------------------------------------------------------------------------------- panel

void App::DrawTriggersPanel(float w)
{
    const uint32_t map = CurrentMapId();
    const auto& triggers = TriggersOnMap();
    const auto& teleports = TeleportsView();
    const bool editable = m_project && m_db.Connected() && !m_terrain.Map().empty();
    auto flyTo = [&](uint32_t toMap, float x, float y, float z) { FlyTo(toMap, x, y, z); };
    auto mapCombo = [&](const char* label, uint32_t& value) {
        bool changed = false;
        if (ImGui::BeginCombo(label, MapLabel(value).c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (const auto& m : m_maps)
                if (ImGui::Selectable((m.name + "  " + std::to_string(m.id) + "  " + m.directory).c_str(), m.id == value)) { value = m.id; changed = true; }
            ImGui::EndCombo();
        }
        return changed;
    };

    if (Section(("Triggers (" + std::to_string(triggers.size()) + ")###triggers").c_str()))
    {
        if (!m_db.Connected()) ImGui::TextColored(kWarn, "Editing needs the world database (File > Server setup).");
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##triggerfilter", "Filter by id or destination", &m_triggerFilter);
        std::string filter = m_triggerFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (ImGui::BeginChild("##triggerlist", { w, 0 }, ImGuiChildFlags_Borders))
            for (const Trigger& t : triggers)
            {
                const auto tp = teleports.find(t.id);
                std::string label = "#" + std::to_string(t.id) + "  " + Shape(t);
                if (tp != teleports.end()) label += "  > " + MapLabel(tp->second.map) + (tp->second.name.empty() ? "" : "  " + tp->second.name);
                if (m_project && m_project->Owns("areatrigger.id", t.id)) label += "  (project)";
                std::string lower = label;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                ImGui::PushID(int(t.id));
                if (ImGui::Selectable(label.c_str(), t.id == m_triggerSel, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_triggerSel = t.id;
                    m_triggerTabId = t.id;   // a list click keeps the list in front (so a double-click reaches it and flies there)
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) flyTo(t.map, t.x, t.y, t.z);
                }
                ImGui::SetItemTooltip("Double-click: fly there");
                ImGui::PopID();
            }
        ImGui::EndChild();
    }

    if (Section("New"))
    {
        bool sphere = m_triggerShape.Sphere();
        if (ImGui::RadioButton("Sphere", sphere)) { m_triggerShape.radius = 5; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Box", !sphere)) { m_triggerShape.radius = 0; m_triggerShape.length = m_triggerShape.width = 10; m_triggerShape.height = 8; }
        ImGui::SetNextItemWidth(w - 90);
        if (sphere) ImGui::SliderFloat("Radius##new", &m_triggerShape.radius, 0.5f, 200.0f, "%.1f yd", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        else
        {
            float lwh[3] = { m_triggerShape.length, m_triggerShape.width, m_triggerShape.height };
            if (ImGui::SliderFloat3("L W H##new", lwh, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
                std::tie(m_triggerShape.length, m_triggerShape.width, m_triggerShape.height) = std::tuple(lwh[0], lwh[1], lwh[2]);
        }
        ImGui::Checkbox("Teleports players (pick the arrival next)", &m_triggerNewTeleport);
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button(m_triggerPick == TriggerPick::Place ? "Click the ground to place  (Esc)" : "Place on the ground", { w, 0 }))
            m_triggerPick = m_triggerPick == TriggerPick::Place ? TriggerPick::None : TriggerPick::Place;
        ImGui::EndDisabled();
        if (m_project)
        {
            const Project::IdRange r = m_project->Range("areatrigger.id");
            ImGui::TextColored(kQuiet, "Next id %u (project range %u-%u).", m_triggers.FreeId(r.first, r.last), r.first, r.last);
        }
        ImGui::TextColored(kQuiet, "The client fires a trigger from AreaTrigger.dbc (export and\nrestart it); worldserver reads `areatrigger` shapes at start.\n"
                                   "Teleports reload at once over SOAP.");
    }

    if (m_triggerSel && (m_triggerEditId != m_triggerSel || m_triggerEditRevision != m_store.Revision()))
    {
        m_triggerEditId = m_triggerSel;
        m_triggerEditRevision = m_store.Revision();
        m_triggerEdit = m_triggers.Find(m_triggerSel).value_or(Trigger{});
        const auto tp = teleports.find(m_triggerSel);
        m_teleportEdit = tp == teleports.end() ? std::nullopt : std::optional<Teleport>(tp->second);
    }
    const bool selectedNow = m_triggerSel && m_triggerSel != m_triggerTabId;
    m_triggerTabId = m_triggerSel;
    if (m_triggerSel && m_triggerEdit.id == m_triggerSel && Section("Selected", selectedNow))
    {
        // As objects: the handles and these fields show at once; each change is saved (one undo step) when let go.
        DrawTransformBar(w);
        Trigger& t = m_triggerEdit;
        ImGui::Text("Trigger #%u on %s", t.id, MapLabel(t.map).c_str());
        ImGui::BeginDisabled(!editable);
        float pos[3] = { t.x, t.y, t.z };
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::DragFloat3("Position", pos, 0.1f, 0, 0, "%.1f")) std::tie(t.x, t.y, t.z) = std::tuple(pos[0], pos[1], pos[2]);
        bool sphere = t.Sphere();
        if (ImGui::RadioButton("Sphere##sel", sphere) && !sphere) t.radius = std::max({ t.length, t.width, 2.0f }) / 2;
        ImGui::SameLine();
        if (ImGui::RadioButton("Box##sel", !sphere) && sphere) { t.length = t.width = t.height = t.radius * 2; t.radius = 0; }
        ImGui::SetNextItemWidth(w - 90);
        if (t.Sphere()) ImGui::SliderFloat("Radius", &t.radius, 0.5f, 200.0f, "%.1f yd", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        else
        {
            float lwh[3] = { t.length, t.width, t.height };
            if (ImGui::SliderFloat3("L W H", lwh, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) std::tie(t.length, t.width, t.height) = std::tuple(lwh[0], lwh[1], lwh[2]);
            ImGui::SetItemTooltip("Length runs along the arrow (the face with the cross), width across it, height up.");
            float deg = t.yaw * 360.0f / kTwoPi;
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::SliderFloat("Turn", &deg, 0, 360, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)) t.yaw = std::fmod(deg + 360.0f, 360.0f) * kTwoPi / 360.0f;
            ImGui::SetItemTooltip("Which way Length runs: the arrow in the viewport. The game fires from any side.");
        }

        ImGui::SeparatorText("Teleport");
        bool teleport = m_teleportEdit.has_value();
        if (ImGui::Checkbox("Sends players somewhere", &teleport))
        {
            if (teleport)
            {
                m_teleportEdit = Teleport{ t.id, t.map, "", t.x, t.y, t.z, 0 };
                m_triggerPick = TriggerPick::Target;   // next: where it sends them
            }
            else m_teleportEdit.reset();
        }
        if (m_teleportEdit)
        {
            Teleport& tp = *m_teleportEdit;
            ImGui::SetNextItemWidth(w - 90);
            ImGui::InputText("Name", &tp.name);
            ImGui::SetNextItemWidth(w - 90);
            mapCombo("To map", tp.map);
            float to[3] = { tp.x, tp.y, tp.z };
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::DragFloat3("Arrival", to, 0.1f, 0, 0, "%.1f")) std::tie(tp.x, tp.y, tp.z) = std::tuple(to[0], to[1], to[2]);
            float deg = tp.o * 360.0f / kTwoPi;
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::SliderFloat("Facing", &deg, 0, 360, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)) tp.o = std::fmod(deg + 360.0f, 360.0f) * kTwoPi / 360.0f;
            if (ImGui::Button(m_triggerPick == TriggerPick::Target ? "Click the arrival point  (Esc)" : "Pick the arrival on the ground", { (w - 8) / 2, 0 }))
                m_triggerPick = m_triggerPick == TriggerPick::Target ? TriggerPick::None : TriggerPick::Target;
            ImGui::SetItemTooltip("Players face where the camera looks.\nAnother map: open it in Maps, then click.");
            ImGui::SameLine();
            if (ImGui::Button("Go to arrival", { (w - 8) / 2, 0 })) flyTo(tp.map, tp.x, tp.y, tp.z);
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("Portal effect");
        ImGui::TextColored(kQuiet, "Catalog > Portal effects: click one to put it here.");

        ImGui::Separator();
        if (ImGui::Button("Go to trigger", { (w - 8) / 2, 0 })) flyTo(t.map, t.x, t.y, t.z);
        ImGui::SameLine();
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button("Delete  Del", { (w - 8) / 2, 0 })) CommitTrigger(t.id, std::nullopt, std::nullopt, "Delete trigger " + std::to_string(t.id));
        ImGui::EndDisabled();

        // Saved once nothing is being dragged or typed (undo puts it back).
        const auto saved = m_triggers.Find(t.id);
        const auto savedTp = teleports.find(t.id);
        const bool changed = !saved || t.ToDbcRow() != saved->ToDbcRow() || m_teleportEdit.has_value() != (savedTp != teleports.end()) ||
                             (m_teleportEdit && m_teleportEdit->ToRow() != savedTp->second.ToRow());
        if (changed && editable && !ImGui::IsAnyItemActive() && !m_gizmoActive) CommitTrigger(t.id, t, m_teleportEdit, "Edit trigger " + std::to_string(t.id));
    }

    if (Section("Entrance"))
    {
        uint32_t entrance = m_entranceMap == ~0u ? map : m_entranceMap;
        ImGui::SetNextItemWidth(w - 90);
        if (mapCombo("Map##entrance", entrance)) m_entranceMap = entrance == map ? ~0u : entrance;
        if (m_entranceMap != ~0u && ImGui::SmallButton("Follow the open map")) m_entranceMap = ~0u, entrance = map;
        const nlohmann::json& row = m_mapRows.Row(entrance);
        if (row.is_null()) { ImGui::TextColored(kQuiet, "No Map.dbc row."); return; }
        const uint32_t type = row.value("InstanceType", 0u);
        const bool instance = type == 1 || type == 2;
        const char* types[] = { "world", "dungeon", "raid", "battleground", "arena" };
        ImGui::TextColored(kQuiet, "Map type: %s", type < 5 ? types[type] : "other");

        ImGui::SeparatorText("Corpse entrance (Map.dbc)");
        const uint32_t corpseMap = row.value("CorpseMapID", kNoMap);
        if (corpseMap >= kNoMap) ImGui::TextColored(kQuiet, "None: dead players' spirits stay on this map.");
        else ImGui::Text("%s at %.1f, %.1f", MapLabel(corpseMap).c_str(), row.value("Corpse[0]", 0.0f), row.value("Corpse[1]", 0.0f));
        ImGui::TextColored(kQuiet, "Where a spirit appears to run back in; AzerothCore\nalso needs it to find a way out (below).");
        ImGui::BeginDisabled(!m_project || m_terrain.Map().empty());
        if (ImGui::Button(m_triggerPick == TriggerPick::Corpse ? "Click the ground  (Esc)" : "Pick on the ground", { (w - 8) / 2, 0 }))
            m_triggerPick = m_triggerPick == TriggerPick::Corpse ? TriggerPick::None : TriggerPick::Corpse;
        ImGui::SetItemTooltip("Open the map the entrance is on (usually a continent), then click.");
        ImGui::SameLine();
        ImGui::BeginDisabled(corpseMap >= kNoMap);
        if (ImGui::Button("Clear", { (w - 8) / 2, 0 }))
        {
            nlohmann::json after = row;
            after["CorpseMapID"] = 0xFFFFFFFFu;
            after["Corpse[0]"] = after["Corpse[1]"] = 0.0f;
            m_mapRows.Commit(entrance, after, "Clear corpse entrance of " + MapLabel(entrance));
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // The instance's template: the map its exit trigger leads to.
        const std::string key = std::to_string(entrance) + "/" + std::to_string(m_store.Revision()) + "/" + (m_db.Connected() ? "db" : "-");
        if (key != m_entranceKey)
        {
            m_entranceKey = key;
            m_entranceInstance = instance ? m_instances.Rows(entrance) : std::vector<nlohmann::json>{};
        }
        uint32_t exitMap = corpseMap;
        if (instance)
        {
            ImGui::SeparatorText("Instance (instance_template)");
            if (!m_db.Connected()) ImGui::TextColored(kWarn, "Needs the world database.");
            else
            {
                const auto& rows = m_entranceInstance;
                uint32_t parent = rows.empty() ? 0 : uint32_t(std::stoul(rows.front().value("parent", std::string("0"))));
                exitMap = rows.empty() ? kNoMap : parent;
                if (rows.empty()) ImGui::TextColored(kWarn, "No row: AzerothCore cannot create this instance.");
                ImGui::SetNextItemWidth(w - 90);
                if (mapCombo("Parent", parent))
                {
                    nlohmann::json after = rows.empty() ? nlohmann::json{ { "map", std::to_string(entrance) }, { "script", "" }, { "allowMount", "0" } } : rows.front();
                    after["parent"] = std::to_string(parent);
                    Change c = m_instances.MakeChange(entrance, rows, { after }, "Instance parent of " + MapLabel(entrance));
                    m_instances.Apply(c);
                    m_store.Commit(std::move(c));
                    Log("instance_template of %s: parent %s (worldserver reads it at start).", MapLabel(entrance).c_str(), MapLabel(parent).c_str());
                }
                ImGui::SetItemTooltip("The map players leave to: the exit is the trigger inside this instance that\nteleports to this map.");
            }
        }

        // Ways in and the way out, as AzerothCore finds them (ObjectMgr::GetMapEntranceTrigger / GetGoBackTrigger).
        ImGui::SeparatorText("Ways in");
        size_t in = 0;
        for (const auto& [id, tp] : teleports)
            if (tp.map == entrance)
            {
                const auto t = m_triggers.Find(id);
                ImGui::PushID(int(id));
                if (ImGui::Selectable(("#" + std::to_string(id) + " from " + (t ? MapLabel(t->map) : "?") + (tp.name.empty() ? "" : "  " + tp.name)).c_str(),
                                      id == m_triggerSel) && t)
                {
                    m_triggerSel = id;
                    flyTo(t->map, t->x, t->y, t->z);
                }
                ImGui::PopID();
                ++in;
            }
        if (!in) ImGui::TextColored(kQuiet, "No trigger teleports here.");
        else ImGui::TextColored(kQuiet, "Dungeon finder and logins without a saved\ninstance arrive at one of these.");
        ImGui::SeparatorText("Way out");
        if (corpseMap >= kNoMap) ImGui::TextColored(instance ? kWarn : kQuiet, "None: without a corpse entrance AzerothCore\nfinds no exit trigger.");
        else
        {
            bool found = false;
            for (const auto& [id, tp] : teleports)
                if (const auto t = m_triggers.Find(id); t && t->map == entrance && tp.map == exitMap)
                {
                    if (ImGui::Selectable(("#" + std::to_string(id) + " to " + MapLabel(tp.map)).c_str(), id == m_triggerSel))
                    {
                        m_triggerSel = id;
                        flyTo(t->map, t->x, t->y, t->z);
                    }
                    found = true;
                    break;
                }
            if (!found)
                ImGui::TextColored(instance ? kWarn : kQuiet, "No trigger on this map teleports to %s.", exitMap >= kNoMap ? "its parent" : MapLabel(exitMap).c_str());
        }
    }
}

// ---------------------------------------------------------------------------------------------- portal effects

void App::PlacePortalEffect(const std::string& model)
{
    const auto t = m_triggerSel ? m_triggers.Find(m_triggerSel) : std::nullopt;
    if (!t) { Log("Select a trigger first (Triggers tool, K)."); return; }
    if (t->map != CurrentMapId()) { Log("Open the trigger's map to place a portal at it."); return; }
    // At the trigger's own position, not the terrain's: most entrances are in caves. Facing the camera: the model's
    // +x (RotationY(rot[1] - 90) of PlacementMatrix) points at it.
    const XMFLOAT3 c = Centre(*t);
    const float yaw = XMConvertToDegrees(std::atan2(-(m_camera.pos.z - c.z), m_camera.pos.x - c.x)) + 90.0f;
    std::vector<ObjectRef> placed;
    if (auto change = m_terrain.PlaceObjects({ { model, { c.x, c.y, c.z }, { 0, yaw, 0 }, m_portalScale, 0 } }, {},
                                             "Portal effect at trigger " + std::to_string(t->id), &placed))
    {
        Log("%s (Objects tool O moves, turns and scales it).", change->label.c_str());
        m_store.Commit(std::move(*change));
        m_objSel = std::set<ObjectRef>(placed.begin(), placed.end());
    }
    else Log("The trigger's tile is not loaded: fly closer first.");
}

void App::DrawPortalCatalog()
{
    // Blizzard's instance portals are plain doodads (client only, no server rows) named InstancePortal* (the
    // INSTANCEPORTAL folder, raid ones in SPELLS\) or InstanceNewPortal* (SPELLS\), without the invisible collision model.
    if (const size_t key = m_catalog.Count(Catalog::Kind::Doodad); key != m_portalModelsKey)
    {
        m_portalModelsKey = key;
        m_portalModels.clear();
        for (const Catalog::Item* item : m_catalog.Filter(Catalog::Kind::Doodad, "", ""))
        {
            const std::string file = item->lower.substr(item->lower.find_last_of("\\/") + 1);
            if ((file.starts_with("instanceportal") || file.starts_with("instancenewportal")) && file.find("collision") == std::string::npos)
                m_portalModels.push_back(item->path);
        }
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("##portalscale", &m_portalScale, 0.1f, 10.0f, "scale %.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##size", &m_thumbSize, 48, 192, "size %.0f");
    ImGui::SameLine(0, 20);
    if (m_triggerSel) ImGui::Text("Click one to put it at trigger #%u, facing the camera.", m_triggerSel);
    else ImGui::TextColored(kWarn, "Select a trigger first (Triggers tool).");

    if (!ImGui::BeginChild("##portals", { 0, 0 }, ImGuiChildFlags_Borders)) { ImGui::EndChild(); return; }
    if (m_portalModels.empty()) ImGui::TextColored(kQuiet, "This client has no instance portal models.");
    const ImGuiStyle& style = ImGui::GetStyle();
    const float thumb = m_thumbSize, cellW = thumb + style.FramePadding.x * 2 + style.ItemSpacing.x;
    const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cellW));
    for (size_t i = 0; i < m_portalModels.size(); ++i)
    {
        const std::string& path = m_portalModels[i];
        ImGui::PushID(int(i));
        ImGui::BeginGroup();
        ID3D11ShaderResourceView* tex = m_models.Thumbnail(path, false, m_mpq, true);
        const bool clicked = tex ? ImGui::ImageButton("##thumb", ImTextureID(intptr_t(tex)), { thumb, thumb })
                                 : ImGui::Button(m_models.ThumbnailFailed(path) ? "no picture" : "...",
                                                 { thumb + style.FramePadding.x * 2, thumb + style.FramePadding.y * 2 });
        ImGui::SetItemTooltip("%s\nMostly particles: the picture may be empty; the client shows the effect.", path.c_str());
        if (clicked) PlacePortalEffect(path);
        std::string name = path.substr(path.find_last_of('\\') + 1);
        while (name.size() > 4 && ImGui::CalcTextSize(name.c_str()).x > thumb + style.FramePadding.x * 2) name.pop_back();
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        if ((i + 1) % size_t(cols)) ImGui::SameLine();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------- checks

void App::CheckTriggers(std::vector<Problem>& problems) const
{
    if (const Project::IdRange r = m_project->Range("areatrigger.id"); r.first && r.last >= r.first)
        m_triggers.CheckIds(r.first, r.last, "Triggers", problems);
    else
        problems.push_back({ Problem::Severity::Error, "IDs", "No areatrigger.id range set (File > Project settings)." });

    // Only what the project touched: Blizzard's own data is not this project's problem.
    std::set<uint32_t> ids;
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain == m_triggers.Domain()) ids.insert(data.at("id").get<uint32_t>());
        else if (domain == m_teleports.Domain()) ids.insert(data.at("key").get<uint32_t>());
    });
    const auto& teleports = TeleportsView();
    auto located = [&](Problem::Severity severity, const std::string& message, const Trigger* at) {
        Problem p{ severity, "Triggers", message };
        if (at)
            for (const auto& m : m_maps)
                if (m.id == at->map)
                {
                    const XMFLOAT3 e = Centre(*at);
                    p.map = m.directory;
                    p.tx = int(e.x / kTileSize);
                    p.ty = int(e.z / kTileSize);
                }
        problems.push_back(std::move(p));
    };
    std::set<uint32_t> targets;
    for (const uint32_t id : ids)
    {
        const auto t = m_triggers.Find(id);
        const auto tp = teleports.find(id);
        if (tp == teleports.end()) continue;
        const std::string name = "Trigger " + std::to_string(id);
        if (!t) { located(Problem::Severity::Error, name + " teleports but has no AreaTrigger.dbc row: the client never fires it.", nullptr); continue; }
        if (std::none_of(m_maps.begin(), m_maps.end(), [&](const MapEntry& m) { return m.id == tp->second.map; }))
        {
            located(Problem::Severity::Error, name + " teleports to map " + std::to_string(tp->second.map) + ", which Map.dbc lacks.", &*t);
            continue;
        }
        for (const Trigger& other : m_triggers.OnMap(tp->second.map))
            if (other.id != id && teleports.count(other.id) && other.Contains(tp->second.x, tp->second.y, tp->second.z))
                located(Problem::Severity::Warning, name + " sends players into trigger " + std::to_string(other.id) + ", which teleports them on.", &*t);
        targets.insert(tp->second.map);
    }
    // Instances the project leads into need a way back out.
    for (const uint32_t map : targets)
    {
        const nlohmann::json& row = m_mapRows.Row(map);
        const uint32_t type = row.is_null() ? 0 : row.value("InstanceType", 0u);
        if (type != 1 && type != 2) continue;
        if (row.value("CorpseMapID", kNoMap) >= kNoMap)
        {
            located(Problem::Severity::Warning, MapLabel(map) + " has no corpse entrance (Map.dbc): AzerothCore finds no exit trigger for it.", nullptr);
            continue;
        }
        if (!m_db.Connected()) continue;
        const auto rows = m_instances.Rows(map);
        if (rows.empty()) { located(Problem::Severity::Error, MapLabel(map) + " has no instance_template row: AzerothCore cannot create it.", nullptr); continue; }
        const uint32_t parent = uint32_t(std::stoul(rows.front().value("parent", std::string("0"))));
        const bool exit = std::any_of(teleports.begin(), teleports.end(), [&](const auto& kv) {
            const auto t = m_triggers.Find(kv.first);
            return t && t->map == map && kv.second.map == parent;
        });
        if (!exit)
            located(Problem::Severity::Warning, MapLabel(map) + ": no trigger inside teleports to its parent " + MapLabel(parent) + " (the way out).", nullptr);
    }
}
