#pragma once

#include "Mpq.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

/// One map the project exports, for the server's data: the tiles whose navigation mesh is rebuilt (the edited ones and
/// their neighbours; empty: every tile of the map).
struct ServerMap
{
    std::string directory;
    uint32_t id = 0;
    std::set<std::pair<int, int>> mmapTiles;
};

/// Builds the server's maps, vmaps and mmaps for the maps the project exports, with AzerothCore's own tools, in the
/// background. Each map gets a staging client holding only that map (its exported tiles, every model they place and the
/// few DBCs the tools read), so the extractors see nothing else; then only that map's files go into the server's Data,
/// the ones they replace saved first.
class ServerDataJob
{
public:
    struct Options
    {
        std::filesystem::path tools;       // folder with map_extractor.exe, vmap4_extractor.exe, vmap4_assembler.exe, mmaps_generator.exe
        std::filesystem::path serverData;  // the server's Data (maps, vmaps, mmaps)
        std::filesystem::path work;        // scratch folder (kept short: map_extractor cuts paths at 127 characters)
        std::filesystem::path exported;    // the project's export (out/client): its files win over the sources
        std::filesystem::path backup;      // replaced server files go here, under maps / vmaps / mmaps
        std::vector<MpqLayer> layers;      // the project's sources, as the editor reads them
        std::vector<std::filesystem::path> skip;   // archives left out of them (the project's installed patch)
    };

    ~ServerDataJob() { Cancel(); }
    void Start(Options options, std::vector<ServerMap> maps);
    bool Running() const { return m_running; }
    void Cancel();
    /// What the job is doing now, how far it is (0..1), and whether the last run finished without errors.
    std::string Status() const;
    float Progress() const { return m_progress; }
    bool Succeeded() const { return m_succeeded; }
    /// Lines logged since the last call.
    std::vector<std::string> TakeLog();

private:
    void Run(Options options, std::vector<ServerMap> maps);
    bool BuildMap(const Options& o, const ServerMap& map, MpqChain& chain, float from, float to);
    /// Runs a tool in `cwd` (its output to <cwd>\<log>), waiting; false with the reason when it fails or is cancelled.
    bool Tool(const std::filesystem::path& exe, const std::wstring& args, const std::filesystem::path& cwd, const std::string& log);
    void Log(const std::string& line);
    void SetStatus(const std::string& status);

    std::thread m_thread;
    std::atomic<bool> m_running{ false }, m_cancel{ false }, m_succeeded{ false };
    std::atomic<float> m_progress{ 0 };
    mutable std::mutex m_mutex;
    std::string m_status;
    std::vector<std::string> m_log;
    void* m_process = nullptr;   // the tool running now (HANDLE), for Cancel
};
