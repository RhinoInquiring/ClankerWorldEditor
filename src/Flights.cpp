#include "Flights.hpp"

#include <algorithm>
#include <cmath>

TaxiNode TaxiNode::FromRow(const nlohmann::json& row)
{
    TaxiNode n;
    n.id = row.value("ID", 0u);
    n.map = row.value("ContinentID", 0u);
    n.x = row.value("Pos[0]", 0.0f);
    n.y = row.value("Pos[1]", 0.0f);
    n.z = row.value("Pos[2]", 0.0f);
    n.name = row.value("Name_lang", std::string());
    n.mount[0] = row.value("MountCreatureID[0]", 0u);
    n.mount[1] = row.value("MountCreatureID[1]", 0u);
    return n;
}

nlohmann::json TaxiNode::ToRow(nlohmann::json base) const
{
    base["ID"] = id;
    base["ContinentID"] = map;
    base["Pos[0]"] = x;
    base["Pos[1]"] = y;
    base["Pos[2]"] = z;
    base["Name_lang"] = name;
    base["MountCreatureID[0]"] = mount[0];
    base["MountCreatureID[1]"] = mount[1];
    return base;
}

TaxiPath TaxiPath::FromRow(const nlohmann::json& row)
{
    return { row.value("ID", 0u), row.value("FromTaxiNode", 0u), row.value("ToTaxiNode", 0u), row.value("Cost", 0u) };
}

nlohmann::json TaxiPath::ToRow(nlohmann::json base) const
{
    base["ID"] = id;
    base["FromTaxiNode"] = from;
    base["ToTaxiNode"] = to;
    base["Cost"] = cost;
    return base;
}

TaxiPoint TaxiPoint::FromRow(const nlohmann::json& row)
{
    TaxiPoint p;
    p.id = row.value("ID", 0u);
    p.path = row.value("PathID", 0u);
    p.index = row.value("NodeIndex", 0u);
    p.map = row.value("ContinentID", 0u);
    p.x = row.value("Loc[0]", 0.0f);
    p.y = row.value("Loc[1]", 0.0f);
    p.z = row.value("Loc[2]", 0.0f);
    return p;
}

nlohmann::json TaxiPoint::ToRow(nlohmann::json base) const
{
    base["ID"] = id;
    base["PathID"] = path;
    base["NodeIndex"] = index;
    base["ContinentID"] = map;
    base["Loc[0]"] = x;
    base["Loc[1]"] = y;
    base["Loc[2]"] = z;
    return base;
}

// ---------------------------------------------------------------------------------------------- adapters

TaxiNodesAdapter::TaxiNodesAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "TaxiNodes",
               { { "ID", 0, 'i' }, { "ContinentID", 1, 'i' }, { "Pos[0]", 2, 'f' }, { "Pos[1]", 3, 'f' }, { "Pos[2]", 4, 'f' },
                 { "Name_lang", 5, 's' }, { "Name_lang_flags", 21, 'i' }, { "MountCreatureID[0]", 22, 'i' }, { "MountCreatureID[1]", 23, 'i' } },
               24)
{
}

nlohmann::json TaxiNodesAdapter::NewRow() const
{
    nlohmann::json row = EmptyRow();
    Read();
    if (!m_client.empty()) row["Name_lang_flags"] = m_client.begin()->second["Name_lang_flags"];
    return row;
}

std::vector<TaxiNode> TaxiNodesAdapter::All() const
{
    std::vector<TaxiNode> out;
    for (const auto& [id, row] : Rows()) out.push_back(TaxiNode::FromRow(row));
    return out;
}

TaxiPathAdapter::TaxiPathAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "TaxiPath", { { "ID", 0, 'i' }, { "FromTaxiNode", 1, 'i' }, { "ToTaxiNode", 2, 'i' }, { "Cost", 3, 'i' } }, 4)
{
}

std::vector<TaxiPath> TaxiPathAdapter::All() const
{
    std::vector<TaxiPath> out;
    for (const auto& [id, row] : Rows()) out.push_back(TaxiPath::FromRow(row));
    return out;
}

TaxiPathNodeAdapter::TaxiPathNodeAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "TaxiPathNode",
               { { "ID", 0, 'i' }, { "PathID", 1, 'i' }, { "NodeIndex", 2, 'i' }, { "ContinentID", 3, 'i' }, { "Loc[0]", 4, 'f' }, { "Loc[1]", 5, 'f' },
                 { "Loc[2]", 6, 'f' }, { "Flags", 7, 'i' }, { "Delay", 8, 'i' }, { "ArrivalEventID", 9, 'i' }, { "DepartureEventID", 10, 'i' } },
               11)
{
}

const std::map<uint32_t, std::vector<TaxiPoint>>& TaxiPathNodeAdapter::ByPath() const
{
    if (m_byPathVersion == Version() && !m_byPath.empty()) return m_byPath;
    m_byPathVersion = Version();
    m_byPath.clear();
    for (const auto& [id, row] : Rows())
    {
        const TaxiPoint p = TaxiPoint::FromRow(row);
        m_byPath[p.path].push_back(p);
    }
    for (auto& [path, points] : m_byPath) std::sort(points.begin(), points.end(), [](const TaxiPoint& a, const TaxiPoint& b) { return a.index < b.index; });
    return m_byPath;
}

std::vector<TaxiPoint> TaxiDeparture(const std::vector<TaxiPoint>& points, float reach)
{
    std::vector<TaxiPoint> out;
    for (const TaxiPoint& p : points)
    {
        if (!out.empty() && p.map != out.front().map) break;
        out.push_back(p);
        if (std::hypot(p.x - points.front().x, p.y - points.front().y) >= reach) break;
    }
    return out;
}

namespace
{
    constexpr float kSample = 4.0f;    // yards between the heights read along the route
    constexpr float kSlack = 5.0f;     // yards of room over the needed heights, so legs can be straightened without going under them
    constexpr float kMargin = 5.0f;    // yards a flight always keeps above anything, even while climbing off a node
    constexpr float kClimb = 0.5f;     // yards up per yard out while climbing to the clearance height off a node

    /// Keeps the fewest points of `pts` (in order) whose straight legs stay within `tolerance` of the dropped ones
    /// (Douglas-Peucker); `gap(a, b, k)` is how far point k lies from the leg a-b.
    template <class P, class Gap>
    std::vector<P> Simplify(const std::vector<P>& pts, float tolerance, const Gap& gap)
    {
        if (pts.size() < 3) return pts;
        std::vector<bool> keep(pts.size(), false);
        keep.front() = keep.back() = true;
        std::vector<std::pair<size_t, size_t>> todo{ { 0, pts.size() - 1 } };
        while (!todo.empty())
        {
            const auto [a, b] = todo.back();
            todo.pop_back();
            size_t worst = 0;
            float widest = tolerance;
            for (size_t k = a + 1; k < b; ++k)
                if (const float g = gap(pts[a], pts[b], pts[k]); g > widest) { widest = g; worst = k; }
            if (!worst) continue;
            keep[worst] = true;
            todo.push_back({ a, worst });
            todo.push_back({ worst, b });
        }
        std::vector<P> out;
        for (size_t i = 0; i < pts.size(); ++i)
            if (keep[i]) out.push_back(pts[i]);
        return out;
    }

    /// Distance of point k from the 3D segment a-b.
    float SegmentGap(const TaxiPoint& a, const TaxiPoint& b, const TaxiPoint& k)
    {
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, len2 = dx * dx + dy * dy + dz * dz;
        const float t = len2 > 0 ? std::clamp(((k.x - a.x) * dx + (k.y - a.y) * dy + (k.z - a.z) * dz) / len2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(k.x - (a.x + dx * t), k.y - (a.y + dy * t), k.z - (a.z + dz * t));
    }
}

std::vector<TaxiPoint> PlanTaxiPoints(const TaxiNode& a, const TaxiNode& b, float cruise, const std::function<std::optional<float>(float, float)>& floor,
                                      const std::vector<TaxiPoint>& takeoff, const std::vector<TaxiPoint>& landing)
{
    auto spot = [](const TaxiNode& n) { TaxiPoint p; p.map = n.map; p.x = n.x; p.y = n.y; p.z = n.z; return p; };
    // Copied takeoff and landing, without their nearly straight points.
    std::vector<TaxiPoint> out = takeoff.empty() ? std::vector<TaxiPoint>{ spot(a) } : Simplify(takeoff, 2.0f, SegmentGap);
    const std::vector<TaxiPoint> end = landing.empty() ? std::vector<TaxiPoint>{ spot(b) } : Simplify(landing, 2.0f, SegmentGap);
    const TaxiPoint s = out.back(), e = end.front();

    // The middle flies straight over the map from s to e. Seen from the side it is the tightest line over the profile
    // of what it must clear (the upper convex hull), then straightened: a point only where something forces a bend.
    struct Spot { float d, h; };
    const float dx = e.x - s.x, dy = e.y - s.y, length = std::sqrt(dx * dx + dy * dy);
    std::vector<Spot> need, profile{ { 0, s.z } };   // the heights it must keep; those plus room to straighten
    const int samples = int(std::ceil(length / kSample));
    std::vector<std::optional<float>> raw(size_t(samples) + 1);
    for (int k = 0; k <= samples; ++k) raw[size_t(k)] = floor(s.x + dx * k / samples, s.y + dy * k / samples);
    for (int k = 1; k < samples; ++k)
    {
        const float d = length * k / samples;
        // What stands here or at either neighbouring sample: an edge between two samples is not missed.
        std::optional<float> under;
        for (int n = k - 1; n <= k + 1; ++n)
            if (raw[size_t(n)]) under = std::max(under.value_or(-1e9f), *raw[size_t(n)]);
        if (!under) continue;
        // The clearance height, reached by climbing off either end, and always some room over anything (that room and
        // the slack grow with the climb, so a flight can leave a node on the ground).
        const float out = std::min(d, length - d), climb = std::min(s.z + kClimb * d, e.z + kClimb * (length - d));
        const float h = std::max(*under + std::min(kMargin, kClimb * out), std::min(*under + cruise, climb));
        need.push_back({ d, h });
        profile.push_back({ d, h + std::min(kSlack, kClimb * out) });
    }
    profile.push_back({ length, e.z });
    std::vector<Spot> hull;
    for (const Spot& p : profile)
    {
        while (hull.size() >= 2)
        {
            const Spot &o = hull[hull.size() - 2], &q = hull.back();
            if ((q.d - o.d) * (p.h - o.h) - (q.h - o.h) * (p.d - o.d) < 0) break;   // a right turn: q stays on top
            hull.pop_back();
        }
        hull.push_back(p);
    }
    // The hull bends at every sample of a rounded hill: from each point, straight on to the farthest one whose leg still
    // keeps every needed height.
    auto over = [&](const Spot& a, const Spot& b) {
        for (const Spot& n : need)
            if (n.d > a.d && n.d < b.d && a.h + (b.h - a.h) * (n.d - a.d) / (b.d - a.d) < n.h - 1e-3f) return false;
        return true;
    };
    std::vector<Spot> kept{ hull.front() };
    for (size_t i = 0; i + 1 < hull.size();)
    {
        size_t j = hull.size() - 1;
        while (j > i + 1 && !over(hull[i], hull[j])) --j;
        kept.push_back(hull[j]);
        i = j;
    }
    hull = std::move(kept);
    for (size_t i = 1; i + 1 < hull.size(); ++i)
    {
        TaxiPoint p;
        p.map = s.map;
        p.x = s.x + dx * hull[i].d / length;
        p.y = s.y + dy * hull[i].d / length;
        p.z = hull[i].h;
        out.push_back(p);
    }
    out.insert(out.end(), end.begin(), end.end());
    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i].index = uint32_t(i);
        out[i].id = out[i].path = 0;   // the caller numbers them
    }
    return out;
}
