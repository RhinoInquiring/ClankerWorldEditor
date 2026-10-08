#include "Triggers.hpp"

#include <cmath>
#include <cstdio>
#include <random>

namespace
{
    std::string Text(float v)
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%.9g", v);
        return buf;
    }
    float Num(const nlohmann::json& row, const char* column)
    {
        const auto it = row.find(column);
        if (it == row.end() || it->is_null()) return 0;
        return it->is_string() ? std::strtof(it->get<std::string>().c_str(), nullptr) : it->get<float>();
    }
}

// ---------------------------------------------------------------------------------------------- Trigger

Trigger Trigger::FromRow(const nlohmann::json& row)
{
    Trigger t;
    t.id = row.value("ID", 0u);
    t.map = row.value("ContinentID", 0u);
    t.x = row.value("Pos[0]", 0.0f);
    t.y = row.value("Pos[1]", 0.0f);
    t.z = row.value("Pos[2]", 0.0f);
    t.radius = row.value("Radius", 0.0f);
    t.length = row.value("Box_length", 0.0f);
    t.width = row.value("Box_width", 0.0f);
    t.height = row.value("Box_height", 0.0f);
    t.yaw = row.value("Box_yaw", 0.0f);
    return t;
}

nlohmann::json Trigger::ToDbcRow(nlohmann::json base) const
{
    base["ID"] = id;
    base["ContinentID"] = map;
    base["Pos[0]"] = x;
    base["Pos[1]"] = y;
    base["Pos[2]"] = z;
    base["Radius"] = radius;
    base["Box_length"] = length;
    base["Box_width"] = width;
    base["Box_height"] = height;
    base["Box_yaw"] = yaw;
    return base;
}

nlohmann::json Trigger::ToServerRow() const
{
    return { { "entry", std::to_string(id) }, { "map", std::to_string(map) }, { "x", Text(x) }, { "y", Text(y) }, { "z", Text(z) },
             { "radius", Text(radius) }, { "length", Text(length) }, { "width", Text(width) }, { "height", Text(height) },
             { "orientation", Text(yaw) } };
}

bool Trigger::Contains(float px, float py, float pz, float delta) const
{
    const float dx = px - x, dy = py - y, dz = pz - z;
    if (Sphere()) return std::sqrt(dx * dx + dy * dy + dz * dz) <= radius + delta;
    // Position::IsWithinBox: the point turned by -yaw about the centre, then an axis-aligned test.
    const float c = std::cos(yaw), s = std::sin(yaw);
    return std::fabs(dx * c + dy * s) <= length / 2 + delta && std::fabs(dy * c - dx * s) <= width / 2 + delta && std::fabs(dz) <= height / 2 + delta;
}

std::optional<float> Trigger::Hit(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& dir) const
{
    const float ox = origin.x - x, oy = origin.y - y, oz = origin.z - z;
    if (Sphere())
    {
        const float b = ox * dir.x + oy * dir.y + oz * dir.z, c = ox * ox + oy * oy + oz * oz - radius * radius;
        if (c <= 0) return 0.0f;
        const float disc = b * b - c;
        if (b > 0 || disc < 0) return std::nullopt;
        return -b - std::sqrt(disc);
    }
    // Slabs in the box's own frame.
    const float cs = std::cos(yaw), sn = std::sin(yaw);
    const float o[3] = { ox * cs + oy * sn, oy * cs - ox * sn, oz }, d[3] = { dir.x * cs + dir.y * sn, dir.y * cs - dir.x * sn, dir.z };
    const float half[3] = { length / 2, width / 2, height / 2 };
    float t0 = 0, t1 = 1e30f;
    for (int i = 0; i < 3; ++i)
    {
        if (std::fabs(d[i]) < 1e-9f)
        {
            if (std::fabs(o[i]) > half[i]) return std::nullopt;
            continue;
        }
        float a = (-half[i] - o[i]) / d[i], b = (half[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return std::nullopt;
    }
    return t0;
}

// ---------------------------------------------------------------------------------------------- Teleport

Teleport Teleport::FromRow(const nlohmann::json& row)
{
    Teleport t;
    t.trigger = uint32_t(Num(row, "ID"));
    t.map = uint32_t(Num(row, "target_map"));
    t.name = row.contains("Name") && row["Name"].is_string() ? row["Name"].get<std::string>() : "";
    t.x = Num(row, "target_position_x");
    t.y = Num(row, "target_position_y");
    t.z = Num(row, "target_position_z");
    t.o = Num(row, "target_orientation");
    return t;
}

nlohmann::json Teleport::ToRow() const
{
    return { { "ID", std::to_string(trigger) }, { "Name", name }, { "target_map", std::to_string(map) }, { "target_position_x", Text(x) },
             { "target_position_y", Text(y) }, { "target_position_z", Text(z) }, { "target_orientation", Text(o) } };
}

// ---------------------------------------------------------------------------------------------- adapters

AreaTriggerAdapter::AreaTriggerAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "AreaTrigger",
               { { "ID", 0, 'i' }, { "ContinentID", 1, 'i' }, { "Pos[0]", 2, 'f' }, { "Pos[1]", 3, 'f' }, { "Pos[2]", 4, 'f' }, { "Radius", 5, 'f' },
                 { "Box_length", 6, 'f' }, { "Box_width", 7, 'f' }, { "Box_height", 8, 'f' }, { "Box_yaw", 9, 'f' } },
               10)
{
}

std::optional<Trigger> AreaTriggerAdapter::Find(uint32_t id) const
{
    const nlohmann::json& row = Row(id);
    if (row.is_null()) return std::nullopt;
    return Trigger::FromRow(row);
}

std::vector<Trigger> AreaTriggerAdapter::OnMap(uint32_t map) const
{
    std::vector<Trigger> out;
    for (const auto& [id, row] : Rows())
        if (row.value("ContinentID", 0u) == map) out.push_back(Trigger::FromRow(row));
    return out;
}

MapRowsAdapter::MapRowsAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "Map", { { "ID", 0, 'i' }, { "Directory", 1, 's' }, { "InstanceType", 2, 'i' }, { "Flags", 3, 'i' }, { "PVP", 4, 'i' },
                                    { "MapName_lang", 5, 's' }, { "AreaTableID", 22, 'i' }, { "MapDescription0_lang", 23, 's' },
                                    { "MapDescription1_lang", 40, 's' }, { "LoadingScreenID", 57, 'i' }, { "MinimapIconScale", 58, 'f' },
                                    { "CorpseMapID", 59, 'i' }, { "Corpse[0]", 60, 'f' }, { "Corpse[1]", 61, 'f' }, { "TimeOfDayOverride", 62, 'i' },
                                    { "ExpansionID", 63, 'i' }, { "RaidOffset", 64, 'i' }, { "MaxPlayers", 65, 'i' } }, 66)
{
}

// ---------------------------------------------------------------------------------------------- self test

bool TriggersSelfTest()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1, 1);
    bool ok = true;
    for (int n = 0; n < 200 && ok; ++n)
    {
        Trigger t;
        t.x = u(rng) * 100;
        t.y = u(rng) * 100;
        t.z = u(rng) * 20;
        if (n % 2) t.radius = 2 + std::fabs(u(rng)) * 10;
        else { t.length = 2 + std::fabs(u(rng)) * 20; t.width = 2 + std::fabs(u(rng)) * 20; t.height = 2 + std::fabs(u(rng)) * 10; t.yaw = u(rng) * 3.2f; }
        for (int k = 0; k < 200 && ok; ++k)
        {
            const float px = t.x + u(rng) * 25, py = t.y + u(rng) * 25, pz = t.z + u(rng) * 15;
            // AzerothCore's own arithmetic (Player::IsInAreaTriggerRadius, Position::IsWithinBox), in double like the core.
            bool core;
            if (t.Sphere()) core = std::sqrt(double(px - t.x) * (px - t.x) + double(py - t.y) * (py - t.y) + double(pz - t.z) * (pz - t.z)) <= t.radius;
            else
            {
                const double rot = 2 * 3.14159265358979 - t.yaw, s = std::sin(rot), c = std::cos(rot);
                const double bx = px - t.x, by = py - t.y;
                const double rx = bx * c - by * s, ry = by * c + bx * s;
                core = std::fabs(rx) <= t.length / 2 && std::fabs(ry) <= t.width / 2 && std::fabs(pz - t.z) <= t.height / 2;
            }
            if (core != t.Contains(px, py, pz) && t.Contains(px, py, pz, 0.01f) == t.Contains(px, py, pz, -0.01f)) ok = false;   // edge rounding aside
            // A ray from outside towards the point: where it enters, the trigger starts.
            DirectX::XMFLOAT3 from{ px + u(rng) * 80, py + u(rng) * 80, pz + 40 };
            DirectX::XMFLOAT3 dir{ px - from.x, py - from.y, pz - from.z };
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            dir = { dir.x / len, dir.y / len, dir.z / len };
            const auto hit = t.Hit(from, dir);
            if (t.Contains(px, py, pz, -0.01f) && !hit) ok = false;   // the ray reaches an inside point
            if (hit && *hit > 0)
            {
                // The entry point lies on the surface (a grazing ray may leave again at once); just before it is outside.
                const float in = *hit, out = *hit - 0.05f;
                if (!t.Contains(from.x + dir.x * in, from.y + dir.y * in, from.z + dir.z * in, 0.01f) ||
                    t.Contains(from.x + dir.x * out, from.y + dir.y * out, from.z + dir.z * out, -0.001f))
                    ok = false;
            }
        }
    }
    return ok;
}
