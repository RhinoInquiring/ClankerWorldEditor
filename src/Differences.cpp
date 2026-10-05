#include "Differences.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string_view>

namespace
{
    double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    std::string AdtPath(const std::string& map, int key)
    {
        return "World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt";
    }

    size_t Hash(const std::vector<uint8_t>& bytes)
    {
        return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }

}

std::set<int> Differences::Region::Tiles() const
{
    std::set<int> keys;
    for (const auto& [gx, gz] : cells) keys.insert(TileKey(gx / 16, gz / 16));
    return keys;
}

void Differences::Start(const MpqChain& baseChain, const std::string& baseMap, const MpqChain& otherChain, const std::string& otherMap,
                        const std::string& otherLabel, std::vector<Change> done, std::filesystem::path file)
{
    Cancel();
    Clear();
    m_baseChain = &baseChain;
    m_otherChain = &otherChain;
    m_baseMap = baseMap;
    m_otherMap = otherMap;
    m_otherLabel = otherLabel;
    m_file = std::move(file);

    // Results and verdicts of an earlier scan of the same pair.
    std::map<int, TileResult> cache;
    if (std::ifstream f(m_file); f)
        try
        {
            const nlohmann::json j = nlohmann::json::parse(f);
            for (const auto& t : j.value("tiles", nlohmann::json::array()))
            {
                TileResult r{ t.at("key"), t.at("base"), t.at("other"), t.at("edits") };
                for (const auto& c : t.at("cells"))
                    r.cells.push_back({ c[0], c[1], uint8_t(c[2]), c[3], uint16_t(c[4]), uint16_t(c[5]), c[6] });
                cache[r.key] = std::move(r);
            }
            for (const auto& c : j.value("pasted", nlohmann::json::array())) m_status[{ c[0], c[1] }] = Status::Pasted;
            for (const auto& c : j.value("rejected", nlohmann::json::array())) m_status[{ c[0], c[1] }] = Status::Rejected;
        }
        catch (const std::exception&) {}   // an unreadable file is scanned afresh

    // Only tiles the other version has can bring anything over.
    auto tiles = [](const MpqChain& chain, const std::string& map, bool& bigAlpha) {
        const auto wdt = chain.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
        bigAlpha = wdt && WdtBigAlpha(*wdt);
        return wdt ? WdtTiles(*wdt) : std::vector<bool>{};
    };
    bool baseBig = false, otherBig = false;
    tiles(baseChain, baseMap, baseBig);
    const std::vector<bool> other = tiles(otherChain, otherMap, otherBig);
    std::vector<int> keys;
    for (size_t k = 0; k < other.size(); ++k)
        if (other[k]) keys.push_back(int(k));
    m_total = keys.size();
    m_done = m_cached = 0;
    m_stop = false;
    m_running = true;
    m_startedAt = Now();
    m_thread = std::thread([this, keys = std::move(keys), done = std::move(done), cache = std::move(cache), baseBig, otherBig]() mutable {
        Run(std::move(keys), std::move(done), std::move(cache), baseBig, otherBig);
    });
}

void Differences::Run(std::vector<int> keys, std::vector<Change> done, std::map<int, TileResult> cache, bool baseBigAlpha, bool otherBigAlpha)
{
    const std::map<int, size_t> edits = TerrainAdapter::EditHashes(done, m_baseMap);
    for (int key : keys)
    {
        if (m_stop) break;
        m_current = key;
        TileResult r{ key };
        const auto otherBytes = m_otherChain->Read(AdtPath(m_otherMap, key));
        const auto baseBytes = m_baseChain->Read(AdtPath(m_baseMap, key));
        r.otherHash = otherBytes ? Hash(*otherBytes) : 0;
        r.baseHash = baseBytes ? Hash(*baseBytes) : 0;
        r.editHash = edits.count(key) ? edits.at(key) : 0;
        if (const auto it = cache.find(key);
            it != cache.end() && it->second.baseHash == r.baseHash && it->second.otherHash == r.otherHash && it->second.editHash == r.editHash)
        {
            r.cells = it->second.cells;
            ++m_cached;
        }
        else if (otherBytes)
            if (auto otherAdt = ParseAdt(*otherBytes, otherBigAlpha))
            {
                std::map<int, LoadedTile> base, other;
                other.emplace(key, LoadedTile::Make(key % 64, key / 64, {}, std::move(*otherAdt)));
                if (baseBytes)
                    if (auto baseAdt = ParseAdt(*baseBytes, baseBigAlpha))
                    {
                        LoadedTile t = LoadedTile::Make(key % 64, key / 64, {}, std::move(*baseAdt));
                        TerrainAdapter::ReplayEdits(t, m_baseMap, done);
                        base.emplace(key, std::move(t));
                    }
                std::set<std::pair<int, int>> cells;
                for (int c = 0; c < 256; ++c) cells.insert({ (key % 64) * 16 + c % 16, (key / 64) * 16 + c / 16 });
                r.cells = CompareCells(base, other, cells);
            }
        {
            std::lock_guard lock(m_lock);
            m_finished.push_back(std::move(r));
        }
        ++m_done;
    }
    m_current = -1;
    m_running = false;
}

void Differences::Cancel()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
    if (Started())
    {
        Update();   // take what it finished
        Save();
    }
}

void Differences::Clear()
{
    m_baseMap.clear();
    m_otherMap.clear();
    m_otherLabel.clear();
    m_tiles.clear();
    m_status.clear();
    m_regions.clear();
    std::lock_guard lock(m_lock);
    m_finished.clear();
}

Differences::Progress Differences::GetProgress() const
{
    Progress p;
    p.running = m_running;
    p.done = m_done;
    p.total = m_total;
    p.cached = m_cached;
    p.tile = m_current;
    p.seconds = Started() ? Now() - m_startedAt : 0;
    return p;
}

bool Differences::Update()
{
    {
        std::lock_guard lock(m_lock);
        for (TileResult& r : m_finished)
        {
            m_tiles[r.key] = std::move(r);
            m_dirty = true;
        }
        m_finished.clear();
    }
    const bool scanning = m_running;
    if (!m_dirty || (scanning && Now() - m_lastGroup < 0.3)) return false;
    if (!scanning && m_thread.joinable()) m_thread.join();   // finished on its own
    Regroup();
    if (!scanning) Save();
    return true;
}

void Differences::Regroup()
{
    m_dirty = false;
    m_lastGroup = Now();
    // Chunks by verdict; pasted ones are done with (and their blend band would differ now by design).
    std::map<std::pair<int, int>, const CellDiff*> cells;
    for (const auto& [key, r] : m_tiles)
        for (const CellDiff& c : r.cells)
            if (auto s = m_status.find({ c.gx, c.gz }); s == m_status.end() || s->second != Status::Pasted) cells[{ c.gx, c.gz }] = &c;
    auto statusOf = [&](std::pair<int, int> cell) {
        const auto s = m_status.find(cell);
        return s == m_status.end() ? Status::Pending : s->second;
    };

    // Areas: chunks touching (sides or corners) with the same verdict.
    std::vector<Region> regions;
    std::set<std::pair<int, int>> seen;
    for (const auto& [start, first] : cells)
    {
        if (seen.count(start)) continue;
        Region r;
        r.status = statusOf(start);
        r.key = std::to_string(start.first) + "_" + std::to_string(start.second);
        r.x0 = r.x1 = start.first;
        r.z0 = r.z1 = start.second;
        std::map<uint32_t, size_t> areas;
        std::vector<std::pair<int, int>> todo{ start };
        seen.insert(start);
        while (!todo.empty())
        {
            const auto cell = todo.back();
            todo.pop_back();
            const CellDiff& c = *cells.at(cell);
            r.cells.push_back(cell);
            r.kinds |= c.kinds;
            r.maxHeight = std::max(r.maxHeight, c.maxHeight);
            r.newObjects += c.newObjects;
            r.goneObjects += c.goneObjects;
            ++areas[c.area];
            r.x0 = std::min(r.x0, cell.first); r.x1 = std::max(r.x1, cell.first);
            r.z0 = std::min(r.z0, cell.second); r.z1 = std::max(r.z1, cell.second);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const std::pair<int, int> n{ cell.first + dx, cell.second + dz };
                    if (cells.count(n) && !seen.count(n) && statusOf(n) == r.status)
                    {
                        seen.insert(n);
                        todo.push_back(n);
                    }
                }
        }
        std::sort(r.cells.begin(), r.cells.end());
        r.area = std::max_element(areas.begin(), areas.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
        if (r.cells.size() <= kMaxCells)
        {
            regions.push_back(std::move(r));
            continue;
        }
        // Too big to review as one (a reworked coast, new islands): one card per tile it covers.
        std::map<int, std::vector<std::pair<int, int>>> byTile;
        for (const auto& cell : r.cells) byTile[TileKey(cell.first / 16, cell.second / 16)].push_back(cell);
        for (auto& [key, part] : byTile)
        {
            Region t;
            t.status = r.status;
            t.cells = std::move(part);
            t.key = std::to_string(t.cells.front().first) + "_" + std::to_string(t.cells.front().second);
            t.x0 = t.x1 = t.cells.front().first;
            t.z0 = t.z1 = t.cells.front().second;
            std::map<uint32_t, size_t> tileAreas;
            for (const auto& cell : t.cells)
            {
                const CellDiff& c = *cells.at(cell);
                t.kinds |= c.kinds;
                t.maxHeight = std::max(t.maxHeight, c.maxHeight);
                t.newObjects += c.newObjects;
                t.goneObjects += c.goneObjects;
                ++tileAreas[c.area];
                t.x0 = std::min(t.x0, cell.first); t.x1 = std::max(t.x1, cell.first);
                t.z0 = std::min(t.z0, cell.second); t.z1 = std::max(t.z1, cell.second);
            }
            t.area = std::max_element(tileAreas.begin(), tileAreas.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
            regions.push_back(std::move(t));
        }
    }
    m_regions = std::move(regions);
}

const Differences::Region* Differences::Find(const std::string& key) const
{
    for (const Region& r : m_regions)
        if (r.key == key) return &r;
    return nullptr;
}

void Differences::SetStatus(const std::vector<std::pair<int, int>>& cells, Status status)
{
    for (const auto& c : cells)
        if (status == Status::Pending) m_status.erase(c);
        else m_status[c] = status;
    Regroup();
    Save();
}

void Differences::Save() const
{
    if (m_file.empty() || !Started()) return;
    nlohmann::json tiles = nlohmann::json::array(), pasted = nlohmann::json::array(), rejected = nlohmann::json::array();
    for (const auto& [key, r] : m_tiles)
    {
        nlohmann::json cells = nlohmann::json::array();
        for (const CellDiff& c : r.cells) cells.push_back({ c.gx, c.gz, c.kinds, c.maxHeight, c.newObjects, c.goneObjects, c.area });
        tiles.push_back({ { "key", key }, { "base", r.baseHash }, { "other", r.otherHash }, { "edits", r.editHash }, { "cells", std::move(cells) } });
    }
    for (const auto& [cell, s] : m_status) (s == Status::Pasted ? pasted : rejected).push_back({ cell.first, cell.second });
    const nlohmann::json j = { { "base", m_baseMap }, { "other", m_otherMap }, { "label", m_otherLabel }, { "tiles", std::move(tiles) },
                               { "pasted", std::move(pasted) }, { "rejected", std::move(rejected) } };
    std::error_code ec;
    std::filesystem::create_directories(m_file.parent_path(), ec);
    std::ofstream(m_file) << j.dump();
}
