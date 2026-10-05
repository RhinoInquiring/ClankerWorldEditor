#include "Areas.hpp"

#include "Formats.hpp"
#include "Mpq.hpp"

#include <cstring>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kExploreBits = 128 * 32;   // AzerothCore PLAYER_EXPLORED_ZONES_SIZE x 32

    bool Zero(const nlohmann::json& v) { return v.is_string() ? v.get<std::string>().empty() : v.is_number() && v.get<double>() == 0; }
}

// ---------------------------------------------------------------------------------------------- any DBC

void DbcTable::Read() const
{
    if (m_read) return;
    m_read = true;
    auto data = m_mpq.Read("DBFilesClient\\" + m_name + ".dbc");
    Dbc dbc;
    uint32_t fields = 0, recordSize = 0;
    if (!data || data->size() < 20) return;
    std::memcpy(&fields, data->data() + 8, 4);
    std::memcpy(&recordSize, data->data() + 12, 4);
    if (fields != m_fieldCount || recordSize != m_fieldCount * 4 || !dbc.Load(*data)) return;   // not the 3.3.5 layout
    m_bytes = std::move(*data);
    for (uint32_t r = 0; r < dbc.Rows(); ++r)
    {
        nlohmann::json row = nlohmann::json::object();
        for (const DbcField& f : m_fields)
            row[f.name] = f.type == 's' ? nlohmann::json(dbc.Str(r, f.index)) : f.type == 'f' ? nlohmann::json(dbc.F32(r, f.index)) : nlohmann::json(dbc.U32(r, f.index));
        m_client[dbc.U32(r, 0)] = std::move(row);
    }
}

nlohmann::json DbcTable::EmptyRow() const
{
    nlohmann::json row = nlohmann::json::object();
    for (const DbcField& f : m_fields) row[f.name] = f.type == 's' ? nlohmann::json("") : f.type == 'f' ? nlohmann::json(0.0f) : nlohmann::json(0u);
    return row;
}

const nlohmann::json& DbcTable::Row(uint32_t id) const
{
    static const nlohmann::json none;
    if (auto it = m_project.find(id); it != m_project.end()) return it->second;
    Read();
    auto it = m_client.find(id);
    return it == m_client.end() ? none : it->second;
}

std::map<uint32_t, nlohmann::json> DbcTable::Rows() const
{
    Read();
    std::map<uint32_t, nlohmann::json> rows = m_client;
    for (const auto& [id, row] : m_project)
        if (row.is_null()) rows.erase(id);
        else rows[id] = row;
    return rows;
}

uint32_t DbcTable::FreeId(uint32_t first, uint32_t last) const
{
    for (uint32_t id = std::max(first, 1u); id && id <= last; ++id)
        if (Row(id).is_null()) return id;
    return 0;
}

Change DbcTable::MakeChange(uint32_t id, const nlohmann::json& before, const nlohmann::json& after, const std::string& label) const
{
    Change c;
    c.domain = Domain();
    c.label = label;
    const nlohmann::json& shown = after.is_null() ? before : after;
    const std::string name = shown.is_object() ? shown.value("AreaName_lang", "") : "";
    c.target = m_name + " " + std::to_string(id) + (name.empty() ? "" : " " + name);
    c.data = { { "id", id }, { "before", before }, { "after", after } };
    return c;
}

void DbcTable::Commit(uint32_t id, const nlohmann::json& after, const std::string& label)
{
    Change c = MakeChange(id, Row(id), after, label);
    Apply(c);
    m_store.Commit(std::move(c));
}

void DbcTable::Set(const Change& change, bool after)
{
    const uint32_t id = change.data.at("id");
    const nlohmann::json& row = change.data.at(after ? "after" : "before");
    Read();
    auto client = m_client.find(id);
    // Back to what the client has: no longer the project's row.
    if (client == m_client.end() ? row.is_null() : row == client->second) m_project.erase(id);
    else m_project[id] = row;
    ++m_version;
}

bool DbcTable::Export(const std::vector<fs::path>& dbcDirs, const fs::path& patchDir, std::string& error) const
{
    std::error_code ec;
    const fs::path patchJson = patchDir / (m_name + ".json");
    if (m_project.empty())   // nothing of ours: no stale files from an earlier export either
    {
        for (const fs::path& dir : dbcDirs) fs::remove(dir / (m_name + ".dbc"), ec);
        fs::remove(patchJson, ec);
        return true;
    }
    Read();
    if (m_bytes.empty()) { error = m_name + ": the client has no readable 3.3.5 " + m_name + ".dbc"; return false; }

    const uint32_t recordSize = m_fieldCount * 4;
    uint32_t records = 0, stringSize = 0;
    std::memcpy(&records, m_bytes.data() + 4, 4);
    std::memcpy(&stringSize, m_bytes.data() + 16, 4);
    const size_t stringsAt = 20 + size_t(records) * recordSize;
    std::string strings(reinterpret_cast<const char*>(m_bytes.data() + stringsAt), stringSize);
    std::map<std::string, uint32_t> added;
    auto addString = [&](const std::string& s) -> uint32_t {
        if (s.empty()) return 0;
        auto [it, fresh] = added.try_emplace(s, uint32_t(strings.size()));
        if (fresh) strings.append(s).push_back('\0');
        return it->second;
    };
    auto write = [&](uint8_t* record, const nlohmann::json& row, const nlohmann::json* original) {
        for (const DbcField& f : m_fields)
        {
            if (!row.contains(f.name)) continue;
            const nlohmann::json& v = row[f.name];
            uint32_t bits = 0;
            if (f.type == 's')
            {
                if (original && (*original)[f.name] == v) continue;   // keeps its string offset
                bits = addString(v.get<std::string>());
            }
            else if (f.type == 'f') { const float x = v.get<float>(); std::memcpy(&bits, &x, 4); }
            else bits = v.get<uint32_t>();
            std::memcpy(record + f.index * 4, &bits, 4);
        }
    };

    std::vector<uint8_t> body;
    body.reserve((size_t(records) + m_project.size()) * recordSize);
    for (uint32_t r = 0; r < records; ++r)
    {
        const uint8_t* src = m_bytes.data() + 20 + size_t(r) * recordSize;
        uint32_t id = 0;
        std::memcpy(&id, src, 4);
        auto it = m_project.find(id);
        if (it != m_project.end() && it->second.is_null()) continue;   // removed
        body.insert(body.end(), src, src + recordSize);
        if (it != m_project.end()) write(body.data() + body.size() - recordSize, it->second, &m_client.at(id));
    }
    nlohmann::json patch = { { "add", nlohmann::json::array() }, { "modify", nlohmann::json::array() } };
    for (const auto& [id, row] : m_project)
    {
        if (row.is_null()) continue;
        auto client = m_client.find(id);
        nlohmann::json entry = { { "ID", id } };
        for (const DbcField& f : m_fields)
            if (f.index && row.contains(f.name) && (client == m_client.end() ? !Zero(row[f.name]) : row[f.name] != client->second[f.name]))
                entry[f.name] = row[f.name];
        patch[client == m_client.end() ? "add" : "modify"].push_back(std::move(entry));
        if (client != m_client.end()) continue;
        body.resize(body.size() + recordSize, 0);
        write(body.data() + body.size() - recordSize, row, nullptr);
    }

    const uint32_t header[5] = { 0x43424457 /* WDBC */, uint32_t(body.size() / recordSize), m_fieldCount, recordSize, uint32_t(strings.size()) };
    for (const fs::path& dir : dbcDirs)
    {
        fs::create_directories(dir, ec);
        std::ofstream f(dir / (m_name + ".dbc"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(header), sizeof header);
        f.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
        f.write(strings.data(), std::streamsize(strings.size()));
        if (!f) { error = "Cannot write " + (dir / (m_name + ".dbc")).string(); return false; }
    }
    fs::create_directories(patchDir, ec);
    std::ofstream f(patchJson);
    f << patch.dump(1) << "\n";
    if (!f) { error = "Cannot write " + patchJson.string(); return false; }
    return true;
}

void DbcTable::CheckIds(uint32_t first, uint32_t last, const char* area, std::vector<Problem>& problems) const
{
    Read();
    // A row the project created whose id the client now has too (another patch added it since): export would overwrite it.
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain != Domain() || !data.at("before").is_null() || data.at("after").is_null()) return;
        const uint32_t id = data.at("id");
        if (m_client.count(id))
            problems.push_back({ Problem::Severity::Error, area, m_name + " " + std::to_string(id) +
                                 ": the project added this id, but the client has it too; another patch uses the project's range" });
    });
    size_t foreign = 0;
    for (const auto& [id, row] : m_client) foreign += id >= first && id <= last && !m_project.count(id);
    if (foreign)
        problems.push_back({ Problem::Severity::Warning, area, std::to_string(foreign) + " client " + m_name + " row(s) are inside the project's id range " +
                             std::to_string(first) + "-" + std::to_string(last) + " (another patch?)" });
}

// ---------------------------------------------------------------------------------------------- AreaTable

AreaAdapter::AreaAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "AreaTable",
               { { "ID", 0, 'i' }, { "ContinentID", 1, 'i' }, { "ParentAreaID", 2, 'i' }, { "AreaBit", 3, 'i' }, { "Flags", 4, 'i' },
                 { "SoundProviderPref", 5, 'i' }, { "SoundProviderPrefUnderwater", 6, 'i' }, { "AmbienceID", 7, 'i' }, { "ZoneMusic", 8, 'i' },
                 { "IntroSound", 9, 'i' }, { "ExplorationLevel", 10, 'i' }, { "AreaName_lang", 11, 's' }, { "AreaName_lang_flags", 27, 'i' },
                 { "FactionGroupMask", 28, 'i' }, { "LiquidTypeID[0]", 29, 'i' }, { "LiquidTypeID[1]", 30, 'i' }, { "LiquidTypeID[2]", 31, 'i' },
                 { "LiquidTypeID[3]", 32, 'i' }, { "MinElevation", 33, 'f' }, { "Ambient_multiplier", 34, 'f' }, { "LightID", 35, 'i' } },
               36)
{
}

std::optional<Area> AreaAdapter::Find(uint32_t id) const
{
    const nlohmann::json& row = Row(id);
    if (row.is_null()) return std::nullopt;
    return Area{ row.value("ID", 0u), row.value("ContinentID", 0u), row.value("ParentAreaID", 0u), row.value("AreaBit", 0u),
                 row.value("Flags", 0u), row.value("ExplorationLevel", 0u), row.value("AreaName_lang", "") };
}

std::vector<Area> AreaAdapter::OnMap(uint32_t map) const
{
    std::vector<Area> out;
    for (const auto& [id, row] : Rows())
        if (row.value("ContinentID", 0u) == map) out.push_back(*Find(id));
    return out;
}

uint32_t AreaAdapter::ZoneOf(uint32_t id) const
{
    for (int depth = 0; depth < 8; ++depth)   // the zone is the area at the top of the parent chain
    {
        const auto a = Find(id);
        if (!a || a->parent == 0) break;
        id = a->parent;
    }
    return id;
}

uint32_t AreaAdapter::FreeBit() const
{
    std::set<uint32_t> used;
    for (const auto& [id, row] : Rows()) used.insert(row.value("AreaBit", 0u));
    for (uint32_t bit = 1; bit < kExploreBits; ++bit)
        if (!used.count(bit)) return bit;
    return 0;
}

nlohmann::json AreaAdapter::NewRow(uint32_t id, uint32_t map, uint32_t parent, const std::string& name, uint32_t level) const
{
    nlohmann::json row = Row(parent);
    if (parent == 0 || row.is_null())
    {
        row = EmptyRow();
        Read();
        if (!m_client.empty()) row["AreaName_lang_flags"] = m_client.begin()->second["AreaName_lang_flags"];
        row["Ambient_multiplier"] = 1.0f;
        parent = 0;
    }
    row["ID"] = id;
    row["ContinentID"] = map;
    row["ParentAreaID"] = parent;
    row["AreaBit"] = FreeBit();
    row["ExplorationLevel"] = level;
    row["AreaName_lang"] = name;
    return row;
}

void AreaAdapter::Check(uint32_t first, uint32_t last, std::vector<Problem>& problems) const
{
    CheckIds(first, last, "Zones", problems);
    // Exploration bits: each area needs its own, and AzerothCore only tracks the first 4096.
    std::map<uint32_t, size_t> byBit;
    for (const auto& [id, row] : Rows()) ++byBit[row.value("AreaBit", 0u)];
    for (const auto& [id, row] : m_project)
    {
        if (row.is_null()) continue;
        const uint32_t bit = row.value("AreaBit", 0u);
        const std::string name = "AreaTable " + std::to_string(id) + " (" + row.value("AreaName_lang", "") + ")";
        if (byBit[bit] > 1 && bit != 0)
            problems.push_back({ Problem::Severity::Error, "Zones", name + ": AreaBit " + std::to_string(bit) + " is shared with " +
                                 std::to_string(byBit[bit] - 1) + " other area(s); exploring one explores both" });
        if (bit >= kExploreBits)
            problems.push_back({ Problem::Severity::Warning, "Zones", name + ": AreaBit " + std::to_string(bit) + " is past AzerothCore's 4096 exploration bits; it cannot be explored" });
    }
}

// ---------------------------------------------------------------------------------------------- WMOAreaTable

WmoAreaAdapter::WmoAreaAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "WMOAreaTable",
               { { "ID", 0, 'i' }, { "WMOID", 1, 'i' }, { "NameSetID", 2, 'i' }, { "WMOGroupID", 3, 'i' }, { "SoundProviderPref", 4, 'i' },
                 { "SoundProviderPrefUnderwater", 5, 'i' }, { "AmbienceID", 6, 'i' }, { "ZoneMusic", 7, 'i' }, { "IntroSound", 8, 'i' },
                 { "Flags", 9, 'i' }, { "AreaTableID", 10, 'i' }, { "AreaName_lang", 11, 's' }, { "AreaName_lang_flags", 27, 'i' } },
               28)
{
}

std::map<uint32_t, std::pair<uint32_t, nlohmann::json>> WmoAreaAdapter::For(uint32_t wmoId, uint32_t nameSet) const
{
    std::map<uint32_t, std::pair<uint32_t, nlohmann::json>> out;
    for (auto& [id, row] : Rows())
        if (row.value("WMOID", 0u) == wmoId && row.value("NameSetID", 0u) == nameSet) out[row.value("WMOGroupID", 0u)] = { id, row };
    return out;
}

uint32_t WmoAreaAdapter::MaxNameSet(uint32_t wmoId) const
{
    uint32_t most = 0;
    for (const auto& [id, row] : Rows())
        if (row.value("WMOID", 0u) == wmoId) most = std::max(most, row.value("NameSetID", 0u));
    return most;
}

nlohmann::json WmoAreaAdapter::NewRow(uint32_t id, uint32_t wmoId, uint32_t nameSet, uint32_t group, uint32_t area) const
{
    nlohmann::json row = EmptyRow();
    row["ID"] = id;
    row["WMOID"] = wmoId;
    row["NameSetID"] = nameSet;
    row["WMOGroupID"] = group;
    row["AreaTableID"] = area;
    return row;
}

// ---------------------------------------------------------------------------------------------- world map

WorldMapAreaAdapter::WorldMapAreaAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "WorldMapArea",
               { { "ID", 0, 'i' }, { "MapID", 1, 'i' }, { "AreaID", 2, 'i' }, { "AreaName", 3, 's' }, { "LocLeft", 4, 'f' }, { "LocRight", 5, 'f' },
                 { "LocTop", 6, 'f' }, { "LocBottom", 7, 'f' }, { "DisplayMapID", 8, 'i' }, { "DefaultDungeonFloor", 9, 'i' },
                 { "ParentWorldMapID", 10, 'i' } },
               11)
{
}

std::optional<uint32_t> WorldMapAreaAdapter::ForZone(uint32_t map, uint32_t zone) const
{
    for (const auto& [id, row] : Rows())
        if (row.value("MapID", 0u) == map && row.value("AreaID", 0u) == zone) return id;
    return std::nullopt;
}

WorldMapOverlayAdapter::WorldMapOverlayAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "WorldMapOverlay",
               { { "ID", 0, 'i' }, { "MapAreaID", 1, 'i' }, { "AreaID[0]", 2, 'i' }, { "AreaID[1]", 3, 'i' }, { "AreaID[2]", 4, 'i' },
                 { "AreaID[3]", 5, 'i' }, { "MapPointX", 6, 'i' }, { "MapPointY", 7, 'i' }, { "TextureName", 8, 's' }, { "TextureWidth", 9, 'i' },
                 { "TextureHeight", 10, 'i' }, { "OffsetX", 11, 'i' }, { "OffsetY", 12, 'i' }, { "HitRectTop", 13, 'i' }, { "HitRectLeft", 14, 'i' },
                 { "HitRectBottom", 15, 'i' }, { "HitRectRight", 16, 'i' } },
               17)
{
}

std::map<uint32_t, nlohmann::json> WorldMapOverlayAdapter::For(uint32_t worldMapArea) const
{
    std::map<uint32_t, nlohmann::json> out;
    for (const auto& [id, row] : Rows())
        if (row.value("MapAreaID", 0u) == worldMapArea) out[id] = row;
    return out;
}
