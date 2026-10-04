#include "Ghosts.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <filesystem>
#include <string_view>

namespace fs = std::filesystem;

namespace
{
    std::string AdtPath(const std::string& map, int x, int y)
    {
        return "World\\Maps\\" + map + "\\" + map + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt";
    }

    bool BigAlpha(const MpqChain& mpq, const std::string& map)
    {
        const auto wdt = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
        return wdt && WdtBigAlpha(*wdt);
    }

    const DirectX::XMFLOAT4 kPalette[] = { { 1.0f, 0.55f, 0.15f, 0.5f }, { 0.95f, 0.3f, 0.85f, 0.5f }, { 0.3f, 0.95f, 0.4f, 0.5f },
                                           { 1.0f, 0.95f, 0.25f, 0.5f }, { 0.25f, 0.9f, 1.0f, 0.5f } };
}

// The background reader. Jobs carry everything they need (never the layers or sources themselves); results are
// matched back to their layer by id and generation.
struct Ghosts::Worker
{
    struct Job { int layer = 0; int key = 0; uint64_t generation = 0; const MpqChain* chain = nullptr; int archive = -1; std::string path; bool bigAlpha = false; };
    struct Done { Job job; std::vector<uint8_t> bytes; std::optional<Adt> adt; };

    std::thread thread;
    std::mutex lock;
    std::condition_variable wake;
    bool stop = false;
    std::vector<Job> wanted;                  // most wanted first
    std::optional<std::pair<int, int>> current;   // (layer, key) in work
    std::vector<Done> done;
    std::set<std::string> seen;               // texture names decoded once already
    std::vector<std::pair<std::string, BlpImage>> images;

    Worker() { thread = std::thread([this] { Run(); }); }
    ~Worker()
    {
        { std::lock_guard l(lock); stop = true; }
        wake.notify_all();
        thread.join();
    }

    /// Queued, in work or finished but not handed over (call with the lock held).
    bool Busy(int layer, int key) const
    {
        if (current == std::pair{ layer, key }) return true;
        return std::any_of(done.begin(), done.end(), [&](const Done& d) { return d.job.layer == layer && d.job.key == key; });
    }

    void Run()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock l(lock);
                current.reset();
                wake.wait(l, [&] { return stop || !wanted.empty(); });
                if (stop) return;
                job = std::move(wanted.front());
                wanted.erase(wanted.begin());
                current = std::pair{ job.layer, job.key };
            }
            Done d{ job };
            if (auto bytes = job.archive < 0 ? job.chain->Read(job.path) : job.chain->ReadFrom(size_t(job.archive), job.path))
            {
                d.adt = ParseAdt(*bytes, job.bigAlpha);
                d.bytes = std::move(*bytes);
            }
            std::vector<std::pair<std::string, BlpImage>> decoded;
            if (d.adt)
                for (const std::string& t : d.adt->textures)
                {
                    std::string key = t;
                    for (char& c : key) c = char(std::tolower((unsigned char)c));
                    {
                        std::lock_guard l(lock);
                        if (!seen.insert(key).second) continue;
                    }
                    if (auto b = job.chain->Read(t))
                        if (auto image = ParseBlp(*b)) decoded.push_back({ t, std::move(*image) });
                }
            std::lock_guard l(lock);
            if (stop) return;
            for (auto& i : decoded) images.push_back(std::move(i));
            done.push_back(std::move(d));
        }
    }
};

Ghosts::Ghosts() = default;
Ghosts::~Ghosts() = default;
Ghosts::Ghosts(Ghosts&&) noexcept = default;
Ghosts& Ghosts::operator=(Ghosts&&) noexcept = default;

void Ghosts::StartWorker()
{
    if (!m_worker) m_worker = std::make_unique<Worker>();
}

void Ghosts::StopWorker()
{
    m_worker.reset();   // joins the thread
}

std::vector<std::pair<std::string, BlpImage>> Ghosts::TakeImages()
{
    if (!m_worker) return {};
    std::lock_guard l(m_worker->lock);
    return std::exchange(m_worker->images, {});
}

void Ghosts::Reset(const MpqChain* project, const std::string& projectName, const std::vector<std::pair<std::string, std::string>>& attached,
                   std::vector<std::string>& errors)
{
    const bool worker = m_worker != nullptr;
    StopWorker();   // it may be reading a chain about to close
    m_layers.clear();
    m_sources.clear();
    m_bigAlpha.clear();
    ++m_generation;
    m_nextLayerId = 1;
    Source own;
    own.name = projectName;
    own.mpq = project;
    m_sources.push_back(std::move(own));
    for (const auto& [name, dir] : attached)
    {
        std::string error;
        if (!AddSource(name, dir, error)) errors.push_back(error);
    }
    if (worker) StartWorker();
}

bool Ghosts::AddSource(const std::string& name, const std::string& dataDir, std::string& error)
{
    // A client folder or its Data folder.
    fs::path dir = dataDir;
    if (fs::is_directory(dir / "Data")) dir /= "Data";
    auto chain = std::make_unique<MpqChain>();
    if (!chain->Open(dir.string())) { error = "No MPQ archives in " + dir.string(); return false; }
    Source s;
    s.name = name.empty() ? fs::path(dataDir).filename().string() : name;
    s.dataDir = dataDir;
    s.mpq = chain.get();
    s.owned = std::move(chain);
    m_sources.push_back(std::move(s));
    return true;
}

void Ghosts::RemoveSource(size_t index)
{
    if (index == 0 || index >= m_sources.size()) return;   // the project's client stays
    const bool worker = m_worker != nullptr;
    StopWorker();   // it may be reading the chain about to close
    m_bigAlpha.clear();
    ++m_generation;
    std::erase_if(m_layers, [&](const Layer& l) { return l.source == index; });
    for (Layer& l : m_layers)
        if (l.source > index) --l.source;
    m_sources.erase(m_sources.begin() + std::ptrdiff_t(index));
    if (worker) StartWorker();
}

std::optional<std::vector<uint8_t>> Ghosts::ReadVersion(size_t source, int archive, const std::string& path) const
{
    const MpqChain& mpq = Chain(source);
    return archive < 0 ? mpq.Read(path) : mpq.ReadFrom(size_t(archive), path);
}

std::vector<Ghosts::Version> Ghosts::Versions(const std::string& map, int tx, int ty, const LoadedTile* main) const
{
    std::vector<Version> out;
    const std::string path = AdtPath(map, tx, ty);
    for (size_t s = 0; s < m_sources.size(); ++s)
    {
        const MpqChain& mpq = *m_sources[s].mpq;
        const bool bigAlpha = BigAlpha(mpq, map);
        bool first = true;
        for (size_t a = 0; a < mpq.Names().size(); ++a)
        {
            if (!mpq.Has(a, path)) continue;
            const auto bytes = mpq.ReadFrom(a, path);
            if (!bytes) continue;
            const size_t hash = std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            const bool inUse = s == 0 && first;   // highest-priority archive of the project's client
            first = false;
            auto same = std::find_if(out.begin(), out.end(), [&](const Version& v) { return v.hash == hash && v.bytes == bytes->size(); });
            if (same != out.end())
            {
                same->alsoIn.push_back(m_sources[s].name + " / " + mpq.Names()[a]);
                same->inUse = same->inUse || inUse;
                continue;
            }
            Version v;
            v.source = s;
            v.archive = int(a);
            v.label = m_sources[s].name + " / " + mpq.Names()[a];
            v.hash = hash;
            v.bytes = bytes->size();
            v.inUse = inUse;
            if (const auto adt = ParseAdt(*bytes, bigAlpha))
            {
                v.doodads = adt->doodads.size();
                v.wmos = adt->wmos.size();
                if (main)
                {
                    // Mean height difference over the chunks both have, matched by grid cell.
                    const LoadedTile other = LoadedTile::Make(tx, ty, {}, *adt);
                    double sum = 0;
                    size_t count = 0;
                    for (size_t cell = 0; cell < 256; ++cell)
                    {
                        const int16_t i = main->byGrid[cell], j = other.byGrid[cell];
                        if (i < 0 || j < 0) continue;
                        const AdtChunk& a0 = main->adt.chunks[size_t(i)];
                        const AdtChunk& b0 = other.adt.chunks[size_t(j)];
                        for (size_t k = 0; k < 145; ++k) sum += std::fabs((a0.baseY + a0.heights[k]) - (b0.baseY + b0.heights[k]));
                        count += 145;
                    }
                    if (count) v.heightDiff = float(sum / count);
                }
            }
            out.push_back(std::move(v));
        }
    }
    return out;
}

Ghosts::Layer& Ghosts::AddLayer(size_t source, int archive, const std::string& label, const std::string& map)
{
    Layer l;
    l.map = map;
    l.id = m_nextLayerId++;
    l.source = source;
    l.archive = archive;
    l.label = label;
    l.tint = kPalette[size_t(l.id - 1) % std::size(kPalette)];
    m_layers.push_back(std::move(l));
    return m_layers.back();
}

void Ghosts::RemoveLayer(int id)
{
    std::erase_if(m_layers, [&](const Layer& l) { return l.id == id; });
}

Ghosts::Layer* Ghosts::Find(int id)
{
    for (Layer& l : m_layers)
        if (l.id == id) return &l;
    return nullptr;
}

Ghosts::Layer* Ghosts::Find(size_t source, int archive, const std::string& map)
{
    for (Layer& l : m_layers)
        if (l.source == source && l.archive == archive && l.map == map) return &l;
    return nullptr;
}

void Ghosts::ClearTiles()
{
    for (Layer& l : m_layers)
    {
        l.tiles.clear();
        l.missing.clear();
    }
    ++m_generation;
}

Ghosts::StreamResult Ghosts::Stream(const std::string& map, float x, float z, int radius, size_t maxTiles)
{
    StreamResult result;
    if (map.empty()) return result;
    const int cx = int(std::floor(x / kTileSize)), cz = int(std::floor(z / kTileSize));
    for (Layer& l : m_layers)
        for (auto it = l.tiles.begin(); it != l.tiles.end();)
        {
            if (std::abs(it->second.x - cx) > radius + 1 || std::abs(it->second.y - cz) > radius + 1)
            {
                result.unloaded.push_back({ l.id, it->first });
                it = l.tiles.erase(it);
            }
            else
                ++it;
        }

    // Missing tiles over every layer, nearest first.
    struct Want { int dist; Layer* layer; int x, y; };
    std::vector<Want> wanted;
    for (Layer& l : m_layers)
        for (int ty = cz - radius; ty <= cz + radius; ++ty)
            for (int tx = cx - radius; tx <= cx + radius; ++tx)
            {
                if (tx < 0 || tx > 63 || ty < 0 || ty > 63) continue;
                const int key = TileKey(tx, ty);
                if (l.tiles.count(key) || l.missing.count(key)) continue;
                wanted.push_back({ (tx - cx) * (tx - cx) + (ty - cz) * (ty - cz), &l, tx, ty });
            }
    std::stable_sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.dist < b.dist; });
    auto bigAlpha = [&](const Layer& l, const std::string& shown) {
        auto [it, added] = m_bigAlpha.try_emplace({ l.source, shown });
        if (added) it->second = BigAlpha(Chain(l.source), shown);
        return it->second;
    };

    if (!m_worker)
    {
        // Reading and parsing an ADT takes a few ms: one per call.
        if (wanted.empty()) return result;
        Layer& l = *wanted.front().layer;
        const int tx = wanted.front().x, ty = wanted.front().y, key = TileKey(tx, ty);
        const std::string& shown = l.map.empty() ? map : l.map;   // the copies share coordinates
        auto bytes = ReadVersion(l.source, l.archive, AdtPath(shown, tx, ty));
        auto adt = bytes ? ParseAdt(*bytes, bigAlpha(l, shown)) : std::nullopt;
        if (!adt) { l.missing.insert(key); return result; }
        l.tiles.emplace(key, LoadedTile::Make(tx, ty, std::move(*bytes), std::move(*adt)));
        result.loaded.push_back({ l.id, key });
        return result;
    }

    std::vector<Worker::Done> finished;
    {
        std::lock_guard lock(m_worker->lock);
        m_worker->wanted.clear();   // the wish list follows the camera; finished work is kept
        for (const Want& w : wanted)
        {
            const int key = TileKey(w.x, w.y);
            if (m_worker->Busy(w.layer->id, key)) continue;
            const std::string& shown = w.layer->map.empty() ? map : w.layer->map;
            m_worker->wanted.push_back({ w.layer->id, key, m_generation, &Chain(w.layer->source), w.layer->archive, AdtPath(shown, w.x, w.y),
                                         bigAlpha(*w.layer, shown) });
        }
        const size_t take = std::min(maxTiles, m_worker->done.size());
        for (size_t i = 0; i < take; ++i) finished.push_back(std::move(m_worker->done[i]));
        m_worker->done.erase(m_worker->done.begin(), m_worker->done.begin() + std::ptrdiff_t(take));
    }
    m_worker->wake.notify_one();
    for (auto& d : finished)
    {
        Layer* l = Find(d.job.layer);
        if (!l || d.job.generation != m_generation || l->tiles.count(d.job.key)) continue;   // stale: layer gone or map changed
        if (!d.adt) { l->missing.insert(d.job.key); continue; }
        l->tiles.emplace(d.job.key, LoadedTile::Make(d.job.key % 64, d.job.key / 64, std::move(d.bytes), std::move(*d.adt)));
        result.loaded.push_back({ l->id, d.job.key });
    }
    return result;
}
