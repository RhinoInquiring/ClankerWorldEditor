#pragma once

#include "Changes.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class Db;

/// Every row of one world table that shares a key value, as project changes: waypoint_data by path id (one row per
/// point), creature_addon by guid (one row). A change holds the full rows before and after
/// ({"key", "before": [rows], "after": [rows]}, rows as column -> value text; an empty list = none), so columns the
/// editor does not show survive. Writing a key replaces its rows: DELETE by key, then INSERT the new ones.
class TableRowsAdapter final : public Adapter
{
public:
    /// `order`: column the rows of a key are sorted by (empty: none).
    TableRowsAdapter(ChangeStore& store, std::string table, std::string key, std::string order = {})
        : m_store(store), m_table(std::move(table)), m_key(std::move(key)), m_order(std::move(order)), m_domain("world." + m_table) {}

    const char* Domain() const override { return m_domain.c_str(); }
    const std::string& Table() const { return m_table; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }

    void SetDb(Db* db) { m_db = db; }
    bool Connected() const;
    /// Writes the final rows of every key the project touches; false with the error.
    bool Sync(std::string& error);

    /// The current rows of a key: the project's if it touched the key, else the database's.
    std::vector<nlohmann::json> Rows(uint32_t key) const;
    /// One change replacing a key's rows (the caller applies it, or commits it in a batch after Apply).
    Change MakeChange(uint32_t key, const std::vector<nlohmann::json>& before, const std::vector<nlohmann::json>& after, const std::string& label) const;

    /// The project's net rows as SQL (<table>.sql) plus the statements putting the originals back (<table>_revert.sql).
    bool ExportSql(const std::filesystem::path& outDir, std::string& error) const;
    /// Keys the project changed.
    size_t Count() const;
    const std::string& LastError() const { return m_lastError; }

private:
    void Set(const Change& change, bool after);
    bool Write(uint32_t key, const nlohmann::json& rows, std::string& error) const;
    /// key -> final rows and the rows before the project touched it.
    void NetState(std::map<uint32_t, nlohmann::json>& now, std::map<uint32_t, nlohmann::json>& original) const;
    std::string Statements(const Db& db, uint32_t key, const nlohmann::json& rows) const;

    ChangeStore& m_store;
    std::string m_table, m_key, m_order, m_domain;
    Db* m_db = nullptr;
    std::string m_lastError;
};
