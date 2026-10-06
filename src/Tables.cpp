#include "Tables.hpp"

#include "Server.hpp"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

bool TableRowsAdapter::Connected() const { return m_db && m_db->Connected(); }

void TableRowsAdapter::NetState(std::map<uint32_t, nlohmann::json>& now, std::map<uint32_t, nlohmann::json>& original) const
{
    ChangeStore::ForEach(m_store.Done(), [&](const std::string& domain, const nlohmann::json& data) {
        if (domain != m_domain) return;
        const uint32_t key = data.at("key");
        original.try_emplace(key, data.at("before"));
        now[key] = data.at("after");
    });
}

std::string TableRowsAdapter::Statements(const Db& db, uint32_t key, const nlohmann::json& rows) const
{
    std::string sql = "DELETE FROM " + m_table + " WHERE `" + m_key + "` = " + std::to_string(key) + ";\n";
    for (const auto& row : rows) sql += UpsertSql(db, m_table, row) + ";\n";
    return sql;
}

bool TableRowsAdapter::Write(uint32_t key, const nlohmann::json& rows, std::string& error) const
{
    if (!Connected()) { error = "Not connected to the world database."; return false; }
    if (!m_db->Query("DELETE FROM " + m_table + " WHERE `" + m_key + "` = " + std::to_string(key), error)) return false;
    for (const auto& row : rows)
        if (!m_db->Query(UpsertSql(*m_db, m_table, row), error)) return false;
    return true;
}

void TableRowsAdapter::Set(const Change& change, bool after)
{
    if (!Connected()) return;   // Sync writes it once connected
    const uint32_t key = change.data.at("key");
    if (std::string error; Write(key, change.data.at(after ? "after" : "before"), error)) m_lastError.clear();
    else m_lastError = m_table + " " + std::to_string(key) + ": " + error;
}

bool TableRowsAdapter::Sync(std::string& error)
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    for (const auto& [key, rows] : now)
        if (!Write(key, rows, error)) { m_lastError = error; return false; }
    m_lastError.clear();
    return true;
}

std::vector<nlohmann::json> TableRowsAdapter::Rows(uint32_t key) const
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    if (auto it = now.find(key); it != now.end()) return it->second.get<std::vector<nlohmann::json>>();
    if (!Connected()) return {};
    std::string error;
    auto rows = m_db->QueryRows("SELECT * FROM " + m_table + " WHERE `" + m_key + "` = " + std::to_string(key) +
                                (m_order.empty() ? "" : " ORDER BY `" + m_order + "`"), error);
    return rows.value_or(std::vector<nlohmann::json>{});
}

std::map<uint32_t, std::vector<nlohmann::json>> TableRowsAdapter::All() const
{
    std::map<uint32_t, std::vector<nlohmann::json>> all;
    if (Connected())
    {
        std::string error;
        for (auto& row : m_db->QueryRows("SELECT * FROM " + m_table, error).value_or(std::vector<nlohmann::json>{}))
            if (row[m_key].is_string()) all[uint32_t(std::stoul(row[m_key].get<std::string>()))].push_back(std::move(row));
    }
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    for (const auto& [key, rows] : now)
        if (rows.empty()) all.erase(key);
        else all[key] = rows.get<std::vector<nlohmann::json>>();
    return all;
}

Change TableRowsAdapter::MakeChange(uint32_t key, const std::vector<nlohmann::json>& before, const std::vector<nlohmann::json>& after,
                                    const std::string& label) const
{
    Change c;
    c.domain = m_domain;
    c.label = label;
    c.target = m_table + " " + std::to_string(key);
    c.data = { { "key", key }, { "before", before }, { "after", after } };
    return c;
}

bool TableRowsAdapter::ExportSql(const fs::path& outDir, std::string& error) const
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    if (now.empty()) return true;
    Db offline;   // escaping by hand when not connected
    const Db& db = m_db ? *m_db : offline;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::ofstream apply(outDir / (m_table + ".sql")), revert(outDir / (m_table + "_revert.sql"));
    apply << "-- wow-world-editor: " << m_table << " rows of this project (world database). Restart worldserver after applying.\n";
    revert << "-- wow-world-editor: puts the " << m_table << " rows back as they were before this project.\n";
    for (const auto& [key, rows] : now) apply << Statements(db, key, rows);
    for (const auto& [key, rows] : original) revert << Statements(db, key, rows);
    if (!apply || !revert) { error = "Cannot write the " + m_table + " SQL in " + outDir.string(); return false; }
    return true;
}

size_t TableRowsAdapter::Count() const
{
    std::map<uint32_t, nlohmann::json> now, original;
    NetState(now, original);
    size_t n = 0;
    for (const auto& [key, rows] : now) n += rows != original[key];
    return n;
}

std::optional<uint32_t> TableRowsAdapter::NextKey(uint32_t first, uint32_t last) const
{
    // Above every key used in the range: the project's (redo-able ones too) and the database's. Never reused.
    uint64_t next = first;
    auto use = [&](uint32_t key) { if (key >= first && key <= last) next = std::max<uint64_t>(next, uint64_t(key) + 1); };
    for (const auto* list : { &m_store.Done(), &m_store.Undone() })
        ChangeStore::ForEach(*list, [&](const std::string& domain, const nlohmann::json& data) {
            if (domain == m_domain) use(data.at("key").get<uint32_t>());
        });
    if (Connected())
    {
        std::string error;
        if (auto rows = m_db->Query("SELECT MAX(`" + m_key + "`) FROM " + m_table + " WHERE `" + m_key + "` BETWEEN " + std::to_string(first) + " AND " +
                                    std::to_string(last), error);
            rows && !rows->empty() && !(*rows)[0][0].empty())
            use(uint32_t(std::stoul((*rows)[0][0])));
    }
    if (!first || next > last) return std::nullopt;
    return uint32_t(next);
}
