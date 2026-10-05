#pragma once

#include "Terrain.hpp"

#include <DirectXMath.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class MpqChain;

/// Other versions of the open map, shown as read-only ghost layers over it.
///
/// A source is a client's Data folder (source 0 is the project's own client). A version of a tile is one
/// archive's copy of its ADT, so the patch history of a tile shows up as separate versions; identical copies
/// are merged into one row. A layer shows one version of the map: either one archive's tiles only, or a
/// whole source resolved like the client would (archive -1).
class Ghosts
{
public:
    struct Source
    {
        std::string name, dataDir;
        std::unique_ptr<MpqChain> owned;   // attached sources own their chain; the project's is borrowed
        const MpqChain* mpq = nullptr;
    };

    struct Version
    {
        size_t source = 0;
        int archive = -1;                    // index into that source's archives
        std::string label;                   // "Source / archive.MPQ"
        std::vector<std::string> alsoIn;     // other archives holding identical bytes
        size_t hash = 0, bytes = 0;
        std::optional<float> heightDiff;     // mean |difference| from the map as edited, yards
        size_t doodads = 0, wmos = 0;
        bool inUse = false;                  // what the project's client loads for this tile
    };

    struct Layer
    {
        int id = 0;                          // renderer layer number (1, 2, ...)
        size_t source = 0;
        int archive = -1;                    // -1: the whole source as the client resolves it
        std::string map;                     // another map's folder shown over the open one; empty = the open map
        std::string label;
        DirectX::XMFLOAT4 tint{ 1, 0.6f, 0.2f, 0.5f };
        bool visible = true;
        std::map<int, LoadedTile> tiles;     // by tile key
        std::set<int> missing;               // tiles this version does not have
        std::set<int> only;                  // keep exactly these tiles loaded wherever the camera is; empty = around the camera
    };

    struct StreamResult
    {
        std::vector<std::pair<int, int>> loaded, unloaded;   // (layer id, tile key)
    };

    /// Starts over with the project's client as source 0 and the attached sources (name, data dir).
    void Reset(const MpqChain* project, const std::string& projectName, const std::vector<std::pair<std::string, std::string>>& attached,
               std::vector<std::string>& errors);
    bool AddSource(const std::string& name, const std::string& dataDir, std::string& error);
    void RemoveSource(size_t index);
    const std::vector<Source>& Sources() const { return m_sources; }
    const MpqChain& Chain(size_t source) const { return *m_sources.at(source).mpq; }

    /// Every version of tile (tx, ty) of `map` across the sources and their archives.
    std::vector<Version> Versions(const std::string& map, int tx, int ty, const LoadedTile* main) const;

    Layer& AddLayer(size_t source, int archive, const std::string& label, const std::string& map = {});
    void RemoveLayer(int id);
    std::vector<Layer>& Layers() { return m_layers; }
    Layer* Find(int id);
    Layer* Find(size_t source, int archive, const std::string& map = {});

    /// Drops every layer's tiles (the map changed); the layers stay.
    void ClearTiles();
    /// Loads missing tiles (any layer) near (x, z) and drops tiles beyond the radius. Without a worker: one tile
    /// per call, read here. With StartWorker: tiles are read, parsed and their textures decoded on a worker
    /// thread; this hands over at most `maxTiles` finished ones per call.
    StreamResult Stream(const std::string& map, float x, float z, int radius, size_t maxTiles = 2);
    void StartWorker();
    void StopWorker();
    /// Textures the worker decoded for the tiles Stream returned: give them to the renderer before uploading.
    std::vector<std::pair<std::string, BlpImage>> TakeImages();

    Ghosts();
    ~Ghosts();
    Ghosts(Ghosts&&) noexcept;
    Ghosts& operator=(Ghosts&&) noexcept;

    /// Renderer and model renderer keys for a layer's tile (negative, clear of the previews' -1 and -2).
    static int Key(int layer, int tileKey) { return -(16 + layer * 4096 + tileKey); }

private:
    std::optional<std::vector<uint8_t>> ReadVersion(size_t source, int archive, const std::string& path) const;

    std::vector<Source> m_sources;
    std::vector<Layer> m_layers;
    int m_nextLayerId = 1;
    uint64_t m_generation = 0;   // bumps when tiles are dropped wholesale; older worker results are stale
    std::map<std::pair<size_t, std::string>, bool> m_bigAlpha;   // (source, map) -> WDT big alpha flag
    struct Worker;
    std::unique_ptr<Worker> m_worker;
};

/// How far another version of some chunks is from the map: what pasting it there would take.
struct AreaDiff
{
    size_t cells = 0;                      // of the compared chunks, how many this version has
    size_t changed = 0;                    // of those, chunks whose heights (> 0.5 yd), textures, holes or water differ
    size_t water = 0;                      // of those, chunks whose terrain liquid differs
    float meanHeight = 0, maxHeight = 0;   // |version - map| over their vertices, yards
    float meanEdge = 0, maxEdge = 0;       // the same on the area's outer edge only: the step a paste has to blend away
    std::vector<DoodadPlacement> newDoodads;   // objects standing on the area that the map lacks (world positions)
    std::vector<WmoPlacement> newWmos;
    size_t goneDoodads = 0, goneWmos = 0;      // objects of the map the version lacks (a paste leaves them standing)
    bool Same() const { return cells && !changed && newDoodads.empty() && newWmos.empty() && !goneDoodads && !goneWmos; }
};

/// How one chunk of another version differs from the map (CompareCells).
struct CellDiff
{
    enum Kind : uint8_t { Heights = 1, Textures = 2, Holes = 4, Water = 8, Objects = 16, NewTerrain = 32 };
    int gx = 0, gz = 0;               // global chunk cell
    uint8_t kinds = 0;
    float maxHeight = 0;              // yards
    uint16_t newObjects = 0, goneObjects = 0;
    uint32_t area = 0;                // the map's area id there (the version's for new terrain)
};

/// Every chunk among `cells` where `version` has something a paste would bring over: heights (> 0.5 yd), textures,
/// holes, water (fishing/fatigue masks aside), objects the map lacks, or terrain the map lacks altogether. Objects
/// count on the chunk they stand on; objects only the map has are noted (goneObjects) but alone are no difference.
std::vector<CellDiff> CompareCells(const std::map<int, LoadedTile>& map, const std::map<int, LoadedTile>& version,
                                   const std::set<std::pair<int, int>>& cells);

/// Compares the chunks at global grid cells `cells` of `version` against `map` (both by tile key). Objects match by model
/// and position (1 yd), so a re-saved copy of the same placement is not counted as new. Buildings count wherever their
/// bounds reach the cells (a cave's origin can lie outside the area it runs through); other objects where they stand.
AreaDiff CompareArea(const std::map<int, LoadedTile>& map, const std::map<int, LoadedTile>& version, const std::set<std::pair<int, int>>& cells);
