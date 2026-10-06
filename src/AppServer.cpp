// Server link (setup wizard, Server panel) and the Problems panel.
#include "App.hpp"

#include "Assets.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <shellapi.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace
{
const ImVec4 kGood{ 0.40f, 0.80f, 0.45f, 1.00f };
const ImVec4 kBad{ 1.00f, 0.42f, 0.38f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };

/// AzerothCore programs the editor uses from the server folder (map data rebuilds in M4).
const char* kServerTools[] = { "worldserver.exe", "map_extractor.exe", "vmap4_extractor.exe", "vmap4_assembler.exe", "mmaps_generator.exe" };

/// "(?)" after the previous item: hover explains, click opens the AzerothCore wiki page (azerothcore.org/wiki/<page>).
void Help(const char* text, const char* wikiPage = nullptr)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        if (wikiPage) ImGui::TextColored(kQuiet, "Click for the wiki page: %s", wikiPage);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (wikiPage && ImGui::IsItemClicked())
        ShellExecuteA(nullptr, "open", ("https://www.azerothcore.org/wiki/" + std::string(wikiPage)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const ServerProfile* FindProfile(const std::vector<ServerProfile>& profiles, const std::string& name)
{
    for (const auto& p : profiles)
        if (p.name == name) return &p;
    return nullptr;
}

/// Connects and reads the world database's core version: proves host, login and database name in one go.
bool TestDatabase(Db& db, const ServerProfile& p, const std::string& password, std::string& result)
{
    std::string error;
    if (!db.Connect(p.dbHost, p.dbPort, p.dbUser, password, p.worldDb, error)) { result = error; return false; }
    const auto rows = db.Query("SELECT core_version FROM version LIMIT 1", error);
    if (!rows) { result = "Connected, but " + p.worldDb + " is not an AzerothCore world database: " + error; return false; }
    result = "Connected to " + p.worldDb + (rows->empty() ? std::string() : " (" + (*rows)[0][0] + ")");
    return true;
}
}

// ---------------------------------------------------------------------------------------------- connection

void App::ConnectServer()
{
    m_db.Close();
    m_soapOk.reset();
    if (!m_project || m_project->serverProfile.empty()) return;
    m_profiles = ServerProfile::LoadAll();
    const ServerProfile* p = FindProfile(m_profiles, m_project->serverProfile);
    if (!p) { Log("Server profile '%s' is not set up on this machine (File > Server setup).", m_project->serverProfile.c_str()); return; }
    // ponytail: connects on the UI thread (3 s timeout); move to a worker if remote servers make opening slow.
    std::string result;
    if (TestDatabase(m_db, *p, ReadSecret(p->name, "db").value_or(""), result))
    {
        Log("Server '%s': %s.", p->name.c_str(), result.c_str());
        // The project's spawn changes go to this database (idempotent; catches up after offline edits).
        for (SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
            if (std::string error; !spawns->Sync(error)) Log("%s spawns not written to the database: %s", spawns->Table(), error.c_str());
        for (TableRowsAdapter* table : TableAdapters())
            if (std::string error; !table->Sync(error)) Log("%s not written to the database: %s", table->Table().c_str(), error.c_str());
    }
    else Log("Server '%s': database not connected: %s", p->name.c_str(), result.c_str());
}

std::optional<std::string> App::RunServerCommand(const std::string& command)
{
    std::string error;
    const ServerProfile* p = m_project ? FindProfile(m_profiles, m_project->serverProfile) : nullptr;
    std::optional<std::string> out;
    if (!p) error = "No server profile for this project (File > Server setup).";
    else out = SoapCommand(p->soapHost, p->soapPort, p->soapUser, ReadSecret(p->name, "soap").value_or(""), command, error);
    m_soapOk = out.has_value();
    m_commandLog.push_back({ command, out ? *out : "error: " + error });
    if (!out) Log("Server command '%s' failed: %s", command.c_str(), error.c_str());
    return out;
}

// ---------------------------------------------------------------------------------------------- setup wizard

void App::OpenSetup()
{
    if (!m_project) return;
    m_profiles = ServerProfile::LoadAll();
    const ServerProfile* p = FindProfile(m_profiles, m_project->serverProfile);
    m_setup = p ? *p : (m_profiles.empty() ? ServerProfile{} : m_profiles.front());
    m_setupClient = m_project->clientDir;
    m_setupDbPassword = ReadSecret(m_setup.name, "db").value_or("");
    m_setupSoapPassword = ReadSecret(m_setup.name, "soap").value_or("");
    m_setupDbResult.clear();
    m_setupSoapResult.clear();
    m_setupOpen = true;
}

void App::DrawSetupModal()
{
    if (m_setupOpen) { ImGui::OpenPopup("Server setup"); m_setupOpen = false; }
    ImGui::SetNextWindowSize({ 640, 0 }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Server setup", nullptr, ImGuiWindowFlags_NoResize)) return;
    const float field = ImGui::GetContentRegionAvail().x - 200;
    auto row = [&](const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
        ImGui::SetNextItemWidth(field);
        ImGui::InputText(label, &value, flags);
    };
    auto port = [&](const char* label, int& value) {
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt(label, &value, 0);
    };
    auto status = [](bool ok, const std::string& text) {
        if (!text.empty()) ImGui::TextColored(ok ? kGood : kBad, "%s", text.c_str());
    };

    ImGui::SeparatorText("1  Client");
    ImGui::SetNextItemWidth(field - 90);
    ImGui::InputText("##client", &m_setupClient);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##client"))
        if (auto dir = PickFolder(m_hwnd, L"WXL client folder (contains Wow.exe and Data)")) m_setupClient = *dir;
    ImGui::SameLine();
    ImGui::TextUnformatted("Client folder");
    Help("The WXL 3.3.5a client (the folder with Wow.exe and Data). The editor reads maps, models and textures from its "
         "MPQs and play-tests in it; changing it reopens the project.");
    const bool clientOk = fs::exists(fs::path(m_setupClient) / "Data");
    status(clientOk, clientOk ? "Data folder found" : "That folder has no Data folder");

    ImGui::SeparatorText("2  Server folder");
    Help("worldserver and the AzerothCore map tools (map_extractor, vmap4_extractor, vmap4_assembler, mmaps_generator). "
         "The tools rebuild the server's maps, vmaps and mmaps for edited tiles.", "windows-server-setup");
    ImGui::SetNextItemWidth(field - 90);
    ImGui::InputText("##serverdir", &m_setup.serverDir);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##server"))
        if (auto dir = PickFolder(m_hwnd, L"AzerothCore server folder (contains worldserver.exe and configs)")) m_setup.serverDir = *dir;
    ImGui::SameLine();
    ImGui::TextUnformatted("Folder");
    Help("The AzerothCore build folder: worldserver.exe, the map tools and configs/worldserver.conf "
         "(e.g. C:/Build/bin/RelWithDebInfo).", "windows-server-setup");
    if (ImGui::Button("Read settings from worldserver.conf"))
    {
        std::string password, note;
        if (auto p = ServerProfile::FromWorldserverConf(m_setup.serverDir, password, note))
        {
            const std::string name = m_setup.name, soapUser = m_setup.soapUser;
            m_setup = *p;
            m_setup.name = name;
            m_setup.soapUser = soapUser;
            m_setupDbPassword = password;
            m_setupDbResult = note.empty() ? "Read database and SOAP settings." : note;
            m_setupDbOk = note.empty();
        }
        else { m_setupDbResult = note; m_setupDbOk = false; }
    }
    Help("Fills the database and SOAP fields from <folder>/configs/worldserver.conf: WorldDatabaseInfo and "
         "CharacterDatabaseInfo (\"IP;Port;Username;Password;database\"), SOAP.IP and SOAP.Port.", "windows-server-setup");
    for (size_t i = 0; i < std::size(kServerTools); ++i)
    {
        const bool found = !m_setup.serverDir.empty() && fs::exists(fs::path(m_setup.serverDir) / kServerTools[i]);
        if (i % 3) ImGui::SameLine(0, 18);
        ImGui::TextColored(found ? kGood : kQuiet, "%s %s", found ? "found" : "missing", kServerTools[i]);
    }

    ImGui::SeparatorText("3  Database");
    Help("The MySQL login worldserver uses (same values as WorldDatabaseInfo). The editor reads and later writes "
         "the world database (acore_world by default) and reads characters (acore_characters).", "database-installation");
    row("Host", m_setup.dbHost);
    port("Port##db", m_setup.dbPort);
    row("User", m_setup.dbUser);
    row("Password##db", m_setupDbPassword, ImGuiInputTextFlags_Password);
    row("World database", m_setup.worldDb);
    row("Characters database", m_setup.characterDb);
    if (ImGui::Button("Test database")) m_setupDbOk = TestDatabase(m_db, m_setup, m_setupDbPassword, m_setupDbResult);
    status(m_setupDbOk, m_setupDbResult);

    ImGui::SeparatorText("4  SOAP (server commands)");
    Help("SOAP lets the editor run GM commands (reloads, teleports) on the running worldserver over HTTP. "
         "Set SOAP.Enabled = 1 in worldserver.conf and restart worldserver.", "remote-access");
    row("Host##soap", m_setup.soapHost);
    port("Port##soap", m_setup.soapPort);
    row("Account", m_setup.soapUser);
    Help("A game account with GM level 3 on all realms. In the worldserver console: account create <name> <password>, "
         "then account set gmlevel <name> 3 -1.", "gm-commands");
    row("Password##soap", m_setupSoapPassword, ImGuiInputTextFlags_Password);
    if (ImGui::Button("Test SOAP"))
    {
        std::string error;
        const auto out = SoapCommand(m_setup.soapHost, m_setup.soapPort, m_setup.soapUser, m_setupSoapPassword, "server info", error);
        m_setupSoapOk = out.has_value();
        m_setupSoapResult = out ? out->substr(0, out->find('\n')) : error;
    }
    status(m_setupSoapOk, m_setupSoapResult);

    ImGui::Separator();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Profile name", &m_setup.name);
    ImGui::SetItemTooltip("Stored for your user on this machine, not in the project. Passwords go to the Windows credential store.");
    ImGui::BeginDisabled(m_setup.name.empty() || !clientOk);
    if (ImGui::Button("Save", { 120, 0 }))
    {
        std::string error;
        auto existing = std::find_if(m_profiles.begin(), m_profiles.end(), [&](const ServerProfile& p) { return p.name == m_setup.name; });
        if (existing != m_profiles.end()) *existing = m_setup;
        else m_profiles.push_back(m_setup);
        const bool secretsOk = WriteSecret(m_setup.name, "db", m_setupDbPassword) && WriteSecret(m_setup.name, "soap", m_setupSoapPassword);
        if (!ServerProfile::SaveAll(m_profiles, error)) Log("Server profile not saved: %s", error.c_str());
        else if (!secretsOk) Log("Server profile saved, but the Windows credential store refused the passwords.");
        const bool clientChanged = m_setupClient != m_project->clientDir;
        m_project->serverProfile = m_setup.name;
        m_project->clientDir = m_setupClient;
        if (!m_project->Save(error)) Log("Project not saved: %s", error.c_str());
        ImGui::CloseCurrentPopup();
        if (clientChanged) GuardUnsaved([this, dir = m_project->dir.string()] { OpenProject(dir); });   // reopens and connects
        else ConnectServer();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Later", { 120, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------- Server panel

void App::DrawServerPanel()
{
    if (!ImGui::Begin("Server")) { ImGui::End(); return; }
    if (!m_project) { ImGui::TextColored(kQuiet, "Open a project first."); ImGui::End(); return; }
    if (m_project->serverProfile.empty())
    {
        ImGui::TextColored(kQuiet, "No server set up for this project.");
        if (ImGui::Button("Server setup...")) OpenSetup();
        ImGui::End();
        return;
    }
    ImGui::Text("Profile %s", m_project->serverProfile.c_str());
    ImGui::SameLine(0, 20);
    if (m_db.Connected()) ImGui::TextColored(kGood, "database connected");
    else ImGui::TextColored(kBad, "database offline");
    ImGui::SameLine(0, 20);
    if (!m_soapOk) ImGui::TextColored(kQuiet, "SOAP untested");
    else if (*m_soapOk) ImGui::TextColored(kGood, "SOAP ok");
    else ImGui::TextColored(kBad, "SOAP failed");
    ImGui::SameLine(0, 20);
    if (ImGui::SmallButton("Reconnect")) ConnectServer();
    ImGui::SameLine();
    if (ImGui::SmallButton("Setup...")) OpenSetup();

    ImGui::SetNextItemWidth(-90);
    const bool enter = ImGui::InputTextWithHint("##cmd", "GM command, e.g. server info", &m_command, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run", { 80, 0 }) || enter) && !m_command.empty())
    {
        std::string command = m_command;
        if (command[0] == '.') command.erase(0, 1);   // the console takes commands without the dot
        RunServerCommand(command);
        m_command.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (ImGui::BeginChild("##out", { 0, 0 }, ImGuiChildFlags_Borders))
    {
        for (const auto& [command, output] : m_commandLog)
        {
            ImGui::TextColored(kQuiet, "> %s", command.c_str());
            ImGui::TextUnformatted(output.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------- Problems

void App::RunChecks()
{
    m_problems.clear();
    m_problemsChecked = true;
    if (!m_project) return;
    // The real export pipeline, written to a scratch folder: whatever export would refuse or fix shows up here.
    const fs::path out = m_project->dir / "out" / "check";
    std::error_code ec;
    fs::remove_all(out, ec);
    std::string error;
    std::vector<fs::path> tiles;
    m_terrain.Export(out, error, &tiles, &m_problems);
    m_terrain.FindCracks(m_problems);
    if (!error.empty()) m_problems.push_back({ Problem::Severity::Error, "Terrain", error });
    m_terrain.TakeDroppedEffects();   // counted per tile in the problems instead
    for (const auto& missing : CopyMissingAssets(m_mpq, tiles, out).missing)
        m_problems.push_back({ Problem::Severity::Error, "Assets", "Referenced by an edited tile, found in no client: " + missing });
    fs::remove_all(out, ec);

    if (!m_project->serverProfile.empty() && !m_db.Connected())
        m_problems.push_back({ Problem::Severity::Warning, "Server", "Database of profile '" + m_project->serverProfile + "' is not connected." });
    for (const SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
        if (!spawns->LastError().empty()) m_problems.push_back({ Problem::Severity::Error, "Server", spawns->LastError() });
    for (const TableRowsAdapter* table : TableAdapters())
        if (!table->LastError().empty()) m_problems.push_back({ Problem::Severity::Error, "Server", table->LastError() });
    // ID ranges: rows someone else put in ours, and ranges with no ids left.
    for (const SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
    {
        const Project::IdRange r = m_project->Range(spawns->IdKind());
        char text[256];
        if (!r.first || r.last < r.first)
        {
            snprintf(text, sizeof text, "No %s range set (File > Project settings).", spawns->IdKind().c_str());
            m_problems.push_back({ Problem::Severity::Error, "IDs", text });
            continue;
        }
        if (!m_db.Connected()) continue;
        if (const auto use = spawns->Use(r.first, r.last); use.others)
        {
            snprintf(text, sizeof text, "%zu %s row(s) in this project's range %u-%u were not added by it (another project or module?).",
                     use.others, spawns->Table(), r.first, r.last);
            m_problems.push_back({ Problem::Severity::Warning, "IDs", text });
        }
        if (!spawns->NextGuid(r.first, r.last))
        {
            snprintf(text, sizeof text, "The %s range %u-%u is full: new spawns cannot be placed.", spawns->IdKind().c_str(), r.first, r.last);
            m_problems.push_back({ Problem::Severity::Error, "IDs", text });
        }
    }
    if (const Project::IdRange r = m_project->Range("area.id"); r.first && r.last >= r.first)
        m_areas.Check(r.first, r.last, m_problems);
    else
        m_problems.push_back({ Problem::Severity::Error, "IDs", "No area.id range set (File > Project settings)." });
    for (const auto& [kind, table] : std::initializer_list<std::pair<const char*, const DbcTable*>>{
             { "wmoarea.id", &m_wmoAreas }, { "worldmaparea.id", &m_worldMaps }, { "worldmapoverlay.id", &m_mapOverlays } })
        if (const Project::IdRange r = m_project->Range(kind); r.first && r.last >= r.first)
            table->CheckIds(r.first, r.last, "Zones", m_problems);
        else
            m_problems.push_back({ Problem::Severity::Error, "IDs", std::string("No ") + kind + " range set (File > Project settings)." });
    CheckTriggers(m_problems);
    CheckPois(m_problems);
    CheckFlights(m_problems);
    if (AreasPainted())
        m_problems.push_back({ Problem::Severity::Warning, "Zones",
                               "Area ids were painted: the server reads areas from its extracted .map files, so extract them again from the exported tiles." });
    if (m_soapOk == false)
        m_problems.push_back({ Problem::Severity::Warning, "Server", "Last SOAP command failed; server reloads will not reach worldserver." });
    std::stable_sort(m_problems.begin(), m_problems.end(), [](const Problem& a, const Problem& b) { return a.severity < b.severity; });
}

void App::DrawProblemsPanel()
{
    if (!ImGui::Begin("Problems")) { ImGui::End(); return; }
    if (ImGui::Button("Check now")) RunChecks();
    ImGui::SetItemTooltip("Runs the export checks on every edited tile (without touching the client) and the server link.");
    ImGui::SameLine();
    const auto errors = std::count_if(m_problems.begin(), m_problems.end(), [](const Problem& p) { return p.severity == Problem::Severity::Error; });
    if (!m_problemsChecked) ImGui::TextColored(kQuiet, "Not checked yet (also runs when the project opens and on export).");
    else if (m_problems.empty()) ImGui::TextColored(kGood, "No problems.");
    else ImGui::TextColored(errors ? kBad : kWarn, "%zd error(s), %zd warning(s)", errors, std::ptrdiff_t(m_problems.size()) - errors);

    if (!m_problems.empty() &&
        ImGui::BeginTable("##problems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Problem");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < m_problems.size(); ++i)
        {
            const Problem& p = m_problems[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            const bool error = p.severity == Problem::Severity::Error;
            // The whole row is the click target: located problems fly the camera to their tile.
            if (ImGui::Selectable(error ? "error" : "warning", false, ImGuiSelectableFlags_SpanAllColumns) && !p.map.empty())
            {
                for (size_t m = 0; m < m_maps.size(); ++m)
                    if (m_maps[m].directory == p.map) SelectMap(m);
                GoToTile(p.map, p.tx, p.ty);
            }
            if (!p.map.empty()) ImGui::SetItemTooltip("Go to %s %d_%d", p.map.c_str(), p.tx, p.ty);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextColored(error ? kBad : kWarn, "%s", p.area.c_str());
            ImGui::TableNextColumn();
            if (p.map.empty()) ImGui::TextColored(kQuiet, "-");
            else ImGui::Text("%s %d_%d", p.map.c_str(), p.tx, p.ty);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", p.message.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------- project settings

void App::OpenProjectSettings()
{
    if (!m_project) return;
    m_settingsName = m_project->name;
    m_settingsAuthor = m_project->author;
    m_settingsRanges = m_project->idRanges;
    m_settingsUse.clear();
    for (const SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
    {
        const Project::IdRange r = m_project->Range(spawns->IdKind());
        m_settingsUse[spawns->IdKind()] = spawns->Use(r.first, r.last);
    }
    m_settingsOpen = true;
}

void App::DrawProjectSettingsModal()
{
    if (m_settingsOpen) { ImGui::OpenPopup("Project settings"); m_settingsOpen = false; }
    ImGui::SetNextWindowSize({ 720, 0 }, ImGuiCond_Appearing);
    if (!m_project || !ImGui::BeginPopupModal("Project settings", nullptr, ImGuiWindowFlags_NoResize)) return;
    const float field = ImGui::GetContentRegionAvail().x - 120;
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Name", &m_settingsName);
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Author", &m_settingsAuthor);
    ImGui::TextColored(kQuiet, "Client: %s", m_project->clientDir.c_str());

    ImGui::SeparatorText("ID ranges");
    ImGui::TextWrapped("New rows take ids from these ranges only, and an id never changes once a change records it. "
                       "Give every project (and module) its own range so their rows never clash.");
    bool valid = true;
    if (ImGui::BeginTable("##ranges", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("First");
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Yours");
        ImGui::TableSetupColumn("Others");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        for (SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
        {
            const std::string kind = spawns->IdKind();
            Project::IdRange& r = m_settingsRanges[kind];
            SpawnAdapter::RangeUse& use = m_settingsUse[kind];
            ImGui::PushID(kind.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kind.c_str());
            bool changed = false;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::InputScalar("##first", ImGuiDataType_U32, &r.first);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::InputScalar("##last", ImGuiDataType_U32, &r.last);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", use.mine);
            ImGui::TableNextColumn();
            if (use.others) ImGui::TextColored(kWarn, "%zu", use.others);
            else ImGui::TextColored(kQuiet, "0");
            ImGui::SetItemTooltip("Database rows inside this range that this project did not add (another project or module).");
            ImGui::TableNextColumn();
            if (ImGui::Button("Suggest", { -1, 0 }))
            {
                // A fresh block of 100,000 above everything in the table.
                r.first = (use.highest / 100000 + 1) * 100000;
                r.last = r.first + 99999;
                changed = true;
            }
            ImGui::SetItemTooltip("A block of 100,000 ids above the highest %s in the database (%u).", spawns->Table(), use.highest);
            if (changed) use = spawns->Use(r.first, r.last);
            if (!r.first || r.last < r.first) valid = false;
            ImGui::PopID();
        }
        for (const char* kind : { "area.id", "wmoarea.id", "worldmaparea.id", "worldmapoverlay.id", "areatrigger.id" })   // DBC ids live in the client's files, not the database
        {
            Project::IdRange& r = m_settingsRanges[kind];
            ImGui::PushID(kind);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kind);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputScalar("##first", ImGuiDataType_U32, &r.first);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputScalar("##last", ImGuiDataType_U32, &r.last);
            ImGui::TableNextColumn();
            ImGui::TextColored(kQuiet, "-");
            ImGui::TableNextColumn();
            ImGui::TextColored(kQuiet, "-");
            ImGui::SetItemTooltip("Problems > Check now lists client DBC rows inside this range.");
            if (!r.first || r.last < r.first || (std::string(kind) == "area.id" && r.last > 65535)) valid = false;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!valid) ImGui::TextColored(kWarn, "A range needs a first id above 0 and a last id at or above it (area ids at most 65535).");
    if (!m_db.Connected()) ImGui::TextColored(kQuiet, "Not connected to the world database: clashes are not checked.");

    ImGui::Separator();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Save", { 120, 0 }))
    {
        m_project->name = m_settingsName;
        m_project->author = m_settingsAuthor;
        m_project->idRanges = m_settingsRanges;
        if (std::string error; m_project->Save(error)) Log("Project settings saved.");
        else Log("%s", error.c_str());
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
