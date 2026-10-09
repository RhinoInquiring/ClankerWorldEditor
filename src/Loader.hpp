#pragma once

#include "Formats.hpp"
#include "Models.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class MpqChain;

/// Prepares streamed tiles on worker threads: reads and parses the ADT, decodes its terrain textures and builds
/// the meshes (and decodes the textures) of the models standing on it. The UI thread then only uploads to the GPU,
/// so flying over the map does not stall a frame per tile. The renderers take prepared images and meshes by name.
class Loader
{
public:
    struct Tile
    {
        std::string map;
        int key = 0;
        std::vector<uint8_t> bytes;
        std::optional<Adt> adt;   // none: the file is missing or does not parse
    };

    Loader() = default;
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;
    ~Loader() { Stop(); }

    /// `threads`: workers; 0 picks half the cores, 1 to 4 (the window, ghosts and differences keep the rest).
    void Start(const MpqChain* mpq, unsigned threads = 0);
    /// Stops the workers and drops everything prepared; call before the MPQs close.
    void Stop();

    /// The tiles of `map` to prepare, most wanted first; replaces the previous list. Tiles ready or in work stay.
    void Want(const std::string& map, bool bigAlpha, std::vector<int> keys);
    /// A tile the worker finished, everything it needs prepared, or none.
    std::optional<Tile> TakeTile();
    /// Models (name, is WMO) to prepare after the wanted tiles, for objects about to be shown that no streamed tile
    /// brings (another version's, compared in place). Names prepared before are skipped.
    void WantModels(std::vector<std::pair<std::string, bool>> models);
    /// Prepared assets by lower-case name, handed out once (none: not prepared, load it yourself).
    std::optional<BlpImage> TakeImage(const std::string& lowerName);
    std::optional<ModelMesh> TakeMesh(const std::string& lowerName);

    size_t Pending() const { return m_pending.load(); }

private:
    void Run();
    void PrepareImage(const std::string& name);
    void PrepareModel(const std::string& name, bool wmo);
    /// True when this thread should prepare `key` now; false when it was prepared before. One another worker is still
    /// preparing is waited for, so a tile is only ready once all it needs is (no deadlock: a worker waits only while
    /// it holds a WMO or an M2, and only on an M2 or an image, which never wait on a WMO).
    bool Claim(const std::string& key);
    void Finish(const std::string& key);

    const MpqChain* m_mpq = nullptr;
    Dbc m_liquidTypes;   // read in Start: which textures a tile's water animates through
    std::vector<std::thread> m_threads;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::condition_variable m_finished;   // an asset of m_preparing is done
    bool m_stop = false;

    std::string m_map;
    bool m_bigAlpha = false;
    std::vector<int> m_wanted;
    std::vector<std::pair<std::string, bool>> m_wantedModels;
    std::set<int> m_working;                 // keys being prepared or ready (not wanted again until taken)
    std::vector<Tile> m_ready;
    std::set<std::string> m_seen;            // asset names already prepared once (the renderers cache them)
    std::set<std::string> m_preparing;       // of m_seen: still being prepared by a worker
    std::map<std::string, BlpImage> m_images;
    std::map<std::string, ModelMesh> m_meshes;
    std::atomic<size_t> m_pending{ 0 };
};
