#include "Spawns.hpp"

#include "Formats.hpp"
#include "Server.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    constexpr float kMapCentre = 32.0f * kTileSize;   // server (0, 0) is the corner of tiles 32 / 32

    std::string Text(const nlohmann::json& row, const char* column, const std::string& fallback = {})
    {
        auto it = row.find(column);
        return it == row.end() || it->is_null() ? fallback : it->get<std::string>();
    }

    std::string Num(float v)
    {
        char b[32];
        snprintf(b, sizeof b, "%.6g", v);
        return b;
    }
}

// ---------------------------------------------------------------------------------------------- rows

Spawn Spawn::FromRow(const nlohmann::json& row, SpawnKind kind)
{
    Spawn s;
    s.kind = kind;
    auto u = [&](const char* c, uint32_t fallback) { return uint32_t(std::stoul(Text(row, c, std::to_string(fallback)))); };
    auto f = [&](const char* c, float fallback) { return std::stof(Text(row, c, Num(fallback))); };
    s.guid = u("guid", 0);
    s.entry = u("id", 0);
    s.map = u("map", 0);
    s.zoneId = u("zoneId", 0);
    s.areaId = u("areaId", 0);
    s.x = f("position_x", 0);
    s.y = f("position_y", 0);
    s.z = f("position_z", 0);
    s.orientation = f("orientation", 0);
    s.spawnTime = u("spawntimesecs", 300);
    s.wander = f("wander_distance", 0);
    s.movementType = u("MovementType", 0);
    s.name = Text(row, "name");
    s.displayId = u("displayId", 0);
    s.size = f("size", 1);
    for (int hand = 0; hand < 2; ++hand)
    {
        const std::string n = "weapon" + std::to_string(hand + 1);
        s.weapons[hand] = u(n.c_str(), 0);
        s.weaponTypes[hand] = u((n + "Type").c_str(), 0);
    }
    std::stringstream events(Text(row, "events"));   // "12,-7" (GROUP_CONCAT)
    for (std::string e; std::getline(events, e, ',');) s.events.push_back(std::stoi(e));
    return s;
}

bool Spawn::InWorld(int event) const
{
    bool any = false, during = false;
    for (int e : events)
    {
        if (e == -event) return false;
        any |= e > 0;
        during |= e == event;
    }
    return !any || during;
}

nlohmann::json Spawn::ToRow(nlohmann::json row) const
{
    // Joined from the template or game_event_*, not spawn columns.
    row.erase("name");
    row.erase("displayId");
    row.erase("size");
    row.erase("events");
    for (const char* c : { "weapon1", "weapon1Type", "weapon2", "weapon2Type" }) row.erase(c);
    const bool turned = Text(row, "orientation") != Num(orientation);
    row["guid"] = std::to_string(guid);
    row["id"] = std::to_string(entry);
    row["map"] = std::to_string(map);
    row["zoneId"] = std::to_string(zoneId);
    row["areaId"] = std::to_string(areaId);
    row["position_x"] = Num(x);
    row["position_y"] = Num(y);
    row["position_z"] = Num(z);
    row["orientation"] = Num(orientation);
    row["spawntimesecs"] = std::to_string(spawnTime);
    if (kind == SpawnKind::Creature)
    {
        row["wander_distance"] = Num(wander);
        // Wander means random movement (1); no wander stops it, but a waypoint path (2) stays.
        if (wander > 0) row["MovementType"] = "1";
        else if (Text(row, "MovementType", "0") != "2") row["MovementType"] = "0";
        if (!row.contains("equipment_id")) row["equipment_id"] = "1";   // as .npc add saves it: the template's first set
    }
    else
    {
        if (turned)   // yaw about z only, as the core builds it from an orientation (GameObject::SetLocalRotationAngles)
        {
            row["rotation0"] = "0";
            row["rotation1"] = "0";
            row["rotation2"] = Num(std::sin(orientation / 2));
            row["rotation3"] = Num(std::cos(orientation / 2));
        }
        if (!row.contains("animprogress")) row["animprogress"] = "100";
        if (!row.contains("state")) row["state"] = "1";   // GO_STATE_READY
    }
    if (!row.contains("spawnMask")) row["spawnMask"] = "1";
    if (!row.contains("phaseMask")) row["phaseMask"] = "1";
    if (!row.contains("Comment")) row["Comment"] = "wow-world-editor";
    return row;
}

DirectX::XMFLOAT3 ServerToEditor(float x, float y, float z) { return { kMapCentre - y, z, kMapCentre - x }; }

void EditorToServer(const DirectX::XMFLOAT3& p, float& x, float& y, float& z)
{
    x = kMapCentre - p.z;
    y = kMapCentre - p.x;
    z = p.y;
}

const char* GameObjectTypeName(uint32_t type)
{
    // GameobjectTypes in AzerothCore SharedDefines.h.
    static const char* const names[] = { "DOOR", "BUTTON", "QUESTGIVER", "CHEST", "BINDER", "GENERIC", "TRAP", "CHAIR", "SPELL_FOCUS",
                                         "TEXT", "GOOBER", "TRANSPORT", "AREADAMAGE", "CAMERA", "MAP_OBJECT", "MO_TRANSPORT",
                                         "DUEL_ARBITER", "FISHINGNODE", "SUMMONING_RITUAL", "MAILBOX", "DO_NOT_USE", "GUARDPOST",
                                         "SPELLCASTER", "MEETINGSTONE", "FLAGSTAND", "FISHINGHOLE", "FLAGDROP", "MINI_GAME",
                                         "DO_NOT_USE_2", "CAPTURE_POINT", "AURA_GENERATOR", "DUNGEON_DIFFICULTY", "BARBER_CHAIR",
                                         "DESTRUCTIBLE_BUILDING", "GUILD_BANK", "TRAPDOOR" };
    return type < std::size(names) ? names[type] : "?";
}

// ---------------------------------------------------------------------------------------------- adapter

bool SpawnAdapter::Connected() const { return m_db && m_db->Connected(); }

std::vector<SpawnAdapter::RowChange> SpawnAdapter::Rows(const nlohmann::json& data)
{
    // {"rows": [{guid, before, after}, ...]}; older changes hold one row as {guid, before, after}.
    std::vector<RowChange> rows;
    if (const auto it = data.find("rows"); it != data.end())
        for (const auto& r : *it) rows.push_back({ r.at("guid"), &r.at("before"), &r.at("after") });
    else
        rows.push_back({ data.at("guid"), &data.at("before"), &data.at("after") });
    return rows;
}

void SpawnAdapter::NetState(std::map<uint32_t, nlohmann::json>& now, std::map<uint32_t, nlohmann::json>& original) const
{
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain != Domain()) return;
        for (const RowChange& r : Rows(data))
        {
            original.try_emplace(r.guid, *r.before);
            now[r.guid] = *r.after;
        }
    });
}

bool SpawnAdapter::Write(uint32_t guid, const nlohmann::json& row, std::string& error) const
{
    if (!Connected()) { error = "Not connected to the world database."; return false; }
    const std::string sql = row.is_null() ? std::string("DELETE FROM ") + Table() + " WHERE guid = " + std::to_string(guid) : UpsertSql(*m_db, Table(), row);
    return m_db->Query(sql, error).has_value();
}

void SpawnAdapter::Set(const Change& change, bool after)
{
    m_aroundKey.clear();
    if (!Connected()) return;   // Sync writes it once connected
    std::vector<RowChange> rows = Rows(change.data);
    if (!after) std::reverse(rows.begin(), rows.end());
    m_lastError.clear();
    for (const RowChange& r : rows)
        if (std::string error; !Write(r.guid, after ? *r.after : *r.before, error))
            m_lastError = "Spawn " + std::to_string(r.guid) + ": " + error;
}

bool SpawnAdapter::Sync(std::string& error)
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    for (const auto& [guid, row] : now)
        if (!Write(guid, row, error)) { m_lastError = error; return false; }
    m_lastError.clear();
    m_aroundKey.clear();
    return true;
}

const std::vector<Spawn>& SpawnAdapter::Around(uint32_t map, float minX, float minY, float maxX, float maxY)
{
    char key[128];
    snprintf(key, sizeof key, "%u %.0f %.0f %.0f %.0f %zu %d", map, minX, minY, maxX, maxY, m_store.Revision(), Connected());
    if (m_aroundKey == key) return m_around;
    m_aroundKey = key;
    ++m_version;
    m_around.clear();
    std::map<uint32_t, Spawn> found;
    std::string error;
    if (Connected())
    {
        const bool creature = m_kind == SpawnKind::Creature;
        const std::string sql =
            "SELECT c.guid, c.id, c.map, c.zoneId, c.areaId, c.position_x, c.position_y, c.position_z, c.orientation, c.spawntimesecs, "
            "(SELECT GROUP_CONCAT(ge.eventEntry) FROM game_event_" + std::string(Table()) + " ge WHERE ge.guid = c.guid) AS events, " +
            std::string(creature ? "c.wander_distance, c.MovementType, m.CreatureDisplayID AS displayId, m.DisplayScale AS size, t.name, "
                                   "i1.displayid AS weapon1, i1.InventoryType AS weapon1Type, i2.displayid AS weapon2, i2.InventoryType AS weapon2Type FROM creature c "
                                   "LEFT JOIN creature_template t ON t.entry = c.id "
                                   "LEFT JOIN creature_template_model m ON m.CreatureID = c.id AND m.Idx = 0 "
                                   // equipment_id 0 = none, -1 = random (the first set shown), else that set
                                   "LEFT JOIN creature_equip_template e ON e.CreatureID = c.id AND e.ID = IF(c.equipment_id < 0, 1, c.equipment_id) "
                                   "LEFT JOIN item_template i1 ON i1.entry = e.ItemID1 LEFT JOIN item_template i2 ON i2.entry = e.ItemID2"
                                 : "t.displayId, t.size, t.name FROM gameobject c LEFT JOIN gameobject_template t ON t.entry = c.id") +
            " WHERE c.map = " + std::to_string(map) + " AND c.position_x BETWEEN " + Num(minX) + " AND " + Num(maxX) +
            " AND c.position_y BETWEEN " + Num(minY) + " AND " + Num(maxY) + " LIMIT 20000";
        if (auto rows = m_db->QueryRows(sql, error))
            for (const auto& r : *rows) { Spawn s = Spawn::FromRow(r, m_kind); found[s.guid] = s; }
    }
    // The project's own state wins (and shows while offline).
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    for (const auto& [guid, row] : now)
    {
        if (row.is_null()) { found.erase(guid); continue; }
        Spawn s = Spawn::FromRow(row, m_kind);
        if (s.map != map || s.x < minX || s.x > maxX || s.y < minY || s.y > maxY) { found.erase(guid); continue; }
        if (auto it = found.find(guid); it != found.end()) {
            s.name = it->second.name;
            s.displayId = it->second.displayId;
            s.size = it->second.size;
            std::copy(std::begin(it->second.weapons), std::end(it->second.weapons), s.weapons);
            std::copy(std::begin(it->second.weaponTypes), std::end(it->second.weaponTypes), s.weaponTypes);
            s.events = it->second.events;
        }
        found[guid] = s;
    }
    for (auto& [guid, s] : found) m_around.push_back(std::move(s));
    return m_around;
}

std::vector<SpawnAdapter::Template> SpawnAdapter::Search(const std::string& text, std::string& error) const
{
    if (!Connected()) { error = "Not connected to the world database."; return {}; }
    const bool number = !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    return Templates(number ? "t.entry = " + text : "t.name LIKE " + m_db->Quote("%" + text + "%"), 60, error);
}

std::vector<SpawnAdapter::Template> SpawnAdapter::Templates(const std::string& where, size_t limit, std::string& error) const
{
    std::vector<Template> out;
    if (!Connected()) { error = "Not connected to the world database."; return out; }
    const std::string tail = " ORDER BY t.name" + (limit ? " LIMIT " + std::to_string(limit) : std::string());
    if (m_kind == SpawnKind::Creature)
    {
        if (auto rows = m_db->Query("SELECT t.entry, t.name, t.subname, t.minlevel, t.maxlevel, COALESCE(m.CreatureDisplayID, 0), COALESCE(m.DisplayScale, 1), "
                                    "COALESCE(i1.displayid, 0), COALESCE(i1.InventoryType, 0), COALESCE(i2.displayid, 0), COALESCE(i2.InventoryType, 0), t.type "
                                    "FROM creature_template t LEFT JOIN creature_template_model m ON m.CreatureID = t.entry AND m.Idx = 0 "
                                    "LEFT JOIN creature_equip_template e ON e.CreatureID = t.entry AND e.ID = 1 "
                                    "LEFT JOIN item_template i1 ON i1.entry = e.ItemID1 LEFT JOIN item_template i2 ON i2.entry = e.ItemID2 WHERE " + where + tail, error))
            for (const auto& r : *rows)
            {
                Template t{ uint32_t(std::stoul(r[0])), r[1], (r[2].empty() ? "" : "<" + r[2] + ">  ") + "(" + r[3] + "-" + r[4] + ")",
                            uint32_t(std::stoul(r[5])), std::stof(r[6]) };
                for (int hand = 0; hand < 2; ++hand)
                {
                    t.weapons[hand] = uint32_t(std::stoul(r[7 + hand * 2]));
                    t.weaponTypes[hand] = uint32_t(std::stoul(r[8 + hand * 2]));
                }
                t.category = uint32_t(std::stoul(r[11]));
                out.push_back(std::move(t));
            }
    }
    else if (auto rows = m_db->Query("SELECT t.entry, t.name, t.type, t.displayId, t.size FROM gameobject_template t WHERE " + where + tail, error))
        for (const auto& r : *rows)
        {
            Template t{ uint32_t(std::stoul(r[0])), r[1], GameObjectTypeName(uint32_t(std::stoul(r[2]))), uint32_t(std::stoul(r[3])), std::stof(r[4]) };
            t.category = uint32_t(std::stoul(r[2]));
            out.push_back(std::move(t));
        }
    return out;
}

std::optional<uint32_t> SpawnAdapter::NextGuid(uint32_t first, uint32_t last) const
{
    // Above every guid used in the range: the project's (redo-able ones too) and the database's. Never reused.
    uint64_t next = first;
    auto use = [&](uint32_t guid) { if (guid >= first && guid <= last) next = std::max<uint64_t>(next, uint64_t(guid) + 1); };
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    for (const auto& [guid, row] : now) use(guid);
    ChangeStore::ForEach(m_store.Undone(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain == Domain())
            for (const RowChange& r : Rows(data)) use(r.guid);
    });
    if (Connected())
    {
        std::string error;
        if (auto rows = m_db->Query(std::string("SELECT MAX(guid) FROM ") + Table() + " WHERE guid BETWEEN " + std::to_string(first) + " AND " +
                                    std::to_string(last), error);
            rows && !rows->empty() && !(*rows)[0][0].empty())
            use(uint32_t(std::stoul((*rows)[0][0])));
    }
    if (!first || next > last) return std::nullopt;
    return uint32_t(next);
}

SpawnAdapter::RangeUse SpawnAdapter::Use(uint32_t first, uint32_t last) const
{
    RangeUse u;
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    std::set<uint32_t> created;   // rows this project added (not ones it edited)
    for (const auto& [guid, row] : now)
        if (original[guid].is_null() && guid >= first && guid <= last) created.insert(guid);
    u.mine = created.size();
    if (!Connected()) return u;
    std::string error;
    if (auto rows = m_db->Query(std::string("SELECT guid FROM ") + Table() + " WHERE guid BETWEEN " + std::to_string(first) + " AND " + std::to_string(last), error))
        for (const auto& r : *rows)
            if (!created.count(uint32_t(std::stoul(r[0])))) ++u.others;
    if (auto rows = m_db->Query(std::string("SELECT COALESCE(MAX(guid), 0) FROM ") + Table(), error); rows && !rows->empty())
        u.highest = uint32_t(std::stoul((*rows)[0][0]));
    return u;
}

std::optional<nlohmann::json> SpawnAdapter::Row(uint32_t guid) const
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    if (auto it = now.find(guid); it != now.end()) return it->second.is_null() ? std::nullopt : std::optional(it->second);
    if (!Connected()) return std::nullopt;
    std::string error;
    auto rows = m_db->QueryRows(std::string("SELECT * FROM ") + Table() + " WHERE guid = " + std::to_string(guid), error);
    if (!rows || rows->empty()) return std::nullopt;
    return (*rows)[0];
}

Change SpawnAdapter::MakeChange(const std::optional<nlohmann::json>& before, const std::optional<nlohmann::json>& after, const std::string& label) const
{
    Change c;
    c.domain = Domain();
    c.label = label;
    const nlohmann::json& any = after ? *after : *before;
    c.target = std::string(Table()) + " " + Text(any, "guid");
    c.data = { { "guid", uint32_t(std::stoul(Text(any, "guid", "0"))) },
               { "before", before ? *before : nlohmann::json() },
               { "after", after ? *after : nlohmann::json() } };
    return c;
}

Change SpawnAdapter::MakeChange(const std::vector<std::pair<std::optional<nlohmann::json>, std::optional<nlohmann::json>>>& rows,
                                const std::string& label) const
{
    if (rows.size() == 1) return MakeChange(rows[0].first, rows[0].second, label);
    Change c;
    c.domain = Domain();
    c.label = label;
    c.target = std::to_string(rows.size()) + " " + Table() + " spawns";
    c.data = { { "rows", nlohmann::json::array() } };
    for (const auto& [before, after] : rows)
    {
        const nlohmann::json& any = after ? *after : *before;
        c.data["rows"].push_back({ { "guid", uint32_t(std::stoul(Text(any, "guid", "0"))) },
                                   { "before", before ? *before : nlohmann::json() },
                                   { "after", after ? *after : nlohmann::json() } });
    }
    return c;
}

bool SpawnAdapter::ExportSql(const fs::path& outDir, std::string& error) const
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    if (now.empty()) return true;
    // Statements without a connection (no escaping service needed for these values: digits and our comment).
    Db offline;
    auto statements = [&](const std::map<uint32_t, nlohmann::json>& rows) {
        std::string sql;
        for (const auto& [guid, row] : rows)
            sql += row.is_null() ? std::string("DELETE FROM ") + Table() + " WHERE guid = " + std::to_string(guid) + ";\n"
                                 : UpsertSql(m_db ? *m_db : offline, Table(), row) + ";\n";
        return sql;
    };
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const std::string table = Table();
    std::ofstream apply(outDir / (table + "_spawns.sql")), revert(outDir / (table + "_spawns_revert.sql"));
    apply << "-- wow-world-editor: " << table << " spawns of this project (world database). Restart worldserver after applying.\n" << statements(now);
    revert << "-- wow-world-editor: puts the " << table << " rows back as they were before this project.\n" << statements(original);
    if (!apply || !revert) { error = "Cannot write the spawn SQL in " + outDir.string(); return false; }
    return true;
}

void SpawnAdapter::Counts(size_t& added, size_t& changed, size_t& deleted) const
{
    added = changed = deleted = 0;
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    for (const auto& [guid, row] : now)
    {
        const bool was = !original[guid].is_null();
        if (row.is_null()) deleted += was;
        else was ? ++changed : ++added;
    }
}
