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
            const bool away = l.only.empty() ? std::abs(it->second.x - cx) > radius + 1 || std::abs(it->second.y - cz) > radius + 1
                                             : !l.only.count(it->first);
            if (away)
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
    {
        for (int key : l.only)
            if (!l.tiles.count(key) && !l.missing.count(key))
                wanted.push_back({ (key % 64 - cx) * (key % 64 - cx) + (key / 64 - cz) * (key / 64 - cz), &l, key % 64, key / 64 });
        if (!l.only.empty()) continue;
        for (int ty = cz - radius; ty <= cz + radius; ++ty)
            for (int tx = cx - radius; tx <= cx + radius; ++tx)
            {
                if (tx < 0 || tx > 63 || ty < 0 || ty > 63) continue;
                const int key = TileKey(tx, ty);
                if (l.tiles.count(key) || l.missing.count(key)) continue;
                wanted.push_back({ (tx - cx) * (tx - cx) + (ty - cz) * (ty - cz), &l, tx, ty });
            }
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

namespace
{
    std::pair<const LoadedTile*, const AdtChunk*> ChunkAt(const std::map<int, LoadedTile>& tiles, int gx, int gz)
    {
        if (gx < 0 || gz < 0) return { nullptr, nullptr };
        const auto it = tiles.find(TileKey(gx / 16, gz / 16));
        if (it == tiles.end()) return { nullptr, nullptr };
        const int16_t i = it->second.byGrid[size_t((gz % 16) * 16 + gx % 16)];
        return { &it->second, i < 0 ? nullptr : &it->second.adt.chunks[size_t(i)] };
    }

    std::vector<std::string> TextureNames(const LoadedTile& t, const AdtChunk& c)
    {
        std::vector<std::string> names;
        for (uint32_t l = 0; l < c.layerCount && l < 4; ++l)
        {
            std::string n = c.textureIds[l] < t.adt.textures.size() ? t.adt.textures[c.textureIds[l]] : std::string();
            for (char& ch : n) ch = ch == '/' ? '\\' : char(std::tolower((unsigned char)ch));
            names.push_back(std::move(n));
        }
        return names;
    }

    /// Water as seen: the fishable / fatigue masks are left out (map editors rewrite them on every save).
    nlohmann::json SeenWater(const LoadedTile& t, const AdtChunk& c)
    {
        nlohmann::json w = TerrainAdapter::LiquidState(t.adt, c);
        for (auto& l : w) { l.erase("fishable"); l.erase("deep"); }
        return w;
    }

    std::string ObjectKey(std::string model, const float pos[3])
    {
        for (char& ch : model) ch = ch == '/' ? '\\' : char(std::tolower((unsigned char)ch));
        if (model.size() > 4 && (model.ends_with(".mdx") || model.ends_with(".mdl"))) model.replace(model.size() - 4, 4, ".m2");
        return model + "|" + std::to_string(std::lround(pos[0])) + "|" + std::to_string(std::lround(pos[1])) + "|" + std::to_string(std::lround(pos[2]));
    }

    std::pair<int, int> CellOf(const float pos[3]) { return { int(std::floor(pos[0] / kChunkSize)), int(std::floor(pos[2] / kChunkSize)) }; }

    /// Whether a building's bounds (x/z) reach any of `cells`.
    bool WmoReaches(const WmoPlacement& w, const std::set<std::pair<int, int>>& cells)
    {
        const int x0 = int(std::floor(w.extMin[0] / kChunkSize)), x1 = int(std::floor(w.extMax[0] / kChunkSize));
        const int z0 = int(std::floor(w.extMin[2] / kChunkSize)), z1 = int(std::floor(w.extMax[2] / kChunkSize));
        if (x1 < x0 || z1 < z0 || int64_t(x1 - x0 + 1) * (z1 - z0 + 1) > 1024 * 1024) return false;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                if (cells.count({ x, z })) return true;
        return false;
    }

    /// Objects on `cells`, once each (an object is listed by every tile it touches), keyed by model and rounded position.
    /// An object belongs where its origin stands; with `wmoBounds` a building also belongs wherever its bounds reach
    /// (a cave whose origin lies off to one side still runs through the area).
    struct Objects { std::map<std::string, DoodadPlacement> doodads; std::map<std::string, WmoPlacement> wmos; };
    Objects GatherObjects(const std::map<int, LoadedTile>& tiles, const std::set<std::pair<int, int>>& cells, bool wmoBounds = false)
    {
        std::set<int> keys;
        for (const auto& [gx, gz] : cells) keys.insert(TileKey(gx / 16, gz / 16));
        Objects o;
        std::set<uint32_t> seenD, seenW;
        for (int key : keys)
        {
            const auto it = tiles.find(key);
            if (it == tiles.end()) continue;
            for (const DoodadPlacement& p : it->second.adt.doodads)
                if (cells.count(CellOf(p.pos)) && seenD.insert(p.uniqueId).second) o.doodads.emplace(ObjectKey(p.model, p.pos), p);
            for (const WmoPlacement& p : it->second.adt.wmos)
                if ((cells.count(CellOf(p.pos)) || (wmoBounds && WmoReaches(p, cells))) && seenW.insert(p.uniqueId).second)
                    o.wmos.emplace(ObjectKey(p.model, p.pos), p);
        }
        return o;
    }
}

AreaDiff CompareArea(const std::map<int, LoadedTile>& map, const std::map<int, LoadedTile>& version, const std::set<std::pair<int, int>>& cells)
{
    AreaDiff d;
    double sum = 0, edgeSum = 0;
    size_t count = 0, edgeCount = 0;
    for (const auto& [gx, gz] : cells)
    {
        const auto [mt, a] = ChunkAt(map, gx, gz);
        const auto [vt, b] = ChunkAt(version, gx, gz);
        if (!a || !b) continue;
        ++d.cells;
        float worst = 0;
        for (size_t k = 0; k < 145; ++k)
        {
            const float diff = std::fabs((b->baseY + b->heights[k]) - (a->baseY + a->heights[k]));
            sum += diff;
            worst = std::max(worst, diff);
        }
        count += 145;
        d.maxHeight = std::max(d.maxHeight, worst);
        const bool wet = SeenWater(*mt, *a) != SeenWater(*vt, *b);
        d.water += wet;
        if (worst > 0.5f || wet || a->holes != b->holes || TextureNames(*mt, *a) != TextureNames(*vt, *b)) ++d.changed;
        // Outer edge: the side's 9 outer vertices where the neighbour is not compared (x sides are columns 8/0, z sides rows 8/0).
        const struct { int dx, dz; } sides[] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (const auto& s : sides)
        {
            if (cells.count({ gx + s.dx, gz + s.dz })) continue;
            for (int k = 0; k <= 8; ++k)
            {
                const int row = s.dz ? (s.dz > 0 ? 8 : 0) : k, col = s.dx ? (s.dx > 0 ? 8 : 0) : k;
                const size_t j = size_t(row * 17 + col);
                const float diff = std::fabs((b->baseY + b->heights[j]) - (a->baseY + a->heights[j]));
                edgeSum += diff;
                ++edgeCount;
                d.maxEdge = std::max(d.maxEdge, diff);
            }
        }
    }
    if (count) d.meanHeight = float(sum / count);
    if (edgeCount) d.meanEdge = float(edgeSum / edgeCount);

    const Objects mine = GatherObjects(map, cells, true), theirs = GatherObjects(version, cells, true);
    for (const auto& [k, p] : theirs.doodads)
        if (!mine.doodads.count(k)) d.newDoodads.push_back(p);
    for (const auto& [k, p] : theirs.wmos)
        if (!mine.wmos.count(k)) d.newWmos.push_back(p);
    for (const auto& [k, p] : mine.doodads) d.goneDoodads += !theirs.doodads.count(k);
    for (const auto& [k, p] : mine.wmos) d.goneWmos += !theirs.wmos.count(k);
    return d;
}

std::vector<CellDiff> CompareCells(const std::map<int, LoadedTile>& map, const std::map<int, LoadedTile>& version,
                                   const std::set<std::pair<int, int>>& cells)
{
    std::map<std::pair<int, int>, CellDiff> out;
    for (const auto& [gx, gz] : cells)
    {
        const auto [mt, a] = ChunkAt(map, gx, gz);
        const auto [vt, b] = ChunkAt(version, gx, gz);
        if (!b) continue;   // nothing the version could give (a chunk only the map has stays as it is)
        CellDiff c{ gx, gz };
        c.area = a ? a->areaId : b->areaId;
        if (!a)
            c.kinds = CellDiff::NewTerrain;
        else
        {
            for (size_t k = 0; k < 145; ++k) c.maxHeight = std::max(c.maxHeight, std::fabs((b->baseY + b->heights[k]) - (a->baseY + a->heights[k])));
            if (c.maxHeight > 0.5f) c.kinds |= CellDiff::Heights;
            if (TextureNames(*mt, *a) != TextureNames(*vt, *b)) c.kinds |= CellDiff::Textures;
            if (a->holes != b->holes) c.kinds |= CellDiff::Holes;
            if (SeenWater(*mt, *a) != SeenWater(*vt, *b)) c.kinds |= CellDiff::Water;
        }
        out[{ gx, gz }] = c;
    }
    // Objects: new ones count on the chunk they stand on in the version, gone ones where the map has them.
    const Objects mine = GatherObjects(map, cells), theirs = GatherObjects(version, cells);
    auto at = [&](const float pos[3]) -> CellDiff& {
        const auto [gx, gz] = CellOf(pos);
        auto [it, added] = out.try_emplace({ gx, gz }, CellDiff{ gx, gz });
        if (added)
            if (const auto [t, c] = ChunkAt(map, gx, gz); c) it->second.area = c->areaId;
        return it->second;
    };
    for (const auto& [k, p] : theirs.doodads)
        if (!mine.doodads.count(k)) { CellDiff& c = at(p.pos); ++c.newObjects; c.kinds |= CellDiff::Objects; }
    for (const auto& [k, p] : theirs.wmos)
        if (!mine.wmos.count(k)) { CellDiff& c = at(p.pos); ++c.newObjects; c.kinds |= CellDiff::Objects; }
    for (const auto& [k, p] : mine.doodads)
        if (!theirs.doodads.count(k)) ++at(p.pos).goneObjects;
    for (const auto& [k, p] : mine.wmos)
        if (!theirs.wmos.count(k)) ++at(p.pos).goneObjects;
    std::vector<CellDiff> list;
    for (const auto& [cell, c] : out)
        if (c.kinds) list.push_back(c);   // only removed objects: a paste cannot take them away, so not an edit to bring over
    return list;
}
