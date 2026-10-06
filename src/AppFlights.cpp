// Flight paths: taxi nodes (TaxiNodes.dbc), the paths between them (TaxiPath.dbc) and their points (TaxiPathNode.dbc).
// Both the client (flight map, names, costs) and worldserver (which node a flight master serves, the route flown) read
// these files, so export writes both copies. Nodes and points move with the shared handles (AppTransform.cpp).
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
constexpr float kEndReach = 30.0f;   // yards: a path end this close to its node starts / ends there (Blizzard's do, 93%)
constexpr float kDepartureReach = 200.0f;   // yards of an existing flight copied as a new one's takeoff or landing (Blizzard's fan out by then)

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

XMFLOAT3 At(const TaxiNode& n) { return ServerToEditor(n.x, n.y, n.z); }
XMFLOAT3 At(const TaxiPoint& p) { return ServerToEditor(p.x, p.y, p.z); }

/// The mounts players can pick from, by team: creature_template entries Blizzard's nodes use.
struct Mount { uint32_t entry; const char* name; };
const Mount kMounts[2][4] = { { { 0, "None (Horde cannot fly here)" }, { 2224, "Wind Rider" }, { 3574, "Riding Bat" }, { 26899, "Red Drake" } },
                              { { 0, "None (Alliance cannot fly here)" }, { 541, "Riding Gryphon" }, { 3837, "Riding Hippogryph" }, { 26899, "Red Drake" } } };

std::string MountName(int team, uint32_t entry)
{
    for (const Mount& m : kMounts[team])
        if (m.entry == entry) return m.name;
    return "creature " + std::to_string(entry);
}
}

// ---------------------------------------------------------------------------------------------- data

const App::FlightView& App::FlightsOnMap() const
{
    const uint32_t map = CurrentMapId();
    const std::string key = std::to_string(map) + "/" + std::to_string(m_taxiNodes.Version()) + "/" + std::to_string(m_taxiPaths.Version()) + "/" +
                            std::to_string(m_taxiPoints.Version()) + "/" + m_terrain.Map();
    if (key == m_flightViewKey) return m_flightView;
    m_flightViewKey = key;
    m_flightView = {};
    if (m_terrain.Map().empty()) return m_flightView;
    std::set<uint32_t> here;
    for (const TaxiNode& n : m_taxiNodes.All())
        if (n.map == map) { m_flightView.nodes.push_back(n); here.insert(n.id); }
    const auto& byPath = m_taxiPoints.ByPath();
    for (const TaxiPath& p : m_taxiPaths.All())
    {
        bool touches = here.count(p.from) || here.count(p.to);
        if (const auto it = byPath.find(p.id); !touches && it != byPath.end())
            touches = std::any_of(it->second.begin(), it->second.end(), [&](const TaxiPoint& q) { return q.map == map; });
        if (touches) m_flightView.paths.push_back(p);
    }
    return m_flightView;
}

const std::vector<TaxiPoint>& App::FlightPoints() const
{
    static const std::vector<TaxiPoint> none;
    const auto& byPath = m_taxiPoints.ByPath();
    const auto it = m_flightPath ? byPath.find(m_flightPath) : byPath.end();
    return it == byPath.end() ? none : it->second;
}

void App::RefreshFlightMasters()
{
    const uint32_t map = CurrentMapId();
    const std::string key = std::to_string(map) + "/" + (m_db.Connected() ? "db" : "-") + "/" + std::to_string(m_store.Revision()) + "/" + m_flightViewKey;
    if (key == m_flightMastersKey) return;
    m_flightMastersKey = key;
    m_flightMasters.clear();
    if (!m_db.Connected() || m_terrain.Map().empty()) return;
    std::string error;
    const auto rows = m_db.Query("SELECT c.guid, t.name, c.position_x, c.position_y, c.position_z FROM creature c JOIN creature_template t ON t.entry = c.id "
                                 "WHERE c.map = " + std::to_string(map) + " AND t.npcflag & 8192", error);
    if (!rows) { Log("Flight masters: %s", error.c_str()); return; }
    const auto& nodes = FlightsOnMap().nodes;
    for (const auto& r : *rows)
    {
        FlightMaster f;
        f.guid = uint32_t(std::stoul(r[0]));
        f.name = r[1];
        const float x = std::stof(r[2]), y = std::stof(r[3]), z = std::stof(r[4]);
        f.pos = ServerToEditor(x, y, z);
        for (int team = 0; team < 2; ++team)   // ObjectMgr::GetNearestTaxiNode
        {
            float best = 1e30f;
            for (const TaxiNode& n : nodes)
            {
                if (!n.mount[team] || n.id > kTaxiMaxNode) continue;
                const float d = (n.x - x) * (n.x - x) + (n.y - y) * (n.y - y) + (n.z - z) * (n.z - z);
                if (d < best) { best = d; f.node[team] = n.id; }
            }
        }
        m_flightMasters.push_back(std::move(f));
    }
}

void App::CommitDbc(std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows, const std::string& label)
{
    std::vector<Change> parts;
    for (auto& [table, id, row] : rows)
    {
        const nlohmann::json before = table->Row(id);
        if (before == row) continue;
        Change c = table->MakeChange(id, before, row, label);
        table->Apply(c);
        parts.push_back(std::move(c));
    }
    if (parts.empty()) return;
    if (parts.size() == 1) m_store.Commit(std::move(parts[0]));
    else m_store.Commit(std::move(parts), label);
    Log("%s: export, then restart the client and worldserver (taxi DBCs).", label.c_str());
}

std::vector<uint32_t> App::FreeDbcIds(const DbcTable& table, const std::string& kind, size_t count)
{
    std::vector<uint32_t> ids;
    if (!m_project) return ids;
    const Project::IdRange r = m_project->Range(kind);
    for (uint32_t id = std::max(r.first, 1u); id && id <= r.last && ids.size() < count; ++id)
        if (table.Row(id).is_null()) ids.push_back(id);
    if (ids.size() < count) Log("Not enough free %s in the project's range %u-%u (File > Project settings).", kind.c_str(), r.first, r.last);
    return ids;
}

void App::CommitNodeMove(const TaxiNode& before, const TaxiNode& after, const std::string& label)
{
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows{ { &m_taxiNodes, after.id, after.ToRow(m_taxiNodes.Row(after.id)) } };
    // Path ends that sat on the node go with it.
    const float dx = after.x - before.x, dy = after.y - before.y, dz = after.z - before.z;
    const auto& byPath = m_taxiPoints.ByPath();
    for (const TaxiPath& p : m_taxiPaths.All())
    {
        const auto it = byPath.find(p.id);
        if (it == byPath.end() || it->second.empty() || (p.from != after.id && p.to != after.id)) continue;
        for (const TaxiPoint* end : { &it->second.front(), &it->second.back() })
            if ((end == &it->second.front() ? p.from : p.to) == after.id && std::hypot(end->x - before.x, end->y - before.y) < kEndReach)
            {
                TaxiPoint q = *end;
                q.x += dx;
                q.y += dy;
                q.z += dz;
                rows.push_back({ &m_taxiPoints, q.id, q.ToRow(m_taxiPoints.Row(q.id)) });
            }
    }
    CommitDbc(std::move(rows), label);
}

std::optional<float> App::FlightFloor(float x, float y) const
{
    const XMFLOAT3 e = ServerToEditor(x, y, 0);
    std::optional<float> h = m_terrain.HeightAt(e.x, e.z);
    if (!h)   // not loaded: the map's low-detail heights (17 x 17 per tile, row = z, column = x)
    {
        if (m_flightWdlMap != m_terrain.Map())
        {
            m_flightWdlMap = m_terrain.Map();
            const auto bytes = m_mpq.Read("World\\Maps\\" + m_flightWdlMap + "\\" + m_flightWdlMap + ".wdl");
            m_flightWdl = bytes ? ParseWdl(*bytes) : std::vector<std::vector<int16_t>>{};
        }
        const int tx = int(std::floor(e.x / kTileSize)), ty = int(std::floor(e.z / kTileSize));
        if (tx >= 0 && ty >= 0 && tx < 64 && ty < 64 && m_flightWdl.size() == 4096 && m_flightWdl[size_t(ty) * 64 + tx].size() >= 289)
        {
            const auto& g = m_flightWdl[size_t(ty) * 64 + tx];
            const float fx = (e.x - tx * kTileSize) / kTileSize * 16, fy = (e.z - ty * kTileSize) / kTileSize * 16;
            const int x0 = std::clamp(int(fx), 0, 15), y0 = std::clamp(int(fy), 0, 15);
            const float ax = fx - x0, ay = fy - y0;
            auto at = [&](int r, int c) { return float(g[size_t(r) * 17 + c]); };
            h = (at(y0, x0) * (1 - ax) + at(y0, x0 + 1) * ax) * (1 - ay) + (at(y0 + 1, x0) * (1 - ax) + at(y0 + 1, x0 + 1) * ax) * ay;
        }
    }
    // The top of any model standing there (buildings, trees, bridges): a ray straight down from well above.
    const float from = h.value_or(0.0f) + 2000.0f;
    if (const auto hit = m_models.Pick(XMVectorSet(e.x, from, e.z, 1), XMVectorSet(0, -1, 0, 0), 4000.0f, ModelRenderer::DrawSettings{}))
        h = std::max(h.value_or(-1e9f), from - hit->distance);
    return h;
}

std::vector<TaxiPoint> App::FlightDeparture(const TaxiNode& node, const TaxiNode& other) const
{
    std::vector<TaxiPoint> best;
    const float ox = other.x - node.x, oy = other.y - node.y, ol = std::hypot(ox, oy);
    if (ol < 1) return best;
    const float reach = std::min(kDepartureReach, ol / 3);   // nodes close together keep room for the middle
    float bestScore = -2;
    const auto& byPath = m_taxiPoints.ByPath();
    for (const TaxiPath& p : m_taxiPaths.All())
    {
        if ((p.from != node.id && p.to != node.id) || p.from == p.to) continue;
        const auto it = byPath.find(p.id);
        if (it == byPath.end() || it->second.size() < 3) continue;
        std::vector<TaxiPoint> points = it->second;
        if (p.from != node.id) std::reverse(points.begin(), points.end());   // an arrival, flown backwards
        if (points.front().map != node.map || std::hypot(points.front().x - node.x, points.front().y - node.y) > kEndReach) continue;
        std::vector<TaxiPoint> out = TaxiDeparture(points, reach);
        const float vx = out.back().x - node.x, vy = out.back().y - node.y, vl = std::hypot(vx, vy);
        if (out.size() < 2 || vl < 1) continue;
        // Heading most towards the other node; a real departure beats a reversed arrival of the same heading.
        const float score = (vx * ox + vy * oy) / (vl * ol) + (p.from == node.id ? 0.05f : 0.0f);
        if (score > bestScore)
        {
            bestScore = score;
            best = std::move(out);
        }
    }
    return best;
}

void App::CreateFlightPath(uint32_t from, uint32_t to, bool both)
{
    const nlohmann::json &a = m_taxiNodes.Row(from), &b = m_taxiNodes.Row(to);
    if (a.is_null() || b.is_null() || from == to) return;
    const TaxiNode na = TaxiNode::FromRow(a), nb = TaxiNode::FromRow(b);
    // Off and onto the nodes the way their existing flights go; in between over whatever stands below.
    size_t copied = 0;
    auto plan = [&](const TaxiNode& from, const TaxiNode& to) {
        const std::vector<TaxiPoint> takeoff = FlightDeparture(from, to);
        std::vector<TaxiPoint> landing = FlightDeparture(to, from);
        std::reverse(landing.begin(), landing.end());
        copied += !takeoff.empty() + !landing.empty();
        return PlanTaxiPoints(from, to, m_flightCruise, [this](float x, float y) { return FlightFloor(x, y); }, takeoff, landing);
    };
    std::vector<std::vector<TaxiPoint>> plans{ plan(na, nb) };
    if (both) plans.push_back(plan(nb, na));
    Log("Flight path: %zu of %zu takeoffs and landings copied from the nodes' existing flights; the rest climbs straight off the node. "
        "Buildings and trees are cleared where their tiles are loaded.", copied, plans.size() * 2);
    size_t pointCount = 0;
    for (const auto& plan : plans) pointCount += plan.size();
    const std::vector<uint32_t> pathIds = FreeDbcIds(m_taxiPaths, "taxipath.id", plans.size());
    const std::vector<uint32_t> pointIds = FreeDbcIds(m_taxiPoints, "taxipathnode.id", pointCount);
    if (pathIds.size() < plans.size() || pointIds.size() < pointCount) return;
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows;
    size_t next = 0;
    for (size_t i = 0; i < plans.size(); ++i)
    {
        const TaxiPath path{ pathIds[i], i ? to : from, i ? from : to, uint32_t(std::max(m_flightCost, 0)) };
        rows.push_back({ &m_taxiPaths, path.id, path.ToRow(nlohmann::json::object()) });
        for (TaxiPoint p : plans[i])
        {
            p.id = pointIds[next++];
            p.path = path.id;
            rows.push_back({ &m_taxiPoints, p.id, p.ToRow(nlohmann::json::object()) });
        }
    }
    CommitDbc(std::move(rows), "Flight path " + na.name + (both ? " <-> " : " -> ") + nb.name);
    m_flightNode = 0;
    m_flightPath = pathIds[0];
    m_flightPoint.reset();
}

void App::DeleteFlightPath(uint32_t path)
{
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows{ { &m_taxiPaths, path, nullptr } };
    if (const auto it = m_taxiPoints.ByPath().find(path); it != m_taxiPoints.ByPath().end())
        for (const TaxiPoint& p : it->second) rows.push_back({ &m_taxiPoints, p.id, nullptr });
    CommitDbc(std::move(rows), "Delete flight path " + std::to_string(path));
    if (m_flightPath == path) { m_flightPath = 0; m_flightPoint.reset(); }
}

void App::DeleteFlightNode(uint32_t node)
{
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows{ { &m_taxiNodes, node, nullptr } };
    for (const TaxiPath& p : m_taxiPaths.All())
        if (p.from == node || p.to == node)
        {
            rows.push_back({ &m_taxiPaths, p.id, nullptr });
            if (const auto it = m_taxiPoints.ByPath().find(p.id); it != m_taxiPoints.ByPath().end())
                for (const TaxiPoint& q : it->second) rows.push_back({ &m_taxiPoints, q.id, nullptr });
        }
    CommitDbc(std::move(rows), "Delete flight node " + std::to_string(node) + " and its paths");
    if (m_flightNode == node) m_flightNode = 0;
}

void App::InsertFlightPoint(size_t after, const XMFLOAT3& at)
{
    const std::vector<TaxiPoint> points = FlightPoints();
    if (points.empty() || after >= points.size()) return;
    const auto id = FreeDbcIds(m_taxiPoints, "taxipathnode.id", 1);
    if (id.empty()) return;
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows;
    for (size_t i = after + 1; i < points.size(); ++i)   // later points move down one place
    {
        TaxiPoint q = points[i];
        ++q.index;
        rows.push_back({ &m_taxiPoints, q.id, q.ToRow(m_taxiPoints.Row(q.id)) });
    }
    TaxiPoint p = points[after];
    p.id = id[0];
    p.index = points[after].index + 1;
    p.map = CurrentMapId();
    float unused;
    EditorToServer(at, p.x, p.y, unused);
    // Level with its neighbours: the click finds the ground, the flight stays in the air.
    p.z = after + 1 < points.size() ? (points[after].z + points[after + 1].z) / 2 : points[after].z;
    rows.push_back({ &m_taxiPoints, p.id, p.ToRow(nlohmann::json::object()) });
    CommitDbc(std::move(rows), "Add point to flight path " + std::to_string(m_flightPath));
    m_flightPoint = after + 1;
}

void App::DeleteFlightPoint(size_t index)
{
    const std::vector<TaxiPoint> points = FlightPoints();
    if (index >= points.size()) return;
    if (points.size() <= 2) { Log("A flight path needs two points at least: delete the path instead."); return; }
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows{ { &m_taxiPoints, points[index].id, nullptr } };
    for (size_t i = index + 1; i < points.size(); ++i)
    {
        TaxiPoint q = points[i];
        --q.index;
        rows.push_back({ &m_taxiPoints, q.id, q.ToRow(m_taxiPoints.Row(q.id)) });
    }
    CommitDbc(std::move(rows), "Delete point of flight path " + std::to_string(m_flightPath));
    m_flightPoint = index ? index - 1 : 0;
}

// ---------------------------------------------------------------------------------------------- moving

std::optional<App::Transformable> App::FlightTransform()
{
    Transformable t;
    t.rotate = false;
    if (m_flightPath && m_flightPoint && *m_flightPoint < FlightPoints().size())
    {
        const TaxiPoint& p = FlightPoints()[*m_flightPoint];
        const XMFLOAT3 at = At(p);
        XMStoreFloat4x4(&t.frame, XMMatrixTranslation(at.x, at.y, at.z));
        t.limits = "Path points only move.";
        t.what = "point " + std::to_string(*m_flightPoint + 1) + " of flight path " + std::to_string(m_flightPath);
        t.begin = [this] { m_flightPointStart = FlightPoints()[*m_flightPoint]; };
        t.preview = [this](FXMMATRIX delta) {
            TaxiPoint q = m_flightPointStart;
            XMFLOAT3 to;
            const XMFLOAT3 from = At(q);
            XMStoreFloat3(&to, XMVector3TransformCoord(XMLoadFloat3(&from), delta));
            EditorToServer(to, q.x, q.y, q.z);
            m_flightPointPreview = q;
        };
        t.commit = [this](const std::string& label) {
            if (const auto q = std::exchange(m_flightPointPreview, std::nullopt))
                CommitDbc({ { &m_taxiPoints, q->id, q->ToRow(m_taxiPoints.Row(q->id)) } }, label);
        };
        t.cancel = [this] { m_flightPointPreview.reset(); };
        t.ground = [this] {
            TaxiPoint q = FlightPoints()[*m_flightPoint];
            const XMFLOAT3 e = At(q);
            if (const auto h = GroundAt(e.x, e.z, e.y)) q.z = *h;
            CommitDbc({ { &m_taxiPoints, q.id, q.ToRow(m_taxiPoints.Row(q.id)) } }, "Drop flight point to the ground");
        };
        t.remove = [this] { DeleteFlightPoint(*m_flightPoint); };
        return t;
    }
    const nlohmann::json& row = m_flightNode ? m_taxiNodes.Row(m_flightNode) : nlohmann::json();
    if (row.is_null()) return std::nullopt;
    const TaxiNode n = TaxiNode::FromRow(row);
    const XMFLOAT3 at = At(n);
    XMStoreFloat4x4(&t.frame, XMMatrixTranslation(at.x, at.y, at.z));
    t.limits = "Nodes only move; path ends on the node move with it.";
    t.what = "flight node " + std::to_string(n.id);
    t.begin = [this] { m_flightNodeStart = TaxiNode::FromRow(m_taxiNodes.Row(m_flightNode)); };
    t.preview = [this](FXMMATRIX delta) {
        TaxiNode q = m_flightNodeStart;
        XMFLOAT3 to;
        const XMFLOAT3 from = At(q);
        XMStoreFloat3(&to, XMVector3TransformCoord(XMLoadFloat3(&from), delta));
        EditorToServer(to, q.x, q.y, q.z);
        m_flightNodePreview = q;
    };
    t.commit = [this](const std::string& label) {
        if (const auto q = std::exchange(m_flightNodePreview, std::nullopt)) CommitNodeMove(m_flightNodeStart, *q, label);
    };
    t.cancel = [this] { m_flightNodePreview.reset(); };
    t.ground = [this] {
        const TaxiNode before = TaxiNode::FromRow(m_taxiNodes.Row(m_flightNode));
        TaxiNode after = before;
        const XMFLOAT3 e = At(after);
        if (const auto h = GroundAt(e.x, e.z, e.y)) after.z = *h;
        CommitNodeMove(before, after, "Drop flight node " + std::to_string(after.id) + " to the ground");
    };
    t.remove = [this] { DeleteFlightNode(m_flightNode); };
    return t;
}

// ---------------------------------------------------------------------------------------------- viewport

void App::FlightsViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    RefreshFlightMasters();
    const ImGuiIO& io = ImGui::GetIO();
    const FlightView& view = FlightsOnMap();
    const uint32_t map = CurrentMapId();
    m_flightHover.reset();
    if (ImGui::IsItemHovered())
    {
        float best = 12.0f;   // pixels
        auto consider = [&](XMFLOAT3 p, const FlightHit& hit) {
            if (const auto s = ToScreen(viewProj, p, origin, size))
                if (const float d = std::hypot(s->x - io.MousePos.x, s->y - io.MousePos.y); d < best) { best = d; m_flightHover = hit; }
        };
        for (const TaxiNode& n : view.nodes)
        {
            XMFLOAT3 p = At(n);
            p.y += 4;
            consider(p, { n.id, 0, 0 });
        }
        const auto& byPath = m_taxiPoints.ByPath();
        for (const TaxiPath& path : view.paths)
            if (const auto it = byPath.find(path.id); it != byPath.end() && (m_flightAll || path.id == m_flightPath))
                for (size_t i = 0; i < it->second.size(); ++i)
                    if (it->second[i].map == map) consider(At(it->second[i]), { 0, path.id, i });
    }
    if (!ImGui::IsItemActivated() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    if (m_flightPlace)
    {
        if (!m_hover) return;
        if (!io.KeyShift) m_flightPlace = false;
        const auto id = FreeDbcIds(m_taxiNodes, "taxinode.id", 1);
        if (id.empty() || id[0] > kTaxiMaxNode) { Log("No free taxi node id up to %u (AzerothCore's limit).", kTaxiMaxNode); return; }
        TaxiNode n = m_flightNew;
        n.id = id[0];
        n.map = map;
        EditorToServer(m_hover->pos, n.x, n.y, n.z);
        CommitDbc({ { &m_taxiNodes, n.id, n.ToRow(m_taxiNodes.NewRow()) } }, "Add flight node " + std::to_string(n.id) + " " + n.name);
        m_flightNode = n.id;
        m_flightPath = 0;
        m_flightPoint.reset();
        m_flightShowSelected = true;
        Log("Flight node %u placed. A flight master (creature with the flight master flag) standing nearest to it serves it.", n.id);
        return;
    }
    if (io.KeyAlt && m_hover && (m_flightNode || m_flightPoint))
    {
        MoveSelectionTo(m_hover->pos);
        return;
    }
    if (m_flightHover)
    {
        m_flightNode = m_flightHover->node;
        m_flightPath = m_flightHover->path;
        m_flightPoint = m_flightHover->path ? std::optional<size_t>(m_flightHover->point) : std::nullopt;
        m_flightShowSelected = true;
        return;
    }
    if (m_flightPath && m_flightPoint && m_hover)   // the ground with a point selected: a new point after it
    {
        InsertFlightPoint(*m_flightPoint, m_hover->pos);
        return;
    }
    m_flightNode = m_flightPath = 0;
    m_flightPoint.reset();
}

void App::BuildFlightOverlay(std::vector<LineVertex>& lines) const
{
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& c) { lines.push_back({ a, c }); lines.push_back({ b, c }); };
    const FlightView& view = FlightsOnMap();
    const uint32_t map = CurrentMapId();
    for (const TaxiNode& saved : view.nodes)
    {
        const TaxiNode& n = m_flightNodePreview && m_flightNodePreview->id == saved.id ? *m_flightNodePreview : saved;
        const XMFLOAT3 g = At(n);
        const bool selected = n.id == m_flightNode, hovered = m_flightHover && m_flightHover->node == n.id;
        // Alliance blue, Horde red, both gold, nobody grey.
        XMFLOAT4 c = n.mount[0] && n.mount[1] ? XMFLOAT4{ 1, 0.8f, 0.25f, 0.9f } : n.mount[1] ? XMFLOAT4{ 0.35f, 0.6f, 1, 0.9f }
                   : n.mount[0]                ? XMFLOAT4{ 1, 0.35f, 0.3f, 0.9f } : XMFLOAT4{ 0.6f, 0.6f, 0.6f, 0.9f };
        if (selected) c = { 1, 1, 1, 1 };
        else if (hovered) c.w = 1;
        line(g, { g.x, g.y + 6, g.z }, c);
        constexpr float kTwoPi = 6.2831853f;
        for (int i = 0; i < 20; ++i)   // a ring on the ground and one at the top
        {
            const float a0 = kTwoPi * i / 20, a1 = kTwoPi * (i + 1) / 20, r = selected || hovered ? 3.0f : 2.2f;
            line({ g.x + std::cos(a0) * r, g.y + 0.2f, g.z + std::sin(a0) * r }, { g.x + std::cos(a1) * r, g.y + 0.2f, g.z + std::sin(a1) * r }, c);
            line({ g.x + std::cos(a0), g.y + 6, g.z + std::sin(a0) }, { g.x + std::cos(a1), g.y + 6, g.z + std::sin(a1) }, c);
        }
    }
    // Flight masters and the node they serve (both teams' when they differ).
    for (const FlightMaster& f : m_flightMasters)
        for (int team = 0; team < 2; ++team)
            if (f.node[team] && (team == 0 || f.node[1] != f.node[0]))
                for (const TaxiNode& n : view.nodes)
                    if (n.id == f.node[team]) line({ f.pos.x, f.pos.y + 1.5f, f.pos.z }, { At(n).x, At(n).y + 1.5f, At(n).z }, { 0.4f, 1, 0.5f, 0.8f });
    // Paths: thin lines; the selected one is drawn solid (BuildFlightSolids).
    const auto& byPath = m_taxiPoints.ByPath();
    for (const TaxiPath& path : view.paths)
    {
        if (path.id == m_flightPath || (!m_flightAll)) continue;
        const auto it = byPath.find(path.id);
        if (it == byPath.end()) continue;
        const bool hovered = m_flightHover && m_flightHover->path == path.id;
        const XMFLOAT4 c = hovered ? XMFLOAT4{ 1, 0.85f, 0.5f, 1 } : XMFLOAT4{ 0.9f, 0.6f, 0.3f, 0.45f };
        for (size_t i = 1; i < it->second.size(); ++i)
            if (it->second[i - 1].map == map && it->second[i].map == map) line(At(it->second[i - 1]), At(it->second[i]), c);
    }
}

void App::BuildFlightSolids(std::vector<LineVertex>& triangles) const
{
    if (!m_flightPath) return;
    std::vector<TaxiPoint> points = FlightPoints();
    if (m_flightPointPreview)
        for (TaxiPoint& p : points)
            if (p.id == m_flightPointPreview->id) p = *m_flightPointPreview;
    const uint32_t map = CurrentMapId();
    const XMFLOAT4 tube{ 1.0f, 0.55f, 0.15f, 1 }, arrow{ 0.25f, 0.55f, 1.0f, 1 };
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (points[i].map != map) continue;
        const bool sel = m_flightPoint == i, hov = m_flightHover && m_flightHover->path == m_flightPath && m_flightHover->point == i;
        AddSphere(triangles, At(points[i]), sel || hov ? 1.6f : 1.2f, sel ? XMFLOAT4{ 1, 1, 1, 1 } : hov ? XMFLOAT4{ 1, 0.85f, 0.55f, 1 } : tube);
        if (i == 0 || points[i - 1].map != map) continue;
        const XMFLOAT3 a = At(points[i - 1]), b = At(points[i]);
        AddTube(triangles, a, b, 0.35f, tube);
        // An arrow mid-leg: the way the flight goes.
        const XMVECTOR va = XMLoadFloat3(&a), d = XMVectorSubtract(XMLoadFloat3(&b), va);
        const float len = XMVectorGetX(XMVector3Length(d));
        if (len < 6) continue;
        XMFLOAT3 base, tip;
        XMStoreFloat3(&base, XMVectorAdd(va, XMVectorScale(d, 0.5f - 1.5f / len)));
        XMStoreFloat3(&tip, XMVectorAdd(va, XMVectorScale(d, 0.5f + 1.5f / len)));
        AddCone(triangles, base, tip, 0.9f, arrow);
    }
}

void App::DrawFlightLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj) const
{
    for (const TaxiNode& n : FlightsOnMap().nodes)
    {
        XMFLOAT3 p = At(n);
        const float dx = p.x - m_camera.pos.x, dz = p.z - m_camera.pos.z;
        const bool selected = n.id == m_flightNode, hovered = m_flightHover && m_flightHover->node == n.id;
        if (!selected && !hovered && dx * dx + dz * dz > 1500.0f * 1500.0f) continue;
        p.y += 7.5f;
        const auto s = ToScreen(viewProj, p, origin, size);
        if (!s) continue;
        const std::string text = n.name + "  #" + std::to_string(n.id);
        const ImVec2 t = ImGui::CalcTextSize(text.c_str());
        dl->AddText({ s->x - t.x / 2 + 1, s->y - t.y + 1 }, IM_COL32(0, 0, 0, 180), text.c_str());
        dl->AddText({ s->x - t.x / 2, s->y - t.y }, selected || hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 230, 160, 230), text.c_str());
    }
    const std::vector<TaxiPoint>& points = FlightPoints();
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].map == CurrentMapId())
        {
            XMFLOAT3 p = At(points[i]);
            p.y += 2.5f;
            if (const auto s = ToScreen(viewProj, p, origin, size))
            {
                const std::string text = std::to_string(i + 1);
                dl->AddText({ s->x + 1, s->y + 1 }, IM_COL32(0, 0, 0, 180), text.c_str());
                dl->AddText(*s, m_flightPoint == i ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 200, 140, 220), text.c_str());
            }
        }
}

// ---------------------------------------------------------------------------------------------- panel

void App::DrawFlightsPanel(float w)
{
    const FlightView& view = FlightsOnMap();
    const bool editable = m_project && !m_terrain.Map().empty();
    auto nodeName = [&](uint32_t id) {
        const nlohmann::json& row = m_taxiNodes.Row(id);
        return row.is_null() ? "#" + std::to_string(id) + " (missing)" : row.value("Name_lang", std::string()) + "  #" + std::to_string(id);
    };
    auto flyTo = [&](const XMFLOAT3& e) {
        m_camera.pos = { e.x, e.y + 60, e.z - 90 };
        m_camera.yaw = 0;
        m_camera.pitch = -0.55f;
    };
    auto mountCombo = [&](const char* label, int team, uint32_t& mount) {
        bool changed = false;
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::BeginCombo(label, MountName(team, mount).c_str()))
        {
            for (const Mount& m : kMounts[team])
                if (ImGui::Selectable(m.name, m.entry == mount)) { mount = m.entry; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("creature_template entry players ride (TaxiNodes MountCreatureID[%d]); none: this team cannot fly from here.", team);
        return changed;
    };
    std::string filter = m_flightFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    auto matches = [&](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return filter.empty() || text.find(filter) != std::string::npos;
    };

    if (Section(("Nodes (" + std::to_string(view.nodes.size()) + ")###flightnodes").c_str()))
    {
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##flightfilter", "Filter by id or name", &m_flightFilter);
        if (ImGui::BeginChild("##nodelist", { w, 0 }, ImGuiChildFlags_Borders))
            for (const TaxiNode& n : view.nodes)
            {
                std::string label = n.name + "  #" + std::to_string(n.id) + (n.mount[1] ? "  A" : "") + (n.mount[0] ? "  H" : "");
                if (m_project && m_project->Owns("taxinode.id", n.id) && !m_taxiNodes.Row(n.id).is_null()) label += "  (project)";
                if (!matches(label)) continue;
                ImGui::PushID(int(n.id));
                if (ImGui::Selectable(label.c_str(), n.id == m_flightNode, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_flightNode = n.id;
                    m_flightPath = 0;
                    m_flightPoint.reset();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) flyTo(At(n));
                }
                ImGui::SetItemTooltip("Double-click: fly there");
                ImGui::PopID();
            }
        ImGui::EndChild();
    }

    if (Section(("Paths (" + std::to_string(view.paths.size()) + ")###flightpaths").c_str()))
    {
        ImGui::Checkbox("Show every path of the map", &m_flightAll);
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##flightfilter2", "Filter by node name or id", &m_flightFilter);
        if (ImGui::BeginChild("##pathlist", { w, 0 }, ImGuiChildFlags_Borders))
            for (const TaxiPath& p : view.paths)
            {
                const std::string label = nodeName(p.from) + "  ->  " + nodeName(p.to) + "   " + std::to_string(p.cost) + "c  (path " + std::to_string(p.id) + ")";
                if (!matches(label)) continue;
                ImGui::PushID(int(p.id));
                if (ImGui::Selectable(label.c_str(), p.id == m_flightPath, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_flightPath = p.id;
                    m_flightNode = 0;
                    m_flightPoint.reset();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !FlightPoints().empty()) flyTo(At(FlightPoints().front()));
                }
                ImGui::SetItemTooltip("Double-click: fly to its start");
                ImGui::PopID();
            }
        ImGui::EndChild();
    }

    if (Section("New node"))
    {
        if (m_flightNew.name.empty()) { m_flightNew.name = "New flight point"; m_flightNew.mount[0] = 2224; m_flightNew.mount[1] = 541; }
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputText("Name", &m_flightNew.name);
        mountCombo("Alliance", 1, m_flightNew.mount[1]);
        mountCombo("Horde", 0, m_flightNew.mount[0]);
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button(m_flightPlace ? "Click the ground to place  (Esc)" : "Place on the ground", { w, 0 })) m_flightPlace = !m_flightPlace;
        ImGui::EndDisabled();
        if (m_project)
        {
            const Project::IdRange r = m_project->Range("taxinode.id");
            ImGui::TextColored(kQuiet, "Ids from the project range %u-%u (AzerothCore stops at %u).", r.first, r.last, kTaxiMaxNode);
        }
        ImGui::TextColored(kQuiet, "Put it where players land. A flight master (Creatures tool:\na template with the flight master flag) serves the nearest\n"
                                   "node on its map with a mount for the player's team.");
    }

    const bool fresh = std::exchange(m_flightShowSelected, false);
    if (!(m_flightNode || m_flightPath) || !Section("Selected", fresh)) return;
    DrawTransformBar(w);
    if (m_flightNode)
    {
        const nlohmann::json& row = m_taxiNodes.Row(m_flightNode);
        if (row.is_null()) { m_flightNode = 0; return; }
        TaxiNode n = TaxiNode::FromRow(row);
        ImGui::Text("Flight node #%u on %s", n.id, MapLabel(n.map).c_str());
        if (n.id > kTaxiMaxNode) ImGui::TextColored(kWarn, "Id above %u: AzerothCore can never fly from it.", kTaxiMaxNode);
        ImGui::BeginDisabled(!editable);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputText("Name", &n.name);
        if (ImGui::IsItemDeactivatedAfterEdit()) CommitDbc({ { &m_taxiNodes, n.id, n.ToRow(row) } }, "Rename flight node " + std::to_string(n.id));
        if (mountCombo("Alliance", 1, n.mount[1]) | mountCombo("Horde", 0, n.mount[0]))
            CommitDbc({ { &m_taxiNodes, n.id, n.ToRow(row) } }, "Mounts of flight node " + std::to_string(n.id));
        float pos[3] = { n.x, n.y, n.z };
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::DragFloat3("Position", pos, 0.1f, 0, 0, "%.1f"))
        {
            TaxiNode moved = n;
            std::tie(moved.x, moved.y, moved.z) = std::tuple(pos[0], pos[1], pos[2]);
            if (!m_flightNodePreview) m_flightNodeStart = n;
            m_flightNodePreview = moved;
        }
        if (ImGui::IsItemDeactivated() && m_flightNodePreview)
            CommitNodeMove(m_flightNodeStart, *std::exchange(m_flightNodePreview, std::nullopt), "Move flight node " + std::to_string(n.id));
        ImGui::EndDisabled();

        ImGui::SeparatorText("Flight masters");
        if (!m_db.Connected()) ImGui::TextColored(kQuiet, "Connect the world database to see them.");
        else
        {
            size_t serving = 0;
            for (const FlightMaster& f : m_flightMasters)
                for (int team = 0; team < 2; ++team)
                    if (f.node[team] == n.id && (team == 1 || f.node[1] != n.id))
                    {
                        const XMFLOAT3 e = At(n);
                        ImGui::Text("%s  (guid %u)  %.0f yd  %s", f.name.c_str(), f.guid,
                                    std::hypot(f.pos.x - e.x, f.pos.z - e.z), f.node[0] == f.node[1] ? "both teams" : team ? "Alliance" : "Horde");
                        ++serving;
                    }
            if (!serving) ImGui::TextColored(kWarn, "None serves this node: players cannot learn or use it.\nPlace a flight master near it (Creatures tool).");
        }

        ImGui::SeparatorText("Paths");
        for (const TaxiPath& p : view.paths)
            if (p.from == n.id || p.to == n.id)
            {
                ImGui::PushID(int(p.id));
                if (ImGui::Selectable((nodeName(p.from) + "  ->  " + nodeName(p.to) + "   " + std::to_string(p.cost) + "c").c_str()))
                {
                    m_flightPath = p.id;
                    m_flightNode = 0;
                    m_flightPoint.reset();
                }
                ImGui::PopID();
            }
        ImGui::SeparatorText("New path");
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::BeginCombo("To", m_flightTo ? nodeName(m_flightTo).c_str() : "Pick a node", ImGuiComboFlags_HeightLarge))
        {
            for (const TaxiNode& other : view.nodes)
                if (other.id != n.id && ImGui::Selectable((other.name + "  #" + std::to_string(other.id)).c_str(), other.id == m_flightTo)) m_flightTo = other.id;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("And back", &m_flightBoth);
        ImGui::SameLine();
        ImGui::SetNextItemWidth((w - ImGui::GetCursorPosX()) / 2);
        ImGui::InputInt("Cost (copper)", &m_flightCost, 10, 100);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Clearance", &m_flightCruise, 10, 200, "%.0f yd", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How high the flight keeps over the ground and buildings once it has climbed off the node.\nThe route is one straight line over the map, with a point only where something forces a bend.");
        ImGui::BeginDisabled(!editable || !m_flightTo || m_flightTo == n.id);
        if (ImGui::Button("Create path", { w, 0 })) CreateFlightPath(n.id, m_flightTo, m_flightBoth);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Button("Go to node", { (w - 8) / 2, 0 })) flyTo(At(n));
        ImGui::SameLine();
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button("Delete  Del", { (w - 8) / 2, 0 })) DeleteFlightNode(n.id);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Deletes the node and every path from or to it (one undo step).");
        return;
    }

    const nlohmann::json& row = m_taxiPaths.Row(m_flightPath);
    if (row.is_null()) { m_flightPath = 0; return; }
    TaxiPath p = TaxiPath::FromRow(row);
    const std::vector<TaxiPoint>& points = FlightPoints();
    ImGui::Text("Flight path %u", p.id);
    for (const uint32_t end : { p.from, p.to })
    {
        if (ImGui::SmallButton((std::string(end == p.from ? "From " : "To ") + nodeName(end)).c_str()))
        {
            m_flightNode = end;
            m_flightPath = 0;
            m_flightPoint.reset();
            return;
        }
    }
    float length = 0;
    for (size_t i = 1; i < points.size(); ++i) length += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y, points[i].z - points[i - 1].z);
    ImGui::TextColored(kQuiet, "%zu points, %.0f yd (about %.0f s at 32 yd/s)", points.size(), length, length / 32);
    ImGui::BeginDisabled(!editable);
    int cost = int(p.cost);
    ImGui::SetNextItemWidth(w - 90);
    ImGui::InputInt("Cost (copper)", &cost, 10, 100);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        p.cost = uint32_t(std::max(cost, 0));
        CommitDbc({ { &m_taxiPaths, p.id, p.ToRow(row) } }, "Cost of flight path " + std::to_string(p.id));
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kQuiet, "Click a ball: select it; the handles move it. Click the\nground: a point after the selected one. Del: delete it.");
    if (m_flightPoint && *m_flightPoint < points.size())
    {
        const TaxiPoint& q = points[*m_flightPoint];
        const XMFLOAT3 e = At(q);
        const auto ground = m_terrain.HeightAt(e.x, e.z);
        ImGui::Text("Point %zu", *m_flightPoint + 1);
        if (ground) { ImGui::SameLine(); ImGui::TextColored(kQuiet, "  %.0f yd above the ground", q.z - *ground); }
        float pos[3] = { q.x, q.y, q.z };
        ImGui::BeginDisabled(!editable);
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::DragFloat3("Position##point", pos, 0.1f, 0, 0, "%.1f"))
        {
            TaxiPoint moved = q;
            std::tie(moved.x, moved.y, moved.z) = std::tuple(pos[0], pos[1], pos[2]);
            m_flightPointPreview = moved;
        }
        if (ImGui::IsItemDeactivated() && m_flightPointPreview)
        {
            const TaxiPoint moved = *std::exchange(m_flightPointPreview, std::nullopt);
            CommitDbc({ { &m_taxiPoints, moved.id, moved.ToRow(m_taxiPoints.Row(moved.id)) } }, "Move point of flight path " + std::to_string(p.id));
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    const bool back = std::any_of(view.paths.begin(), view.paths.end(), [&](const TaxiPath& o) { return o.from == p.to && o.to == p.from; });
    ImGui::BeginDisabled(!editable || back);
    if (ImGui::Button(back ? "Has a path back" : "Make the path back", { w, 0 }))
    {
        // The same route reversed.
        const auto pathId = FreeDbcIds(m_taxiPaths, "taxipath.id", 1);
        const auto pointIds = FreeDbcIds(m_taxiPoints, "taxipathnode.id", points.size());
        if (!pathId.empty() && pointIds.size() == points.size())
        {
            std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows{ { &m_taxiPaths, pathId[0], TaxiPath{ pathId[0], p.to, p.from, p.cost }.ToRow(nlohmann::json::object()) } };
            for (size_t i = 0; i < points.size(); ++i)
            {
                TaxiPoint r = points[points.size() - 1 - i];
                r.id = pointIds[i];
                r.path = pathId[0];
                r.index = uint32_t(i);
                rows.push_back({ &m_taxiPoints, r.id, r.ToRow(nlohmann::json::object()) });
            }
            CommitDbc(std::move(rows), "Flight path back " + nodeName(p.to) + " -> " + nodeName(p.from));
        }
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Go to start", { (w - 8) / 2, 0 }) && !points.empty()) flyTo(At(points.front()));
    ImGui::SameLine();
    ImGui::BeginDisabled(!editable);
    if (ImGui::Button("Delete path", { (w - 8) / 2, 0 })) DeleteFlightPath(p.id);
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------------------------- checks

void App::CheckFlights(std::vector<Problem>& problems)
{
    for (const char* kind : { "taxinode.id", "taxipath.id", "taxipathnode.id" })
        if (const Project::IdRange r = m_project->Range(kind); !r.first || r.last < r.first)
            problems.push_back({ Problem::Severity::Error, "IDs", std::string("No ") + kind + " range set (File > Project settings)." });
    if (m_project->Range("taxinode.id").last > kTaxiMaxNode)
        problems.push_back({ Problem::Severity::Error, "IDs", "taxinode.id range goes above " + std::to_string(kTaxiMaxNode) + ": AzerothCore cannot use such nodes." });
    // Ids the project added that the client has too (first > last: the client's own nodes fill the range by design).
    m_taxiNodes.CheckIds(1, 0, "Flights", problems);
    m_taxiPaths.CheckIds(1, 0, "Flights", problems);
    m_taxiPoints.CheckIds(1, 0, "Flights", problems);

    // Only what the project touched.
    std::set<uint32_t> nodes, paths;
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain == m_taxiNodes.Domain()) nodes.insert(data.at("id").get<uint32_t>());
        else if (domain == m_taxiPaths.Domain()) paths.insert(data.at("id").get<uint32_t>());
        else if (domain == m_taxiPoints.Domain())
            for (const char* side : { "before", "after" })
                if (!data.at(side).is_null()) paths.insert(data.at(side).value("PathID", 0u));
    });
    const auto& byPath = m_taxiPoints.ByPath();
    const auto allPaths = m_taxiPaths.All();
    for (const uint32_t id : nodes)
    {
        const nlohmann::json& row = m_taxiNodes.Row(id);
        if (row.is_null()) continue;
        const TaxiNode n = TaxiNode::FromRow(row);
        const std::string name = "Flight node " + std::to_string(id) + " " + n.name;
        if (id > kTaxiMaxNode) problems.push_back({ Problem::Severity::Error, "Flights", name + ": id above " + std::to_string(kTaxiMaxNode) + ", AzerothCore cannot use it." });
        if (!n.mount[0] && !n.mount[1]) problems.push_back({ Problem::Severity::Error, "Flights", name + " has no mount for either team: nobody can fly from it." });
        if (std::none_of(allPaths.begin(), allPaths.end(), [&](const TaxiPath& p) { return p.from == id || p.to == id; }))
            problems.push_back({ Problem::Severity::Warning, "Flights", name + " has no paths: players can learn it but fly nowhere." });
    }
    for (const uint32_t id : paths)
    {
        const nlohmann::json& row = m_taxiPaths.Row(id);
        if (row.is_null()) continue;
        const TaxiPath p = TaxiPath::FromRow(row);
        const auto it = byPath.find(id);
        const std::string name = "Flight path " + std::to_string(id);
        if (it == byPath.end() || it->second.size() < 2) { problems.push_back({ Problem::Severity::Error, "Flights", name + " has fewer than two points." }); continue; }
        for (const auto& [end, point] : { std::pair{ p.from, it->second.front() }, std::pair{ p.to, it->second.back() } })
        {
            const nlohmann::json& node = m_taxiNodes.Row(end);
            if (node.is_null()) problems.push_back({ Problem::Severity::Error, "Flights", name + " ends at node " + std::to_string(end) + ", which TaxiNodes.dbc lacks." });
            else if (const TaxiNode n = TaxiNode::FromRow(node); n.map != point.map || std::hypot(n.x - point.x, n.y - point.y) > kEndReach)
                problems.push_back({ Problem::Severity::Warning, "Flights", name + (end == p.from ? " does not start" : " does not end") + " at its node " + n.name + "." });
        }
        for (size_t i = 0; i < it->second.size(); ++i)
            if (it->second[i].index != i)
            {
                problems.push_back({ Problem::Severity::Error, "Flights", name + ": point numbers have a gap (NodeIndex " + std::to_string(it->second[i].index) + ")." });
                break;
            }
    }
}
