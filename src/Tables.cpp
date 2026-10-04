#include "Tables.hpp"

#include "Server.hpp"

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
