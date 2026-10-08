#include "ServerData.hpp"

#include "Formats.hpp"
#include "Models.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }

    std::optional<std::vector<uint8_t>> ReadFile(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return std::nullopt;
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    /// The model names a WMO root's doodads use (MODN: zero-terminated strings).
    std::vector<std::string> WmoDoodadNames(const std::vector<uint8_t>& root)
    {
        std::vector<std::string> names;
        for (size_t pos = 0; pos + 8 <= root.size();)
        {
            uint32_t magic = 0, size = 0;
            std::memcpy(&magic, root.data() + pos, 4);
            std::memcpy(&size, root.data() + pos + 4, 4);
            if (size > root.size() - pos - 8) break;
            if (magic == 'MODN')
                for (size_t p = pos + 8; p < pos + 8 + size;)
                {
                    const char* s = reinterpret_cast<const char*>(root.data() + p);
                    const size_t n = strnlen(s, pos + 8 + size - p);
                    if (n) names.emplace_back(s, n);
                    p += n + 1;
                }
            pos += 8 + size;
        }
        return names;
    }

    /// A DBC with no rows (the vmap extractor needs GameObjectDisplayInfo.dbc to exist; the server keeps its own
    /// gameobject models).
    std::vector<uint8_t> EmptyDbc(uint32_t fields)
    {
        std::vector<uint8_t> d(20 + 1, 0);
        std::memcpy(d.data(), "WDBC", 4);
        const uint32_t header[4] = { 0, fields, fields * 4, 1 };
        std::memcpy(d.data() + 4, header, 16);
        return d;
    }

    std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}

void ServerDataJob::Start(Options options, std::vector<ServerMap> maps)
{
    if (m_running) return;
    if (m_thread.joinable()) m_thread.join();
    m_cancel = false;
    m_succeeded = false;
    m_progress = 0;
    m_running = true;
    m_thread = std::thread([this, options = std::move(options), maps = std::move(maps)]() mutable { Run(std::move(options), std::move(maps)); });
}

void ServerDataJob::Cancel()
{
    m_cancel = true;
    {
        std::lock_guard lock(m_mutex);
        if (m_process) TerminateProcess(HANDLE(m_process), 1);
    }
    if (m_thread.joinable()) m_thread.join();
}

std::string ServerDataJob::Status() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

std::vector<std::string> ServerDataJob::TakeLog()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_log, {});
}

void ServerDataJob::Log(const std::string& line)
{
    std::lock_guard lock(m_mutex);
    m_log.push_back(line);
}

void ServerDataJob::SetStatus(const std::string& status)
{
    {
        std::lock_guard lock(m_mutex);
        m_status = status;
    }
    Log(status);
}

void ServerDataJob::Run(Options o, std::vector<ServerMap> maps)
{
    MpqChain chain;   // its own: the editor's is used on the main thread
    chain.Open(o.layers);
    bool ok = true;
    for (size_t i = 0; i < maps.size() && ok && !m_cancel; ++i)
        ok = BuildMap(o, maps[i], chain, float(i) / maps.size(), float(i + 1) / maps.size());
    std::error_code ec;
    if (ok && !m_cancel) fs::remove_all(o.work, ec);   // kept after a failure, with the tools' logs
    m_succeeded = ok && !m_cancel;
    m_progress = 1;
    SetStatus(m_cancel ? "Cancelled." : ok ? "Done: restart the worldserver to load the new server data." : "Failed: see the log above.");
    m_running = false;
}

bool ServerDataJob::Tool(const fs::path& exe, const std::wstring& args, const fs::path& cwd, const std::string& log)
{
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE out = CreateFileW((cwd / log).wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);   // "press any key" gets EOF
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = in;
    PROCESS_INFORMATION pi{};
    std::wstring line = L"\"" + exe.wstring() + L"\" " + args;
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.wstring().c_str(), &si, &pi);
    CloseHandle(out);
    CloseHandle(in);
    if (!started)
    {
        Log("Cannot start " + exe.string() + " (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_process = pi.hProcess;
    }
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT && !m_cancel) {}
    if (m_cancel) TerminateProcess(pi.hProcess, 1), WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    {
        std::lock_guard lock(m_mutex);
        m_process = nullptr;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0 && !m_cancel)
    {
        Log(exe.filename().string() + " failed (exit code " + std::to_string(code) + "); its output: " + (cwd / log).string());
        if (const auto text = ReadFile(cwd / log))   // the last lines say why
        {
            std::string s(text->begin(), text->end());
            std::replace(s.begin(), s.end(), '\r', '\n');
            std::istringstream lines(s.substr(s.size() > 1500 ? s.size() - 1500 : 0));
            for (std::string l; std::getline(lines, l);)
                if (!l.empty()) Log("    " + l);
        }
    }
    return code == 0 && !m_cancel;
}

bool ServerDataJob::BuildMap(const Options& o, const ServerMap& map, MpqChain& chain, float from, float to)
{
    auto progress = [&](float f) { m_progress = from + (to - from) * f; };
    std::error_code ec;
    fs::remove_all(o.work, ec);
    fs::create_directories(o.work / "stage" / "Data" / "enUS", ec);
    if (ec) { Log("Cannot make " + o.work.string() + ": " + ec.message()); return false; }

    // The files: the project's export first, then the sources.
    auto read = [&](const std::string& name) -> std::optional<std::vector<uint8_t>> {
        if (Lower(name) == "dbfilesclient\\gameobjectdisplayinfo.dbc") return EmptyDbc(19);
        if (auto f = ReadFile(o.exported / name)) return f;
        return chain.Read(name);
    };

    SetStatus("[" + map.directory + "] Listing the map's tiles and models...");
    const std::string mapPath = "World\\Maps\\" + map.directory + "\\" + map.directory;
    const auto wdt = read(mapPath + ".wdt");
    if (!wdt) { Log("No " + mapPath + ".wdt"); return false; }
    std::vector<std::string> names{ "DBFilesClient\\Map.dbc", "DBFilesClient\\LiquidType.dbc", "DBFilesClient\\GameObjectDisplayInfo.dbc", mapPath + ".wdt" };
    std::map<std::string, std::string> models, wmos;   // lower-case -> name
    if (const auto global = WdtGlobalWmo(*wdt)) wmos.emplace(Lower(global->model), global->model);
    const std::vector<bool> present = WdtTiles(*wdt);
    size_t tiles = 0;
    for (int k = 0; k < 4096 && !m_cancel; ++k)
    {
        if (!present[size_t(k)]) continue;
        const std::string adtName = mapPath + "_" + std::to_string(k % 64) + "_" + std::to_string(k / 64) + ".adt";
        const auto bytes = read(adtName);
        if (!bytes) continue;
        names.push_back(adtName);
        ++tiles;
        if (const auto adt = ParseAdt(*bytes, false))
        {
            for (const DoodadPlacement& d : adt->doodads) models.emplace(Lower(M2Name(d.model)), M2Name(d.model));
            for (const WmoPlacement& w : adt->wmos) wmos.emplace(Lower(w.model), w.model);
        }
        progress(0.1f * k / 4096);
    }
    for (const auto& [key, wmo] : wmos)
    {
        const auto root = read(wmo);
        if (!root) continue;
        names.push_back(wmo);
        uint32_t groups = 0;
        float bounds[6];
        if (WmoRootInfo(*root, groups, bounds))
            for (uint32_t g = 0; g < groups && g < 512; ++g) names.push_back(WmoGroupFile(wmo, *root, g));
        for (const std::string& d : WmoDoodadNames(*root)) models.emplace(Lower(M2Name(d)), M2Name(d));
    }
    for (const auto& [key, m2] : models) names.push_back(m2);
    Log("[" + map.directory + "] " + std::to_string(tiles) + " tiles, " + std::to_string(wmos.size()) + " buildings, " +
        std::to_string(models.size()) + " models");
    if (m_cancel) return false;

    SetStatus("[" + map.directory + "] Packing the staging client...");
    std::string error;
    size_t packed = 0;
    if (!WriteMpqFrom(o.work / "stage" / "Data" / "common.MPQ", names, read, error, &packed)) { Log(error); return false; }
    // The build number the map extractor checks (component.wow-enUS.txt in the locale archive).
    const auto component = [&](const std::string&) -> std::optional<std::vector<uint8_t>> {
        if (auto c = chain.Read("component.wow-enUS.txt")) return c;
        const std::string text = "version=\"12340\"";
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    if (!WriteMpqFrom(o.work / "stage" / "Data" / "enUS" / "locale-enUS.MPQ", { "component.wow-enUS.txt" }, component, error)) { Log(error); return false; }
    Log("[" + map.directory + "] staging client: " + std::to_string(packed) + " files");
    progress(0.25f);

    char id[8];
    snprintf(id, sizeof id, "%03u", map.id);
    SetStatus("[" + map.directory + "] map_extractor: terrain, liquids, areas, holes...");
    if (!Tool(o.tools / "map_extractor.exe", L"-e 1 -i stage -o .", o.work, "map_extractor.log")) return false;
    progress(0.35f);
    SetStatus("[" + map.directory + "] vmap4_extractor: buildings and models...");
    if (!Tool(o.tools / "vmap4_extractor.exe", L"-d stage/Data", o.work, "vmap4_extractor.log")) return false;
    progress(0.5f);
    SetStatus("[" + map.directory + "] vmap4_assembler...");
    fs::create_directories(o.work / "vmaps", ec);
    if (!Tool(o.tools / "vmap4_assembler.exe", L"Buildings vmaps", o.work, "vmap4_assembler.log")) return false;
    progress(0.6f);

    // mmaps: the generator's own settings, reading and writing here.
    if (auto config = ReadFile(o.tools / "mmaps-config.yaml"))
    {
        const std::string text = std::regex_replace(std::string(config->begin(), config->end()), std::regex(R"(dataDir:\s*"[^"]*")"), "dataDir: \"./\"");
        std::ofstream(o.work / "mmaps-config.yaml", std::ios::binary) << text;
    }
    else { Log("No mmaps-config.yaml next to mmaps_generator.exe"); return false; }
    fs::create_directories(o.work / "mmaps", ec);
    if (map.mmapTiles.empty())
    {
        SetStatus("[" + map.directory + "] mmaps_generator: every tile (this can take long)...");
        if (!Tool(o.tools / "mmaps_generator.exe", Wide(std::to_string(map.id)) + L" --config mmaps-config.yaml --silent", o.work, "mmaps_generator.log"))
            return false;
    }
    else
    {
        size_t done = 0;
        for (const auto& [x, y] : map.mmapTiles)
        {
            if (m_cancel) return false;
            SetStatus("[" + map.directory + "] mmaps_generator: tile " + std::to_string(x) + "_" + std::to_string(y) + " (" + std::to_string(++done) +
                      " of " + std::to_string(map.mmapTiles.size()) + ")...");
            const std::wstring args = Wide(std::to_string(map.id)) + L" --tile " + Wide(std::to_string(x) + "," + std::to_string(y)) +
                                      L" --config mmaps-config.yaml --silent";
            if (!Tool(o.tools / "mmaps_generator.exe", args, o.work, "mmaps_generator.log")) return false;
            progress(0.6f + 0.35f * done / map.mmapTiles.size());
        }
    }

    // Into the server's Data: this map's files only, the ones replaced saved first (the first original wins).
    SetStatus("[" + map.directory + "] Installing into " + o.serverData.string() + "...");
    size_t installed = 0;
    auto install = [&](const char* sub, auto wanted) {
        for (const auto& entry : fs::directory_iterator(o.work / sub, ec))
        {
            const std::string name = entry.path().filename().string();
            if (!entry.is_regular_file() || !wanted(name)) continue;
            const fs::path dest = o.serverData / sub / name, saved = o.backup / sub / name;
            if (fs::exists(dest) && !fs::exists(saved))
            {
                fs::create_directories(saved.parent_path(), ec);
                fs::copy_file(dest, saved, ec);
            }
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
            if (ec) { Log("Cannot write " + dest.string() + ": " + ec.message()); return false; }
            ++installed;
        }
        return true;
    };
    const std::string prefix = id;
    auto ofMap = [&](const std::string& n) { return n.rfind(prefix, 0) == 0; };
    // vmaps: the map's tree and tiles, and the models they use. Never temp_gameobject_models: the server's lists every
    // gameobject model and this one none.
    auto vmapFile = [&](const std::string& n) { return ofMap(n) || (n.size() > 4 && Lower(n.substr(n.size() - 4)) == ".vmo"); };
    if (!install("maps", ofMap) || !install("vmaps", vmapFile) || !install("mmaps", ofMap)) return false;
    Log("[" + map.directory + "] " + std::to_string(installed) + " server files written (replaced ones saved in " + o.backup.string() + ")");
    progress(1);
    return true;
}
