#pragma once

#include "Areas.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

/// AzerothCore keeps the taxi nodes a player knows as a bit mask of 14 x 32 bits (TaxiMaskSize): node ids above 448
/// can never be learned or flown from.
constexpr uint32_t kTaxiMaxNode = 14 * 32;

/// A TaxiNodes.dbc row: a flight point. A flight master (creature npcflag 0x2000) serves the nearest node on its map
/// that has a mount for the player's team (ObjectMgr::GetNearestTaxiNode).
struct TaxiNode
{
    uint32_t id = 0, map = 0;
    float x = 0, y = 0, z = 0;          // server coordinates
    std::string name;
    uint32_t mount[2] = {};             // MountCreatureID: [0] Horde, [1] Alliance (creature_template entries; 0 = that team cannot fly here)

    static TaxiNode FromRow(const nlohmann::json& row);
    nlohmann::json ToRow(nlohmann::json base) const;
};

/// A TaxiPath.dbc row: one direction between two nodes (the way back is another path).
struct TaxiPath
{
    uint32_t id = 0, from = 0, to = 0, cost = 0;   // cost in copper

    static TaxiPath FromRow(const nlohmann::json& row);
    nlohmann::json ToRow(nlohmann::json base) const;
};

/// A TaxiPathNode.dbc row: one point the flight passes, in NodeIndex order. A point on another map than the previous
/// one teleports the player there.
struct TaxiPoint
{
    uint32_t id = 0, path = 0, index = 0, map = 0;
    float x = 0, y = 0, z = 0;

    static TaxiPoint FromRow(const nlohmann::json& row);
    nlohmann::json ToRow(nlohmann::json base) const;   // Flags, Delay and the event ids stay as in `base`
};

class TaxiNodesAdapter final : public DbcTable
{
public:
    TaxiNodesAdapter(MpqChain& mpq, ChangeStore& store);
    /// A row of zeros with the client's enUS string flags (new rows).
    nlohmann::json NewRow() const;
    std::vector<TaxiNode> All() const;
};

class TaxiPathAdapter final : public DbcTable
{
public:
    TaxiPathAdapter(MpqChain& mpq, ChangeStore& store);
    std::vector<TaxiPath> All() const;
};

class TaxiPathNodeAdapter final : public DbcTable
{
public:
    TaxiPathNodeAdapter(MpqChain& mpq, ChangeStore& store);
    /// Every path's points in NodeIndex order, rebuilt when the table changes.
    const std::map<uint32_t, std::vector<TaxiPoint>>& ByPath() const;

private:
    mutable std::map<uint32_t, std::vector<TaxiPoint>> m_byPath;
    mutable uint64_t m_byPathVersion = ~0ull;
};

/// How a flight leaves its first point: the points up to the first one `reach` yards (horizontally) away, on the same
/// map. Reversed, how a flight arrives.
std::vector<TaxiPoint> TaxiDeparture(const std::vector<TaxiPoint>& points, float reach);

/// Points for a new path from `a` to `b`, as few as will do: `takeoff` (from a, outwards; empty: straight off the node) and
/// `landing` (ending at b; empty: straight onto the node) without their nearly straight points, and between them one
/// straight line over the map that bends (seen from the side) only where something forces it: it keeps `cruise` yards over
/// `floor` (ground and buildings; nullopt where unknown), climbing to that 1 yard per 2 off either end, never less than
/// 5 yards over anything. Ids are left 0.
std::vector<TaxiPoint> PlanTaxiPoints(const TaxiNode& a, const TaxiNode& b, float cruise,
                                      const std::function<std::optional<float>(float x, float y)>& floor,
                                      const std::vector<TaxiPoint>& takeoff = {}, const std::vector<TaxiPoint>& landing = {});
