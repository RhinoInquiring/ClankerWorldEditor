#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// One AzerothCore server the editor talks to. Kept per user (not in the project, which is shared through git);
/// passwords live in the Windows credential store under Secret(name, "db" | "soap").
struct ServerProfile
{
    std::string name = "local";
    std::string dbHost = "127.0.0.1";
    int dbPort = 3306;
    std::string dbUser;
    std::string worldDb = "acore_world";
    std::string characterDb = "acore_characters";
    std::string soapHost = "127.0.0.1";
    int soapPort = 7878;
    std::string soapUser;   // game account with GM level 3 (SEC_ADMINISTRATOR)
    std::string serverDir;  // folder with worldserver.exe and the AC map tools

    /// %APPDATA%\wow-world-editor\profiles.json
    static std::vector<ServerProfile> LoadAll();
    static bool SaveAll(const std::vector<ServerProfile>& profiles, std::string& error);
    /// Fills a profile from <serverDir>\configs\worldserver.conf (WorldDatabaseInfo, CharacterDatabaseInfo, SOAP.*);
    /// the DB password from the file is returned in dbPassword.
    static std::optional<ServerProfile> FromWorldserverConf(const std::filesystem::path& serverDir, std::string& dbPassword, std::string& error);
};

/// Windows credential store, target "wow-world-editor:<profile>:<what>".
std::optional<std::string> ReadSecret(const std::string& profile, const std::string& what);
bool WriteSecret(const std::string& profile, const std::string& what, const std::string& secret);

/// %APPDATA%\wow-world-editor (created when missing): per-user settings, panel layout, recent projects, server profiles.
std::filesystem::path SettingsDir();

/// MySQL connection (libmysql.dll, delay-loaded: the editor runs without it, only server features stop).
class Db
{
public:
    Db();
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    bool Connect(const std::string& host, int port, const std::string& user, const std::string& password,
                 const std::string& database, std::string& error);
    void Close();
    bool Connected() const;
    /// Every value as text (NULL = empty). Rows of a statement that returns none are empty.
    std::optional<std::vector<std::vector<std::string>>> Query(const std::string& sql, std::string& error);
    /// Rows as objects, column name -> value text (SQL NULL -> null).
    std::optional<std::vector<nlohmann::json>> QueryRows(const std::string& sql, std::string& error);
    /// A string as a quoted, escaped SQL literal.
    std::string Quote(const std::string& value) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/// INSERT ... ON DUPLICATE KEY UPDATE for a row (column -> value text, null = NULL): writes it whether or not it exists.
std::string UpsertSql(const Db& db, const std::string& table, const nlohmann::json& row);

/// Runs a GM command on worldserver over SOAP (SOAP.Enabled = 1 in worldserver.conf). Returns the command output,
/// or nullopt with error (connection, 401 bad login, 403 GM level below 3, command failure).
std::optional<std::string> SoapCommand(const std::string& host, int port, const std::string& user, const std::string& password,
                                       const std::string& command, std::string& error);
