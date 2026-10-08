// Points of interest: world map landmarks (AreaPOI.dbc), gossip map flags (points_of_interest), .tele bookmarks (game_tele).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
constexpr float kTwoPi = 6.2831853f;
constexpr int kIconColumns = 14;           // WorldMapFrame.lua NUM_WORLDMAP_POI_COLUMNS: 18 px cells on a 256 px texture
constexpr float kIconCell = 18.0f / 256;
const char* const kIconTexture = "Interface\\Minimap\\POIIcons.blp";

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

/// Texture coordinates of a POIIcons cell (WorldMap_GetPOITextureCoords).
std::pair<ImVec2, ImVec2> IconUv(uint32_t icon)
{
    const float x = float(icon % kIconColumns) * kIconCell, y = float(icon / kIconColumns) * kIconCell, pad = 1.0f / 256;
    return { { x + pad, y + pad }, { x + kIconCell - pad, y + kIconCell - pad } };
}

/// Server orientation of where the camera looks (0 = north, counter-clockwise).
float Facing(const XMFLOAT3& forward)
{
    const float o = std::atan2(-forward.x, -forward.z);
    return o < 0 ? o + kTwoPi : o;
}

const char* KindName(PoiKind k) { return k == PoiKind::MapIcon ? "landmark" : k == PoiKind::Gossip ? "gossip point" : "teleport"; }
const char* IdKind(PoiKind k) { return k == PoiKind::MapIcon ? "areapoi.id" : k == PoiKind::Gossip ? "points_of_interest.id" : "game_tele.id"; }

/// The point as its row: for comparing edits.
nlohmann::json RowOf(const Poi& p)
{
    return p.kind == PoiKind::MapIcon ? p.ToDbcRow(nlohmann::json::object()) : p.kind == PoiKind::Gossip ? p.ToGossipRow() : p.ToTeleRow();
}

/// Fields of a new point, as Blizzard's usually have them.
Poi NewPoi(PoiKind kind)
{
    Poi p;
    p.kind = kind;
    if (kind == PoiKind::MapIcon) { p.name = "New landmark"; p.icon = 7; p.importance = 3; p.flags = 517; }
    else if (kind == PoiKind::Gossip) { p.name = "New point"; p.icon = 7; p.flags = 99; }
    else p.name = "NewPlace";
    return p;
}

/// AreaPOI Importance / Flags / Icon of Blizzard's landmarks (what each Flags bit does is not documented).
struct Preset { const char* name; uint32_t importance, flags, icon; };
const Preset kPresets[] = { { "Village (Goldshire)", 3, 517, 7 }, { "Town (Darkshire)", 3, 525, 5 }, { "Capital (Stormwind City)", 3, 541, 6 } };
}

// ---------------------------------------------------------------------------------------------- data

const std::vector<Poi>& App::PoisOnMap() const
{
    const uint32_t map = CurrentMapId();
    const std::string key = std::to_string(int(m_poiKind)) + "/" + std::to_string(map) + "/" + std::to_string(m_store.Revision()) + "/" +
                            std::to_string(m_areaPois.Version()) + "/" + (m_db.Connected() ? "db" : "-") + "/" + m_terrain.Map();
    if (key == m_poiViewKey) return m_poiView;
    m_poiViewKey = key;
    m_poiView.clear();
    if (m_terrain.Map().empty()) return m_poiView;
    if (m_poiKind == PoiKind::MapIcon)
    {
        for (const auto& [id, row] : m_areaPois.Rows())
            if (row.value("ContinentID", 0u) == map) m_poiView.push_back(Poi::FromDbcRow(row));
    }
    else
        for (const auto& [id, rows] : (m_poiKind == PoiKind::Gossip ? m_gossipPois : m_teles).All())
        {
            Poi p = m_poiKind == PoiKind::Gossip ? Poi::FromGossipRow(rows.front()) : Poi::FromTeleRow(rows.front());
            if (p.kind == PoiKind::Gossip || p.map == map) m_poiView.push_back(std::move(p));
        }
    return m_poiView;
}

std::optional<XMFLOAT3> App::PoiPoint(const Poi& p) const
{
    XMFLOAT3 e = ServerToEditor(p.x, p.y, p.z);
    if (p.kind == PoiKind::Gossip || (p.kind == PoiKind::MapIcon && p.z == 0))   // no height of its own: stands on the ground
    {
        const auto h = m_terrain.HeightAt(e.x, e.z);
        if (!h) return std::nullopt;
        e.y = *h;
    }
    return e;
}

bool App::PoiIcon(uint32_t icon, float size)
{
    ID3D11ShaderResourceView* tex = m_renderer.TextureFor(kIconTexture, m_mpq);
    if (!tex) return false;
    const auto [uv0, uv1] = IconUv(icon);
    ImGui::Image(ImTextureID(intptr_t(tex)), { size, size }, uv0, uv1);
    return true;
}

void App::CommitPoi(PoiKind kind, uint32_t id, const std::optional<Poi>& after, const std::string& label)
{
    if (kind == PoiKind::MapIcon)
    {
        const nlohmann::json before = m_areaPois.Row(id);
        const nlohmann::json row = after ? after->ToDbcRow(before.is_null() ? m_areaPois.NewRow() : before) : nlohmann::json();
        if (row == before) return;
        m_areaPois.Commit(id, row, label);
        Log("%s: export and restart the client (AreaPOI.dbc).", label.c_str());
    }
    else
    {
        if (!m_db.Connected())
        {
            Log("%ss are world database rows: connect it first (File > Server setup).", KindName(kind));
            return;
        }
        TableRowsAdapter& table = kind == PoiKind::Gossip ? m_gossipPois : m_teles;
        const std::vector<nlohmann::json> before = table.Rows(id);
        std::vector<nlohmann::json> rows;
        if (after) rows.push_back(kind == PoiKind::Gossip ? after->ToGossipRow() : after->ToTeleRow());
        if (rows == before) return;
        Change c = table.MakeChange(id, before, rows, label);
        table.Apply(c);
        if (!table.LastError().empty()) Log("%s", table.LastError().c_str());
        m_store.Commit(std::move(c));
        if (RunServerCommand(kind == PoiKind::Gossip ? ".reload points_of_interest" : ".reload game_tele")) Log("%s: reloaded on the server.", label.c_str());
    }
    if (!after && m_poiSel == id) m_poiSel = 0;
}

void App::FlyToPoi(const Poi& p)
{
    FlyTo(p.kind == PoiKind::Gossip ? CurrentMapId() : p.map, p.x, p.y, p.z, p.kind == PoiKind::Gossip || (p.kind == PoiKind::MapIcon && p.z == 0));
    Log("Camera to %s %u %s (%.0f, %.0f).", KindName(p.kind), p.id, p.name.c_str(), p.x, p.y);
}

// ---------------------------------------------------------------------------------------------- from other versions

nlohmann::json App::VersionPois(const MpqChain& chain, const std::string& mapDir, const std::set<std::pair<int, int>>& cells, int originX, int originZ) const
{
    nlohmann::json out = nlohmann::json::array();
    if (&chain == &m_mpq && _stricmp(mapDir.c_str(), m_terrain.Map().c_str()) == 0) return out;   // the open map: its landmarks are there
    const auto mapDbc = chain.Read("DBFilesClient\\Map.dbc");
    if (!mapDbc) return out;
    std::optional<uint32_t> map;
    for (const MapEntry& m : ParseMapDbc(*mapDbc))   // ID and Directory lead the row in every build
        if (_stricmp(m.directory.c_str(), mapDir.c_str()) == 0) map = m.id;
    if (!map) return out;
    for (const Poi& p : ReadAreaPois(chain, *map))
    {
        const XMFLOAT3 e = ServerToEditor(p.x, p.y, p.z);
        if (!cells.count({ int(std::floor(e.x / kChunkSize)), int(std::floor(e.z / kChunkSize)) })) continue;
        out.push_back({ { "pos", { e.x - originX * kChunkSize, e.y, e.z - originZ * kChunkSize } }, { "ground", p.z == 0 },
                        { "row", p.ToDbcRow(nlohmann::json::object()) } });
    }
    return out;
}

std::vector<Change> App::AddPastedPois(const nlohmann::json& pois, const std::string& label)
{
    std::vector<Change> parts;
    if (pois.empty() || !m_project) return parts;
    const uint32_t map = CurrentMapId();
    const Project::IdRange r = m_project->Range("areapoi.id");
    const auto existing = m_areaPois.Rows();
    size_t there = 0;
    for (const auto& item : pois)
    {
        Poi p = Poi::FromDbcRow(item.at("row"));
        const XMFLOAT3 e{ item.at("pos")[0], item.at("pos")[1], item.at("pos")[2] };
        EditorToServer(e, p.x, p.y, p.z);
        if (item.value("ground", false)) p.z = 0;
        // The same town in both versions: a landmark of that name close by.
        const bool dup = std::any_of(existing.begin(), existing.end(), [&](const auto& kv) {
            const nlohmann::json& row = kv.second;
            return row.value("ContinentID", 0u) == map && _stricmp(row.value("Name_lang", std::string()).c_str(), p.name.c_str()) == 0 &&
                   std::hypot(row.value("Pos[0]", 0.0f) - p.x, row.value("Pos[1]", 0.0f) - p.y) < 150.0f;
        });
        if (dup) { ++there; continue; }
        p.id = m_areaPois.FreeId(r.first, r.last);
        if (!p.id) { Log("No free areapoi.id in the project's range %u-%u: landmarks not added (File > Project settings).", r.first, r.last); break; }
        p.map = map;
        p.area = 0;   // the area of the chunk it lands on
        if (const auto ref = m_terrain.ChunkAtGrid(int(std::floor(e.x / kChunkSize)), int(std::floor(e.z / kChunkSize))))
            if (const AdtChunk* c = m_terrain.Chunk(*ref)) p.area = c->areaId;
        Change c = m_areaPois.MakeChange(p.id, nullptr, p.ToDbcRow(m_areaPois.NewRow()), label);
        m_areaPois.Apply(c);
        parts.push_back(std::move(c));
    }
    if (!parts.empty() || there)
        Log("Landmarks: %zu added from the other version (export and restart the client to see them), %zu already on this map.", parts.size(), there);
    return parts;
}

// ---------------------------------------------------------------------------------------------- viewport

void App::PoisViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    const ImGuiIO& io = ImGui::GetIO();
    m_poiHover.reset();
    if (ImGui::IsItemHovered())
    {
        float best = 14.0f;   // pixels from the marker's top
        for (const Poi& p : PoisOnMap())
            if (auto at = PoiPoint(p))
            {
                at->y += 4;
                if (const auto s = ToScreen(viewProj, *at, origin, size))
                    if (const float d = std::hypot(s->x - io.MousePos.x, s->y - io.MousePos.y); d < best) { best = d; m_poiHover = p.id; }
            }
    }
    if (!ImGui::IsItemActivated() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    const PoiPick pick = m_poiPick == PoiPick::None && io.KeyAlt && m_poiSel ? PoiPick::Move : m_poiPick;
    if (pick == PoiPick::None)
    {
        m_poiSel = m_poiHover.value_or(0);
        m_poiShowSelected = m_poiSel != 0;
        if (m_poiSel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))   // double-click a marker: fly to it
            for (const Poi& p : PoisOnMap())
                if (p.id == m_poiSel) FlyToPoi(p);
        return;
    }
    if (!m_hover) return;
    if (pick != PoiPick::Place || !io.KeyShift) m_poiPick = PoiPick::None;
    Poi p;
    if (pick == PoiPick::Place)
    {
        const Project::IdRange r = m_project->Range(IdKind(m_poiKind));
        uint32_t id = 0;
        if (m_poiKind == PoiKind::MapIcon) id = m_areaPois.FreeId(r.first, r.last);
        else
        {
            const auto all = (m_poiKind == PoiKind::Gossip ? m_gossipPois : m_teles).All();
            for (uint32_t i = std::max(r.first, 1u); i && i <= r.last && !id; ++i)
                if (!all.count(i)) id = i;
        }
        if (!id) { Log("No free %s in the project's range %u-%u (File > Project settings).", IdKind(m_poiKind), r.first, r.last); return; }
        p = m_poiNew;
        p.kind = m_poiKind;
        p.id = id;
    }
    else if (m_poiEditKey == std::pair{ int(m_poiKind), m_poiSel }) p = m_poiEdit;   // with any fields typed but not applied
    else return;
    p.map = CurrentMapId();
    EditorToServer(m_hover->pos, p.x, p.y, p.z);
    if (p.kind == PoiKind::MapIcon)
        if (const AdtChunk* c = m_terrain.Chunk(m_hover->chunk)) p.area = c->areaId;
    if (p.kind == PoiKind::Tele)
    {
        XMFLOAT3 forward;
        XMStoreFloat3(&forward, m_camera.Heading());
        p.o = Facing(forward);   // players arrive looking where the camera looks
    }
    CommitPoi(p.kind, p.id, p, std::string(pick == PoiPick::Place ? "Add " : "Move ") + KindName(p.kind) + " " + std::to_string(p.id));
    m_poiSel = p.id;
    m_poiShowSelected = true;
}

void App::BuildPoiOverlay(std::vector<LineVertex>& lines) const
{
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& c) { lines.push_back({ a, c }); lines.push_back({ b, c }); };
    for (const Poi& saved : PoisOnMap())
    {
        // The selected one as the panel has it, so a move shows before Apply.
        const Poi& p = saved.id == m_poiSel && m_poiEditKey == std::pair{ int(saved.kind), saved.id } ? m_poiEdit : saved;
        const auto at = PoiPoint(p);
        if (!at) continue;
        const XMFLOAT3 g = *at;
        const float dx = g.x - m_camera.pos.x, dz = g.z - m_camera.pos.z;
        const bool selected = p.id == m_poiSel, hovered = m_poiHover == p.id;
        if (!selected && dx * dx + dz * dz > 900.0f * 900.0f) continue;
        XMFLOAT4 c = p.kind == PoiKind::MapIcon ? XMFLOAT4{ 1, 0.8f, 0.25f, 0.8f } : p.kind == PoiKind::Gossip ? XMFLOAT4{ 1, 0.4f, 0.35f, 0.8f } : XMFLOAT4{ 0.4f, 0.9f, 1, 0.8f };
        if (selected) c = { 1, 1, 1, 1 };
        else if (hovered) c.w = 1;
        const XMFLOAT3 top{ g.x, g.y + 4, g.z };
        line(g, top, c);
        if (p.kind == PoiKind::MapIcon)   // a square on the pole
        {
            const XMFLOAT3 k[4] = { { g.x - 0.8f, top.y, g.z }, { g.x, top.y + 0.8f, g.z }, { g.x + 0.8f, top.y, g.z }, { g.x, top.y - 0.8f, g.z } };
            for (int i = 0; i < 4; ++i) line(k[i], k[(i + 1) % 4], c);
        }
        else if (p.kind == PoiKind::Gossip)   // a flag
        {
            line(top, { g.x + 1.6f, top.y - 0.6f, g.z }, c);
            line({ g.x + 1.6f, top.y - 0.6f, g.z }, { g.x, top.y - 1.2f, g.z }, c);
        }
        else   // a cross on the ground and the way players face (server angle from +x towards +y)
        {
            line({ g.x - 1.5f, g.y + 0.1f, g.z }, { g.x + 1.5f, g.y + 0.1f, g.z }, c);
            line({ g.x, g.y + 0.1f, g.z - 1.5f }, { g.x, g.y + 0.1f, g.z + 1.5f }, c);
            line({ g.x, g.y + 0.2f, g.z }, { g.x - std::sin(p.o) * 4, g.y + 0.2f, g.z - std::cos(p.o) * 4 }, c);
        }
        if (selected || hovered)   // a ring on the ground
            for (int i = 0; i < 16; ++i)
            {
                const float a0 = kTwoPi * i / 16, a1 = kTwoPi * (i + 1) / 16;
                line({ g.x + std::cos(a0) * 2, g.y + 0.15f, g.z + std::sin(a0) * 2 }, { g.x + std::cos(a1) * 2, g.y + 0.15f, g.z + std::sin(a1) * 2 }, c);
            }
    }
}

void App::DrawPoiLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    ID3D11ShaderResourceView* tex = m_poiKind == PoiKind::Tele ? nullptr : m_renderer.TextureFor(kIconTexture, m_mpq);
    for (const Poi& p : PoisOnMap())
    {
        auto at = PoiPoint(p);
        if (!at) continue;
        const float dx = at->x - m_camera.pos.x, dy = at->y - m_camera.pos.y, dz = at->z - m_camera.pos.z;
        const bool selected = p.id == m_poiSel, hovered = m_poiHover == p.id;
        if (!selected && !hovered && dx * dx + dy * dy + dz * dz > 400.0f * 400.0f) continue;
        at->y += 5.5f;
        const auto s = ToScreen(viewProj, *at, origin, size);
        if (!s) continue;
        const std::string text = p.name.empty() ? "#" + std::to_string(p.id) : p.name;
        const ImVec2 t = ImGui::CalcTextSize(text.c_str());
        const float icon = tex ? 18.0f : 0.0f, left = s->x - (t.x + icon) / 2;
        if (tex)
        {
            const auto [uv0, uv1] = IconUv(p.icon);
            dl->AddImage(ImTextureID(intptr_t(tex)), { left, s->y - icon }, { left + icon, s->y }, uv0, uv1);
        }
        const ImU32 col = selected || hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 235, 190, 230);
        dl->AddText({ left + icon + 1, s->y - t.y + 1 }, IM_COL32(0, 0, 0, 180), text.c_str());
        dl->AddText({ left + icon, s->y - t.y }, col, text.c_str());
    }
}

// ---------------------------------------------------------------------------------------------- panel

void App::DrawPoisPanel(float w)
{
    const bool db = m_db.Connected(), table = m_poiKind != PoiKind::MapIcon;
    const bool editable = m_project && !m_terrain.Map().empty() && (!table || db);
    const auto& points = PoisOnMap();

    // Fields shared by New and Selected.
    auto fields = [&](Poi& p) {
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputText("Name", &p.name);
        if (p.kind == PoiKind::Tele) ImGui::SetItemTooltip("`.tele <name>` goes here: keep it one word.");
        if (p.kind == PoiKind::MapIcon)
        {
            ImGui::SetNextItemWidth(w - 90);
            ImGui::InputText("Description", &p.description);
            ImGui::SetItemTooltip("The second line of the world map tooltip (most of Blizzard's are empty).");
        }
        if (p.kind == PoiKind::Tele) return;
        // Icon: the current one opens a grid of every POIIcons cell.
        ImGui::PushID("icon");
        ID3D11ShaderResourceView* tex = m_renderer.TextureFor(kIconTexture, m_mpq);
        const auto [uv0, uv1] = IconUv(p.icon);
        if (tex ? ImGui::ImageButton("##icon", ImTextureID(intptr_t(tex)), { 20, 20 }, uv0, uv1) : ImGui::Button(std::to_string(p.icon).c_str()))
            ImGui::OpenPopup("icons");
        ImGui::SameLine();
        ImGui::Text("Icon %u", p.icon);
        if (ImGui::BeginPopup("icons"))
        {
            for (uint32_t i = 0; i < kIconColumns * kIconColumns; ++i)
            {
                ImGui::PushID(int(i));
                const auto [a, b] = IconUv(i);
                if (tex && ImGui::ImageButton("##cell", ImTextureID(intptr_t(tex)), { 20, 20 }, a, b)) { p.icon = i; ImGui::CloseCurrentPopup(); }
                ImGui::SetItemTooltip("%u", i);
                ImGui::PopID();
                if ((i + 1) % kIconColumns) ImGui::SameLine();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (p.kind == PoiKind::MapIcon)
        {
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::BeginCombo("Like", "Blizzard's ..."))
            {
                for (const Preset& preset : kPresets)
                    if (ImGui::Selectable(preset.name)) { p.importance = preset.importance; p.flags = preset.flags; p.icon = preset.icon; }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Importance, flags and icon of a Blizzard landmark of that size.");
        }
        int values[2] = { int(p.importance), int(p.flags) };
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::InputInt2("Importance, flags", values)) { p.importance = uint32_t(std::max(values[0], 0)); p.flags = uint32_t(std::max(values[1], 0)); }
        ImGui::SetItemTooltip(p.kind == PoiKind::MapIcon ? "Blizzard's flags: 517 villages, 525 towns, 541 capitals." : "Blizzard's gossip points: flags 99, importance 0.");
        if (p.kind == PoiKind::MapIcon)
        {
            int state = int(p.worldState);
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::InputInt("World state", &state)) p.worldState = uint32_t(std::max(state, 0));
            ImGui::SetItemTooltip("0 for a plain landmark. Battleground and Wintergrasp points follow a world state.");
        }
    };

    if (Section(("Points (" + std::to_string(points.size()) + ")###points").c_str()))
    {
        const PoiKind before = m_poiKind;
        int kind = int(m_poiKind);
        ImGui::RadioButton("Landmarks", &kind, int(PoiKind::MapIcon));
        ImGui::SameLine();
        ImGui::RadioButton("Gossip", &kind, int(PoiKind::Gossip));
        ImGui::SameLine();
        ImGui::RadioButton("Teleports", &kind, int(PoiKind::Tele));
        m_poiKind = PoiKind(kind);
        if (m_poiKind != before) { m_poiSel = 0; m_poiPick = PoiPick::None; }
        ImGui::TextColored(kQuiet, "%s", m_poiKind == PoiKind::MapIcon ? "AreaPOI.dbc: icons on the world map (client only)."
                                       : m_poiKind == PoiKind::Gossip ? "points_of_interest: the flag a gossip option marks;\nno map: shown wherever the ground is loaded."
                                                                      : "game_tele: `.tele <name>` destinations.");
        if (table && !db) ImGui::TextColored(kWarn, "Needs the world database (File > Server setup).");
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##poifilter", "Filter by id or name", &m_poiFilter);
        std::string filter = m_poiFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (ImGui::BeginChild("##poilist", { w, 0 }, ImGuiChildFlags_Borders))
            for (const Poi& p : points)
            {
                std::string label = "#" + std::to_string(p.id) + "  " + p.name;
                if (m_project && m_project->Owns(IdKind(p.kind), p.id)) label += "  (project)";
                std::string lower = label;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                ImGui::PushID(int(p.id));
                if (ImGui::Selectable(label.c_str(), p.id == m_poiSel, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_poiSel = p.id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) FlyToPoi(p);
                }
                ImGui::SetItemTooltip("Double-click: fly there");
                ImGui::PopID();
            }
        ImGui::EndChild();
    }

    if (Section("New"))
    {
        if (m_poiNew.kind != m_poiKind) m_poiNew = NewPoi(m_poiKind);
        fields(m_poiNew);
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button(m_poiPick == PoiPick::Place ? "Click the ground to place  (Esc)" : "Place on the ground", { w, 0 }))
            m_poiPick = m_poiPick == PoiPick::Place ? PoiPick::None : PoiPick::Place;
        ImGui::EndDisabled();
        if (m_project)
        {
            const Project::IdRange r = m_project->Range(IdKind(m_poiKind));
            ImGui::TextColored(kQuiet, "Ids from the project range %u-%u.", r.first, r.last);
        }
        ImGui::TextColored(kQuiet, "%s", m_poiKind == PoiKind::MapIcon ? "Export, then restart the client to see it on the world map."
                                       : m_poiKind == PoiKind::Gossip ? "Saved and reloaded on the server at once. A gossip option\n(gossip_menu_option.ActionPoiID) shows it."
                                                                      : "Saved and reloaded on the server at once.\nTeleports face where the camera looks.");
    }

    const std::pair<int, uint32_t> key{ int(m_poiKind), m_poiSel };
    const bool fresh = std::exchange(m_poiShowSelected, false);
    if (m_poiSel && (m_poiEditKey != key || m_poiEditRevision != m_store.Revision()))
    {
        const auto it = std::find_if(points.begin(), points.end(), [&](const Poi& p) { return p.id == m_poiSel; });
        if (it == points.end()) m_poiSel = 0;
        else
        {

            m_poiEditKey = key;
            m_poiEditRevision = m_store.Revision();
            m_poiEdit = *it;
        }
    }
    if (m_poiSel && m_poiEditKey == key && Section("Selected", fresh))
    {
        // As objects: the handles and these fields show at once; each change is saved (one undo step) when let go.
        DrawTransformBar(w);
        Poi& p = m_poiEdit;
        ImGui::Text("%s #%u%s", KindName(p.kind), p.id, p.kind == PoiKind::Gossip ? "" : (" on " + MapLabel(p.map)).c_str());
        ImGui::BeginDisabled(!editable);
        fields(p);
        float pos[3] = { p.x, p.y, p.z };
        ImGui::SetNextItemWidth(w - 90);
        if (p.kind == PoiKind::Gossip ? ImGui::DragFloat2("Position", pos, 0.1f, 0, 0, "%.1f") : ImGui::DragFloat3("Position", pos, 0.1f, 0, 0, "%.1f"))
            std::tie(p.x, p.y, p.z) = std::tuple(pos[0], pos[1], pos[2]);
        if (p.kind == PoiKind::MapIcon) ImGui::TextColored(kQuiet, "Area %s", AreaLabel(p.area).c_str());
        if (p.kind == PoiKind::Tele)
        {
            float deg = p.o * 360.0f / kTwoPi;
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::SliderFloat("Facing", &deg, 0, 360, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)) p.o = std::fmod(deg + 360.0f, 360.0f) * kTwoPi / 360.0f;
            for (const Poi& other : PoisOnMap())   // this map's; the check in Problems covers every map
                if (other.id != p.id && _stricmp(other.name.c_str(), p.name.c_str()) == 0)
                    ImGui::TextColored(kWarn, "Teleport %u has this name too: .tele finds one of them.", other.id);
        }
        if (p.kind == PoiKind::Gossip && db)
        {
            const std::string usesKey = std::to_string(p.id) + "/" + std::to_string(m_store.Revision());
            if (usesKey != m_poiUsesKey)
            {
                m_poiUsesKey = usesKey;
                m_poiUses.clear();
                std::string error;
                if (const auto rows = m_db.Query("SELECT o.MenuID, o.OptionText, COALESCE(GROUP_CONCAT(DISTINCT t.name SEPARATOR ', '), '') FROM gossip_menu_option o "
                                                 "LEFT JOIN creature_template t ON t.gossip_menu_id = o.MenuID WHERE o.ActionPoiID = " + std::to_string(p.id) +
                                                 " GROUP BY o.MenuID, o.OptionID, o.OptionText LIMIT 20", error))
                    for (const auto& r : *rows) m_poiUses.push_back("menu " + r[0] + ": " + r[1] + (r[2].empty() ? "" : "  (" + r[2] + ")"));
                else Log("gossip_menu_option: %s", error.c_str());
            }
            ImGui::SeparatorText("Shown by");
            if (m_poiUses.empty()) ImGui::TextColored(kQuiet, "No gossip option.");
            for (const std::string& use : m_poiUses) ImGui::TextWrapped("%s", use.c_str());
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Button("Go to", { (w - 8) / 2, 0 })) FlyToPoi(p);
        ImGui::SameLine();
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button("Delete  Del", { (w - 8) / 2, 0 })) CommitPoi(p.kind, p.id, std::nullopt, std::string("Delete ") + KindName(p.kind) + " " + std::to_string(p.id));
        ImGui::EndDisabled();
        // Saved once nothing is being dragged or typed (undo puts it back).
        const auto saved = std::find_if(points.begin(), points.end(), [&](const Poi& s) { return s.id == p.id; });
        if (saved != points.end() && RowOf(*saved) != RowOf(p) && editable && !ImGui::IsAnyItemActive() && !m_gizmoActive)
            CommitPoi(p.kind, p.id, p, std::string("Edit ") + KindName(p.kind) + " " + std::to_string(p.id));
    }
}

// ---------------------------------------------------------------------------------------------- checks

void App::CheckPois(std::vector<Problem>& problems)
{
    for (const PoiKind kind : { PoiKind::MapIcon, PoiKind::Gossip, PoiKind::Tele })
        if (const Project::IdRange r = m_project->Range(IdKind(kind)); !r.first || r.last < r.first)
            problems.push_back({ Problem::Severity::Error, "IDs", std::string("No ") + IdKind(kind) + " range set (File > Project settings)." });
        else if (kind == PoiKind::MapIcon)
            m_areaPois.CheckIds(r.first, r.last, "POIs", problems);

    // Only what the project touched: Blizzard's own data is not this project's problem.
    std::set<uint32_t> teles, gossip;
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain == m_teles.Domain()) teles.insert(data.at("key").get<uint32_t>());
        else if (domain == m_gossipPois.Domain()) gossip.insert(data.at("key").get<uint32_t>());
    });
    if (!m_db.Connected() || (teles.empty() && gossip.empty())) return;
    if (!teles.empty())
    {
        std::map<std::string, std::vector<uint32_t>> names;   // lower-case name -> ids
        for (const auto& [id, rows] : m_teles.All())
        {
            std::string name = Poi::FromTeleRow(rows.front()).name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            names[name].push_back(id);
        }
        for (const auto& [name, ids] : names)
            if (ids.size() > 1 && std::any_of(ids.begin(), ids.end(), [&](uint32_t id) { return teles.count(id); }))
                problems.push_back({ Problem::Severity::Warning, "POIs", "Teleport name '" + name + "' is used by " + std::to_string(ids.size()) + " game_tele rows: .tele finds one of them." });
    }
    for (const uint32_t id : gossip)
    {
        if (m_gossipPois.Rows(id).empty()) continue;
        std::string error;
        const auto rows = m_db.Query("SELECT COUNT(*) FROM gossip_menu_option WHERE ActionPoiID = " + std::to_string(id), error);
        if (rows && !rows->empty() && (*rows)[0][0] == "0")
            problems.push_back({ Problem::Severity::Warning, "POIs", "Gossip point " + std::to_string(id) + " is shown by no gossip option (gossip_menu_option.ActionPoiID)." });
    }
}
