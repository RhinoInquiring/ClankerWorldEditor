#pragma once

#include "Areas.hpp"

#include <DirectXMath.h>

#include <optional>
#include <vector>

/// An area trigger: a sphere (radius > 0) or a box turned by `yaw` about its centre, in server coordinates. The client
/// fires it from its AreaTrigger.dbc row; AzerothCore checks the player against its `areatrigger` row (same id, same
/// shape; Player::IsInAreaTriggerRadius) before acting on it (areatrigger_teleport, quests, taverns, scripts).
struct Trigger
{
    uint32_t id = 0, map = 0;
    float x = 0, y = 0, z = 0;
    float radius = 0, length = 0, width = 0, height = 0, yaw = 0;   // box: length along the yaw direction, width across

    bool Sphere() const { return radius > 0; }
    static Trigger FromRow(const nlohmann::json& dbcRow);
    /// `base` (the current row, or empty) with the trigger's fields.
    nlohmann::json ToDbcRow(nlohmann::json base = nlohmann::json::object()) const;
    /// The world database's `areatrigger` row (column -> value text).
    nlohmann::json ToServerRow() const;
    /// Whether a point (server coordinates) is inside, as the server tests it.
    bool Contains(float px, float py, float pz, float delta = 0) const;
    /// Entry distance along a ray (server coordinates); 0 when it starts inside, nullopt when it misses.
    std::optional<float> Hit(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& dir) const;
};

/// An areatrigger_teleport row: where entering a trigger sends the player.
struct Teleport
{
    uint32_t trigger = 0, map = 0;
    std::string name;
    float x = 0, y = 0, z = 0, o = 0;

    static Teleport FromRow(const nlohmann::json& row);
    nlohmann::json ToRow() const;
};

/// AreaTrigger.dbc.
class AreaTriggerAdapter final : public DbcTable
{
public:
    AreaTriggerAdapter(MpqChain& mpq, ChangeStore& store);
    std::optional<Trigger> Find(uint32_t id) const;
    std::vector<Trigger> OnMap(uint32_t map) const;
};

/// Map.dbc, only the fields the editor reads (InstanceType) and changes: CorpseMapID and Corpse (x, y), where a dead
/// player's spirit is sent to run back into an instance. AzerothCore (MapEntry::entrance_map) finds no exit trigger
/// for a map without one, and for maps that are not dungeons the exit is the trigger leading to CorpseMapID.
class MapRowsAdapter final : public DbcTable
{
public:
    MapRowsAdapter(MpqChain& mpq, ChangeStore& store);
};

/// Contains and Hit against AzerothCore's formulas on synthetic triggers; false on a mismatch.
bool TriggersSelfTest();
/// The shared move / rotate / scale math (AppTransform.cpp): facings, trigger frames; false on a mismatch.
bool TransformSelfTest();
