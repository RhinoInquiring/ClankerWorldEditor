#pragma once

#include "Changes.hpp"

#include <string>
#include <vector>

class SpawnAdapter;
class TableRowsAdapter;

/// One waypoint of a creature's path (server coordinates). `row` keeps the point's other waypoint_data columns
/// (action, orientation, ...) when it came from the database.
struct PathPoint
{
    float x = 0, y = 0, z = 0;
    uint32_t delay = 0;      // ms waited at the point
    uint32_t moveType = 0;   // 0 walk, 1 run, 2 land, 3 take off (WaypointMoveType)
    nlohmann::json row;      // null for a new point
};

/// A creature's path: creature.MovementType 2 + creature_addon.path_id + waypoint_data rows (the core walks them
/// in point order and loops back to the first).
struct CreaturePath
{
    SpawnAdapter& creatures;
    TableRowsAdapter& waypoints;   // waypoint_data by id
    TableRowsAdapter& addons;      // creature_addon by guid

    /// The path id a creature uses (creature_addon.path_id), else a free one for it: guid * 10 (+0..9 if taken).
    uint32_t PathId(uint32_t guid) const;
    /// The creature's current points (empty: no path).
    std::vector<PathPoint> Load(uint32_t guid) const;
    /// Writes the creature's path (empty `points`: removes it, the creature stands still) and returns the applied
    /// parts for ChangeStore::Commit(parts, label): one undo step. Empty when the creature is unknown.
    std::vector<Change> Save(uint32_t guid, const std::vector<PathPoint>& points, std::string& error) const;
};
