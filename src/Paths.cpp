#include "Paths.hpp"

#include "Spawns.hpp"
#include "Tables.hpp"

#include <cstdio>

namespace
{
    std::string Num(float v)
    {
        char b[32];
        snprintf(b, sizeof b, "%.6g", v);
        return b;
    }

    uint32_t U32(const nlohmann::json& row, const char* column)
    {
        auto it = row.find(column);
        return it == row.end() || it->is_null() ? 0 : uint32_t(std::stoul(it->get<std::string>()));
    }
}

uint32_t CreaturePath::PathId(uint32_t guid) const
{
    for (const auto& row : addons.Rows(guid))
        if (const uint32_t id = U32(row, "path_id")) return id;
    for (uint32_t i = 0; i < 10; ++i)   // AzerothCore's convention, guid * 10; the next free id when another path has it
        if (waypoints.Rows(guid * 10 + i).empty()) return guid * 10 + i;
    return guid * 10;
}

std::vector<PathPoint> CreaturePath::Load(uint32_t guid) const
{
    std::vector<PathPoint> points;
    uint32_t id = 0;
    for (const auto& row : addons.Rows(guid)) id = U32(row, "path_id");
    if (!id) return points;
    for (const auto& row : waypoints.Rows(id))
    {
        PathPoint p;
        p.x = std::stof(row.at("position_x").get<std::string>());
        p.y = std::stof(row.at("position_y").get<std::string>());
        p.z = std::stof(row.at("position_z").get<std::string>());
        p.delay = U32(row, "delay");
        p.moveType = U32(row, "move_type");
        p.row = row;
        points.push_back(std::move(p));
    }
    return points;
}

std::vector<Change> CreaturePath::Save(uint32_t guid, const std::vector<PathPoint>& points, std::string& error) const
{
    std::vector<Change> parts;
    const auto creature = creatures.Row(guid);
    if (!creature) { error = "Creature " + std::to_string(guid) + " is not in the database."; return parts; }
    const uint32_t id = PathId(guid);

    // waypoint_data: the points in order, numbered from 1.
    std::vector<nlohmann::json> rows;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const PathPoint& p = points[i];
        nlohmann::json row = p.row.is_object() ? p.row : nlohmann::json{ { "orientation", nullptr }, { "velocity", "0" }, { "smoothTransition", "0" },
                                                                           { "action", "0" }, { "action_chance", "100" }, { "wpguid", "0" } };
        row["id"] = std::to_string(id);
        row["point"] = std::to_string(i + 1);
        row["position_x"] = Num(p.x);
        row["position_y"] = Num(p.y);
        row["position_z"] = Num(p.z);
        row["delay"] = std::to_string(p.delay);
        row["move_type"] = std::to_string(p.moveType);
        rows.push_back(std::move(row));
    }
    const auto oldPoints = waypoints.Rows(id);
    if (rows != oldPoints) parts.push_back(waypoints.MakeChange(id, oldPoints, rows, ""));

    // creature_addon.path_id (other columns kept; a new row has only guid + path_id, the rest default).
    const auto oldAddon = addons.Rows(guid);
    std::vector<nlohmann::json> addon = oldAddon;
    if (addon.empty() && !points.empty()) addon.push_back({ { "guid", std::to_string(guid) } });
    for (auto& row : addon) row["path_id"] = std::to_string(points.empty() ? 0 : id);
    if (addon != oldAddon) parts.push_back(addons.MakeChange(guid, oldAddon, addon, ""));

    // creature: walk the path (2), or stand (0) without one; a path replaces random movement.
    nlohmann::json after = *creature;
    after["MovementType"] = points.empty() ? "0" : "2";
    if (!points.empty()) after["wander_distance"] = "0";
    if (after != *creature) parts.push_back(creatures.MakeChange(creature, after, ""));

    for (const Change& c : parts)   // the tools apply before committing
        if (c.domain == creatures.Domain()) creatures.Apply(c);
        else if (c.domain == waypoints.Domain()) waypoints.Apply(c);
        else addons.Apply(c);
    return parts;
}
