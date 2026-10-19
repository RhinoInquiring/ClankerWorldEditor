// A building's own doodads: the furniture a WMO file places in its doodad sets, edited in the Objects tool. The WMO
// file itself changes (a project copy only the edited placement uses, or the original in place), so the edit shows
// wherever that file stands. Selection, handles and keys are the shared ones (AppTransform.cpp).
#include "App.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

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

/// A point of the building's own (WoW) axes in the world, and back.
XMVECTOR ToWorld(const float p[3], FXMMATRIX world) { return XMVector3TransformCoord(XMVectorSet(p[0], p[2], -p[1], 1), world); }
XMFLOAT3 ToLocal(const XMFLOAT3& w, FXMMATRIX world)
{
    XMFLOAT3 e;
    XMStoreFloat3(&e, XMVector3TransformCoord(XMLoadFloat3(&w), XMMatrixInverse(nullptr, world)));
    return { e.x, -e.z, e.y };
}

void Grow(WmoBox& all, const WmoBox& b, bool& first)
{
    if (first) { all = b; first = false; return; }
    for (int k = 0; k < 3; ++k) { all[k] = std::min(all[k], b[k]); all[3 + k] = std::max(all[3 + k], b[3 + k]); }
}
}

// ---------------------------------------------------------------------------------------------- session

void App::BeginInterior(uint32_t wmoUid)
{
    const auto w = m_terrain.FindWmo(wmoUid);
    if (!w) return;
    Interior in;
    in.wmoUid = wmoUid;
    in.wmo = w->model;
    if (const auto* e = m_wmoEdits.Find(w->model)) in.live = e->doodads;
    else
    {
        std::string error;
        const auto root = m_mpq.Read(w->model);
        auto read = root ? ReadWmoDoodads(*root, {}, error) : std::nullopt;
        if (!read) { Log("Cannot edit the doodads of %s: %s", FileOf(w->model).c_str(), root ? error.c_str() : "file not found"); return; }
        in.live = std::move(*read);
    }
    in.set = w->doodadSet < in.live.sets.size() ? w->doodadSet : 0;
    m_interior = std::move(in);
    m_armed.reset();
    PreviewInterior();   // drawn in the order the session's ids name
}

std::optional<XMFLOAT4X4> App::InteriorWorld() const
{
    if (!m_interior) return std::nullopt;
    const auto w = m_terrain.FindWmo(m_interior->wmoUid);
    if (!w) return std::nullopt;
    return Store(PlacementMatrix(w->pos, w->rot, 1.0f));
}

void App::MakeInteriorEditable(bool inPlace)
{
    Interior& in = *m_interior;
    const auto w = m_terrain.FindWmo(in.wmoUid);
    if (!w) return;
    const std::string from = w->model;
    const std::string name = inPlace ? from : WmoCopyName(from, [this](const std::string& n) { return m_wmoEdits.Find(n) || m_mpq.HasOwn(n); });
    std::string error;
    auto create = m_wmoEdits.MakeCreate(name, from, error);
    if (!create) { Log("Cannot edit the doodads of %s: %s", FileOf(from).c_str(), error.c_str()); return; }
    m_wmoEdits.Apply(*create);
    const auto* e = m_wmoEdits.Find(name);
    if (!e) { Log("Cannot edit the doodads of %s: its groups could not be read.", FileOf(from).c_str()); return; }
    const std::string label = create->label;
    std::vector<Change> parts{ std::move(*create) };
    if (!inPlace)   // this placement uses the copy: one undo step with the copy
    {
        m_terrain.BeginObjectEdit({ ObjectRef{ true, in.wmoUid } });
        m_terrain.PreviewObjectEdit([](DoodadPlacement&) {}, [&](WmoPlacement& p) { p.model = name; });
        if (auto moved = m_terrain.EndObjectEdit(label)) parts.push_back(std::move(*moved));
    }
    m_store.Commit(std::move(parts), label);
    Log("%s", label.c_str());
    in.wmo = name;
    in.live = e->doodads;
}

void App::PreviewInterior()
{
    if (!m_interior) return;
    std::vector<ModelMesh::Doodad> doodads;
    std::vector<ModelMesh::DoodadSet> sets;
    MeshDoodads(m_interior->live, doodads, sets, m_interior->ids);
    m_models.SetWmoDoodads(m_interior->wmo, std::move(doodads), std::move(sets));
    ReloadTilesWith(m_interior->wmo);
}

void App::CommitInterior(const std::string& label)
{
    if (!m_interior) return;
    const auto* e = m_wmoEdits.Find(m_interior->wmo);
    if (!e) { PreviewInterior(); return; }
    auto change = m_wmoEdits.MakeEdit(e->name, e->doodads, m_interior->live, label);
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
}

void App::UpdateEditedWmos()
{
    if (m_wmoEdits.TakeFilesChanged())
    {
        ++m_assetsVersion;   // the project's file layer lists the new (or removed) files
        UpdateProjectFiles();
    }
    for (const std::string& name : m_wmoEdits.TakeRebuilt())   // groups moved, added or cut: read the building again
    {
        for (int key : m_models.Forget(name))
            if (const auto it = m_terrain.Tiles().find(key); it != m_terrain.Tiles().end()) m_models.AddTile(key, it->second.adt, m_mpq);
        m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;   // a gameobject may show it
    }
    for (const std::string& name : m_wmoEdits.TakeChanged())
    {
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
        if (m_interior && Same(m_interior->wmo, name) && !m_gizmoActive)   // undo, redo: the session shows what is saved
        {
            m_interior->live = now;
            std::erase_if(m_interior->sel, [&](uint32_t id) { return !now.doodads.count(id); });
            std::erase_if(m_interior->groupSel, [&](uint32_t id) { return !now.slots.count(id); });
            if (m_interior->set >= now.sets.size()) m_interior->set = 0;
        }
        std::vector<ModelMesh::Doodad> doodads;
        std::vector<ModelMesh::DoodadSet> sets;
        std::vector<uint32_t> ids;
        MeshDoodads(now, doodads, sets, ids);
        if (m_interior && Same(m_interior->wmo, name)) m_interior->ids = ids;
        m_models.SetWmoDoodads(name, std::move(doodads), std::move(sets));
        ReloadTilesWith(name);
    }
    // The session ends with its placement or the tool, and follows the placement to another file (a copy undone).
    if (!m_interior) return;
    const auto w = m_terrain.FindWmo(m_interior->wmoUid);
    if (!w || m_tool != Tool::Objects) m_interior.reset();
    else if (!Same(w->model, m_interior->wmo) && !m_gizmoActive) BeginInterior(m_interior->wmoUid);
}

// ---------------------------------------------------------------------------------------------- viewport

std::optional<WmoDoodads::Doodad> App::InteriorDoodadAt(const XMFLOAT3& point) const
{
    const auto world = InteriorWorld();
    if (!world || !m_armed) return std::nullopt;
    XMFLOAT3 local;
    XMStoreFloat3(&local, XMVector3TransformCoord(XMLoadFloat3(&point), XMMatrixInverse(nullptr, XMLoadFloat4x4(&*world))));
    WmoDoodads::Doodad d;
    d.model = m_armed->path;
    d.pos[0] = local.x;   // editor -> WoW axes: (x, y, z) -> (x, -z, y)
    d.pos[1] = -local.z;
    d.pos[2] = local.y;
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(d.rot), XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), XMConvertToRadians(m_placeYaw)));
    d.scale = m_placeScale;
    d.set = m_interior->set;
    // Lit like the nearest doodad of the same set (the colour tints interior doodads).
    float nearest = INFINITY;
    for (const auto& [id, o] : m_interior->live.doodads)
    {
        const float gap = std::pow(o.pos[0] - d.pos[0], 2.0f) + std::pow(o.pos[1] - d.pos[1], 2.0f) + std::pow(o.pos[2] - d.pos[2], 2.0f);
        if (o.set == d.set && gap < nearest) { nearest = gap; d.color = o.color; d.flags = o.flags; }
    }
    return d;
}

void App::InteriorViewport(FXMVECTOR p0, FXMVECTOR dir, const ImVec2&, const ImVec2&)
{
    Interior& in = *m_interior;
    float ground = m_hover ? XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&m_hover->pos), p0))) : 6000.0f;
    in.point = m_hover ? std::optional(m_hover->pos) : std::nullopt;
    // Walls hide what is behind them: the nearest building surface limits the doodad pick.
    ModelRenderer::DrawSettings surfaces = m_modelSettings;
    surfaces.doodads = false;
    surfaces.attached = false;
    float wall = ground;
    if (const auto hit = m_models.Pick(p0, dir, ground, surfaces); hit && hit->wmo) wall = hit->distance;
    in.hover.reset();
    in.groupHover.reset();
    if (in.groups)
    {
        // The group under the cursor: drawn index -> slot (the building as last written, or as the game has it).
        if (const auto hit = m_models.PickWmoGroup(in.wmoUid, p0, dir, ground))
        {
            const auto* e = m_wmoEdits.Find(in.wmo);
            const uint32_t slot = e ? (hit->group < e->order.size() ? e->order[hit->group] : UINT32_MAX) : hit->group;
            if (in.live.slots.count(slot)) in.groupHover = slot;
            wall = std::min(wall, hit->distance);
        }
    }
    else if (const auto hit = m_models.PickDoodad(in.wmoUid, p0, dir, wall + 0.05f))
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

void App::InteriorOutlines(std::vector<LineVertex>& lines) const
{
    const Interior& in = *m_interior;
    auto box = [&](const XMFLOAT3 (&c)[8], XMFLOAT4 color) {
        const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& e : edges) { lines.push_back({ c[e[0]], color }); lines.push_back({ c[e[1]], color }); }
    };
    XMFLOAT3 c[8];
    if (m_models.Corners(true, in.wmoUid, c)) box(c, { 0.3f, 0.62f, 1, 0.35f });   // the building being furnished
    auto doodad = [&](uint32_t id, XMFLOAT4 color) {
        const auto it = std::find(in.ids.begin(), in.ids.end(), id);
        if (it != in.ids.end() && m_models.DoodadCorners(in.wmoUid, uint32_t(it - in.ids.begin()), c)) box(c, color);
    };
    if (in.groups)
    {
        // Group boxes; the selected ones where the handle has them while it is dragged.
        const auto world = InteriorWorld();
        if (!world) return;
        const XMMATRIX w = XMLoadFloat4x4(&*world);
        const XMMATRIX drag = in.groupDrag ? XMLoadFloat4x4(&*in.groupDrag) : XMMatrixIdentity();
        const std::string base = InteriorBase();
        auto group = [&](uint32_t id, XMFLOAT4 color) {
            const auto s = in.live.slots.find(id);
            if (s == in.live.slots.end()) return;
            const auto b = const_cast<WmoEditAdapter&>(m_wmoEdits).SlotBox(base, s->second);
            if (!b) return;
            XMFLOAT3 corners[8];
            for (int i = 0; i < 8; ++i)
            {
                const float p[3] = { (*b)[i & 1 ? 3 : 0], (*b)[i & 2 ? 4 : 1], (*b)[i & 4 ? 5 : 2] };
                XMStoreFloat3(&corners[i], XMVector3TransformCoord(ToWorld(p, w), in.groupSel.count(id) ? drag : XMMatrixIdentity()));
            }
            box(corners, color);
        };
        for (uint32_t id : in.groupSel) group(id, { 1, 0.75f, 0.2f, 1 });
        if (in.groupHover && !in.groupSel.count(*in.groupHover)) group(*in.groupHover, { 1, 1, 1, 0.6f });
        return;
    }
    for (uint32_t id : in.sel) doodad(id, { 1, 0.75f, 0.2f, 1 });
    if (in.hover && !in.sel.count(*in.hover)) doodad(*in.hover, { 1, 1, 1, 0.6f });
}

// ---------------------------------------------------------------------------------------------- groups

std::string App::InteriorBase() const
{
    if (!m_interior) return {};
    const auto* e = m_wmoEdits.Find(m_interior->wmo);
    return e ? e->from : m_interior->wmo;
}

std::vector<std::pair<uint32_t, WmoBox>> App::InteriorBoxes()
{
    std::vector<std::pair<uint32_t, WmoBox>> out;
    const std::string base = InteriorBase();
    for (const auto& [id, s] : m_interior->live.slots)
        if (const auto b = m_wmoEdits.SlotBox(base, s)) out.push_back({ id, *b });
    return out;
}

std::set<uint32_t> App::DoodadsInSlots(const std::set<uint32_t>& slots)
{
    const auto boxes = InteriorBoxes();
    std::vector<WmoBox> only;
    for (const auto& [id, b] : boxes) only.push_back(b);
    std::set<uint32_t> out;
    for (const auto& [id, d] : m_interior->live.doodads)
    {
        bool in = std::any_of(d.groups.begin(), d.groups.end(), [&](uint16_t g) { return slots.count(g) != 0; });
        if (d.groups.empty())
            if (const int g = BoxGroupOf(d, only); g >= 0) in = slots.count(boxes[size_t(g)].first) != 0;
        if (in) out.insert(id);
    }
    return out;
}

void App::CopyGroups(bool cut)
{
    if (!m_interior || m_interior->groupSel.empty()) return;
    Interior& in = *m_interior;
    if (cut && !m_wmoEdits.Find(in.wmo)) { Log("Make this building editable first (a copy or the original) to cut from it."); return; }
    const std::string base = InteriorBase();
    WmoClip clip;
    clip.sets = in.live.sets;
    bool first = true;
    for (uint32_t id : in.groupSel)
    {
        const auto it = in.live.slots.find(id);
        if (it == in.live.slots.end()) continue;
        WmoSlot s = it->second;
        if (s.from.empty()) s.from = base;   // a clip always names its files: it outlives this building's session
        clip.slots[id] = s;
        if (const auto b = m_wmoEdits.SlotBox(base, it->second)) Grow(clip.box, *b, first);
    }
    // The doodads standing in them come along, each listed by the groups it stands in.
    const auto boxes = InteriorBoxes();
    std::vector<WmoBox> only;
    for (const auto& [id, b] : boxes) only.push_back(b);
    const std::set<uint32_t> carried = DoodadsInSlots(in.groupSel);
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
    CommitInterior("Cut " + std::to_string(n) + " group(s) from " + FileOf(in.wmo));
}

void App::DeleteGroups()
{
    if (!m_interior || m_interior->groupSel.empty() || !m_wmoEdits.Find(m_interior->wmo)) return;
    Interior& in = *m_interior;
    const std::set<uint32_t> carried = DoodadsInSlots(in.groupSel);
    for (uint32_t id : in.groupSel) in.live.slots.erase(id);
    for (uint32_t id : carried) in.live.doodads.erase(id);
    const size_t n = in.groupSel.size();
    in.groupSel.clear();
    CommitInterior("Delete " + std::to_string(n) + " group(s) from " + FileOf(in.wmo));
}

void App::PasteGroups()
{
    if (!m_interior || !m_wmoClip) return;
    Interior& in = *m_interior;
    if (!m_wmoEdits.Find(in.wmo)) { Log("Make this building editable first (a copy or the original) to paste into it."); return; }
    const WmoClip& clip = *m_wmoClip;
    // Where: its bottom middle on the point under the cursor (a floor, or the ground); else where it was.
    WmoTransform at;
    if (const auto world = InteriorWorld(); world && in.point)
    {
        const XMFLOAT3 p = ToLocal(*in.point, XMLoadFloat4x4(&*world));
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
    CommitInterior("Paste " + clip.label + " into " + FileOf(in.wmo));
}

std::optional<App::Transformable> App::InteriorGroupTransform()
{
    if (!m_interior || m_interior->groupSel.empty() || !m_wmoEdits.Find(m_interior->wmo)) return std::nullopt;
    const auto world = InteriorWorld();
    if (!world) return std::nullopt;
    const XMMATRIX w = XMLoadFloat4x4(&*world);
    WmoBox all{};
    bool first = true;
    const std::string base = InteriorBase();
    for (uint32_t id : m_interior->groupSel)
        if (const auto s = m_interior->live.slots.find(id); s != m_interior->live.slots.end())
            if (const auto b = m_wmoEdits.SlotBox(base, s->second)) Grow(all, *b, first);
    if (first) return std::nullopt;
    const float centre[3] = { (all[0] + all[3]) / 2, (all[1] + all[4]) / 2, (all[2] + all[5]) / 2 };
    Transformable t;
    t.yawOnly = true;
    t.limits = "Groups move freely and turn in quarter turns about the vertical (their collision stays exact).";
    t.frame = Store(XMMatrixTranslationFromVector(ToWorld(centre, w)));
    t.what = std::to_string(m_interior->groupSel.size()) + " group(s)";
    t.begin = [this] { m_interior->groupDrag = Store(XMMatrixIdentity()); };
    t.preview = [this](FXMMATRIX delta) { m_interior->groupDrag = Store(delta); };
    t.commit = [this, w, c = std::array<float, 3>{ centre[0], centre[1], centre[2] }](const std::string& label) {
        if (!m_interior->groupDrag) return;
        // The handle's move in the building's own axes, its turn snapped to quarter turns about the group centre.
        const XMMATRIX local = w * XMLoadFloat4x4(&*m_interior->groupDrag) * XMMatrixInverse(nullptr, w);
        const XMMATRIX wow = kWowToEditor * local * XMMatrixTranspose(kWowToEditor);
        m_interior->groupDrag.reset();
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
        const std::set<uint32_t> carried = DoodadsInSlots(m_interior->groupSel);
        for (uint32_t id : m_interior->groupSel)
            if (const auto s = m_interior->live.slots.find(id); s != m_interior->live.slots.end()) s->second.at = s->second.at.Then(d);
        for (uint32_t id : carried) TransformDoodad(m_interior->live.doodads[id], d);
        CommitInterior(label + " in " + FileOf(m_interior->wmo));
    };
    t.cancel = [this] { m_interior->groupDrag.reset(); };
    t.remove = [this] { DeleteGroups(); };
    return t;
}

std::optional<App::Transformable> App::InteriorTransform()
{
    if (m_interior && m_interior->groups) return InteriorGroupTransform();
    if (!m_interior || m_interior->sel.empty() || !m_wmoEdits.Find(m_interior->wmo)) return std::nullopt;
    const auto world = InteriorWorld();
    if (!world) return std::nullopt;
    const XMMATRIX w = XMLoadFloat4x4(&*world);
    std::vector<XMFLOAT3> points;
    std::optional<XMMATRIX> only;
    for (uint32_t id : m_interior->sel)
        if (const auto it = m_interior->live.doodads.find(id); it != m_interior->live.doodads.end())
        {
            const XMMATRIX m = DoodadMatrix(it->second) * w;
            points.push_back(Origin(m));
            only = m;
        }
    if (points.empty()) return std::nullopt;
    Transformable t;
    t.scale = Transformable::Scale::Uniform;
    t.what = std::to_string(points.size()) + " doodad(s)";
    if (points.size() == 1)   // its own frame, without its scale, so local handles follow its turn
    {
        XMVECTOR s, q, p;
        XMMatrixDecompose(&s, &q, &p, *only);
        t.frame = Store(XMMatrixRotationQuaternion(q) * XMMatrixTranslationFromVector(p));
    }
    else
    {
        XMFLOAT3 c{};
        for (const XMFLOAT3& p : points) { c.x += p.x; c.y += p.y; c.z += p.z; }
        const float n = float(points.size());
        t.frame = Store(XMMatrixTranslation(c.x / n, c.y / n, c.z / n));
    }
    const std::string in = " in " + FileOf(m_interior->wmo);
    t.begin = [this] { m_interior->start = m_interior->live; };
    t.preview = [this, w](FXMMATRIX delta) {
        Interior& s = *m_interior;
        s.live = s.start;
        const XMMATRIX back = XMMatrixInverse(nullptr, w);
        for (uint32_t id : s.sel)
            if (const auto it = s.start.doodads.find(id); it != s.start.doodads.end())
            {
                WmoDoodads::Doodad& d = s.live.doodads[id];
                SetDoodadMatrix(d, DoodadMatrix(it->second) * w * delta * back);
                d.groups.clear();   // the group whose box holds it now
            }
        PreviewInterior();
    };
    t.commit = [this, in](const std::string& label) { CommitInterior(label + in); };
    t.cancel = [this] {
        m_interior->live = m_interior->start;
        PreviewInterior();
    };
    t.ground = [this, w, in] {
        // Onto the building's floor below each one (its own furniture skipped), else the terrain.
        ModelRenderer::DrawSettings floors;
        floors.doodads = false;
        floors.attached = false;
        const XMMATRIX back = XMMatrixInverse(nullptr, w);
        for (uint32_t id : m_interior->sel)
        {
            const auto it = m_interior->live.doodads.find(id);
            if (it == m_interior->live.doodads.end()) continue;
            const XMMATRIX m = DoodadMatrix(it->second) * w;
            const XMFLOAT3 at = Origin(m);
            std::optional<float> y;
            if (const auto hit = m_models.Pick(XMVectorSet(at.x, at.y + 0.5f, at.z, 0), XMVectorSet(0, -1, 0, 0), 500.0f, floors); hit && hit->wmo)
                y = at.y + 0.5f - hit->distance;
            if (const auto h = m_terrain.HeightAt(at.x, at.z); h && (!y || (*h > *y && *h <= at.y + 0.5f))) y = h;
            if (!y) continue;
            SetDoodadMatrix(it->second, m * XMMatrixTranslation(0, *y - at.y, 0) * back);
            it->second.groups.clear();
        }
        CommitInterior("Drop " + std::to_string(m_interior->sel.size()) + " doodad(s) to the floor" + in);
    };
    t.remove = [this, in] {
        for (uint32_t id : m_interior->sel) m_interior->live.doodads.erase(id);
        const size_t n = m_interior->sel.size();
        m_interior->sel.clear();
        CommitInterior("Delete " + std::to_string(n) + " doodad(s)" + in);
    };
    return t;
}

// ---------------------------------------------------------------------------------------------- panel

void App::DrawInteriorPanel()
{
    Interior& in = *m_interior;
    const auto placed = m_terrain.FindWmo(in.wmoUid);
    if (!placed) return;
    const auto* edited = m_wmoEdits.Find(in.wmo);
    ImGui::TextColored(kAccent, "Doodads of %s", FileOf(in.wmo).c_str());
    ImGui::SetItemTooltip("%s", in.wmo.c_str());
    if (edited && !Same(edited->from, edited->name)) ImGui::TextColored(kQuiet, "Project copy of %s, used by this placement", FileOf(edited->from).c_str());
    else if (edited) ImGui::TextColored(kWarn, "The original file: every placement of it changes");
    if (ImGui::Button("Back to the map  Esc")) { m_interior.reset(); return; }
    ImGui::Spacing();

    if (!edited)
    {
        size_t uses = 0;
        for (const auto& [key, tile] : m_terrain.Tiles())
            for (const WmoPlacement& p : tile.adt.wmos) uses += Same(p.model, in.wmo);
        ImGui::TextWrapped("This is the game's own building. To change its furniture, make it editable:");
        if (ImGui::Button("Make a copy for this placement")) MakeInteriorEditable(false);
        ImGui::SetItemTooltip("A new WMO file in the project (%s); only this placement uses it. Undo puts the original back.",
                              FileOf(WmoCopyName(in.wmo, [this](const std::string& n) { return m_wmoEdits.Find(n) || m_mpq.HasOwn(n); })).c_str());
        if (ImGui::Button("Edit the original")) MakeInteriorEditable(true);
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
        const std::string base = InteriorBase();
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
            if (ImGui::Button("Copy  Ctrl+C")) CopyGroups(false);
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited);
            if (ImGui::Button("Cut  Ctrl+X")) CopyGroups(true);
            ImGui::SameLine();
            if (ImGui::Button("Delete")) DeleteGroups();
            const char* kinds[] = { "As it is", "Indoor", "Outdoor" };
            const WmoSlot& first = in.live.slots.at(*in.groupSel.begin());
            int kind = first.outdoor + 1;
            ImGui::SetNextItemWidth(gw - 120);
            if (ImGui::Combo("Lit as", &kind, kinds, 3))
            {
                for (uint32_t id : in.groupSel) in.live.slots[id].outdoor = int8_t(kind - 1);
                CommitInterior("Light " + std::to_string(in.groupSel.size()) + " group(s) of " + FileOf(in.wmo) + " as " + kinds[kind]);
            }
            ImGui::SetItemTooltip("Outdoor groups are lit by the sky and drawn from outside; indoor ones by their own light, seen\n"
                                  "through portals. A group pasted without its neighbours has no portal: outdoor keeps it visible.");
            ImGui::EndDisabled();
        }
        if (m_wmoClip)
        {
            ImGui::BeginDisabled(!edited);
            if (ImGui::Button(("Paste " + m_wmoClip->label + "  Ctrl+V").c_str())) PasteGroups();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Lands with its bottom middle where the cursor last pointed in the building (a floor or the ground)");
        }
        if (edited)
            for (const std::string& n : edited->notes) ImGui::TextColored(kWarn, "%s", n.c_str());
        ImGui::TextColored(kQuiet, "Groups move freely and turn in quarter turns. Portals stay only between groups that\n"
                                   "came in together and moved together; the doodads standing in a group go with it.");
        return;
    }

    // Sets: set 0 always shows; the placement adds one more.
    WmoDoodads& d = in.live;
    std::vector<size_t> counts(d.sets.size());
    for (const auto& [id, e] : d.doodads)
        if (e.set < counts.size()) ++counts[e.set];
    auto setLabel = [&](size_t i) {
        std::string s = std::to_string(i) + "  " + (d.sets[i].empty() ? "(unnamed)" : d.sets[i]) + "  (" + std::to_string(counts[i]) + ")";
        if (i == 0) s += "  always shown";
        else if (i == placed->doodadSet) s += "  shown here";
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
    if (in.set != 0 && in.set != placed->doodadSet)
    {
        ImGui::TextColored(kWarn, "This placement does not show set %u.", in.set);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show it here"))
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
            CommitInterior("Add doodad set " + std::string(in.newSet) + " to " + FileOf(in.wmo));
            in.newSet[0] = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Rename") && in.newSet[0] && in.set < d.sets.size())
        {
            d.sets[in.set] = in.newSet;
            CommitInterior("Rename doodad set " + std::to_string(in.set) + " of " + FileOf(in.wmo));
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
            CommitInterior("Delete doodad set " + std::to_string(gone) + " of " + FileOf(in.wmo));
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

    if (!edited) return;
    if (m_armed && !m_armed->wmo) ImGui::TextColored(kAccent, "Click inside the building to add %s to set %u.", FileOf(m_armed->path).c_str(), in.set);
    else ImGui::TextColored(kQuiet, "To add one: pick a doodad in the Catalog, then click inside the building.");
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
        if (changed) PreviewInterior();
        if (ImGui::IsItemDeactivated() && m_fieldEditing)
        {
            m_fieldEditing = false;
            CommitInterior("Edit doodads in " + FileOf(in.wmo));
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
            CommitInterior("Reset the turn of " + FileOf(e.model) + " in " + FileOf(in.wmo));
        }
        ImGui::SameLine();
        if (m_armed && !m_armed->wmo && !Same(m_armed->path, e.model) && ImGui::Button(("Replace with " + FileOf(m_armed->path)).c_str()))
        {
            const std::string label = "Replace " + FileOf(e.model) + " with " + FileOf(m_armed->path) + " in " + FileOf(in.wmo);
            e.model = m_armed->path;
            m_armed.reset();
            CommitInterior(label);
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
        CommitInterior("Duplicate " + std::to_string(copies.size()) + " doodad(s) in " + FileOf(in.wmo));
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        if (const auto t = InteriorTransform(); t && t->remove) t->remove();
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
                    CommitInterior("Move " + std::to_string(in.sel.size()) + " doodad(s) to set " + std::to_string(i) + " of " + FileOf(in.wmo));
                    in.set = uint32_t(i);
                }
            ImGui::EndCombo();
        }
    }
}
