// Populate: creature and gameobject spawns (the Spawns tool, its panel, markers, labels and gameobject models).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
constexpr float kTwoPi = 6.2831853f;

/// Screen position of a point in the viewport, if in front of the camera.
std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}
}

uint32_t App::CurrentMapId() const
{
    for (const auto& m : m_maps)
        if (m.directory == m_terrain.Map()) return m.id;
    return 0;
}

void App::UpdateSpawnView()
{
    // The Creatures and Gameobjects tools each work on their own table; switching drops the other's selection.
    if (SpawnTool())
        if (const SpawnKind kind = m_tool == Tool::Creatures ? SpawnKind::Creature : SpawnKind::GameObject; kind != m_spawnKind)
        {
            m_spawnKind = kind;
            m_spawnSel.clear();
            m_spawnHover.reset();
            m_spawnArmed.reset();
            m_spawnResults.clear();
            m_spawnQuery.clear();
        }

    m_spawnView.clear();
    if (!m_project || m_terrain.Map().empty() || !m_db.Connected() && m_store.Done().empty()) return;
    // The loaded square around the camera, snapped to tiles so the query only runs when the camera changes tile.
    const int tx = int(std::floor(m_camera.pos.x / kTileSize)), ty = int(std::floor(m_camera.pos.z / kTileSize));
    const float x0 = (tx - m_loadRadius) * kTileSize, x1 = (tx + m_loadRadius + 1) * kTileSize;
    const float z0 = (ty - m_loadRadius) * kTileSize, z1 = (ty + m_loadRadius + 1) * kTileSize;
    float minX, minY, maxX, maxY, unused;
    EditorToServer({ x1, 0, z1 }, minX, minY, unused);
    EditorToServer({ x0, 0, z0 }, maxX, maxY, unused);
    for (SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
    {
        const auto& around = spawns->Around(CurrentMapId(), minX, minY, maxX, maxY);
        m_spawnView.insert(m_spawnView.end(), around.begin(), around.end());
    }
    RefreshPathView();
}

void App::UpdateSpawnModels()
{
    // One model tile per kind, rebuilt when its spawns change; hidden kinds have none.
    for (const SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
    {
        const int kind = int(spawns->Kind());
        const uint32_t version = m_showSpawns[kind] ? spawns->Version() : ~0u - 1;
        if (version == m_spawnModelVersion[kind]) continue;
        m_spawnModelVersion[kind] = version;
        const int key = SpawnTileKey(spawns->Kind());
        m_models.RemoveTile(key);
        if (!m_showSpawns[kind]) continue;
        for (const auto& s : m_spawnView)
            if (s.kind == spawns->Kind())
                if (const auto model = m_looks.SpawnLook(s)) AddSpawnModel(m_models, key, *model, ServerToEditor(s.x, s.y, s.z), s.orientation, s.guid, m_mpq);
        m_models.AddTile(key, Adt{}, m_mpq);   // the tile exists even when nothing has a model
    }
    // The armed template follows the cursor, facing the camera as it will when placed.
    m_models.RemoveTile(-4);
    if (SpawnTool() && m_spawnArmed && m_hover)
        if (const auto model = m_looks.SpawnLook(ArmedSpawn()))
        {
            float cx, cy, cz, x, y, z;
            EditorToServer(m_camera.pos, cx, cy, cz);
            EditorToServer(m_hover->pos, x, y, z);
            AddSpawnModel(m_models, -4, *model, m_hover->pos, std::atan2(cy - y, cx - x), 0, m_mpq);
        }
}

Spawn App::ArmedSpawn() const
{
    Spawn s;
    s.kind = m_spawnKind;
    if (!m_spawnArmed) return s;
    s.entry = m_spawnArmed->entry;
    s.displayId = m_spawnArmed->displayId;
    s.size = m_spawnArmed->size;
    std::copy(std::begin(m_spawnArmed->weapons), std::end(m_spawnArmed->weapons), s.weapons);
    std::copy(std::begin(m_spawnArmed->weaponTypes), std::end(m_spawnArmed->weaponTypes), s.weaponTypes);
    return s;
}

std::optional<uint32_t> App::SpawnAt(const ImVec2& mouse, const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj) const
{
    std::optional<uint32_t> best;
    float bestDist = 14.0f;   // pixels
    for (const auto& s : m_spawnView)
    {
        if (s.kind != m_spawnKind) continue;
        XMFLOAT3 p = ServerToEditor(s.x, s.y, s.z);
        p.y += 1.5f;
        if (const auto at = ToScreen(viewProj, p, origin, size))
            if (const float d = std::hypot(at->x - mouse.x, at->y - mouse.y); d < bestDist) { bestDist = d; best = s.guid; }
    }
    return best;
}

void App::CommitSpawns(const std::vector<std::pair<std::optional<nlohmann::json>, std::optional<nlohmann::json>>>& rows, const std::string& label)
{
    if (rows.empty()) return;
    Change change = Spawns().MakeChange(rows, label);
    Spawns().Apply(change);   // writes the database now; the store records it for undo
    if (!Spawns().LastError().empty()) Log("%s", Spawns().LastError().c_str());
    Log("%s (restart worldserver to see spawn changes in game).", label.c_str());
    m_store.Commit(std::move(change));
}

void App::PlaceSpawn(const XMFLOAT3& at)
{
    if (!m_spawnArmed || !m_project) return;
    Spawn s;
    s.kind = m_spawnKind;
    const Project::IdRange range = m_project->Range(Spawns().IdKind());
    const auto guid = Spawns().NextGuid(range.first, range.last);
    if (!guid)
    {
        Log("No free %s left in the project's range %u-%u: widen it in File > Project settings.", Spawns().IdKind().c_str(), range.first, range.last);
        return;
    }
    s.guid = *guid;
    s.entry = m_spawnArmed->entry;
    s.map = CurrentMapId();
    if (m_hover)
        if (const AdtChunk* c = m_terrain.Chunk(m_hover->chunk)) { s.areaId = c->areaId; s.zoneId = m_areas.ZoneOf(c->areaId); }
    EditorToServer(at, s.x, s.y, s.z);
    // Facing the camera: server orientation is the angle from +x (north) towards +y (west).
    float cx, cy, cz;
    EditorToServer(m_camera.pos, cx, cy, cz);
    s.orientation = std::fmod(std::atan2(cy - s.y, cx - s.x) + kTwoPi, kTwoPi);
    CommitSpawns({ { std::nullopt, s.ToRow() } }, "Place " + m_spawnArmed->name + " (" + Spawns().Table() + " guid " + std::to_string(s.guid) + ")");
    m_spawnSel = { s.guid };
}

void App::EditSpawns(const std::string& label, const std::function<void(Spawn&)>& fn)
{
    std::vector<std::pair<std::optional<nlohmann::json>, std::optional<nlohmann::json>>> rows;
    for (uint32_t guid : m_spawnSel)
    {
        const auto before = Spawns().Row(guid);
        if (!before) { Log("Spawn %u is not in the database.", guid); continue; }
        Spawn s = Spawn::FromRow(*before, m_spawnKind);
        fn(s);
        if (nlohmann::json after = s.ToRow(*before); after != *before) rows.push_back({ before, std::move(after) });
    }
    CommitSpawns(rows, label);
}

void App::DeleteSpawns()
{
    std::vector<std::pair<std::optional<nlohmann::json>, std::optional<nlohmann::json>>> rows;
    for (uint32_t guid : m_spawnSel)
        if (const auto before = Spawns().Row(guid)) rows.push_back({ before, std::nullopt });
    CommitSpawns(rows, "Delete " + std::to_string(rows.size()) + " " + Spawns().Table() + " spawn(s)");
    m_spawnSel.clear();
}

void App::MoveSpawns(const XMFLOAT3& at)
{
    // The selection keeps its layout: its centre goes to `at`, every spawn lands on the ground there.
    float cx = 0, cy = 0;
    size_t n = 0;
    for (const auto& s : m_spawnView)
        if (s.kind == m_spawnKind && m_spawnSel.count(s.guid)) { cx += s.x; cy += s.y; ++n; }
    if (!n) return;
    cx /= n;
    cy /= n;
    float tx, ty, tz;
    EditorToServer(at, tx, ty, tz);
    EditSpawns("Move " + std::to_string(n) + " spawn(s)", [&](Spawn& s) {
        s.x += tx - cx;
        s.y += ty - cy;
        const XMFLOAT3 e = ServerToEditor(s.x, s.y, s.z);
        s.z = n == 1 ? tz : m_terrain.HeightAt(e.x, e.z).value_or(s.z);
        if (const auto ref = m_terrain.ChunkAtGrid(int(std::floor(e.x / kChunkSize)), int(std::floor(e.z / kChunkSize))))
            if (const AdtChunk* c = m_terrain.Chunk(*ref)) { s.areaId = c->areaId; s.zoneId = m_areas.ZoneOf(c->areaId); }
    });
}

void App::SpawnsViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    // Click a spawn (marker or model): select it. Drag: box select. Shift adds, Ctrl removes. Alt+click: move the
    // selection there. Click the ground with a template picked: place it (it stays picked; Esc stops).
    if (m_path && m_tool == Tool::Creatures)   // editing a path: clicks add and move its points
    {
        PathViewport(origin, size, viewProj);
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        m_boxStart[0] = io.MousePos.x;
        m_boxStart[1] = io.MousePos.y;
        m_boxing = false;
        m_spawnPress = m_spawnHover;
    }
    const bool leftHeld = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (leftHeld && !io.KeyAlt && std::fabs(io.MousePos.x - m_boxStart[0]) + std::fabs(io.MousePos.y - m_boxStart[1]) > 5) m_boxing = true;
    if (!ImGui::IsItemDeactivated() || !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;

    if (m_boxing)
    {
        const float x0 = std::min(m_boxStart[0], io.MousePos.x), x1 = std::max(m_boxStart[0], io.MousePos.x);
        const float y0 = std::min(m_boxStart[1], io.MousePos.y), y1 = std::max(m_boxStart[1], io.MousePos.y);
        if (!io.KeyShift && !io.KeyCtrl) m_spawnSel.clear();
        for (const auto& s : m_spawnView)
        {
            if (s.kind != m_spawnKind || !m_showSpawns[int(s.kind)]) continue;
            XMFLOAT3 p = ServerToEditor(s.x, s.y, s.z);
            p.y += 1.5f;
            if (const auto at = ToScreen(viewProj, p, origin, size); at && at->x >= x0 && at->x <= x1 && at->y >= y0 && at->y <= y1)
            {
                if (io.KeyCtrl) m_spawnSel.erase(s.guid);
                else m_spawnSel.insert(s.guid);
            }
        }
    }
    else if (io.KeyAlt && !m_spawnSel.empty() && m_hover)
        MoveSpawns(m_hover->pos);
    else if (m_spawnPress)
    {
        if (io.KeyCtrl) m_spawnSel.erase(*m_spawnPress);
        else if (io.KeyShift) m_spawnSel.insert(*m_spawnPress);
        else m_spawnSel = { *m_spawnPress };
    }
    else if (m_spawnArmed && m_hover)
        PlaceSpawn(m_hover->pos);
    else if (!io.KeyShift && !io.KeyCtrl)
        m_spawnSel.clear();
    m_boxing = false;
    m_spawnPress.reset();
}

void App::BuildSpawnOverlay(std::vector<LineVertex>& lines) const
{
    if (!SpawnTool() && m_spawnView.size() > 3000) return;   // a crowded city: markers only while populating
    for (const auto& s : m_spawnView)
    {
        if (!m_showSpawns[int(s.kind)]) continue;
        const XMFLOAT3 p = ServerToEditor(s.x, s.y, s.z);
        const bool go = s.kind == SpawnKind::GameObject;
        const bool mine = s.kind == m_spawnKind && SpawnTool();
        const bool selected = mine && m_spawnSel.count(s.guid), hovered = mine && m_spawnHover == s.guid;
        const bool own = m_project && m_project->Owns(go ? "gameobject.guid" : "creature.guid", s.guid);
        XMFLOAT4 col = selected ? XMFLOAT4{ 1, 1, 1, 1 }
                       : own    ? XMFLOAT4{ 0.35f, 1.0f, 0.45f, 1 }
                       : go     ? XMFLOAT4{ 0.35f, 0.75f, 1.0f, 1 }
                                : XMFLOAT4{ 1.0f, 0.85f, 0.25f, 1 };
        if (SpawnTool() && !mine) col = { col.x * 0.45f, col.y * 0.45f, col.z * 0.45f, 1 };   // the other kind, dimmed
        const float r = selected || hovered ? 1.2f : 0.8f;
        lines.push_back({ p, col });
        lines.push_back({ { p.x, p.y + 3.0f, p.z }, col });
        if (go)   // gameobject: a diamond
        {
            const XMFLOAT3 d[4] = { { p.x - r, p.y + 1.5f, p.z }, { p.x, p.y + 1.5f, p.z - r }, { p.x + r, p.y + 1.5f, p.z }, { p.x, p.y + 1.5f, p.z + r } };
            for (int i = 0; i < 4; ++i) { lines.push_back({ d[i], col }); lines.push_back({ d[(i + 1) % 4], col }); }
        }
        else      // creature: a cross
        {
            lines.push_back({ { p.x - r, p.y + 1.5f, p.z }, col });
            lines.push_back({ { p.x + r, p.y + 1.5f, p.z }, col });
            lines.push_back({ { p.x, p.y + 1.5f, p.z - r }, col });
            lines.push_back({ { p.x, p.y + 1.5f, p.z + r }, col });
        }
        if (selected || hovered)   // a ring on the ground
            for (int i = 0; i < 16; ++i)
            {
                const float a0 = kTwoPi * i / 16, a1 = kTwoPi * (i + 1) / 16;
                lines.push_back({ { p.x + std::cos(a0) * 1.5f, p.y + 0.15f, p.z + std::sin(a0) * 1.5f }, col });
                lines.push_back({ { p.x + std::cos(a1) * 1.5f, p.y + 0.15f, p.z + std::sin(a1) * 1.5f }, col });
            }
        // Facing: server angle from +x (editor -z) towards +y (editor -x).
        lines.push_back({ { p.x, p.y + 0.2f, p.z }, col });
        lines.push_back({ { p.x - std::sin(s.orientation) * 2.5f, p.y + 0.2f, p.z - std::cos(s.orientation) * 2.5f }, col });
    }
}

void App::DrawSpawnLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj) const
{
    for (const auto& s : m_spawnView)
    {
        if (!m_showSpawns[int(s.kind)]) continue;
        XMFLOAT3 p = ServerToEditor(s.x, s.y, s.z);
        const float dx = p.x - m_camera.pos.x, dy = p.y - m_camera.pos.y, dz = p.z - m_camera.pos.z;
        const bool mine = s.kind == m_spawnKind && SpawnTool();
        const bool selected = mine && m_spawnSel.count(s.guid), hovered = mine && m_spawnHover == s.guid;
        if (!selected && !hovered && dx * dx + dy * dy + dz * dz > 120.0f * 120.0f) continue;   // names close by only
        p.y += 3.5f;
        if (const auto at = ToScreen(viewProj, p, origin, size))
        {
            const std::string text = s.name.empty() ? "#" + std::to_string(s.entry) : s.name;
            const ImVec2 t = ImGui::CalcTextSize(text.c_str());
            const ImU32 col = selected || hovered ? IM_COL32(255, 255, 255, 255)
                            : s.kind == SpawnKind::GameObject ? IM_COL32(150, 210, 255, 230) : IM_COL32(255, 230, 150, 230);
            dl->AddText({ at->x - t.x / 2 + 1, at->y - t.y + 1 }, IM_COL32(0, 0, 0, 180), text.c_str());
            dl->AddText({ at->x - t.x / 2, at->y - t.y }, col, text.c_str());
        }
    }
}

void App::DrawSpawnsPanel(float w)
{
    const bool creature = m_spawnKind == SpawnKind::Creature;
    const std::string table = Spawns().Table();
    ImGui::SeparatorText(creature ? "Creatures" : "Gameobjects");
    ImGui::Checkbox("Show creatures", &m_showSpawns[int(SpawnKind::Creature)]);
    ImGui::SameLine();
    ImGui::Checkbox("Show gameobjects", &m_showSpawns[int(SpawnKind::GameObject)]);
    if (!m_db.Connected())
    {
        ImGui::TextColored(kWarn, "Needs the world database: File > Server setup.");
        return;
    }
    ImGui::TextColored(kQuiet, "Pick a %s, click the ground to place it.\nClick a spawn or its model: select   Drag: box select\n"
                               "Shift: add   Ctrl: remove   Alt+click: move selection there\nDel: delete   Esc: stop / deselect",
                       creature ? "creature" : "gameobject");
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint("##template", ("Search " + table + "_template (name or entry)").c_str(), &m_spawnQuery, ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit() && !m_spawnQuery.empty()))
    {
        std::string error;
        m_spawnResults = Spawns().Search(m_spawnQuery, error);
        if (!error.empty()) Log("%s search: %s", table.c_str(), error.c_str());
    }
    if (ImGui::BeginListBox("##results", { w, std::min(10.0f, float(m_spawnResults.size()) + 0.5f) * ImGui::GetTextLineHeightWithSpacing() }))
    {
        for (const auto& t : m_spawnResults)
        {
            char line[300];
            snprintf(line, sizeof line, "%s  %s  #%u", t.name.c_str(), t.detail.c_str(), t.entry);
            if (ImGui::Selectable(line, m_spawnArmed && m_spawnArmed->entry == t.entry)) m_spawnArmed = t;
        }
        ImGui::EndListBox();
    }
    if (m_spawnArmed)
    {
        ImGui::Text("Placing: %s", m_spawnArmed->name.c_str());
        if (!m_looks.SpawnLook(ArmedSpawn()))
            ImGui::TextColored(kWarn, "displayId %u has no model in this client (marker only).", m_spawnArmed->displayId);
    }

    std::vector<const Spawn*> sel;
    for (const auto& s : m_spawnView)
        if (s.kind == m_spawnKind && m_spawnSel.count(s.guid)) sel.push_back(&s);
    ImGui::SeparatorText(sel.size() > 1 ? (std::to_string(sel.size()) + " selected").c_str() : "Selected");
    if (sel.empty()) ImGui::TextColored(kQuiet, "None. Click a spawn or drag a box in the viewport.");
    else
    {
        const Spawn& first = *sel.front();
        if (sel.size() == 1)
        {
            ImGui::Text("%s  #%u", first.name.c_str(), first.entry);
            ImGui::TextColored(kQuiet, "guid %u   zone %u   area %u", first.guid, first.zoneId, first.areaId);
            ImGui::TextColored(kQuiet, "%.2f, %.2f, %.2f", first.x, first.y, first.z);
            const auto look = m_looks.SpawnLook(first);
            ImGui::TextColored(kQuiet, "display %u: %s", first.displayId, look ? look->look.model.c_str() : "(no model)");
        }
        else
        {
            std::map<std::string, int> names;
            for (const Spawn* s : sel) ++names[s->name.empty() ? "#" + std::to_string(s->entry) : s->name];
            std::string summary;
            for (const auto& [name, count] : names) summary += (summary.empty() ? "" : ", ") + name + (count > 1 ? " x" + std::to_string(count) : "");
            ImGui::TextWrapped("%s", summary.c_str());
            ImGui::TextColored(kQuiet, "Fields show the first; editing one sets it on all %zu.", sel.size());
        }
        // Edits show at once in the field; the change (one database write per spawn, one undo step) happens on release.
        const std::string what = sel.size() == 1 ? "spawn " + std::to_string(first.guid) : std::to_string(sel.size()) + " spawns";
        float degrees = first.orientation * 360.0f / kTwoPi;
        ImGui::SetNextItemWidth(w - 110);
        if (ImGui::SliderFloat("Facing", &degrees, 0, 360, "%.0f deg")) m_spawnPending = degrees * kTwoPi / 360.0f;
        if (ImGui::IsItemDeactivatedAfterEdit() && m_spawnPending)
            EditSpawns("Turn " + what, [o = *m_spawnPending](Spawn& s) { s.orientation = o; });
        if (creature)
        {
            float wander = first.wander;
            ImGui::SetNextItemWidth(w - 110);
            ImGui::DragFloat("Wander", &wander, 0.25f, 0, 60, "%.1f yd");
            if (ImGui::IsItemDeactivatedAfterEdit()) EditSpawns("Wander " + what, [wander](Spawn& s) { s.wander = wander; });
            ImGui::SetItemTooltip("Random movement radius (MovementType 1); 0 stands still.");
        }
        int respawn = int(first.spawnTime);
        ImGui::SetNextItemWidth(w - 110);
        ImGui::InputInt("Respawn s", &respawn, 30, 300);
        if (ImGui::IsItemDeactivatedAfterEdit())
            EditSpawns("Respawn time " + what, [respawn](Spawn& s) { s.spawnTime = uint32_t(std::max(respawn, 0)); });
        if (ImGui::Button("Drop to ground", { (w - 8) / 2, 0 }))
            EditSpawns("Drop " + what + " to the ground", [&](Spawn& s) {
                const XMFLOAT3 e = ServerToEditor(s.x, s.y, s.z);
                if (const auto h = m_terrain.HeightAt(e.x, e.z)) s.z = *h;
            });
        ImGui::SameLine();
        if (ImGui::Button("Delete  Del", { (w - 8) / 2, 0 })) DeleteSpawns();
    }
    if (creature && (m_path || sel.size() == 1)) DrawPathPanel(w);
    ImGui::Separator();
    for (const SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
    {
        size_t added, changed, deleted;
        spawns->Counts(added, changed, deleted);
        ImGui::TextColored(kQuiet, "%s: %zu added, %zu changed, %zu deleted.", spawns->Table(), added, changed, deleted);
    }
    ImGui::TextColored(kQuiet, "The game shows them after a worldserver restart.");
}
