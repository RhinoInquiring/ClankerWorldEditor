#pragma once

#include "Changes.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class MpqChain;

/// One field of a DBC as mod-dbc-patch's schemas name it: 's' = enUS string (the other 15 locale slots stay empty),
/// 'f' = float, 'i' = 32-bit integer.
struct DbcField { const char* name; uint32_t index; char type; };

/// A client DBC whose rows the project adds and edits. Domain "dbc.<Name>". Change data: {"id", "before": row | null,
/// "after": row | null}; a row holds every field by name, so fields the editor does not show survive. null = no such row.
class DbcTable : public Adapter
{
public:
    /// `fields` covers every 32-bit field of the 3.3.5 layout the editor reads (`fieldCount` fields in all).
    DbcTable(MpqChain& mpq, ChangeStore& store, std::string name, std::vector<DbcField> fields, uint32_t fieldCount)
        : m_mpq(mpq), m_store(store), m_name(std::move(name)), m_domain("dbc." + m_name), m_fields(std::move(fields)), m_fieldCount(fieldCount) {}

    const char* Domain() const override { return m_domain.c_str(); }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }
    const std::string& Name() const { return m_name; }

    /// Drops the client's rows and the project's (project closed); the client's are read again on next use.
    void Reset() { m_read = false; m_client.clear(); m_bytes.clear(); m_project.clear(); ++m_version; }

    /// The current row (the project's if it touched the id, else the client's); null when there is none.
    const nlohmann::json& Row(uint32_t id) const;
    /// Every current row, by id.
    std::map<uint32_t, nlohmann::json> Rows() const;
    /// The lowest id in [first, last] no row uses; 0 when the range is full.
    uint32_t FreeId(uint32_t first, uint32_t last) const;
    /// One change replacing a row; the caller applies and commits it (or batches it).
    Change MakeChange(uint32_t id, const nlohmann::json& before, const nlohmann::json& after, const std::string& label) const;
    void Commit(uint32_t id, const nlohmann::json& after, const std::string& label);

    /// Writes <Name>.dbc (the client's rows plus the project's) to each folder in `dbcDirs`, and the project's rows as
    /// a mod-dbc-patch edit file (add / modify) <Name>.json in `patchDir`. Removes those files when the project has no rows.
    bool Export(const std::vector<std::filesystem::path>& dbcDirs, const std::filesystem::path& patchDir, std::string& error) const;
    /// Ids the project added or edited.
    size_t Count() const { return m_project.size(); }
    /// Rows the project added whose id the client has too, and client rows inside the project's range [first, last].
    void CheckIds(uint32_t first, uint32_t last, const char* area, std::vector<Problem>& problems) const;
    /// Bumped by every apply and revert (for caches).
    uint64_t Version() const { return m_version; }
    /// The project's row of an id without reading the client's file: null when the project has not touched it (a null
    /// json = removed).
    const nlohmann::json* Edited(uint32_t id) const { auto it = m_project.find(id); return it == m_project.end() ? nullptr : &it->second; }

protected:
    void Read() const;
    /// A row of every field at zero (strings empty).
    nlohmann::json EmptyRow() const;

    mutable std::map<uint32_t, nlohmann::json> m_client;
    std::map<uint32_t, nlohmann::json> m_project;       // id -> row now (null: the project removed it)

private:
    void Set(const Change& change, bool after);

    MpqChain& m_mpq;
    ChangeStore& m_store;
    std::string m_name, m_domain;
    std::vector<DbcField> m_fields;
    uint32_t m_fieldCount;
    mutable bool m_read = false;
    mutable std::vector<uint8_t> m_bytes;               // the client's file
    uint64_t m_version = 0;
};

/// One AreaTable row, the fields the editor shows.
struct Area
{
    uint32_t id = 0, map = 0, parent = 0, bit = 0, flags = 0, level = 0;
    std::string name;
};

/// AreaTable.dbc: zones and their sub-areas.
class AreaAdapter final : public DbcTable
{
public:
    AreaAdapter(MpqChain& mpq, ChangeStore& store);

    std::optional<Area> Find(uint32_t id) const;
    /// Every area of a map (ContinentID), by id.
    std::vector<Area> OnMap(uint32_t map) const;
    /// The zone an area belongs to: the top of its parent chain.
    uint32_t ZoneOf(uint32_t id) const;
    /// The lowest exploration bit (AreaBit) no row uses, below AzerothCore's 128 x 32 explored-zone bits; 0 when none.
    uint32_t FreeBit() const;
    /// A new area copied from `parent` (sound, music, flags), or from scratch when parent is 0 (a new zone).
    nlohmann::json NewRow(uint32_t id, uint32_t map, uint32_t parent, const std::string& name, uint32_t level) const;
    /// CheckIds plus AreaBits used twice or past what AzerothCore tracks.
    void Check(uint32_t first, uint32_t last, std::vector<Problem>& problems) const;
};

/// WMOAreaTable.dbc: the area of each group of a WMO placed with a name set (the client and server look up
/// (WMOID from the root's MOHD, MODF name set, MOGP group id); no row = the terrain's area under it).
class WmoAreaAdapter final : public DbcTable
{
public:
    WmoAreaAdapter(MpqChain& mpq, ChangeStore& store);

    /// Rows of one WMO and name set, by WMOGroupID (-1, the whole WMO, as 0xFFFFFFFF).
    std::map<uint32_t, std::pair<uint32_t, nlohmann::json>> For(uint32_t wmoId, uint32_t nameSet) const;   // group -> (id, row)
    /// The highest name set any row of this WMO uses.
    uint32_t MaxNameSet(uint32_t wmoId) const;
    /// A new row for one group.
    nlohmann::json NewRow(uint32_t id, uint32_t wmoId, uint32_t nameSet, uint32_t group, uint32_t area) const;
};

/// WorldMapArea.dbc: a zone's map picture, Interface\WorldMap\<AreaName>\<AreaName>1..12.blp (4 x 3 tiles of 256 px, of
/// which the top-left 1002 x 668 show), and the world rectangle it covers in WoW axes: LocLeft / LocRight = the west and
/// east y, LocTop / LocBottom = the north and south x.
class WorldMapAreaAdapter final : public DbcTable
{
public:
    WorldMapAreaAdapter(MpqChain& mpq, ChangeStore& store);
    /// The row showing this zone on this map, if any.
    std::optional<uint32_t> ForZone(uint32_t map, uint32_t zone) const;
};

/// WorldMapOverlay.dbc: pieces of a zone map (Interface\WorldMap\<zone folder>\<TextureName>1..n.blp, 256 px tiles over
/// TextureWidth x TextureHeight, placed at OffsetX / OffsetY of the 1002 x 668 view) shown once the player explored any
/// of AreaID[0..3].
class WorldMapOverlayAdapter final : public DbcTable
{
public:
    WorldMapOverlayAdapter(MpqChain& mpq, ChangeStore& store);
    /// Overlays of one WorldMapArea, by id.
    std::map<uint32_t, nlohmann::json> For(uint32_t worldMapArea) const;
};
