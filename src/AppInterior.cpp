// The building editor: one WMO per tab in a window of its own, the building alone in its view (at the origin, in its
// own axes). Its doodads (the furniture of its doodad sets) and its groups (parts, cut and pasted between buildings)
// are edited there. The WMO file itself changes (a project copy only one placement uses, or the original in place), so
// the edit shows wherever that file stands, the map included. Selection, handles and keys are the shared ones
// (AppTransform.cpp).
#include "App.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <random>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
const ImVec4 kAccent{ 0.30f, 0.62f, 1.00f, 1.00f };

std::string FileOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool Same(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

XMFLOAT4X4 Store(FXMMATRIX m)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, m);
    return out;
}

XMFLOAT3 Origin(FXMMATRIX m)
{
    XMFLOAT3 p;
    XMStoreFloat3(&p, m.r[3]);
    return p;
}

const XMMATRIX kWowToEditor = XMMatrixSet(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1);   // v_editor = v_wow * C

/// A point of the building's own (WoW) axes in the view (editor axes, the building at the origin), and back.
XMVECTOR ToView(const float p[3]) { return XMVectorSet(p[0], p[2], -p[1], 1); }
XMFLOAT3 ToLocal(const XMFLOAT3& e) { return { e.x, -e.z, e.y }; }

void Grow(WmoBox& all, const WmoBox& b, bool& first)
{
    if (first) { all = b; first = false; return; }
    for (int k = 0; k < 3; ++k) { all[k] = std::min(all[k], b[k]); all[3 + k] = std::max(all[3 + k], b[3 + k]); }
}

float RandomUnit()
{
    static std::mt19937 rng{ std::random_device{}() };
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

/// The view's camera: it looks at `center` from `distance` away, along yaw and pitch.
XMVECTOR Back(float yaw, float pitch) { return XMVectorSet(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw), 0); }

const int kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
}

// ---------------------------------------------------------------------------------------------- tabs

App::Interior* App::BuildingTab(const std::string& wmo)
{
    for (size_t i = 0; i < m_buildings.size(); ++i)
        if (Same(m_buildings[i]->wmo, wmo) || Same(m_buildings[i]->origin, wmo))
        {
            m_buildingTab = i;
            return m_buildings[i].get();
        }
    auto in = std::make_unique<Interior>();
    in->serial = ++m_buildingSerial;
    in->wmo = in->origin = wmo;
    if (!LoadInterior(*in)) return nullptr;
    if (m_armed && m_armed->wmo) m_armed.reset();   // a building cannot go inside one
    m_buildings.push_back(std::move(in));
    m_buildingTab = m_buildings.size() - 1;
    PreviewInterior(*m_buildings.back());   // drawn in the order the tab's ids name
    return m_buildings.back().get();
}

void App::OpenBuilding(uint32_t wmoUid)
{
    const auto w = m_terrain.FindWmo(wmoUid);
    if (!w) return;
    Interior* in = BuildingTab(w->model);
    if (!in) return;
    in->wmoUid = wmoUid;
    if (w->doodadSet < in->live.sets.size() && w->doodadSet != in->set)   // the furniture this placement shows
    {
        in->set = w->doodadSet;
        ShowBuilding(*in);
    }
    m_showBuildings = m_buildingFront = m_buildingSelect = true;
}

void App::OpenBuildingFile(const std::string& wmo)
{
    if (!BuildingTab(wmo)) return;
    m_showBuildings = m_buildingFront = m_buildingSelect = true;
}

bool App::LoadInterior(Interior& in)
{
    if (const auto* e = m_wmoEdits.Find(in.wmo)) in.live = e->doodads;
    else
    {
        std::string error;
        const auto root = m_mpq.Read(in.wmo);
        auto read = root ? ReadWmoDoodads(*root, {}, error) : std::nullopt;
        if (!read) { Log("Cannot edit %s: %s", FileOf(in.wmo).c_str(), root ? error.c_str() : "file not found"); return false; }
        in.live = std::move(*read);
    }
    if (in.set >= in.live.sets.size()) in.set = 0;
    std::erase_if(in.sel, [&](uint32_t id) { return !in.live.doodads.count(id); });
    std::erase_if(in.groupSel, [&](uint32_t id) { return !in.live.slots.count(id); });
    return true;
}

void App::CloseBuilding(size_t tab)
{
    if (tab >= m_buildings.size()) return;
    Interior& in = *m_buildings[tab];
    if (in.gizmo.active && in.gizmo.cancel) in.gizmo.cancel();
    m_models.RemoveTile(in.TileKey());
    m_models.RemoveTile(in.TileKey() - 1);
    m_buildings.erase(m_buildings.begin() + std::ptrdiff_t(tab));
    if (m_buildingTab >= m_buildings.size()) m_buildingTab = m_buildings.empty() ? 0 : m_buildings.size() - 1;
}

void App::ShowBuilding(Interior& in)
{
    m_models.RemoveTile(in.TileKey());
    Adt adt;
    adt.wmos.push_back({ in.wmo, { 0, 0, 0 }, { 0, 90, 0 }, {}, {}, in.Uid(), uint16_t(in.set) });   // rotation 90: no turn
    m_models.AddTile(in.TileKey(), adt, m_mpq, in.Layer());
}

std::optional<WmoPlacement> App::InteriorPlacement(const Interior& in) const
{
    if (!in.wmoUid) return std::nullopt;
    const auto w = m_terrain.FindWmo(in.wmoUid);
    if (!w || (!Same(w->model, in.wmo) && !Same(w->model, in.origin))) return std::nullopt;
    return w;
}

void App::MakeInteriorEditable(Interior& in, bool inPlace)
{
    const std::string from = in.wmo;
    const std::string name = inPlace ? from : WmoCopyName(from, [this](const std::string& n) { return m_wmoEdits.Find(n) || m_mpq.HasOwn(n); });
    std::string error;
    auto create = m_wmoEdits.MakeCreate(name, from, error);
    if (!create) { Log("Cannot edit %s: %s", FileOf(from).c_str(), error.c_str()); return; }
    m_wmoEdits.Apply(*create);
    const auto* e = m_wmoEdits.Find(name);
    if (!e) { Log("Cannot edit %s: its groups could not be read.", FileOf(from).c_str()); return; }
    const std::string label = create->label;
    std::vector<Change> parts{ std::move(*create) };
    const bool placed = InteriorPlacement(in).has_value();
    if (!inPlace && placed)   // the placement uses the copy: one undo step with the copy
    {
        m_terrain.BeginObjectEdit({ ObjectRef{ true, in.wmoUid } });
        m_terrain.PreviewObjectEdit([](DoodadPlacement&) {}, [&](WmoPlacement& p) { p.model = name; });
        if (auto moved = m_terrain.EndObjectEdit(label)) parts.push_back(std::move(*moved));
    }
    m_store.Commit(std::move(parts), label);
    Log("%s", label.c_str());
    in.wmo = name;
    in.live = e->doodads;
    PreviewInterior(in);
    if (!inPlace && !placed)   // a copy nothing stands on yet: ready to place on the map
    {
        m_armed = Armed{ name, true };
        m_tool = Tool::Objects;
        Log("%s is picked for placing: click the map to put it there.", FileOf(name).c_str());
    }
}

void App::PreviewInterior(Interior& in)
{
    std::vector<ModelMesh::Doodad> doodads;
    std::vector<ModelMesh::DoodadSet> sets;
    MeshDoodads(in.live, doodads, sets, in.ids);
    if (!m_models.SetWmoDoodads(in.wmo, doodads, sets))   // not loaded yet: the tab's own tile loads it
    {
        ShowBuilding(in);
        m_models.SetWmoDoodads(in.wmo, std::move(doodads), std::move(sets));
    }
    ReloadTilesWith(in.wmo);
}

void App::CommitInterior(Interior& in, const std::string& label)
{
    const auto* e = m_wmoEdits.Find(in.wmo);
    if (!e) { PreviewInterior(in); return; }
    auto change = m_wmoEdits.MakeEdit(e->name, e->doodads, in.live, label);
    if (!change) return;
    m_wmoEdits.Apply(*change);
    Log("%s", change->label.c_str());
    m_store.Commit(std::move(*change));
}

void App::ReloadTilesWith(const std::string& wmo)
{
    for (const auto& [key, tile] : m_terrain.Tiles())
        if (m_models.HasTile(key) && std::any_of(tile.adt.wmos.begin(), tile.adt.wmos.end(), [&](const WmoPlacement& p) { return Same(p.model, wmo); }))
        {
            m_models.RemoveTile(key);
            m_models.AddTile(key, tile.adt, m_mpq);
        }
    if (const auto& global = m_terrain.GlobalWmo(); global && Same(global->model, wmo)) m_models.RemoveTile(kGlobalWmoKey);   // added back by the frame
    for (auto& b : m_buildings)
        if (Same(b->wmo, wmo)) ShowBuilding(*b);
}

void App::UpdateEditedWmos()
{
    if (m_wmoEdits.TakeFilesChanged())
    {
        ++m_assetsVersion;   // the project's file layer lists the new (or removed) files
        UpdateProjectFiles();
    }
    bool any = false;
    for (const std::string& name : m_wmoEdits.TakeRebuilt())   // groups moved, added or cut: read the building again
    {
        any = true;
        for (int key : m_models.Forget(name))
            if (const auto it = m_terrain.Tiles().find(key); it != m_terrain.Tiles().end()) m_models.AddTile(key, it->second.adt, m_mpq);
        for (auto& b : m_buildings)
            if (Same(b->wmo, name)) ShowBuilding(*b);
        m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;   // a gameobject may show it
    }
    for (const std::string& name : m_wmoEdits.TakeChanged())
    {
        any = true;
        if (!m_project) continue;
        WmoDoodads now;
        if (const auto* e = m_wmoEdits.Find(name)) now = e->doodads;
        else if (const auto root = m_mpq.Read(name))   // no longer edited: as the game has it
        {
            std::string error;
            auto read = ReadWmoDoodads(*root, {}, error);
            if (!read) continue;
            now = std::move(*read);
        }
        else continue;
        std::vector<ModelMesh::Doodad> doodads;
        std::vector<ModelMesh::DoodadSet> sets;
        std::vector<uint32_t> ids;
        MeshDoodads(now, doodads, sets, ids);
        for (auto& b : m_buildings)
            if (Same(b->wmo, name) && !b->gizmo.active)   // undo, redo: the tab shows what is saved
            {
                b->live = now;
                b->ids = ids;
                std::erase_if(b->sel, [&](uint32_t id) { return !now.doodads.count(id); });
                std::erase_if(b->groupSel, [&](uint32_t id) { return !now.slots.count(id); });
                if (b->set >= now.sets.size()) b->set = 0;
            }
        m_models.SetWmoDoodads(name, std::move(doodads), std::move(sets));
        ReloadTilesWith(name);
    }
    if (!any) return;
    // A tab follows its placement to another file (a copy made, undone or redone), and leaves a copy that is gone.
    for (size_t i = 0; i < m_buildings.size();)
    {
        Interior& b = *m_buildings[i];
        std::string to;
        if (const auto w = b.wmoUid && !b.gizmo.active ? m_terrain.FindWmo(b.wmoUid) : std::nullopt;
            w && !Same(w->model, b.wmo) && (Same(w->model, b.origin) || m_wmoEdits.Find(w->model)))
            to = w->model;
        else if (!b.gizmo.active && !m_wmoEdits.Find(b.wmo) && !Same(b.wmo, b.origin) && !m_mpq.Read(b.wmo))
            to = b.origin;
        if (!to.empty())
        {
            const std::string was = b.wmo;
            b.wmo = to;
            if (!LoadInterior(b)) { b.wmo = was; CloseBuilding(i); continue; }
            PreviewInterior(b);
        }
        ++i;
    }
}

// ---------------------------------------------------------------------------------------------- the view

std::optional<WmoDoodads::Doodad> App::InteriorDoodadAt(const Interior& in, const XMFLOAT3& point) const
{
    if (!m_armed) return std::nullopt;
    WmoDoodads::Doodad d;
    d.model = m_armed->path;
    const XMFLOAT3 local = ToLocal(point);
    d.pos[0] = local.x;
    d.pos[1] = local.y;
    d.pos[2] = local.z;
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(d.rot), XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), XMConvertToRadians(m_placeYaw)));
    d.scale = m_placeScale;
    d.set = in.set;
    // Lit like the nearest doodad of the same set (the colour tints interior doodads).
    float nearest = INFINITY;
    for (const auto& [id, o] : in.live.doodads)
    {
        const float gap = std::pow(o.pos[0] - d.pos[0], 2.0f) + std::pow(o.pos[1] - d.pos[1], 2.0f) + std::pow(o.pos[2] - d.pos[2], 2.0f);
        if (o.set == d.set && gap < nearest) { nearest = gap; d.color = o.color; d.flags = o.flags; }
    }
    return d;
}

void App::BuildingHover(Interior& in, FXMVECTOR p0, FXMVECTOR dir)
{
    // Where things would go: the building's surface under the cursor, else its floor plane (y = 0, the building's origin).
    float ground = 5000.0f;
    in.point.reset();
    if (const float dy = XMVectorGetY(dir); dy < -1e-4f)
        if (const float t = -XMVectorGetY(p0) / dy; t > 0 && t < ground)
        {
            ground = t;
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVectorAdd(p0, XMVectorScale(dir, t)));
            in.point = p;
        }
    // Walls hide what is behind them: the nearest building surface limits the doodad pick.
    ModelRenderer::DrawSettings surfaces;
    surfaces.doodads = false;
    surfaces.attached = false;
    float wall = ground;
    if (const auto hit = m_models.Pick(p0, dir, ground, surfaces, in.Layer()); hit && hit->wmo) wall = hit->distance;
    in.hover.reset();
    in.groupHover.reset();
    if (in.groups)
    {
        // The group under the cursor: drawn index -> slot (the building as last written, or as the game has it).
        if (const auto hit = m_models.PickWmoGroup(in.Uid(), p0, dir, ground, in.Layer()))
        {
            const auto* e = m_wmoEdits.Find(in.wmo);
            const uint32_t slot = e ? (hit->group < e->order.size() ? e->order[hit->group] : UINT32_MAX) : hit->group;
            if (in.live.slots.count(slot)) in.groupHover = slot;
            wall = std::min(wall, hit->distance);
        }
    }
    else if (const auto hit = m_models.PickDoodad(in.Uid(), p0, dir, wall + 0.05f, in.Layer()))
    {
        if (hit->index < in.ids.size()) in.hover = in.ids[hit->index];
        wall = std::min(wall, hit->distance);   // furniture is somewhere to stand things too
    }
    if (wall < ground)
    {
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVectorAdd(p0, XMVectorScale(dir, wall)));
        in.point = p;
    }
}

void App::BuildingOutlines(const Interior& in, FXMMATRIX viewProj, const ImVec2& origin, const ImVec2& size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, { origin.x + size.x, origin.y + size.y }, true);
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 color) {
        // Clipped at the near plane, so a box around the camera still draws.
        XMVECTOR pa = XMVector4Transform(XMVectorSet(a.x, a.y, a.z, 1), viewProj), pb = XMVector4Transform(XMVectorSet(b.x, b.y, b.z, 1), viewProj);
        constexpr float kNear = 1e-3f;
        const float wa = XMVectorGetW(pa), wb = XMVectorGetW(pb);
        if (wa < kNear && wb < kNear) return;
        if (wa < kNear) pa = XMVectorLerp(pa, pb, (kNear - wa) / (wb - wa));
        else if (wb < kNear) pb = XMVectorLerp(pb, pa, (kNear - wb) / (wa - wb));
        auto screen = [&](FXMVECTOR p) {
            const float w = XMVectorGetW(p);
            return ImVec2(origin.x + (XMVectorGetX(p) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(p) / w * 0.5f) * size.y);
        };
        dl->AddLine(screen(pa), screen(pb), color, 1.5f);
    };
    auto box = [&](const XMFLOAT3 (&c)[8], ImU32 color) {
        for (const auto& e : kEdges) line(c[e[0]], c[e[1]], color);
    };
    const ImU32 picked = IM_COL32(255, 191, 51, 255), hovered = IM_COL32(255, 255, 255, 150);
    XMFLOAT3 c[8];
    if (in.groups)
    {
        // Group boxes; the selected ones where the handle has them while it is dragged.
        const XMMATRIX drag = in.groupDrag ? XMLoadFloat4x4(&*in.groupDrag) : XMMatrixIdentity();
        const std::string base = InteriorBase(in);
        auto group = [&](uint32_t id, ImU32 color) {
            const auto s = in.live.slots.find(id);
            if (s == in.live.slots.end()) return;
            const auto b = m_wmoEdits.SlotBox(base, s->second);
            if (!b) return;
            for (int i = 0; i < 8; ++i)
            {
                const float p[3] = { (*b)[i & 1 ? 3 : 0], (*b)[i & 2 ? 4 : 1], (*b)[i & 4 ? 5 : 2] };
                XMStoreFloat3(&c[i], XMVector3TransformCoord(ToView(p), in.groupSel.count(id) ? drag : XMMatrixIdentity()));
            }
            box(c, color);
        };
        for (uint32_t id : in.groupSel) group(id, picked);
        if (in.groupHover && !in.groupSel.count(*in.groupHover)) group(*in.groupHover, hovered);
    }
    else
    {
        auto doodad = [&](uint32_t id, ImU32 color) {
            const auto it = std::find(in.ids.begin(), in.ids.end(), id);
            if (it != in.ids.end() && m_models.DoodadCorners(in.Uid(), uint32_t(it - in.ids.begin()), c, in.Layer())) box(c, color);
        };
        for (uint32_t id : in.sel) doodad(id, picked);
        if (in.hover && !in.sel.count(*in.hover)) doodad(*in.hover, hovered);
    }
    dl->PopClipRect();
}

bool App::FrameBuilding(Interior& in)
{
    // The selection's boxes, else the whole building.
    XMFLOAT3 lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
    auto grow = [&](const XMFLOAT3 (&c)[8]) {
        for (const XMFLOAT3& p : c)
        {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
    };
    XMFLOAT3 c[8];
    if (in.groups)
    {
        const std::string base = InteriorBase(in);
        for (uint32_t id : in.groupSel)
            if (const auto s = in.live.slots.find(id); s != in.live.slots.end())
                if (const auto b = m_wmoEdits.SlotBox(base, s->second))
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        const float p[3] = { (*b)[i & 1 ? 3 : 0], (*b)[i & 2 ? 4 : 1], (*b)[i & 4 ? 5 : 2] };
                        XMStoreFloat3(&c[i], ToView(p));
                    }
                    grow(c);
                }
    }
    else
        for (uint32_t id : in.sel)
            if (const auto it = std::find(in.ids.begin(), in.ids.end(), id); it != in.ids.end())
                if (m_models.DoodadCorners(in.Uid(), uint32_t(it - in.ids.begin()), c, in.Layer())) grow(c);
    if (lo.x > hi.x && m_models.Corners(true, in.Uid(), c, in.Layer())) grow(c);
    if (lo.x > hi.x) return false;
    in.scene.Frame(lo, hi, 1.0f);
    in.scene.distance = std::max(in.scene.distance * 0.65f, 3.0f);   // a 60 degree view: nearer than the viewers' 40
    return true;
}

void App::DrawBuildingView(Interior& in, const ImVec2& size)
{
    ScenePreview& v = in.scene;
    const UINT w = UINT(std::max(size.x, 8.0f)), h = UINT(std::max(size.y, 8.0f));
    if (!SceneTarget(v, w, h)) return;
    if (!in.framed) in.framed = FrameBuilding(in);
    const ImGuiIO& io = ImGui::GetIO();

    // The scene: this building alone on its own layer, with the fixed light of the viewers.
    XMVECTOR center = XMVectorSet(v.center[0], v.center[1], v.center[2], 0);
    XMVECTOR eye = XMVectorAdd(center, XMVectorScale(Back(v.yaw, v.pitch), v.distance));
    const XMMATRIX view = XMMatrixLookAtRH(eye, center, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), float(w) / float(h), 0.1f, 5000.0f);
    const XMMATRIX viewProj = view * proj;
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDsv;
        m_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
        UINT viewports = 1;
        D3D11_VIEWPORT oldVp{};
        m_context->RSGetViewports(&viewports, &oldVp);
        const float bg[4] = { v.background[0], v.background[1], v.background[2], 1 };
        const D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        m_context->OMSetRenderTargets(1, v.rtv.GetAddressOf(), v.dsv.Get());
        m_context->RSSetViewports(1, &vp);
        m_context->ClearRenderTargetView(v.rtv.Get(), bg);
        m_context->ClearDepthStencilView(v.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        ModelRenderer::DrawSettings settings;
        settings.layer = in.Layer();
        settings.distance = 5000.0f;
        settings.details = settings.previews = settings.sceneLit = false;
        XMFLOAT3 at;
        XMStoreFloat3(&at, eye);
        m_models.Draw(viewProj, at, settings);
        m_context->OMSetRenderTargets(1, oldRtv.GetAddressOf(), oldDsv.Get());
        if (viewports) m_context->RSSetViewports(1, &oldVp);
    }

    // Shown without blending, like the main viewport: the alpha channel holds texture masks, not coverage. The handles
    // run before the view's button: ImGuizmo only takes a click when no item is hovered.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd* cmd) {
        static_cast<ID3D11DeviceContext*>(cmd->UserCallbackData)->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }, m_context);
    dl->AddImage(ImTextureID(intptr_t(v.srv.Get())), origin, { origin.x + size.x, origin.y + size.y });
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    const bool gizmoHot = RunGizmo(BuildingTransform(), in.gizmo, view, proj, false, origin, size, "building");
    if (gizmoHot) ImGui::Dummy(size);
    else ImGui::InvisibleButton("##building", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = gizmoHot || ImGui::IsItemHovered();

    // Camera: right drag looks around (the eye stays), WASD / QE fly, middle drag orbits (Shift: pans), the wheel zooms
    // towards the centre (while looking: sets the fly speed). A right click that did not turn the view stops placing.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) in.looking = true;
    if (in.looking && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16 && m_armed)
    {
        m_armed.reset();
        Log("Stopped placing.");
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) in.looking = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) in.orbiting = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) in.orbiting = false;
    if (in.looking)
    {
        v.yaw += io.MouseDelta.x * 0.005f;
        v.pitch = std::clamp(v.pitch + io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
        center = XMVectorSubtract(eye, XMVectorScale(Back(v.yaw, v.pitch), v.distance));
    }
    else if (in.orbiting && io.KeyShift)
    {
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(center, eye), XMVectorSet(0, 1, 0, 0)));
        const XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, XMVectorSubtract(center, eye)));
        const float k = v.distance * 0.0015f;
        center = XMVectorSubtract(center, XMVectorScale(XMVectorSubtract(XMVectorScale(right, io.MouseDelta.x), XMVectorScale(up, io.MouseDelta.y)), k));
    }
    else if (in.orbiting)
    {
        v.yaw += io.MouseDelta.x * 0.01f;
        v.pitch = std::clamp(v.pitch + io.MouseDelta.y * 0.01f, -1.55f, 1.55f);
    }
    if (hovered && io.MouseWheel != 0)
    {
        if (in.looking) in.speed = std::clamp(in.speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 2.0f, 500.0f);
        else v.distance = std::clamp(v.distance * std::pow(0.88f, io.MouseWheel), 0.5f, 3000.0f);
    }
    if ((hovered || in.looking || ImGui::IsWindowFocused()) && !io.WantTextInput && !io.KeyCtrl)
    {
        const float step = in.speed * (io.KeyShift ? 5.0f : 1.0f) * io.DeltaTime;
        const XMVECTOR fwd = XMVectorNegate(Back(v.yaw, v.pitch));
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(fwd, XMVectorSet(0, 1, 0, 0)));
        XMVECTOR move = XMVectorZero();
        if (ImGui::IsKeyDown(ImGuiKey_W)) move = XMVectorAdd(move, fwd);
        if (ImGui::IsKeyDown(ImGuiKey_S)) move = XMVectorSubtract(move, fwd);
        if (ImGui::IsKeyDown(ImGuiKey_D)) move = XMVectorAdd(move, right);
        if (ImGui::IsKeyDown(ImGuiKey_A)) move = XMVectorSubtract(move, right);
        if (ImGui::IsKeyDown(ImGuiKey_E)) move = XMVectorAdd(move, XMVectorSet(0, 1, 0, 0));
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move = XMVectorSubtract(move, XMVectorSet(0, 1, 0, 0));
        center = XMVectorAdd(center, XMVectorScale(move, step));
    }
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(v.center), center);

    // Picking under the mouse, with this frame's camera (the one drawn).
    if (hovered && !in.looking && !in.orbiting)
    {
        const float mx = (io.MousePos.x - origin.x) / size.x * 2 - 1, my = 1 - (io.MousePos.y - origin.y) / size.y * 2;
        const XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
        const XMVECTOR p0 = XMVector3TransformCoord(XMVectorSet(mx, my, 0, 1), inv);
        const XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(XMVector3TransformCoord(XMVectorSet(mx, my, 1, 1), inv), p0));
        if (!gizmoHot) BuildingHover(in, p0, dir);
    }
    else
    {
        in.hover.reset();
        in.groupHover.reset();
    }

    // The armed catalog doodad follows the cursor, as it will stand in the building.
    const bool editable = m_wmoEdits.Find(in.wmo) != nullptr;
    m_models.RemoveTile(in.TileKey() - 1);
    if (m_armed && !m_armed->wmo && !in.groups && hovered && in.point)
        if (const auto d = InteriorDoodadAt(in, *in.point)) m_models.AddModel(in.TileKey() - 1, ModelLook{ d->model }, DoodadMatrix(*d), d->scale, 0, m_mpq, in.Layer());

    // Clicks pick (Shift adds, Ctrl removes), Alt+click moves the selection there, an armed catalog doodad is added.
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        in.press[0] = io.MousePos.x;
        in.press[1] = io.MousePos.y;
    }
    const bool click = ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                       std::fabs(io.MousePos.x - in.press[0]) + std::fabs(io.MousePos.y - in.press[1]) <= 5;
    std::set<uint32_t>& picked = in.groups ? in.groupSel : in.sel;
    if (click && m_armed && !m_armed->wmo && !in.groups && in.point)
    {
        if (!editable) Log("Make this building editable first (a copy or the original) to add to it.");
        else if (const auto d = InteriorDoodadAt(in, *in.point))
        {
            const uint32_t id = in.live.NextId();
            in.live.doodads[id] = *d;
            CommitInterior(in, "Add " + FileOf(d->model) + " to " + FileOf(in.wmo));
            in.sel = { id };
            if (!io.KeyShift) m_armed.reset();
            if (m_placeRandomYaw) m_placeYaw = RandomUnit() * 360.0f;
        }
    }
    else if (click && io.KeyAlt && !picked.empty() && in.point)
        MoveSelectionTo(*in.point, true);
    else if (click && !m_armed)
    {
        const std::optional<uint32_t> hit = in.groups ? in.groupHover : in.hover;
        if (!hit) { if (!io.KeyShift && !io.KeyCtrl) picked.clear(); }
        else if (io.KeyCtrl) picked.erase(*hit);
        else if (io.KeyShift) picked.insert(*hit);
        else picked = { *hit };
    }

    BuildingOutlines(in, viewProj, origin, size);
    const std::string hint = in.groups ? "Groups: click picks (Shift adds, Ctrl removes)  Ctrl+C / X / V  " + TransformHint()
                                       : "Doodads: click picks (Shift adds, Ctrl removes)  " + TransformHint();
    dl->AddText({ origin.x + 10, origin.y + 8 }, IM_COL32(220, 220, 220, 230), hint.c_str());
    dl->AddText({ origin.x + 10, origin.y + size.y - ImGui::GetTextLineHeight() - 8 }, IM_COL32(170, 170, 170, 200),
                "Right drag look  WASD / QE fly (Shift fast)  middle drag orbit (Shift pan)  wheel zoom  F frame");
}

// ---------------------------------------------------------------------------------------------- the window

void App::DrawBuildings()
{
    m_buildingFocused = false;
    if (!m_showBuildings) return;
    ImGui::SetNextWindowSize({ 1180, 720 }, ImGuiCond_FirstUseEver);
    if (m_buildingFront) ImGui::SetNextWindowFocus();
    const unsigned int space = PanelHost("Building editor", &m_showBuildings, [](unsigned int node) {
        ImGuiID rest = node;
        const ImGuiID left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.30f, nullptr, &rest);
        ImGui::DockBuilderDockWindow("Building parts", left);
        ImGui::DockBuilderDockWindow("Building view", rest);
    });
    if (m_buildingFront) ImGui::SetNextWindowFocus();
    m_buildingFront = false;
    if (!m_showBuildings) return;

    std::optional<size_t> close;
    if (BeginPanel("Building view", space))
    {
        m_buildingFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        if (m_buildings.empty())
        {
            m_buildingSelect = false;
            ImGui::TextColored(kQuiet, "No building open.");
            ImGui::TextWrapped("Open one from the map (Objects tool: select a building, then Edit building in the Object window) "
                               "or from the Catalog (right-click a building).");
        }
        else if (ImGui::BeginTabBar("##buildings", ImGuiTabBarFlags_FittingPolicyScroll))
        {
            for (size_t i = 0; i < m_buildings.size(); ++i)
            {
                const Interior& b = *m_buildings[i];
                bool open = true;
                const ImGuiTabItemFlags flags = m_buildingSelect && i == m_buildingTab ? ImGuiTabItemFlags_SetSelected : 0;
                const bool shown = ImGui::BeginTabItem((FileOf(b.wmo) + "###building" + std::to_string(b.serial)).c_str(), &open, flags);
                ImGui::SetItemTooltip("%s", b.wmo.c_str());
                if (shown)
                {
                    if (i == m_buildingTab) m_buildingSelect = false;   // the one asked for is up
                    else if (!m_buildingSelect) m_buildingTab = i;
                    ImGui::EndTabItem();
                }
                if (!open) close = i;
            }
            ImGui::EndTabBar();
        }
        if (Interior* in = Building(); in && !close)
        {
            const auto* edited = m_wmoEdits.Find(in->wmo);
            ImGui::TextColored(kAccent, "%s", FileOf(in->wmo).c_str());
            ImGui::SameLine();
            ImGui::TextColored(kQuiet, "%s", !edited ? "the game's own file: not editable yet"
                                             : Same(edited->from, edited->name) ? "the original, edited in place"
                                                                                : ("project copy of " + FileOf(edited->from)).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Frame  F")) FrameBuilding(*in);
            DrawBuildingView(*in, ImGui::GetContentRegionAvail());
        }
    }
    ImGui::End();
    if (close) CloseBuilding(*close);

    if (BeginPanel("Building parts", space))
    {
        m_buildingFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        if (Interior* in = Building())
        {
            DrawTransformBar(ImGui::GetContentRegionAvail().x, true);
            DrawInteriorPanel(*in);
        }
        else ImGui::TextColored(kQuiet, "No building open.");
    }
    ImGui::End();
}

void App::BuildingKeys()
{
    Interior* in = Building();
    if (!in) return;
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (in->groups)   // a building's groups: copy, cut, paste them
    {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, global)) CopyGroups(*in, false);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, global)) CopyGroups(*in, true);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, global)) PasteGroups(*in);
    }
    if (ImGui::GetIO().KeyCtrl) return;
    TransformKeys(true);
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameBuilding(*in);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        if (m_armed) m_armed.reset();   // stop placing a catalog doodad
        else if (in->groups) in->groupSel.clear();
        else in->sel.clear();
    }
}

// ---------------------------------------------------------------------------------------------- groups

std::string App::InteriorBase(const Interior& in) const
{
    const auto* e = m_wmoEdits.Find(in.wmo);
    return e ? e->from : in.wmo;
}

std::vector<std::pair<uint32_t, WmoBox>> App::InteriorBoxes(const Interior& in)
{
    std::vector<std::pair<uint32_t, WmoBox>> out;
    const std::string base = InteriorBase(in);
    for (const auto& [id, s] : in.live.slots)
        if (const auto b = m_wmoEdits.SlotBox(base, s)) out.push_back({ id, *b });
    return out;
}

std::set<uint32_t> App::DoodadsInSlots(const Interior& in, const std::set<uint32_t>& slots)
{
    const auto boxes = InteriorBoxes(in);
    std::vector<WmoBox> only;
    for (const auto& [id, b] : boxes) only.push_back(b);
    std::set<uint32_t> out;
    for (const auto& [id, d] : in.live.doodads)
    {
        bool inside = std::any_of(d.groups.begin(), d.groups.end(), [&](uint16_t g) { return slots.count(g) != 0; });
        if (d.groups.empty())
            if (const int g = BoxGroupOf(d, only); g >= 0) inside = slots.count(boxes[size_t(g)].first) != 0;
        if (inside) out.insert(id);
    }
    return out;
}

void App::CopyGroups(Interior& in, bool cut)
{
    if (in.groupSel.empty()) return;
    if (cut && !m_wmoEdits.Find(in.wmo)) { Log("Make this building editable first (a copy or the original) to cut from it."); return; }
    const std::string base = InteriorBase(in);
    WmoClip clip;
    clip.sets = in.live.sets;
    bool first = true;
    for (uint32_t id : in.groupSel)
    {
        const auto it = in.live.slots.find(id);
        if (it == in.live.slots.end()) continue;
        WmoSlot s = it->second;
        if (s.from.empty()) s.from = base;   // a clip always names its files: it outlives this building's tab
        clip.slots[id] = s;
        if (const auto b = m_wmoEdits.SlotBox(base, it->second)) Grow(clip.box, *b, first);
    }
    // The doodads standing in them come along, each listed by the groups it stands in.
    const auto boxes = InteriorBoxes(in);
    std::vector<WmoBox> only;
    for (const auto& [id, b] : boxes) only.push_back(b);
    const std::set<uint32_t> carried = DoodadsInSlots(in, in.groupSel);
    for (uint32_t id : carried)
    {
        WmoDoodads::Doodad d = in.live.doodads.at(id);
        std::erase_if(d.groups, [&](uint16_t g) { return !clip.slots.count(g); });
        if (d.groups.empty())
            if (const int g = BoxGroupOf(d, only); g >= 0) d.groups = { uint16_t(boxes[size_t(g)].first) };
        clip.doodads.push_back(std::move(d));
    }
    clip.label = std::to_string(clip.slots.size()) + " group(s) of " + FileOf(in.wmo);
    Log("%s %s with %zu doodad(s).", cut ? "Cut" : "Copied", clip.label.c_str(), clip.doodads.size());
    m_wmoClip = std::move(clip);
    if (!cut) return;
    for (uint32_t id : in.groupSel) in.live.slots.erase(id);
    for (uint32_t id : carried) in.live.doodads.erase(id);
    const size_t n = in.groupSel.size();
    in.groupSel.clear();
    CommitInterior(in, "Cut " + std::to_string(n) + " group(s) from " + FileOf(in.wmo));
}

void App::DeleteGroups(Interior& in)
{
    if (in.groupSel.empty() || !m_wmoEdits.Find(in.wmo)) return;
    const std::set<uint32_t> carried = DoodadsInSlots(in, in.groupSel);
    for (uint32_t id : in.groupSel) in.live.slots.erase(id);
    for (uint32_t id : carried) in.live.doodads.erase(id);
    const size_t n = in.groupSel.size();
    in.groupSel.clear();
    CommitInterior(in, "Delete " + std::to_string(n) + " group(s) from " + FileOf(in.wmo));
}

void App::PasteGroups(Interior& in)
{
    if (!m_wmoClip) return;
    if (!m_wmoEdits.Find(in.wmo)) { Log("Make this building editable first (a copy or the original) to paste into it."); return; }
    const WmoClip& clip = *m_wmoClip;
    // Where: its bottom middle on the point under the cursor (a floor, or the floor plane); else where it was.
    WmoTransform at;
    if (in.point)
    {
        const XMFLOAT3 p = ToLocal(*in.point);
        at.offset[0] = p.x - (clip.box[0] + clip.box[3]) / 2;
        at.offset[1] = p.y - (clip.box[1] + clip.box[4]) / 2;
        at.offset[2] = p.z - clip.box[2];
    }
    // New slots: a batch per batch they came in (portals between them stay), ids counting on.
    std::map<std::pair<std::string, uint32_t>, uint32_t> batches;
    std::map<uint32_t, uint32_t> ids;
    uint32_t nextBatch = in.live.NextBatch(), nextSlot = in.live.NextSlotId();
    std::set<uint32_t> pasted;
    for (const auto& [old, s] : clip.slots)
    {
        WmoSlot n = s;
        const auto [b, fresh] = batches.try_emplace({ s.from, s.batch }, nextBatch);
        if (fresh) ++nextBatch;
        n.batch = b->second;
        n.at = s.at.Then(at);
        ids[old] = nextSlot;
        pasted.insert(nextSlot);
        in.live.slots[nextSlot++] = n;
    }
    for (WmoDoodads::Doodad d : clip.doodads)
    {
        TransformDoodad(d, at);
        for (uint16_t& g : d.groups) g = uint16_t(ids.count(g) ? ids[g] : g);
        // Its set by name: the building's own of that name, else a new one.
        const std::string set = d.set < clip.sets.size() ? clip.sets[d.set] : std::string("Set_$DefaultGlobal");
        auto it = std::find(in.live.sets.begin(), in.live.sets.end(), set);
        if (d.set == 0) it = in.live.sets.begin();   // set 0 shows everywhere: it stays set 0
        if (it == in.live.sets.end()) { in.live.sets.push_back(set); it = in.live.sets.end() - 1; }
        d.set = uint32_t(it - in.live.sets.begin());
        in.live.doodads[in.live.NextId()] = d;
    }
    in.groupSel = pasted;
    in.groups = true;
    CommitInterior(in, "Paste " + clip.label + " into " + FileOf(in.wmo));
}

std::optional<App::Transformable> App::InteriorGroupTransform(Interior& in)
{
    if (in.groupSel.empty() || !m_wmoEdits.Find(in.wmo)) return std::nullopt;
    WmoBox all{};
    bool first = true;
    const std::string base = InteriorBase(in);
    for (uint32_t id : in.groupSel)
        if (const auto s = in.live.slots.find(id); s != in.live.slots.end())
            if (const auto b = m_wmoEdits.SlotBox(base, s->second)) Grow(all, *b, first);
    if (first) return std::nullopt;
    const float centre[3] = { (all[0] + all[3]) / 2, (all[1] + all[4]) / 2, (all[2] + all[5]) / 2 };
    Interior* p = &in;   // the tab outlives the handle drag (closing it cancels the drag first)
    Transformable t;
    t.yawOnly = true;
    t.limits = "Groups move freely and turn in quarter turns about the vertical (their collision stays exact).";
    t.frame = Store(XMMatrixTranslationFromVector(ToView(centre)));
    t.what = std::to_string(in.groupSel.size()) + " group(s)";
    t.begin = [p] { p->groupDrag = Store(XMMatrixIdentity()); };
    t.preview = [p](FXMMATRIX delta) { p->groupDrag = Store(delta); };
    t.commit = [this, p, c = std::array<float, 3>{ centre[0], centre[1], centre[2] }](const std::string& label) {
        if (!p->groupDrag) return;
        // The handle's move in the building's own axes, its turn snapped to quarter turns about the group centre.
        const XMMATRIX wow = kWowToEditor * XMLoadFloat4x4(&*p->groupDrag) * XMMatrixTranspose(kWowToEditor);
        p->groupDrag.reset();
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, wow);
        WmoTransform d;
        d.turns = int(std::lround(std::atan2(m._12, m._11) / XM_PIDIV2)) & 3;
        XMFLOAT3 moved;
        XMStoreFloat3(&moved, XMVector3TransformCoord(XMVectorSet(c[0], c[1], c[2], 1), wow));
        float turned[3] = { c[0], c[1], c[2] };
        d.Vector(turned);
        d.offset[0] = moved.x - turned[0];
        d.offset[1] = moved.y - turned[1];
        d.offset[2] = moved.z - turned[2];
        if (d.Identity()) return;
        const std::set<uint32_t> carried = DoodadsInSlots(*p, p->groupSel);
        for (uint32_t id : p->groupSel)
            if (const auto s = p->live.slots.find(id); s != p->live.slots.end()) s->second.at = s->second.at.Then(d);
        for (uint32_t id : carried) TransformDoodad(p->live.doodads[id], d);
        CommitInterior(*p, label + " in " + FileOf(p->wmo));
    };
    t.cancel = [p] { p->groupDrag.reset(); };
    t.remove = [this, p] { DeleteGroups(*p); };
    return t;
}

std::optional<App::Transformable> App::BuildingTransform()
{
    Interior* p = Building();
    if (!p) return std::nullopt;
    if (p->groups) return InteriorGroupTransform(*p);
    if (p->sel.empty() || !m_wmoEdits.Find(p->wmo)) return std::nullopt;
    std::vector<XMFLOAT3> points;
    std::optional<XMMATRIX> only;
    for (uint32_t id : p->sel)
        if (const auto it = p->live.doodads.find(id); it != p->live.doodads.end())
        {
            const XMMATRIX m = DoodadMatrix(it->second);
            points.push_back(Origin(m));
            only = m;
        }
    if (points.empty()) return std::nullopt;
    Transformable t;
    t.scale = Transformable::Scale::Uniform;
    t.what = std::to_string(points.size()) + " doodad(s)";
    if (points.size() == 1)   // its own frame, without its scale, so local handles follow its turn
    {
        XMVECTOR s, q, o;
        XMMatrixDecompose(&s, &q, &o, *only);
        t.frame = Store(XMMatrixRotationQuaternion(q) * XMMatrixTranslationFromVector(o));
    }
    else
    {
        XMFLOAT3 c{};
        for (const XMFLOAT3& q : points) { c.x += q.x; c.y += q.y; c.z += q.z; }
        const float n = float(points.size());
        t.frame = Store(XMMatrixTranslation(c.x / n, c.y / n, c.z / n));
    }
    const std::string where = " in " + FileOf(p->wmo);
    t.begin = [p] { p->start = p->live; };
    t.preview = [this, p](FXMMATRIX delta) {
        p->live = p->start;
        for (uint32_t id : p->sel)
            if (const auto it = p->start.doodads.find(id); it != p->start.doodads.end())
            {
                WmoDoodads::Doodad& d = p->live.doodads[id];
                SetDoodadMatrix(d, DoodadMatrix(it->second) * delta);
                d.groups.clear();   // the group whose box holds it now
            }
        PreviewInterior(*p);
    };
    t.commit = [this, p, where](const std::string& label) { CommitInterior(*p, label + where); };
    t.cancel = [this, p] {
        p->live = p->start;
        PreviewInterior(*p);
    };
    t.ground = [this, p, where] {
        // Onto the building's floor below each one (its own furniture skipped).
        ModelRenderer::DrawSettings floors;
        floors.doodads = false;
        floors.attached = false;
        size_t dropped = 0;
        for (uint32_t id : p->sel)
        {
            const auto it = p->live.doodads.find(id);
            if (it == p->live.doodads.end()) continue;
            const XMMATRIX m = DoodadMatrix(it->second);
            const XMFLOAT3 at = Origin(m);
            const auto hit = m_models.Pick(XMVectorSet(at.x, at.y + 0.5f, at.z, 0), XMVectorSet(0, -1, 0, 0), 500.0f, floors, p->Layer());
            if (!hit || !hit->wmo) continue;
            SetDoodadMatrix(it->second, m * XMMatrixTranslation(0, 0.5f - hit->distance, 0));
            it->second.groups.clear();
            ++dropped;
        }
        if (dropped) CommitInterior(*p, "Drop " + std::to_string(dropped) + " doodad(s) to the floor" + where);
    };
    t.remove = [this, p, where] {
        for (uint32_t id : p->sel) p->live.doodads.erase(id);
        const size_t n = p->sel.size();
        p->sel.clear();
        CommitInterior(*p, "Delete " + std::to_string(n) + " doodad(s)" + where);
    };
    return t;
}

// ---------------------------------------------------------------------------------------------- panel

void App::DrawInteriorPanel(Interior& in)
{
    const auto placed = InteriorPlacement(in);
    const auto* edited = m_wmoEdits.Find(in.wmo);
    const uint32_t setBefore = in.set;
    ImGui::TextColored(kAccent, "%s", FileOf(in.wmo).c_str());
    ImGui::SetItemTooltip("%s", in.wmo.c_str());
    if (edited && !Same(edited->from, edited->name))
        ImGui::TextColored(kQuiet, placed ? "Project copy of %s, used by the placement it was opened from" : "Project copy of %s", FileOf(edited->from).c_str());
    else if (edited) ImGui::TextColored(kWarn, "The original file: every placement of it changes");
    if (placed) ImGui::TextColored(kQuiet, "Opened from placement %u on %s", in.wmoUid, m_terrain.Map().c_str());
    ImGui::Spacing();

    if (!edited)
    {
        size_t uses = 0;
        for (const auto& [key, tile] : m_terrain.Tiles())
            for (const WmoPlacement& p : tile.adt.wmos) uses += Same(p.model, in.wmo);
        const std::string copy = FileOf(WmoCopyName(in.wmo, [this](const std::string& n) { return m_wmoEdits.Find(n) || m_mpq.HasOwn(n); }));
        ImGui::TextWrapped("This is the game's own building. To change it, make it editable:");
        if (ImGui::Button(placed ? "Make a copy for this placement" : "Make a copy")) MakeInteriorEditable(in, false);
        if (placed) ImGui::SetItemTooltip("A new WMO file in the project (%s); only this placement uses it. Undo puts the original back.", copy.c_str());
        else ImGui::SetItemTooltip("A new WMO file in the project (%s), picked for placing on the map when made.", copy.c_str());
        if (ImGui::Button("Edit the original")) MakeInteriorEditable(in, true);
        ImGui::SetItemTooltip("Changes %s itself: every placement of it, on every map (%zu on the loaded tiles).", FileOf(in.wmo).c_str(), uses);
        ImGui::Spacing();
    }
    if (ImGui::RadioButton("Doodads", !in.groups)) in.groups = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Groups", in.groups)) in.groups = true;
    ImGui::SetItemTooltip("The building's parts (WMO groups): copy them into other buildings, move, turn, delete");
    ImGui::Spacing();
    if (in.groups)
    {
        const std::string base = InteriorBase(in);
        const float gw = ImGui::GetContentRegionAvail().x;
        if (ImGui::BeginChild("##interiorGroups", { gw, ImGui::GetTextLineHeightWithSpacing() * 10 }, ImGuiChildFlags_Borders))
            for (const auto& [id, s] : in.live.slots)
            {
                const std::string src = s.from.empty() ? base : s.from;
                const WmoGroupInfo* info = m_wmoEdits.GroupInfo(src, s.group);
                std::string label = std::to_string(id) + "  " + (info && !info->name.empty() ? info->name : "group " + std::to_string(s.group));
                if (!Same(src, base)) label += "  (" + FileOf(src) + ")";
                if (info) label += (s.outdoor == 1 || (s.outdoor < 0 && (info->flags & 0x8))) ? "  outdoor" : "  indoor";
                if (ImGui::Selectable((label + "##g" + std::to_string(id)).c_str(), in.groupSel.count(id) != 0))
                {
                    if (ImGui::GetIO().KeyCtrl) { if (!in.groupSel.erase(id)) in.groupSel.insert(id); }
                    else in.groupSel = { id };
                }
            }
        ImGui::EndChild();
        if (!in.groupSel.empty())
        {
            ImGui::TextColored(kAccent, "%zu group(s) selected", in.groupSel.size());
            if (ImGui::Button("Copy  Ctrl+C")) CopyGroups(in, false);
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited);
            if (ImGui::Button("Cut  Ctrl+X")) CopyGroups(in, true);
            ImGui::SameLine();
            if (ImGui::Button("Delete")) DeleteGroups(in);
            const char* kinds[] = { "As it is", "Indoor", "Outdoor" };
            const WmoSlot& first = in.live.slots.at(*in.groupSel.begin());
            int kind = first.outdoor + 1;
            ImGui::SetNextItemWidth(gw - 120);
            if (ImGui::Combo("Lit as", &kind, kinds, 3))
            {
                for (uint32_t id : in.groupSel) in.live.slots[id].outdoor = int8_t(kind - 1);
                CommitInterior(in, "Light " + std::to_string(in.groupSel.size()) + " group(s) of " + FileOf(in.wmo) + " as " + kinds[kind]);
            }
            ImGui::SetItemTooltip("Outdoor groups are lit by the sky and drawn from outside; indoor ones by their own light, seen\n"
                                  "through portals. A group pasted without its neighbours has no portal: outdoor keeps it visible.");
            ImGui::EndDisabled();
        }
        if (m_wmoClip)
        {
            ImGui::BeginDisabled(!edited);
            if (ImGui::Button(("Paste " + m_wmoClip->label + "  Ctrl+V").c_str())) PasteGroups(in);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Lands with its bottom middle where the cursor last pointed in the view (a floor, else the floor plane)");
        }
        if (edited)
            for (const std::string& n : edited->notes) ImGui::TextColored(kWarn, "%s", n.c_str());
        ImGui::TextColored(kQuiet, "Groups move freely and turn in quarter turns. Portals stay only between groups that\n"
                                   "came in together and moved together; the doodads standing in a group go with it.\n"
                                   "To move groups to another building: copy here, open the other one in a tab, paste.");
        return;
    }

    // Sets: set 0 always shows; the view shows the set picked here with it (a placement shows one set of its choice).
    WmoDoodads& d = in.live;
    std::vector<size_t> counts(d.sets.size());
    for (const auto& [id, e] : d.doodads)
        if (e.set < counts.size()) ++counts[e.set];
    auto setLabel = [&](size_t i) {
        std::string s = std::to_string(i) + "  " + (d.sets[i].empty() ? "(unnamed)" : d.sets[i]) + "  (" + std::to_string(counts[i]) + ")";
        if (i == 0) s += "  always shown";
        else if (placed && i == placed->doodadSet) s += "  the placement's";
        return s;
    };
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w - 70);
    if (ImGui::BeginCombo("Set", in.set < d.sets.size() ? setLabel(in.set).c_str() : "?"))
    {
        for (size_t i = 0; i < d.sets.size(); ++i)
            if (ImGui::Selectable(setLabel(i).c_str(), i == in.set)) { in.set = uint32_t(i); in.sel.clear(); }
        ImGui::EndCombo();
    }
    if (placed && in.set != 0 && in.set != placed->doodadSet)
    {
        ImGui::TextColored(kWarn, "The placement shows set %u, not this one.", placed->doodadSet);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show it there"))
        {
            const uint16_t set = uint16_t(in.set);
            m_terrain.BeginObjectEdit({ ObjectRef{ true, in.wmoUid } });
            m_terrain.PreviewObjectEdit([](DoodadPlacement&) {}, [set](WmoPlacement& p) { p.doodadSet = set; });
            EndObjectEdit("Doodad set " + std::to_string(set) + " for " + FileOf(in.wmo));
        }
    }
    if (edited)
    {
        ImGui::SetNextItemWidth(w - 220);
        ImGui::InputTextWithHint("##newSet", "set name (19 letters)", in.newSet, sizeof in.newSet);
        ImGui::SameLine();
        if (ImGui::Button("Add set") && in.newSet[0])
        {
            d.sets.push_back(in.newSet);
            in.set = uint32_t(d.sets.size() - 1);
            CommitInterior(in, "Add doodad set " + std::string(in.newSet) + " to " + FileOf(in.wmo));
            in.newSet[0] = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Rename") && in.newSet[0] && in.set < d.sets.size())
        {
            d.sets[in.set] = in.newSet;
            CommitInterior(in, "Rename doodad set " + std::to_string(in.set) + " of " + FileOf(in.wmo));
            in.newSet[0] = 0;
        }
        ImGui::SameLine();
        const bool last = in.set > 0 && in.set + 1 == d.sets.size();
        ImGui::BeginDisabled(!last);
        if (ImGui::Button("Delete set"))
        {
            const uint32_t gone = in.set;
            std::erase_if(d.doodads, [gone](const auto& e) { return e.second.set == gone; });
            d.sets.pop_back();
            in.set = 0;
            in.sel.clear();
            CommitInterior(in, "Delete doodad set " + std::to_string(gone) + " of " + FileOf(in.wmo));
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Only the last set (placements name sets by number); set 0 stays. Its doodads go with it.");
    }
    ImGui::Spacing();

    // The set's doodads.
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("##doodadFilter", "filter by name", in.filter, sizeof in.filter);
    std::string filter = in.filter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](char c) { return char(std::tolower((unsigned char)c)); });
    if (ImGui::BeginChild("##interiorDoodads", { w, ImGui::GetTextLineHeightWithSpacing() * 10 }, ImGuiChildFlags_Borders))
    {
        for (const auto& [id, e] : d.doodads)
        {
            if (e.set != in.set) continue;
            std::string name = FileOf(e.model), lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(std::tolower((unsigned char)c)); });
            if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
            if (ImGui::Selectable((name + "##" + std::to_string(id)).c_str(), in.sel.count(id) != 0))
            {
                if (ImGui::GetIO().KeyCtrl) { if (!in.sel.erase(id)) in.sel.insert(id); }
                else in.sel = { id };
            }
            ImGui::SetItemTooltip("%s  (id %u)", e.model.c_str(), id);
        }
    }
    ImGui::EndChild();

    if (in.set != setBefore) ShowBuilding(in);   // the view shows the set picked
    if (!edited) return;
    if (m_armed && !m_armed->wmo) ImGui::TextColored(kAccent, "Click in the view to add %s to set %u.", FileOf(m_armed->path).c_str(), in.set);
    else ImGui::TextColored(kQuiet, "To add one: pick a doodad in the Catalog, then click in the view.");
    if (in.sel.empty()) return;
    ImGui::Spacing();
    ImGui::Separator();

    // The selection: typed values preview live and commit as one change when the field is let go.
    auto field = [&](bool changed) {
        if (changed && !m_fieldEditing)
        {
            in.start = in.live;
            m_fieldEditing = true;
        }
        if (changed) PreviewInterior(in);
        if (ImGui::IsItemDeactivated() && m_fieldEditing)
        {
            m_fieldEditing = false;
            CommitInterior(in, "Edit doodads in " + FileOf(in.wmo));
        }
    };
    if (in.sel.size() == 1 && d.doodads.count(*in.sel.begin()))
    {
        WmoDoodads::Doodad& e = d.doodads[*in.sel.begin()];
        ImGui::TextColored(kAccent, "%s", FileOf(e.model).c_str());
        ImGui::SetNextItemWidth(w - 70);
        float pos[3] = { e.pos[0], e.pos[1], e.pos[2] };
        const bool moved = ImGui::DragFloat3("Position", pos, 0.05f, 0, 0, "%.2f");
        if (moved) { std::copy(pos, pos + 3, e.pos); e.groups.clear(); }
        field(moved);
        ImGui::SetItemTooltip("In the building's own axes (x, y across, z up), yards");
        ImGui::SetNextItemWidth(w - 70);
        field(ImGui::SliderFloat("Scale", &e.scale, 1.0f / 1024.0f, 63.0f, "%.3f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp));
        float color[4] = { float((e.color >> 16) & 0xFF) / 255, float((e.color >> 8) & 0xFF) / 255, float(e.color & 0xFF) / 255, float(e.color >> 24) / 255 };
        ImGui::SetNextItemWidth(w - 70);
        const bool tinted = ImGui::ColorEdit4("Colour", color, ImGuiColorEditFlags_AlphaBar);
        if (tinted)
        {
            auto byte = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255 + 0.5f); };
            e.color = byte(color[3]) << 24 | byte(color[0]) << 16 | byte(color[1]) << 8 | byte(color[2]);
        }
        field(tinted);
        ImGui::SetItemTooltip("The light colour the game tints it with indoors (MODD colour)");
        if (ImGui::Button("Reset turn"))
        {
            e.rot[0] = e.rot[1] = e.rot[2] = 0;
            e.rot[3] = 1;
            CommitInterior(in, "Reset the turn of " + FileOf(e.model) + " in " + FileOf(in.wmo));
        }
        ImGui::SameLine();
        if (m_armed && !m_armed->wmo && !Same(m_armed->path, e.model) && ImGui::Button(("Replace with " + FileOf(m_armed->path)).c_str()))
        {
            const std::string label = "Replace " + FileOf(e.model) + " with " + FileOf(m_armed->path) + " in " + FileOf(in.wmo);
            e.model = m_armed->path;
            m_armed.reset();
            CommitInterior(in, label);
        }
    }
    else ImGui::TextColored(kAccent, "%zu doodads selected", in.sel.size());

    if (ImGui::Button("Duplicate"))
    {
        std::set<uint32_t> copies;
        for (uint32_t id : in.sel)
            if (const auto it = d.doodads.find(id); it != d.doodads.end())
            {
                WmoDoodads::Doodad copy = it->second;
                copy.pos[0] += 0.5f;   // beside it, so both show
                copy.groups.clear();
                const uint32_t added = d.NextId();
                d.doodads[added] = copy;
                copies.insert(added);
            }
        in.sel = copies;
        CommitInterior(in, "Duplicate " + std::to_string(copies.size()) + " doodad(s) in " + FileOf(in.wmo));
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        if (const auto t = BuildingTransform(); t && t->remove) t->remove();
    if (d.sets.size() > 1)
    {
        ImGui::SetNextItemWidth(w - 70);
        if (ImGui::BeginCombo("Move to set", "pick a set"))
        {
            for (size_t i = 0; i < d.sets.size(); ++i)
                if (i != in.set && ImGui::Selectable(setLabel(i).c_str()))
                {
                    for (uint32_t id : in.sel)
                        if (const auto it = d.doodads.find(id); it != d.doodads.end()) it->second.set = uint32_t(i);
                    CommitInterior(in, "Move " + std::to_string(in.sel.size()) + " doodad(s) to set " + std::to_string(i) + " of " + FileOf(in.wmo));
                    in.set = uint32_t(i);
                    ShowBuilding(in);
                }
            ImGui::EndCombo();
        }
    }
}
