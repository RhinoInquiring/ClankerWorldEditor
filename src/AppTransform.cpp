// Move, rotate and scale: one set of handles, keys and panel controls for every tool whose selection can be moved
// (objects, creatures, gameobjects, triggers, points of interest, path points). Each tool describes its selection as a
// Transformable (ActiveTransform); everything else here is shared, so they all behave the same.
#include "App.hpp"

#include <ImGuizmo.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
constexpr float kTwoPi = 6.2831853f;

/// Editor-space frame of something standing at `at` facing server orientation `o` (local -z points where it faces).
XMMATRIX Facing(const XMFLOAT3& at, float o) { return XMMatrixRotationY(o) * XMMatrixTranslation(at.x, at.y, at.z); }

/// Server orientation of a facing direction in editor axes.
float OrientationOf(FXMVECTOR dir)
{
    const float o = std::atan2(-XMVectorGetX(dir), -XMVectorGetZ(dir));
    return o < 0 ? o + kTwoPi : o;
}

/// Where a point goes, and which way a facing turns, under `delta`.
XMFLOAT3 Moved(const XMFLOAT3& p, FXMMATRIX delta)
{
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3TransformCoord(XMLoadFloat3(&p), delta));
    return out;
}
float Turned(float o, FXMMATRIX delta) { return OrientationOf(XMVector3TransformNormal(XMVectorSet(-std::sin(o), 0, -std::cos(o), 0), delta)); }

XMFLOAT4X4 Store(FXMMATRIX m)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, m);
    return out;
}

/// The selection's centre as a frame.
XMFLOAT4X4 CentreFrame(const std::vector<XMFLOAT3>& points)
{
    XMFLOAT3 c{};
    for (const XMFLOAT3& p : points) { c.x += p.x; c.y += p.y; c.z += p.z; }
    const float n = float(std::max<size_t>(points.size(), 1));
    return Store(XMMatrixTranslation(c.x / n, c.y / n, c.z / n));
}

/// A trigger as a frame: its centre, turned by its yaw, scaled by its size (width x, height y, length along -z).
XMMATRIX TriggerFrame(const Trigger& t)
{
    const XMFLOAT3 c = ServerToEditor(t.x, t.y, t.z);
    const XMMATRIX s = t.Sphere() ? XMMatrixScaling(t.radius, t.radius, t.radius) : XMMatrixScaling(t.width, t.height, t.length);
    return s * XMMatrixRotationY(t.Sphere() ? 0.0f : t.yaw) * XMMatrixTranslation(c.x, c.y, c.z);
}

Trigger FromFrame(Trigger t, FXMMATRIX m)
{
    XMVECTOR s, q, p;
    XMMatrixDecompose(&s, &q, &p, m);
    XMFLOAT3 at, size;
    XMStoreFloat3(&at, p);
    XMStoreFloat3(&size, s);
    EditorToServer(at, t.x, t.y, t.z);
    if (t.Sphere()) t.radius = std::clamp(std::max({ size.x, size.y, size.z }), 0.5f, 500.0f);
    else
    {
        t.width = std::clamp(size.x, 0.5f, 1000.0f);
        t.height = std::clamp(size.y, 0.5f, 1000.0f);
        t.length = std::clamp(size.z, 0.5f, 1000.0f);
        t.yaw = OrientationOf(XMVector3Rotate(XMVectorSet(0, 0, -1, 0), q));
    }
    return t;
}
}

// ---------------------------------------------------------------------------------------------- the tools' selections

std::optional<App::Transformable> App::ActiveTransform()
{
    if (m_terrain.Map().empty()) return std::nullopt;
    if (m_tool == Tool::Objects) return ObjectTransform();
    if (m_tool == Tool::Creatures && m_path) return PathPointTransform();   // editing a path: its points, not the creatures
    if (SpawnTool()) return SpawnTransform();
    if (m_tool == Tool::Triggers) return TriggerTransform();
    if (m_tool == Tool::Pois) return PoiTransform();
    if (m_tool == Tool::Flights) return FlightTransform();
    return std::nullopt;
}

std::optional<App::Transformable> App::ObjectTransform()
{
    if (m_objSel.empty()) return std::nullopt;
    Transformable t;
    t.frame = GizmoFrame();
    const bool doodads = std::any_of(m_objSel.begin(), m_objSel.end(), [](const ObjectRef& r) { return !r.wmo; });
    t.scale = doodads ? Transformable::Scale::Uniform : Transformable::Scale::None;
    if (!doodads) t.limits = "Buildings (WMOs) cannot be scaled.";
    t.what = std::to_string(m_objSel.size()) + " object(s)";
    t.begin = [this] {
        // Furniture placed on the map inside a selected building moves with it (it is not part of the WMO file).
        m_gizmoRiders = m_carryInside ? m_terrain.DoodadsInside(m_objSel) : std::set<ObjectRef>{};
        std::set<ObjectRef> moving = m_objSel;
        moving.insert(m_gizmoRiders.begin(), m_gizmoRiders.end());
        m_terrain.BeginObjectEdit(moving);
    };
    t.preview = [this](FXMMATRIX delta) {
        PreviewObjects([&](float* pos, float* rot, float* scale) {
            float s = 1;
            DecomposePlacement(PlacementMatrix(pos, rot, scale ? *scale : 1.0f) * delta, pos, rot, s);
            if (scale) *scale = std::clamp(s, 1.0f / 1024.0f, 63.0f);   // WMOs cannot scale: theirs is dropped
        });
    };
    t.commit = [this](const std::string& label) {
        EndObjectEdit(label + (m_gizmoRiders.empty() ? "" : " with " + std::to_string(m_gizmoRiders.size()) + " inside"));
    };
    t.cancel = [this] { m_terrain.CancelObjectEdit(); };
    t.ground = [this] {
        EditObjects("Drop " + std::to_string(m_objSel.size()) + " object(s) to the ground", [&](float* pos, float*, float*) {
            if (const auto h = m_terrain.HeightAt(pos[0], pos[2])) pos[1] = *h;
        });
    };
    t.remove = [this] { DeleteSelectedObjects(); };
    return t;
}

void App::SetSpawnArea(Spawn& s) const
{
    const XMFLOAT3 e = ServerToEditor(s.x, s.y, s.z);
    if (const auto ref = m_terrain.ChunkAtGrid(int(std::floor(e.x / kChunkSize)), int(std::floor(e.z / kChunkSize))))
        if (const AdtChunk* c = m_terrain.Chunk(*ref)) { s.areaId = c->areaId; s.zoneId = m_areas.ZoneOf(c->areaId); }
}

std::optional<App::Transformable> App::SpawnTransform()
{
    std::vector<const Spawn*> sel;
    for (const Spawn& s : m_spawnView)
        if (s.kind == m_spawnKind && m_spawnSel.count(s.guid)) sel.push_back(&s);
    if (sel.empty()) return std::nullopt;
    Transformable t;
    t.yawOnly = true;
    const bool creature = m_spawnKind == SpawnKind::Creature;
    t.limits = creature ? "Creatures turn about the vertical only and have no scale of their own."
                        : "Gameobjects turn about the vertical here (tilt is not edited yet) and have no scale of their own.";
    std::vector<XMFLOAT3> points;
    for (const Spawn* s : sel) points.push_back(ServerToEditor(s->x, s->y, s->z));
    t.frame = sel.size() == 1 ? Store(Facing(points[0], sel[0]->orientation)) : CentreFrame(points);
    t.what = std::to_string(sel.size()) + " " + (creature ? "creature(s)" : "gameobject(s)");
    t.begin = [this] {
        m_spawnStart.clear();
        for (const Spawn& s : m_spawnView)
            if (s.kind == m_spawnKind && m_spawnSel.count(s.guid)) m_spawnStart[s.guid] = s;
    };
    t.preview = [this](FXMMATRIX delta) {
        m_spawnPreview.clear();
        for (const auto& [guid, s] : m_spawnStart)
        {
            Spawn n = s;
            EditorToServer(Moved(ServerToEditor(s.x, s.y, s.z), delta), n.x, n.y, n.z);
            n.orientation = Turned(s.orientation, delta);
            m_spawnPreview[guid] = n;
        }
        m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;   // models follow the preview
    };
    t.commit = [this](const std::string& label) {
        const auto preview = std::move(m_spawnPreview);
        m_spawnPreview.clear();
        m_spawnStart.clear();
        EditSpawns(label, [&](Spawn& s) {
            const auto it = preview.find(s.guid);
            if (it == preview.end()) return;
            s.x = it->second.x;
            s.y = it->second.y;
            s.z = it->second.z;
            s.orientation = it->second.orientation;
            SetSpawnArea(s);
        });
        m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;
    };
    t.cancel = [this] {
        m_spawnPreview.clear();
        m_spawnStart.clear();
        m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;
    };
    t.ground = [this, what = t.what] {
        EditSpawns("Drop " + what + " to the ground", [&](Spawn& s) {
            const XMFLOAT3 e = ServerToEditor(s.x, s.y, s.z);
            if (const auto h = m_terrain.HeightAt(e.x, e.z)) s.z = *h;
        });
    };
    t.remove = [this] { DeleteSpawns(); };
    return t;
}

std::optional<App::Transformable> App::TriggerTransform()
{
    if (!m_triggerSel || m_triggerEdit.id != m_triggerSel || !m_db.Connected()) return std::nullopt;
    Transformable t;
    const bool sphere = m_triggerEdit.Sphere();
    t.rotate = !sphere;
    t.yawOnly = true;
    t.scale = sphere ? Transformable::Scale::Uniform : Transformable::Scale::Axes;
    t.limits = sphere ? "A sphere has no turn; scaling sets its radius." : "Boxes turn about the vertical; scaling sets width, height and length.";
    t.frame = Store(TriggerFrame(m_triggerEdit));
    t.what = "trigger " + std::to_string(m_triggerSel);
    t.begin = [this] { m_triggerStart = m_triggerEdit; };
    t.preview = [this](FXMMATRIX delta) { m_triggerEdit = FromFrame(m_triggerStart, TriggerFrame(m_triggerStart) * delta); };
    t.commit = [this](const std::string& label) { CommitTrigger(m_triggerEdit.id, m_triggerEdit, m_teleportEdit, label); };
    t.cancel = [this] { m_triggerEdit = m_triggerStart; };
    t.ground = [this] {
        Trigger g = m_triggerEdit;
        const XMFLOAT3 e = ServerToEditor(g.x, g.y, g.z);
        if (const auto h = m_terrain.HeightAt(e.x, e.z)) g.z = *h + (g.Sphere() ? 0 : g.height / 2);
        CommitTrigger(g.id, g, m_teleportEdit, "Drop trigger " + std::to_string(g.id) + " to the ground");
    };
    t.remove = [this] { CommitTrigger(m_triggerSel, std::nullopt, std::nullopt, "Delete trigger " + std::to_string(m_triggerSel)); };
    return t;
}

std::optional<App::Transformable> App::PoiTransform()
{
    if (!m_poiSel || m_poiEditKey != std::pair{ int(m_poiKind), m_poiSel }) return std::nullopt;
    if (m_poiKind != PoiKind::MapIcon && !m_db.Connected()) return std::nullopt;
    const auto at = PoiPoint(m_poiEdit);
    if (!at) return std::nullopt;   // stands on ground that is not loaded
    Transformable t;
    const bool tele = m_poiKind == PoiKind::Tele;
    t.rotate = tele;
    t.yawOnly = true;
    t.limits = tele ? "Teleports turn about the vertical (the way players face); no scale." : "Points have no turn or scale.";
    t.frame = Store(tele ? Facing(*at, m_poiEdit.o) : XMMatrixTranslation(at->x, at->y, at->z));
    t.what = "point " + std::to_string(m_poiSel);
    t.begin = [this] {
        m_poiStart = m_poiEdit;
        m_poiStartPoint = PoiPoint(m_poiEdit);
    };
    t.preview = [this](FXMMATRIX delta) {
        if (!m_poiStartPoint) return;
        Poi p = m_poiStart;
        const XMFLOAT3 to = Moved(*m_poiStartPoint, delta);
        EditorToServer(to, p.x, p.y, p.z);
        // Standing on the ground without a height of its own: it keeps none unless lifted (gossip points never have one).
        const bool ground = p.kind == PoiKind::Gossip || (p.kind == PoiKind::MapIcon && m_poiStart.z == 0);
        if (ground && (p.kind == PoiKind::Gossip || std::fabs(to.y - m_poiStartPoint->y) < 0.01f)) p.z = m_poiStart.z;
        if (p.kind == PoiKind::Tele) p.o = Turned(m_poiStart.o, delta);
        if (p.kind == PoiKind::MapIcon)   // the area under it
            if (const auto ref = m_terrain.ChunkAtGrid(int(std::floor(to.x / kChunkSize)), int(std::floor(to.z / kChunkSize))))
                if (const AdtChunk* c = m_terrain.Chunk(*ref)) p.area = c->areaId;
        m_poiEdit = p;
    };
    t.commit = [this](const std::string& label) { CommitPoi(m_poiEdit.kind, m_poiEdit.id, m_poiEdit, label); };
    t.cancel = [this] { m_poiEdit = m_poiStart; };
    if (m_poiKind != PoiKind::Gossip)
        t.ground = [this] {
            Poi p = m_poiEdit;
            const XMFLOAT3 e = ServerToEditor(p.x, p.y, p.z);
            if (const auto h = m_terrain.HeightAt(e.x, e.z)) p.z = *h;
            CommitPoi(p.kind, p.id, p, "Drop point " + std::to_string(p.id) + " to the ground");
        };
    t.remove = [this] { CommitPoi(m_poiKind, m_poiSel, std::nullopt, "Delete point " + std::to_string(m_poiSel)); };
    return t;
}

std::optional<App::Transformable> App::PathPointTransform()
{
    if (!m_path || !m_path->sel || *m_path->sel >= m_path->points.size()) return std::nullopt;
    const PathPoint& p = m_path->points[*m_path->sel];
    Transformable t;
    t.rotate = false;
    t.limits = "Path points only move (Enter saves the path).";
    const XMFLOAT3 at = ServerToEditor(p.x, p.y, p.z);
    t.frame = Store(XMMatrixTranslation(at.x, at.y, at.z));
    t.what = "path point " + std::to_string(*m_path->sel + 1);
    t.begin = [this] { m_pathStart = m_path->points[*m_path->sel]; };
    t.preview = [this](FXMMATRIX delta) {
        PathPoint& q = m_path->points[*m_path->sel];
        EditorToServer(Moved(ServerToEditor(m_pathStart.x, m_pathStart.y, m_pathStart.z), delta), q.x, q.y, q.z);
    };
    t.commit = [this](const std::string&) { m_path->dirty = true; };   // the path is one change, saved with Enter
    t.cancel = [this] { if (m_path && m_path->sel && *m_path->sel < m_path->points.size()) m_path->points[*m_path->sel] = m_pathStart; };
    t.ground = [this] { DropPathPoint(); };
    t.remove = [this] { DeletePathPoint(); };
    return t;
}

// ---------------------------------------------------------------------------------------------- shared controls

bool App::UpdateGizmo(const ImVec2& origin, const ImVec2& size)
{
    const auto t = ActiveTransform();
    const bool allowed = t && (m_gizmo == Gizmo::Move || (m_gizmo == Gizmo::Rotate && t->rotate) ||
                               (m_gizmo == Gizmo::Scale && t->scale != Transformable::Scale::None));
    if (!allowed)
    {
        if (m_gizmoActive)
        {
            if (m_gizmoCancel) m_gizmoCancel();
            m_gizmoActive = false;
        }
        return false;
    }
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(false);
    XMFLOAT4X4 view, proj;
    XMStoreFloat4x4(&view, m_camera.View());
    XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), size.x / size.y, 1.0f, 6000.0f));
    if (!m_gizmoActive) m_gizmoMatrix = t->frame;
    const XMFLOAT4X4 before = m_gizmoMatrix;

    const ImGuizmo::OPERATION op = m_gizmo == Gizmo::Move     ? ImGuizmo::TRANSLATE
                                 : m_gizmo == Gizmo::Rotate   ? (t->yawOnly ? ImGuizmo::ROTATE_Y : ImGuizmo::ROTATE)
                                 : t->scale == Transformable::Scale::Uniform ? ImGuizmo::SCALEU : ImGuizmo::SCALE;
    const bool snapping = m_gizmoSnap != ImGui::GetIO().KeyCtrl;
    const float snapValue = m_gizmo == Gizmo::Move ? m_snapMove : m_gizmo == Gizmo::Rotate ? m_snapRotate : m_snapScale;
    const float snap[3] = { snapValue, snapValue, snapValue };
    // Things that turn about the vertical only keep upright handles; scaling a box works along its own axes.
    const bool local = m_gizmoLocal || (m_gizmo == Gizmo::Scale && t->scale == Transformable::Scale::Axes);
    ImGuizmo::Manipulate(&view._11, &proj._11, op, local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &m_gizmoMatrix._11, nullptr, snapping ? snap : nullptr);

    const bool dragging = ImGuizmo::IsUsing();
    if (dragging && !m_gizmoActive)
    {
        m_gizmoStart = before;
        t->begin();
        m_gizmoCancel = t->cancel;
    }
    if (dragging) t->preview(XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_gizmoStart)) * XMLoadFloat4x4(&m_gizmoMatrix));
    if (!dragging && m_gizmoActive)
    {
        t->commit(std::string(m_gizmo == Gizmo::Move ? "Move " : m_gizmo == Gizmo::Rotate ? "Rotate " : "Scale ") + t->what);
        m_gizmoCancel = nullptr;
    }
    m_gizmoActive = dragging;
    return dragging || ImGuizmo::IsOver();
}

void App::ApplyTransform(const Transformable& t, FXMMATRIX delta, const std::string& label)
{
    t.begin();
    t.preview(delta);
    t.commit(label);
}

void App::MoveSelectionTo(const XMFLOAT3& at)
{
    if (const auto t = ActiveTransform())
        ApplyTransform(*t, XMMatrixTranslation(at.x - t->frame._41, at.y - t->frame._42, at.z - t->frame._43), "Move " + t->what);
}

void App::TransformKeys()
{
    if (!TransformTool() || m_gizmoActive || ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) m_gizmo = Gizmo::Move;
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) m_gizmo = Gizmo::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) m_gizmo = Gizmo::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) m_gizmoLocal = !m_gizmoLocal;
    const auto t = ActiveTransform();
    if (!t) return;
    const bool shift = ImGui::GetIO().KeyShift;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_PageDown))
    {
        const float step = (ImGui::IsKeyPressed(ImGuiKey_PageUp) ? 1.0f : -1.0f) * (shift ? 0.1f : 1.0f);
        ApplyTransform(*t, XMMatrixTranslation(0, step, 0), (step > 0 ? "Raise " : "Lower ") + t->what);
    }
    const bool up = ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd);
    const bool down = ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract);
    if ((up || down) && t->scale != Transformable::Scale::None)
    {
        // About the handle frame, as the scale handle does.
        const float f = up ? (shift ? 1.02f : 1.1f) : 1.0f / (shift ? 1.02f : 1.1f);
        const XMMATRIX frame = XMLoadFloat4x4(&t->frame);
        ApplyTransform(*t, XMMatrixInverse(nullptr, frame) * XMMatrixScaling(f, f, f) * frame, "Scale " + t->what);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && t->ground) t->ground();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && t->remove) t->remove();
}

std::string App::TransformHint() const
{
    return "1 move  2 rotate  3 scale  X world/local  Ctrl snap  PgUp/PgDn height  G ground  Alt+click move here  Del";
}

bool TransformSelfTest()
{
    // Facing frames: a spawn facing o, turned a about the vertical, faces o + a, and its frame's -z is its facing line.
    for (const float o : { 0.0f, 1.0f, 4.0f })
        for (const float a : { 0.5f, -2.0f })
        {
            float want = std::fmod(o + a + 2 * kTwoPi, kTwoPi);
            if (std::fabs(Turned(o, XMMatrixRotationY(a)) - want) > 1e-4f) return false;
            const XMVECTOR line = XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), Facing({ 0, 0, 0 }, o));
            if (std::fabs(XMVectorGetX(line) + std::sin(o)) > 1e-4f || std::fabs(XMVectorGetZ(line) + std::cos(o)) > 1e-4f) return false;
        }
    // Triggers survive their frame; a box turned and scaled about its centre keeps its centre.
    Trigger box{ 7, 0, 100, 200, 30, 0, 10, 4, 6, 0.5f };
    const Trigger same = FromFrame(box, TriggerFrame(box));
    if (std::fabs(same.x - box.x) > 1e-3f || std::fabs(same.y - box.y) > 1e-3f || std::fabs(same.z - box.z) > 1e-3f ||
        std::fabs(same.length - box.length) > 1e-3f || std::fabs(same.width - box.width) > 1e-3f || std::fabs(same.yaw - box.yaw) > 1e-4f)
        return false;
    const XMMATRIX frame = TriggerFrame(box);
    // Scaled in its own axes (as the scale handle does), then turned about its centre in the world (as the rotate handle does).
    const XMVECTOR centre = frame.r[3];
    const XMMATRIX about = XMMatrixTranslationFromVector(XMVectorNegate(centre)) * XMMatrixRotationY(1.0f) * XMMatrixTranslationFromVector(centre);
    const Trigger turned = FromFrame(box, XMMatrixScaling(2, 2, 2) * frame * about);
    if (std::fabs(turned.length - 20) > 1e-3f || std::fabs(turned.height - 12) > 1e-3f || std::fabs(turned.yaw - 1.5f) > 1e-4f ||
        std::fabs(turned.x - box.x) > 1e-3f || std::fabs(turned.y - box.y) > 1e-3f)
        return false;
    // The box's corners follow the frame: the +length face centre is where the overlay draws the arrow from.
    const XMVECTOR face = XMVector3TransformCoord(XMVectorSet(0, 0, -0.5f, 1), frame);
    float fx, fy, fz;
    EditorToServer({ XMVectorGetX(face), XMVectorGetY(face), XMVectorGetZ(face) }, fx, fy, fz);
    return std::fabs(fx - (box.x + std::cos(box.yaw) * 5)) < 1e-2f && std::fabs(fy - (box.y + std::sin(box.yaw) * 5)) < 1e-2f;
}
