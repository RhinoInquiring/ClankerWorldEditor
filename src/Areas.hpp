#pragma once

#include "Changes.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class MpqChain;

/// One AreaTable row, the fields the editor shows.
struct Area
{
    uint32_t id = 0, map = 0, parent = 0, bit = 0, flags = 0, level = 0;
    std::string name;
};

/// AreaTable.dbc: the client's rows with the project's added and edited rows on top. Domain "dbc.AreaTable".
/// Change data: {"id", "before": row | null, "after": row | null}; a row holds every field by its mod-dbc-patch
/// schema name (AreaName_lang = enUS), so fields the editor does not show survive. null = no such row.
class AreaAdapter final : public Adapter
{
public:
    AreaAdapter(MpqChain& mpq, ChangeStore& store) : m_mpq(mpq), m_store(store) {}

    const char* Domain() const override { return "dbc.AreaTable"; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }

    /// Drops the client's rows (another client opened); read again on next use.
    void Reset() { m_read = false; m_client.clear(); m_bytes.clear(); ++m_version; }

    /// The current row (the project's if it touched the id, else the client's); null when there is none.
    nlohmann::json Row(uint32_t id) const;
    std::optional<Area> Find(uint32_t id) const;
    /// Every area of a map (ContinentID), by id.
    std::vector<Area> OnMap(uint32_t map) const;
    /// The zone an area belongs to: the top of its parent chain.
    uint32_t ZoneOf(uint32_t id) const;
    /// The lowest id in [first, last] no row uses; 0 when the range is full.
    uint32_t FreeId(uint32_t first, uint32_t last) const;
    /// The lowest exploration bit (AreaBit) no row uses, below AzerothCore's 128 x 32 explored-zone bits; 0 when none.
    uint32_t FreeBit() const;
    /// A new area copied from `parent` (sound, music, flags), or from scratch when parent is 0 (a new zone).
    nlohmann::json NewRow(uint32_t id, uint32_t map, uint32_t parent, const std::string& name, uint32_t level) const;
    /// One change replacing a row; the caller applies and commits it.
    Change MakeChange(uint32_t id, const nlohmann::json& before, const nlohmann::json& after, const std::string& label) const;
    void Commit(uint32_t id, const nlohmann::json& after, const std::string& label);

    /// Writes AreaTable.dbc (the client's rows plus the project's) to each folder in `dbcDirs`, and the project's rows
    /// as a mod-dbc-patch edit file (add / modify) at `patchJson`. Nothing is written when the project has no rows.
    bool Export(const std::vector<std::filesystem::path>& dbcDirs, const std::filesystem::path& patchJson, std::string& error) const;
    /// Ids the project added or edited.
    size_t Count() const { return m_project.size(); }
    /// Rows in [first, last] that the client already has and the project did not make, and AreaBits used twice.
    void Check(uint32_t first, uint32_t last, std::vector<Problem>& problems) const;
    /// Bumped by every apply and revert (for caches).
    uint64_t Version() const { return m_version; }

private:
    void Set(const Change& change, bool after);
    void Read() const;

    MpqChain& m_mpq;
    ChangeStore& m_store;
    mutable bool m_read = false;
    mutable std::vector<uint8_t> m_bytes;               // the client's AreaTable.dbc
    mutable std::map<uint32_t, nlohmann::json> m_client;
    std::map<uint32_t, nlohmann::json> m_project;       // id -> row now (null: the project removed it)
    uint64_t m_version = 0;
};
