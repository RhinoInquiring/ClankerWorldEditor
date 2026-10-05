#pragma once

#include "Ghosts.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class MpqChain;

/// Every place where another version of a map differs from the project's map: found tile by tile on a worker thread,
/// then grouped into areas of touching chunks for review. Per-tile results are kept in the project (reused while
/// neither tile file nor the tile's project edits change), with the user's verdict per chunk (pasted, rejected).
class Differences
{
public:
    enum class Status { Pending, Pasted, Rejected };

    struct Region
    {
        std::string key;                           // its first chunk "gx_gz": stable while the area keeps that chunk
        std::vector<std::pair<int, int>> cells;    // global chunk cells
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;        // cell bounds, inclusive
        uint8_t kinds = 0;                         // CellDiff::Kind bits
        float maxHeight = 0;
        size_t newObjects = 0, goneObjects = 0;
        uint32_t area = 0;                         // the most common area id
        Status status = Status::Pending;
        std::set<int> Tiles() const;
    };

    struct Progress
    {
        bool running = false;
        size_t done = 0, total = 0, cached = 0;    // tiles
        int tile = -1;                             // tile key in work
        double seconds = 0;                        // since the scan started
    };

    Differences() = default;
    Differences(const Differences&) = delete;
    Differences& operator=(const Differences&) = delete;
    ~Differences() { Cancel(); }

    /// Scans `otherMap` (read through `otherChain`) against `baseMap` as the project has it: `baseChain` plus the
    /// project's applied changes `done`. `file` holds the per-tile results and the verdicts (read now, written as
    /// the scan ends and on every verdict). Both chains must outlive the scan (Cancel before closing one).
    void Start(const MpqChain& baseChain, const std::string& baseMap, const MpqChain& otherChain, const std::string& otherMap,
               const std::string& otherLabel, std::vector<Change> done, std::filesystem::path file);
    /// Stops and joins the worker; what it found so far stays (and is saved).
    void Cancel();
    /// Forgets the scan and its results (the file stays).
    void Clear();

    bool Started() const { return !m_baseMap.empty(); }
    Progress GetProgress() const;
    const std::string& BaseMap() const { return m_baseMap; }
    const std::string& OtherMap() const { return m_otherMap; }
    const std::string& OtherLabel() const { return m_otherLabel; }

    /// Takes the tiles the worker finished and regroups (at most every 0.3 s while scanning); true when Regions changed.
    bool Update();
    const std::vector<Region>& Regions() const { return m_regions; }
    const Region* Find(const std::string& key) const;

    /// Records a verdict for chunks (Pending forgets one) and saves.
    void SetStatus(const std::vector<std::pair<int, int>>& cells, Status status);

private:
    static constexpr size_t kMaxCells = 256;   // a larger area is split into one card per tile
    struct TileResult { int key = 0; size_t baseHash = 0, otherHash = 0, editHash = 0; std::vector<CellDiff> cells; };

    void Run(std::vector<int> keys, std::vector<Change> done, std::map<int, TileResult> cache, bool baseBigAlpha, bool otherBigAlpha);
    void Regroup();
    void Save() const;

    const MpqChain* m_baseChain = nullptr;
    const MpqChain* m_otherChain = nullptr;
    std::string m_baseMap, m_otherMap, m_otherLabel;
    std::filesystem::path m_file;

    std::thread m_thread;
    std::atomic<bool> m_stop{ false }, m_running{ false };
    std::atomic<size_t> m_done{ 0 }, m_total{ 0 }, m_cached{ 0 };
    std::atomic<int> m_current{ -1 };
    double m_startedAt = 0;   // steady clock seconds

    mutable std::mutex m_lock;
    std::vector<TileResult> m_finished;    // from the worker, not yet taken (under m_lock)

    std::map<int, TileResult> m_tiles;     // every tile's result so far (main thread)
    std::map<std::pair<int, int>, Status> m_status;
    std::vector<Region> m_regions;
    bool m_dirty = false;
    double m_lastGroup = 0;
};
