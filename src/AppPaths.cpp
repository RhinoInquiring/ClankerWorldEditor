// Populate: creature waypoint paths (edit mode in the Creatures tool, path lines, walk preview, panel).
#include "App.hpp"

#include <imgui.h>
#include <ImGuizmo.h>

#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
constexpr float kTwoPi = 6.2831853f;
// Default creature speeds (yd/s): walk 2.5, run 7 (creature_template speed_walk / speed_run scale these).
constexpr float kWalkSpeed = 2.5f, kRunSpeed = 7.0f;
constexpr float kPointRadius = 0.6f;   // path point spheres, yards
constexpr float kTubeRadius = 0.18f;   // path lines, yards
constexpr float kArrowLength = 1.0f, kArrowRadius = 0.45f, kArrowSpacing = 8.0f;   // arrow heads on path lines, yards

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

XMFLOAT3 Editor(const PathPoint& p) { return ServerToEditor(p.x, p.y, p.z); }

/// Where a creature walking the loop of points is after `t` seconds (it waits `delay` at each point it reaches).
std::optional<XMFLOAT3> WalkAt(const std::vector<PathPoint>& points, double t)
{
    if (points.size() < 2) return std::nullopt;
    struct Leg { XMFLOAT3 from, to; float move, wait; };
    std::vector<Leg> legs;
    double total = 0;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const PathPoint& a = points[i];
        const PathPoint& b = points[(i + 1) % points.size()];
        const float dist = std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
        legs.push_back({ Editor(a), Editor(b), dist / (b.moveType == 1 ? kRunSpeed : kWalkSpeed), b.delay / 1000.0f });
        total += legs.back().move + legs.back().wait;
    }
    if (total <= 0) return Editor(points[0]);
    t = std::fmod(t, total);
    for (const Leg& l : legs)
    {
        if (t < l.move)
        {
            const float f = float(t / std::max(l.move, 1e-3f));
            return XMFLOAT3{ l.from.x + (l.to.x - l.from.x) * f, l.from.y + (l.to.y - l.from.y) * f, l.from.z + (l.to.z - l.from.z) * f };
        }
        t -= l.move;
        if (t < l.wait) return l.to;
        t -= l.wait;
    }
    return Editor(points[0]);
}
}

void App::BeginPathEdit(uint32_t guid)
{
    PathEdit e;
    e.guid = guid;
    for (const auto& s : m_spawnView)
        if (s.kind == SpawnKind::Creature && s.guid == guid) e.name = s.name;
    e.points = PathOf().Load(guid);
    if (!e.points.empty()) e.sel = e.points.size() - 1;
    m_path = std::move(e);
    m_spawnArmed.reset();
}

void App::SavePathEdit()
{
    if (!m_path) return;
    if (m_path->dirty)
    {
        std::string error;
        auto parts = PathOf().Save(m_path->guid, m_path->points, error);
        if (!error.empty()) { Log("Path not saved: %s", error.c_str()); return; }
        for (const TableRowsAdapter* t : { &m_waypoints, &m_addons })
            if (!t->LastError().empty()) Log("%s", t->LastError().c_str());
        const std::string label = (m_path->points.empty() ? "Remove path of " : "Path for ") + m_path->name + " (" + std::to_string(m_path->guid) + ", " +
                                  std::to_string(m_path->points.size()) + " points)";
        if (!parts.empty())
        {
            m_store.Commit(std::move(parts), label);
            Log("%s. A creature that already walked a path picks up the new one with .wp reload %u; a new path needs a restart.",
                label.c_str(), PathOf().PathId(m_path->guid));
        }
    }
    m_path.reset();
    m_pathHover.reset();
}

void App::CancelPathEdit()
{
    m_path.reset();
    m_pathHover.reset();
}

void App::DeletePathPoint()
{
    if (!m_path || !m_path->sel || *m_path->sel >= m_path->points.size()) return;
    m_path->points.erase(m_path->points.begin() + *m_path->sel);
    m_path->dirty = true;
    if (m_path->points.empty()) m_path->sel.reset();
    else if (*m_path->sel > 0) --*m_path->sel;
}

void App::RefreshPathView()
{
    const uint32_t guid = m_tool == Tool::Creatures && m_spawnSel.size() == 1 ? *m_spawnSel.begin() : 0;
    if (guid == m_pathViewGuid && m_store.Revision() == m_pathViewRevision) return;
    m_pathViewGuid = guid;
    m_pathViewRevision = m_store.Revision();
    m_pathView.clear();
    for (const auto& s : m_spawnView)
        if (guid && s.kind == SpawnKind::Creature && s.guid == guid && s.movementType == 2) m_pathView = PathOf().Load(guid);
}

const std::vector<PathPoint>* App::ShownPath() const
{
    if (m_tool != Tool::Creatures) return nullptr;
    if (m_path) return &m_path->points;
    if (m_spawnSel.size() != 1 || *m_spawnSel.begin() != m_pathViewGuid || m_pathView.empty()) return nullptr;
    return &m_pathView;
}

XMFLOAT3 App::PathPointCenter(const PathPoint& p)
{
    XMFLOAT3 c = Editor(p);
    c.y += kPointRadius;   // the sphere sits on the point
    return c;
}

void App::PathViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    // Click a sphere: select it (the handles then move it). Click the ground: a new point after the selected one.
    PathEdit& e = *m_path;
    const ImGuiIO& io = ImGui::GetIO();
    m_pathHover.reset();
    if (ImGui::IsItemHovered())
    {
        const float mx = (io.MousePos.x - origin.x) / size.x * 2 - 1, my = 1 - (io.MousePos.y - origin.y) / size.y * 2;
        const XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
        const XMVECTOR p0 = XMVector3TransformCoord(XMVectorSet(mx, my, 0, 1), inv);
        const XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(XMVector3TransformCoord(XMVectorSet(mx, my, 1, 1), inv), p0));
        float best = m_hover ? XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&m_hover->pos), p0))) + kPointRadius : 6000.0f;
        for (size_t i = 0; i < e.points.size(); ++i)
        {
            const XMFLOAT3 c = PathPointCenter(e.points[i]);
            const XMVECTOR to = XMVectorSubtract(XMLoadFloat3(&c), p0);
            const float along = XMVectorGetX(XMVector3Dot(to, dir));
            const float miss = XMVectorGetX(XMVector3LengthSq(to)) - along * along;
            const float r = kPointRadius * 1.3f;   // a little generous
            if (along > 0 && miss < r * r && along < best) { best = along; m_pathHover = i; }
        }
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (m_pathHover) e.sel = m_pathHover;
        else if (m_hover)
        {
            PathPoint p;
            EditorToServer(m_hover->pos, p.x, p.y, p.z);
            const size_t at = e.sel ? *e.sel + 1 : e.points.size();
            if (at > 0) p.moveType = e.points[at - 1].moveType;
            e.points.insert(e.points.begin() + at, p);
            e.sel = at;
            e.dirty = true;
        }
    }
}

bool App::UpdatePathGizmo(const ImVec2& origin, const ImVec2& size)
{
    PathPoint& p = m_path->points[*m_path->sel];
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(false);
    XMFLOAT4X4 view, proj, frame;
    XMStoreFloat4x4(&view, m_camera.View());
    XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), size.x / size.y, 1.0f, 6000.0f));
    const XMFLOAT3 at = Editor(p);
    XMStoreFloat4x4(&frame, XMMatrixTranslation(at.x, at.y, at.z));
    const bool snapping = m_gizmoSnap != ImGui::GetIO().KeyCtrl;
    const float snap[3] = { m_snapMove, m_snapMove, m_snapMove };
    if (ImGuizmo::Manipulate(&view._11, &proj._11, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &frame._11, nullptr, snapping ? snap : nullptr))
    {
        EditorToServer({ frame._41, frame._42, frame._43 }, p.x, p.y, p.z);
        m_path->dirty = true;
    }
    return ImGuizmo::IsUsing() || ImGuizmo::IsOver();
}

void App::DropPathPoint()
{
    if (!m_path || !m_path->sel || *m_path->sel >= m_path->points.size()) return;
    PathPoint& p = m_path->points[*m_path->sel];
    const XMFLOAT3 e = Editor(p);
    if (const auto h = m_terrain.HeightAt(e.x, e.z)) { p.z = *h; m_path->dirty = true; }
}

void App::BuildPathSolids(std::vector<LineVertex>& triangles) const
{
    const std::vector<PathPoint>* path = ShownPath();
    if (!path) return;
    const bool editing = m_path.has_value();
    for (size_t i = 0; i < path->size(); ++i)
    {
        const bool sel = editing && m_path->sel == i, hov = editing && m_pathHover == i;
        XMFLOAT4 col = sel ? XMFLOAT4{ 1, 1, 1, 1 } : hov ? XMFLOAT4{ 1, 0.85f, 0.55f, 1 }
                     : (*path)[i].moveType == 1 ? XMFLOAT4{ 0.95f, 0.3f, 0.25f, 1 } : XMFLOAT4{ 1.0f, 0.6f, 0.15f, 1 };   // red = run to it
        if (!editing) col = { col.x * 0.75f, col.y * 0.75f, col.z * 0.75f, 1 };
        AddSphere(triangles, PathPointCenter((*path)[i]), editing ? kPointRadius : kPointRadius * 0.7f, col);
    }
    // Thick tubes between the balls with arrow heads pointing the way the creature walks (the overlay's thin lines
    // still show the route through hills).
    const XMFLOAT4 line = editing ? XMFLOAT4{ 1.0f, 0.55f, 0.15f, 1 } : XMFLOAT4{ 0.85f, 0.6f, 0.3f, 1 };
    const XMFLOAT4 back{ line.x * 0.55f, line.y * 0.55f, line.z * 0.55f, 1 };   // last -> first and spawn -> first
    const float tube = editing ? kTubeRadius : kTubeRadius * 0.7f;
    const XMFLOAT4 arrow{ 0.25f, 0.55f, 1.0f, 1 }, arrowDim{ 0.18f, 0.35f, 0.7f, 1 };   // blue, so the direction stands out from the line
    auto leg = [&](const XMFLOAT3& from, const XMFLOAT3& to, float radius, const XMFLOAT4& col) {
        AddTube(triangles, from, to, radius, col);
        const XMVECTOR a = XMLoadFloat3(&from), d = XMVectorSubtract(XMLoadFloat3(&to), a);
        const float length = XMVectorGetX(XMVector3Length(d));
        const float clear = length - 2 * kPointRadius;   // the part between the balls
        if (clear < kArrowLength) return;
        const XMVECTOR dir = XMVectorScale(d, 1.0f / length);
        // One arrow in the middle of a short leg, one every kArrowSpacing yards along a long one.
        const int count = std::max(1, int(clear / kArrowSpacing));
        for (int k = 0; k < count; ++k)
        {
            const float centre = kPointRadius + clear * (k + 0.5f) / count;
            XMFLOAT3 base, tip;
            XMStoreFloat3(&base, XMVectorAdd(a, XMVectorScale(dir, centre - kArrowLength / 2)));
            XMStoreFloat3(&tip, XMVectorAdd(a, XMVectorScale(dir, centre + kArrowLength / 2)));
            AddCone(triangles, base, tip, radius * (kArrowRadius / kTubeRadius), col.x < line.x ? arrowDim : arrow);
        }
    };
    for (size_t i = 0; path->size() > 1 && i < path->size(); ++i)
        leg(PathPointCenter((*path)[i]), PathPointCenter((*path)[(i + 1) % path->size()]), tube, i + 1 == path->size() ? back : line);
    const uint32_t guid = editing ? m_path->guid : m_pathViewGuid;
    for (const auto& s : m_spawnView)
        if (!path->empty() && s.kind == SpawnKind::Creature && s.guid == guid)
        {
            XMFLOAT3 home = ServerToEditor(s.x, s.y, s.z);
            home.y += kPointRadius;
            leg(home, PathPointCenter((*path)[0]), tube * 0.7f, back);
        }
    if (m_pathPreview)   // a small cyan ball walking the loop
        if (const auto w = WalkAt(*path, ImGui::GetTime())) AddSphere(triangles, { w->x, w->y + 0.4f, w->z }, 0.4f, { 0.35f, 1.0f, 0.9f, 1 });
}

void App::BuildPathOverlay(std::vector<LineVertex>& lines) const
{
    // Lines between the spheres (drawn over the world so a hill cannot hide the route).
    const std::vector<PathPoint>* path = ShownPath();
    if (!path || path->empty()) return;
    const bool editing = m_path.has_value();
    const XMFLOAT4 line = editing ? XMFLOAT4{ 1.0f, 0.55f, 0.15f, 1 } : XMFLOAT4{ 0.85f, 0.6f, 0.3f, 1 };
    const XMFLOAT4 back{ line.x * 0.5f, line.y * 0.5f, line.z * 0.5f, 1 };   // last point -> first: the core loops back
    auto at = [&](size_t i) { return PathPointCenter((*path)[i]); };
    // From the spawn to the first point (the creature starts at its spawn).
    const uint32_t guid = editing ? m_path->guid : m_pathViewGuid;
    for (const auto& s : m_spawnView)
        if (s.kind == SpawnKind::Creature && s.guid == guid)
        {
            XMFLOAT3 home = ServerToEditor(s.x, s.y, s.z);
            home.y += kPointRadius;
            lines.push_back({ home, back });
            lines.push_back({ at(0), back });
        }
    for (size_t i = 0; path->size() > 1 && i < path->size(); ++i)
    {
        const bool closing = i + 1 == path->size();
        lines.push_back({ at(i), closing ? back : line });
        lines.push_back({ at((i + 1) % path->size()), closing ? back : line });
    }
}

void App::DrawPathLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj) const
{
    const std::vector<PathPoint>* path = ShownPath();
    if (!path) return;
    for (size_t i = 0; i < path->size(); ++i)
    {
        XMFLOAT3 p = PathPointCenter((*path)[i]);
        p.y += 1.0f;
        if (const auto at = ToScreen(viewProj, p, origin, size))
        {
            const std::string text = std::to_string(i + 1) + ((*path)[i].delay ? "  " + std::to_string((*path)[i].delay / 1000.0f).substr(0, 4) + "s" : "");
            dl->AddText({ at->x + 1, at->y + 1 }, IM_COL32(0, 0, 0, 200), text.c_str());
            dl->AddText(*at, IM_COL32(255, 200, 140, 255), text.c_str());
        }
    }
}

void App::DrawPathPanel(float w)
{
    if (!Section("Path", m_path && !m_pathTabShown)) { m_pathTabShown = m_path.has_value(); return; }
    m_pathTabShown = m_path.has_value();
    if (!m_path)
    {
        const uint32_t guid = *m_spawnSel.begin();
        const std::vector<PathPoint>* shown = ShownPath();
        if (shown) ImGui::TextColored(kQuiet, "Walks a path of %zu points (path %u).", shown->size(), PathOf().PathId(guid));
        else ImGui::TextColored(kQuiet, "No path: it stands or wanders.");
        if (ImGui::Button(shown ? "Edit path" : "Draw a path", { w, 0 })) BeginPathEdit(guid);
        if (shown && m_soapOk.value_or(false) == false && !m_project->serverProfile.empty())
            ImGui::TextColored(kQuiet, "Server reload needs SOAP (Server panel).");
        if (shown && ImGui::Button(("Reload on server  (.wp reload " + std::to_string(PathOf().PathId(guid)) + ")").c_str(), { w, 0 }))
            RunServerCommand(".wp reload " + std::to_string(PathOf().PathId(guid)));
        return;
    }
    PathEdit& e = *m_path;
    ImGui::TextColored(kWarn, "Editing the path of %s", e.name.c_str());
    ImGui::TextColored(kQuiet, "Click the ground: add a point after the selected one\nClick a ball: select it; the handles move it (Ctrl snaps)\n"
                               "G: drop it to the ground   Del: delete it\nEnter: save   Esc: cancel");
    ImGui::Checkbox("Walk preview", &m_pathPreview);
    ImGui::SetItemTooltip("A ball walking the loop at default walk/run speed, waiting at each point.");
    if (ImGui::BeginTable("##points", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                          { w, std::min(12.0f, float(e.points.size()) + 1.5f) * ImGui::GetFrameHeightWithSpacing() }))
    {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28);
        ImGui::TableSetupColumn("Wait s", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < e.points.size(); ++i)
        {
            PathPoint& p = e.points[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), e.sel == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) e.sel = i;
            ImGui::TableNextColumn();
            float seconds = p.delay / 1000.0f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##wait", &seconds, 0.1f, 0, 600, "%.1f")) { p.delay = uint32_t(std::max(seconds, 0.0f) * 1000 + 0.5f); e.dirty = true; }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            int move = int(std::min<uint32_t>(p.moveType, 3));
            if (ImGui::Combo("##move", &move, "Walk\0Run\0Land\0Take off\0")) { p.moveType = uint32_t(move); e.dirty = true; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (e.points.empty()) ImGui::TextColored(kQuiet, "No points yet. Click the ground to start.");
    else if (e.points.size() == 1) ImGui::TextColored(kWarn, "One point: the creature walks there and stays.");
    const float half = (w - 8) / 2;
    if (ImGui::Button("Drop to ground  G", { half, 0 })) DropPathPoint();
    ImGui::SameLine();
    if (ImGui::Button("Delete point  Del", { half, 0 })) DeletePathPoint();
    if (ImGui::Button("Clear all", { w, 0 }) && !e.points.empty()) { e.points.clear(); e.sel.reset(); e.dirty = true; }
    ImGui::BeginDisabled(!e.dirty);
    if (ImGui::Button(e.points.empty() ? "Save (remove path)" : "Save  Enter", { half, 0 })) SavePathEdit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel  Esc", { half, 0 })) CancelPathEdit();
}
