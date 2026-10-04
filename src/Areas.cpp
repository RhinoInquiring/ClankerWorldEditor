#include "Areas.hpp"

#include "Formats.hpp"
#include "Mpq.hpp"

#include <cstring>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{
    // AreaTable.dbc, 3.3.5.12340: 36 fields of 32 bits. Names as mod-dbc-patch's schemas/AreaTable.json spells them;
    // 's' = enUS string (the other 15 locale slots stay empty), 'f' = float, 'i' = 32-bit integer.
    struct Field { const char* name; uint32_t index; char type; };
    constexpr Field kFields[] = {
        { "ID", 0, 'i' }, { "ContinentID", 1, 'i' }, { "ParentAreaID", 2, 'i' }, { "AreaBit", 3, 'i' }, { "Flags", 4, 'i' },
        { "SoundProviderPref", 5, 'i' }, { "SoundProviderPrefUnderwater", 6, 'i' }, { "AmbienceID", 7, 'i' }, { "ZoneMusic", 8, 'i' },
        { "IntroSound", 9, 'i' }, { "ExplorationLevel", 10, 'i' }, { "AreaName_lang", 11, 's' }, { "AreaName_lang_flags", 27, 'i' },
        { "FactionGroupMask", 28, 'i' }, { "LiquidTypeID[0]", 29, 'i' }, { "LiquidTypeID[1]", 30, 'i' }, { "LiquidTypeID[2]", 31, 'i' },
        { "LiquidTypeID[3]", 32, 'i' }, { "MinElevation", 33, 'f' }, { "Ambient_multiplier", 34, 'f' }, { "LightID", 35, 'i' },
    };
    constexpr uint32_t kFieldCount = 36, kRecordSize = kFieldCount * 4;
    constexpr uint32_t kExploreBits = 128 * 32;   // AzerothCore PLAYER_EXPLORED_ZONES_SIZE x 32

    Area ToArea(const nlohmann::json& row)
    {
        return { row.value("ID", 0u), row.value("ContinentID", 0u), row.value("ParentAreaID", 0u), row.value("AreaBit", 0u),
                 row.value("Flags", 0u), row.value("ExplorationLevel", 0u), row.value("AreaName_lang", "") };
    }

    bool Zero(const nlohmann::json& v) { return v.is_string() ? v.get<std::string>().empty() : v.is_number() && v.get<double>() == 0; }
}

void AreaAdapter::Read() const
{
    if (m_read) return;
    m_read = true;
    auto data = m_mpq.Read("DBFilesClient\\AreaTable.dbc");
    Dbc dbc;
    uint32_t fields = 0, recordSize = 0;
    if (!data || data->size() < 20) return;
    std::memcpy(&fields, data->data() + 8, 4);
    std::memcpy(&recordSize, data->data() + 12, 4);
    if (fields != kFieldCount || recordSize != kRecordSize || !dbc.Load(*data)) return;   // not the 3.3.5 layout
    m_bytes = std::move(*data);
    for (uint32_t r = 0; r < dbc.Rows(); ++r)
    {
        nlohmann::json row = nlohmann::json::object();
        for (const Field& f : kFields)
            row[f.name] = f.type == 's' ? nlohmann::json(dbc.Str(r, f.index)) : f.type == 'f' ? nlohmann::json(dbc.F32(r, f.index)) : nlohmann::json(dbc.U32(r, f.index));
        m_client[dbc.U32(r, 0)] = std::move(row);
    }
}

nlohmann::json AreaAdapter::Row(uint32_t id) const
{
    if (auto it = m_project.find(id); it != m_project.end()) return it->second;
    Read();
    auto it = m_client.find(id);
    return it == m_client.end() ? nlohmann::json() : it->second;
}

std::optional<Area> AreaAdapter::Find(uint32_t id) const
{
    const nlohmann::json row = Row(id);
    if (row.is_null()) return std::nullopt;
    return ToArea(row);
}

std::vector<Area> AreaAdapter::OnMap(uint32_t map) const
{
    Read();
    std::set<uint32_t> ids;
    for (const auto& [id, row] : m_client) ids.insert(id);
    for (const auto& [id, row] : m_project) ids.insert(id);
    std::vector<Area> out;
    for (uint32_t id : ids)
        if (const auto a = Find(id); a && a->map == map) out.push_back(*a);
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

uint32_t AreaAdapter::FreeId(uint32_t first, uint32_t last) const
{
    for (uint32_t id = std::max(first, 1u); id && id <= last; ++id)
        if (Row(id).is_null()) return id;
    return 0;
}

uint32_t AreaAdapter::FreeBit() const
{
    Read();
    std::set<uint32_t> used;
    for (const auto& [id, row] : m_client) used.insert(row.value("AreaBit", 0u));
    for (const auto& [id, row] : m_project)
        if (!row.is_null()) used.insert(row.value("AreaBit", 0u));
    for (uint32_t bit = 1; bit < kExploreBits; ++bit)
        if (!used.count(bit)) return bit;
    return 0;
}

nlohmann::json AreaAdapter::NewRow(uint32_t id, uint32_t map, uint32_t parent, const std::string& name, uint32_t level) const
{
    nlohmann::json row = Row(parent);
    if (parent == 0 || row.is_null())
    {
        row = nlohmann::json::object();
        for (const Field& f : kFields) row[f.name] = f.type == 's' ? nlohmann::json("") : f.type == 'f' ? nlohmann::json(0.0f) : nlohmann::json(0u);
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

Change AreaAdapter::MakeChange(uint32_t id, const nlohmann::json& before, const nlohmann::json& after, const std::string& label) const
{
    Change c;
    c.domain = Domain();
    c.label = label;
    const nlohmann::json& shown = after.is_null() ? before : after;
    c.target = "AreaTable " + std::to_string(id) + (shown.is_object() ? " " + shown.value("AreaName_lang", "") : "");
    c.data = { { "id", id }, { "before", before }, { "after", after } };
    return c;
}

void AreaAdapter::Commit(uint32_t id, const nlohmann::json& after, const std::string& label)
{
    Change c = MakeChange(id, Row(id), after, label);
    Apply(c);
    m_store.Commit(std::move(c));
}

void AreaAdapter::Set(const Change& change, bool after)
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

bool AreaAdapter::Export(const std::vector<fs::path>& dbcDirs, const fs::path& patchJson, std::string& error) const
{
    std::error_code ec;
    if (m_project.empty())   // nothing of ours: no stale files from an earlier export either
    {
        for (const fs::path& dir : dbcDirs) fs::remove(dir / "AreaTable.dbc", ec);
        fs::remove(patchJson, ec);
        return true;
    }
    Read();
    if (m_bytes.empty()) { error = "AreaTable: the client has no readable 3.3.5 AreaTable.dbc"; return false; }

    uint32_t records = 0, stringSize = 0;
    std::memcpy(&records, m_bytes.data() + 4, 4);
    std::memcpy(&stringSize, m_bytes.data() + 16, 4);
    const size_t stringsAt = 20 + size_t(records) * kRecordSize;
    std::string strings(reinterpret_cast<const char*>(m_bytes.data() + stringsAt), stringSize);
    std::map<std::string, uint32_t> added;
    auto addString = [&](const std::string& s) -> uint32_t {
        if (s.empty()) return 0;
        auto [it, fresh] = added.try_emplace(s, uint32_t(strings.size()));
        if (fresh) strings.append(s).push_back('\0');
        return it->second;
    };
    auto write = [&](uint8_t* record, const nlohmann::json& row, const nlohmann::json* original) {
        for (const Field& f : kFields)
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
    body.reserve((size_t(records) + m_project.size()) * kRecordSize);
    for (uint32_t r = 0; r < records; ++r)
    {
        const uint8_t* src = m_bytes.data() + 20 + size_t(r) * kRecordSize;
        uint32_t id = 0;
        std::memcpy(&id, src, 4);
        auto it = m_project.find(id);
        if (it != m_project.end() && it->second.is_null()) continue;   // removed
        body.insert(body.end(), src, src + kRecordSize);
        if (it != m_project.end()) write(body.data() + body.size() - kRecordSize, it->second, &m_client.at(id));
    }
    nlohmann::json patch = { { "add", nlohmann::json::array() }, { "modify", nlohmann::json::array() } };
    for (const auto& [id, row] : m_project)
    {
        if (row.is_null()) continue;
        auto client = m_client.find(id);
        nlohmann::json entry = { { "ID", id } };
        for (const Field& f : kFields)
            if (f.index && row.contains(f.name) && (client == m_client.end() ? !Zero(row[f.name]) : row[f.name] != client->second[f.name]))
                entry[f.name] = row[f.name];
        patch[client == m_client.end() ? "add" : "modify"].push_back(std::move(entry));
        if (client != m_client.end()) continue;
        body.resize(body.size() + kRecordSize, 0);
        write(body.data() + body.size() - kRecordSize, row, nullptr);
    }

    const uint32_t header[5] = { 0x43424457 /* WDBC */, uint32_t(body.size() / kRecordSize), kFieldCount, kRecordSize, uint32_t(strings.size()) };
    for (const fs::path& dir : dbcDirs)
    {
        fs::create_directories(dir, ec);
        std::ofstream f(dir / "AreaTable.dbc", std::ios::binary);
        f.write(reinterpret_cast<const char*>(header), sizeof header);
        f.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
        f.write(strings.data(), std::streamsize(strings.size()));
        if (!f) { error = "Cannot write " + (dir / "AreaTable.dbc").string(); return false; }
    }
    fs::create_directories(patchJson.parent_path(), ec);
    std::ofstream f(patchJson);
    f << patch.dump(1) << "\n";
    if (!f) { error = "Cannot write " + patchJson.string(); return false; }
    return true;
}

void AreaAdapter::Check(uint32_t first, uint32_t last, std::vector<Problem>& problems) const
{
    Read();
    // A row the project created whose id the client now has too (another patch added it since): export would overwrite it.
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain != Domain() || !data.at("before").is_null() || data.at("after").is_null()) return;
        const uint32_t id = data.at("id");
        if (auto it = m_client.find(id); it != m_client.end())
            problems.push_back({ Problem::Severity::Error, "Zones",
                                 "AreaTable " + std::to_string(id) + ": the project added this id, but the client has it too (" +
                                     it->second.value("AreaName_lang", "") + "); another patch uses the project's range" });
    });
    for (const auto& [id, row] : m_client)
        if (id >= first && id <= last && !m_project.count(id))
            problems.push_back({ Problem::Severity::Warning, "Zones",
                                 "AreaTable " + std::to_string(id) + " (" + row.value("AreaName_lang", "") + ") is in the project's area id range but comes from the client" });
    // Exploration bits: each area needs its own, and AzerothCore only tracks the first 4096.
    std::map<uint32_t, std::vector<uint32_t>> byBit;
    for (const auto& [id, row] : m_client)
        if (!m_project.count(id)) byBit[row.value("AreaBit", 0u)].push_back(id);
    for (const auto& [id, row] : m_project)
        if (!row.is_null()) byBit[row.value("AreaBit", 0u)].push_back(id);
    for (const auto& [id, row] : m_project)
    {
        if (row.is_null()) continue;
        const uint32_t bit = row.value("AreaBit", 0u);
        const std::string name = "AreaTable " + std::to_string(id) + " (" + row.value("AreaName_lang", "") + ")";
        if (byBit[bit].size() > 1 && bit != 0)
            problems.push_back({ Problem::Severity::Error, "Zones", name + ": AreaBit " + std::to_string(bit) + " is shared with " +
                                 std::to_string(byBit[bit].size() - 1) + " other area(s); exploring one explores both" });
        if (bit >= kExploreBits)
            problems.push_back({ Problem::Severity::Warning, "Zones", name + ": AreaBit " + std::to_string(bit) + " is past AzerothCore's 4096 exploration bits; it cannot be explored" });
    }
}
