#include "Server.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <wincred.h>
#include <winhttp.h>
#include <shlobj.h>

#include <mysql.h>

#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

std::filesystem::path SettingsDir()
{
    PWSTR appData = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = std::filesystem::path(appData) / "wow-world-editor";
    CoTaskMemFree(appData);
    std::error_code ec;
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    return dir;
}

namespace
{
std::wstring Wide(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

fs::path ProfilesFile() { return SettingsDir() / "profiles.json"; }

std::string XmlEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    return out;
}

std::string XmlUnescape(std::string s)
{
    for (const auto& [from, to] : { std::pair{ "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&#xD;", "\r" }, { "&#xA;", "\n" }, { "&amp;", "&" } })
        for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += std::strlen(to)) s.replace(p, std::strlen(from), to);
    return s;
}

/// Text of the first <tag>...</tag> (any namespace prefix), or nullopt.
std::optional<std::string> XmlText(const std::string& xml, const std::string& tag)
{
    const std::regex re("<(?:\\w+:)?" + tag + "(?:\\s[^>]*)?>([\\s\\S]*?)</(?:\\w+:)?" + tag + ">");
    std::smatch m;
    if (!std::regex_search(xml, m, re)) return std::nullopt;
    return XmlUnescape(m[1].str());
}
}

// ---------------------------------------------------------------------------------------------- profiles

std::vector<ServerProfile> ServerProfile::LoadAll()
{
    std::vector<ServerProfile> out;
    try
    {
        std::ifstream f(ProfilesFile());
        if (!f) return out;
        for (const auto& j : nlohmann::json::parse(f).value("profiles", nlohmann::json::array()))
        {
            ServerProfile p;
            p.name = j.value("name", p.name);
            p.dbHost = j.value("dbHost", p.dbHost);
            p.dbPort = j.value("dbPort", p.dbPort);
            p.dbUser = j.value("dbUser", p.dbUser);
            p.worldDb = j.value("worldDb", p.worldDb);
            p.characterDb = j.value("characterDb", p.characterDb);
            p.soapHost = j.value("soapHost", p.soapHost);
            p.soapPort = j.value("soapPort", p.soapPort);
            p.soapUser = j.value("soapUser", p.soapUser);
            p.serverDir = j.value("serverDir", p.serverDir);
            out.push_back(p);
        }
    }
    catch (const std::exception&) {}   // a broken file reads as no profiles; saving rewrites it
    return out;
}

bool ServerProfile::SaveAll(const std::vector<ServerProfile>& profiles, std::string& error)
{
    nlohmann::json list = nlohmann::json::array();
    for (const auto& p : profiles)
        list.push_back({ { "name", p.name }, { "dbHost", p.dbHost }, { "dbPort", p.dbPort }, { "dbUser", p.dbUser },
                         { "worldDb", p.worldDb }, { "characterDb", p.characterDb }, { "soapHost", p.soapHost },
                         { "soapPort", p.soapPort }, { "soapUser", p.soapUser }, { "serverDir", p.serverDir } });
    const fs::path file = ProfilesFile();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file);
    f << nlohmann::json{ { "profiles", list } }.dump(2) << "\n";
    if (!f) { error = "Cannot write " + file.string(); return false; }
    return true;
}

std::optional<ServerProfile> ServerProfile::FromWorldserverConf(const fs::path& serverDir, std::string& dbPassword, std::string& error)
{
    const fs::path conf = serverDir / "configs" / "worldserver.conf";
    std::ifstream f(conf);
    if (!f) { error = "No " + conf.string(); return std::nullopt; }
    std::map<std::string, std::string> values;
    const std::regex line("^\\s*([A-Za-z0-9_.]+)\\s*=\\s*\"?([^\"]*)\"?\\s*$");
    for (std::string s; std::getline(f, s);)
        if (std::smatch m; std::regex_match(s, m, line)) values[m[1].str()] = m[2].str();

    ServerProfile p;
    p.serverDir = serverDir.string();
    auto dbInfo = [&](const char* key) {   // "host;port;user;password;database"
        std::vector<std::string> parts;
        std::stringstream ss(values[key]);
        for (std::string part; std::getline(ss, part, ';');) parts.push_back(part);
        return parts;
    };
    const auto world = dbInfo("WorldDatabaseInfo");
    if (world.size() < 5) { error = conf.string() + " has no WorldDatabaseInfo"; return std::nullopt; }
    p.dbHost = world[0] == "." ? "127.0.0.1" : world[0];   // "." = named pipe on Windows; TCP to localhost works too
    p.dbPort = std::atoi(world[1].c_str());
    p.dbUser = world[2];
    dbPassword = world[3];
    p.worldDb = world[4];
    if (const auto chars = dbInfo("CharacterDatabaseInfo"); chars.size() >= 5) p.characterDb = chars[4];
    if (!values["SOAP.IP"].empty()) p.soapHost = values["SOAP.IP"] == "0.0.0.0" ? "127.0.0.1" : values["SOAP.IP"];
    if (!values["SOAP.Port"].empty()) p.soapPort = std::atoi(values["SOAP.Port"].c_str());
    if (values["SOAP.Enabled"] != "1") error = "SOAP is off in worldserver.conf (SOAP.Enabled = 0); server commands will fail until it is on.";
    return p;
}

// ---------------------------------------------------------------------------------------------- secrets

static std::wstring SecretTarget(const std::string& profile, const std::string& what)
{
    return Wide("wow-world-editor:" + profile + ":" + what);
}

std::optional<std::string> ReadSecret(const std::string& profile, const std::string& what)
{
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(SecretTarget(profile, what).c_str(), CRED_TYPE_GENERIC, 0, &cred)) return std::nullopt;
    std::string secret(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
    CredFree(cred);
    return secret;
}

bool WriteSecret(const std::string& profile, const std::string& what, const std::string& secret)
{
    std::wstring target = SecretTarget(profile, what);
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target.data();
    cred.CredentialBlobSize = DWORD(secret.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) != FALSE;
}

// ---------------------------------------------------------------------------------------------- MySQL

struct Db::Impl
{
    MYSQL* conn = nullptr;
};

Db::Db() : m(std::make_unique<Impl>()) {}
Db::~Db() { Close(); }

void Db::Close()
{
    if (m->conn) mysql_close(m->conn);
    m->conn = nullptr;
}

bool Db::Connected() const { return m->conn != nullptr; }

bool Db::Connect(const std::string& host, int port, const std::string& user, const std::string& password,
                 const std::string& database, std::string& error)
{
    Close();
    // libmysql.dll is delay-loaded; load it here so a machine without it gets a message instead of a crash.
    if (!LoadLibraryW(L"libmysql.dll")) { error = "libmysql.dll not found next to the editor (install MySQL 8.x and rebuild)."; return false; }
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) { error = "mysql_init failed"; return false; }
    const unsigned timeout = 3;   // seconds; the UI waits on this
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    if (!mysql_real_connect(conn, host.c_str(), user.c_str(), password.c_str(), database.c_str(), unsigned(port), nullptr, 0))
    {
        error = mysql_error(conn);
        mysql_close(conn);
        return false;
    }
    mysql_set_character_set(conn, "utf8mb4");
    m->conn = conn;
    return true;
}

std::optional<std::vector<std::vector<std::string>>> Db::Query(const std::string& sql, std::string& error)
{
    if (!m->conn) { error = "Not connected to the database."; return std::nullopt; }
    if (mysql_real_query(m->conn, sql.data(), unsigned long(sql.size())))
    {
        error = mysql_error(m->conn);
        return std::nullopt;
    }
    std::vector<std::vector<std::string>> rows;
    MYSQL_RES* res = mysql_store_result(m->conn);
    if (!res)
    {
        if (mysql_field_count(m->conn)) { error = mysql_error(m->conn); return std::nullopt; }
        return rows;   // INSERT / UPDATE / ...
    }
    const unsigned fields = mysql_num_fields(res);
    while (MYSQL_ROW row = mysql_fetch_row(res))
    {
        const unsigned long* lengths = mysql_fetch_lengths(res);
        auto& out = rows.emplace_back(fields);
        for (unsigned i = 0; i < fields; ++i)
            if (row[i]) out[i].assign(row[i], lengths[i]);
    }
    mysql_free_result(res);
    return rows;
}

std::optional<std::vector<nlohmann::json>> Db::QueryRows(const std::string& sql, std::string& error)
{
    if (!m->conn) { error = "Not connected to the database."; return std::nullopt; }
    if (mysql_real_query(m->conn, sql.data(), unsigned long(sql.size()))) { error = mysql_error(m->conn); return std::nullopt; }
    std::vector<nlohmann::json> rows;
    MYSQL_RES* res = mysql_store_result(m->conn);
    if (!res)
    {
        if (mysql_field_count(m->conn)) { error = mysql_error(m->conn); return std::nullopt; }
        return rows;
    }
    const unsigned fields = mysql_num_fields(res);
    const MYSQL_FIELD* info = mysql_fetch_fields(res);
    while (MYSQL_ROW row = mysql_fetch_row(res))
    {
        const unsigned long* lengths = mysql_fetch_lengths(res);
        nlohmann::json& out = rows.emplace_back(nlohmann::json::object());
        for (unsigned i = 0; i < fields; ++i)
            out[info[i].name] = row[i] ? nlohmann::json(std::string(row[i], lengths[i])) : nlohmann::json();
    }
    mysql_free_result(res);
    return rows;
}

std::string Db::Quote(const std::string& value) const
{
    if (!m->conn)   // no connection to ask: escape quote and backslash by hand
    {
        std::string out;
        for (char c : value)
        {
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        return "'" + out + "'";
    }
    std::string out(value.size() * 2 + 1, '\0');
    out.resize(mysql_real_escape_string(m->conn, out.data(), value.data(), unsigned long(value.size())));
    return "'" + out + "'";
}

// ---------------------------------------------------------------------------------------------- SOAP

std::optional<std::string> SoapCommand(const std::string& host, int port, const std::string& user, const std::string& password,
                                       const std::string& command, std::string& error)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\" xmlns:ns1=\"urn:AC\">"
        "<SOAP-ENV:Body><ns1:executeCommand><command>" + XmlEscape(command) + "</command></ns1:executeCommand>"
        "</SOAP-ENV:Body></SOAP-ENV:Envelope>";

    HINTERNET session = WinHttpOpen(L"wow-world-editor", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET connect = session ? WinHttpConnect(session, Wide(host).c_str(), INTERNET_PORT(port), 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", L"/", nullptr, nullptr, nullptr, 0) : nullptr;
    std::string response;
    DWORD status = 0;
    bool ok = false;
    if (request)
    {
        WinHttpSetTimeouts(request, 3000, 3000, 3000, 15000);   // a slow command may take a while; connecting may not
        const std::wstring u = Wide(user), pw = Wide(password);
        WinHttpSetCredentials(request, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, u.c_str(), pw.c_str(), nullptr);
        ok = WinHttpSendRequest(request, L"Content-Type: text/xml; charset=utf-8\r\nSOAPAction: \"urn:AC#executeCommand\"",
                                DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) &&
             WinHttpReceiveResponse(request, nullptr);
        if (ok)
        {
            DWORD size = sizeof status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
            for (DWORD avail = 0; WinHttpQueryDataAvailable(request, &avail) && avail;)
            {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
                response.append(chunk, 0, read);
            }
        }
    }
    const DWORD lastError = GetLastError();
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);

    if (!ok)
    {
        error = lastError == ERROR_WINHTTP_CANNOT_CONNECT || lastError == ERROR_WINHTTP_TIMEOUT
                    ? "No SOAP server at " + host + ":" + std::to_string(port) + " (worldserver running with SOAP.Enabled = 1?)"
                    : "SOAP request failed (WinHTTP error " + std::to_string(lastError) + ")";
        return std::nullopt;
    }
    if (status == 401) { error = "SOAP login refused: wrong account name or password."; return std::nullopt; }
    if (status == 403) { error = "SOAP account's GM level is below 3 (administrator)."; return std::nullopt; }
    if (auto result = XmlText(response, "result"); result && status == 200) return *result;
    error = XmlText(response, "detail").value_or(XmlText(response, "faultstring").value_or("HTTP " + std::to_string(status)));
    if (!error.empty() && error.back() == '\n') error.pop_back();
    return std::nullopt;
}

std::string UpsertSql(const Db& db, const std::string& table, const nlohmann::json& row)
{
    std::string cols, vals, update;
    for (const auto& [column, value] : row.items())
    {
        const std::string v = value.is_null() ? "NULL" : db.Quote(value.get<std::string>());
        cols += (cols.empty() ? "`" : ", `") + column + "`";
        vals += (vals.empty() ? "" : ", ") + v;
        update += (update.empty() ? "`" : ", `") + column + "` = VALUES(`" + column + "`)";
    }
    return "INSERT INTO " + table + " (" + cols + ") VALUES (" + vals + ") ON DUPLICATE KEY UPDATE " + update;
}
