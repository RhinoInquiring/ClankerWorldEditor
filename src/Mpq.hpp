#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// Packs every file under `root` into a new MPQ at `archive` (archived names: paths relative to root with '\'
/// separators), zlib-compressed, with (listfile) and (attributes). Built beside it first, then moved over an older one;
/// false (and `error`) when it cannot be written, e.g. a running client holds the old one open.
/// `large`: format 4, past 4 GB (StormLib and so the editor read it; the 3.3.5a client does not).
bool WriteMpq(const std::filesystem::path& archive, const std::filesystem::path& root, std::string& error, size_t* files = nullptr,
              bool large = false);
/// An uncompressed MPQ of `names`, each read when it is written (nothing held in memory but one file); names `read`
/// cannot give are left out. `written` gets how many went in.
bool WriteMpqFrom(const std::filesystem::path& archive, const std::vector<std::string>& names,
                  const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read, std::string& error, size_t* written = nullptr);

struct CdnFetcher;

/// Read counters across every chain (profiling): reads that went past a chain's own archives to its fallbacks, CASC
/// reads, CASC files not on disk, CDN fetches queued, time in fallback reads, and the first fallback names seen.
struct MpqStats
{
    uint64_t fallbackReads = 0, fallbackHits = 0, cascReads = 0, cascLocalMisses = 0, cdnQueued = 0, fallbackMicros = 0;
    std::vector<std::string> fallbackNames;
};
MpqStats GetMpqStats();

/// CASC files missing on disk come from Blizzard's CDN. Off (the default): a read downloads and waits (exports,
/// checks: complete). On (the editor's window): a read misses at once and the file is fetched in the background;
/// CdnArrivals() counts the files that have landed since, so whoever cached a miss can ask again.
void SetCdnAsync(bool on);
uint64_t CdnArrivals();

/// The products a CASC install lists in its .build.info (e.g. wow, wow_classic_era, wow_classic_beta); empty when none.
std::vector<std::string> CascProducts(const std::filesystem::path& install);
/// A .build.info cell (`column` e.g. "Build Key") of the product's row; empty when there is none.
std::string CascBuildInfo(const std::filesystem::path& install, const std::string& product, const std::string& column);

/// True when the client loads archive `a` after archive `b` (file names, e.g. "patch-enUS-Z.MPQ"): `a`'s files win.
bool LoadsAfter(const std::string& a, const std::string& b);

/// One layer of game files: a folder of MPQs (a client's Data folder: its archives in the client's load order, locale
/// folders included), a single MPQ, or an unpacked folder laid out by game path (World\Maps\..., DBFilesClient\...).
/// Casc: a modern client's CASC storage (path = the install folder holding .build.info, `product` = which of its
/// products, e.g. wow_classic_beta). Names come from a community listfile (listfile.csv, "id;path" lines) beside the
/// editor; files it does not name are still readable as FILE%08X.dat (FileDataID, hex).
struct MpqLayer
{
    enum class Kind { MpqFolder, MpqFile, Folder, Casc };
    Kind kind = Kind::MpqFolder;
    std::string path;
    bool enabled = true;
    /// Players' clients have this layer (a client's Data, installed module patches): files only it has are not copied
    /// into the project's patch. Off for an unpacked folder of new art: the patch carries what the edits use from it.
    bool installed = true;
    /// The folder a scan found this layer in (ScanForLayers), so a rescan can update it; empty when added by hand.
    std::string from;
    /// The user's name for it, shown in the Maps window beside the maps it supplies (empty: none shown).
    std::string label;
    /// Casc only: the product code (.build.info "Product").
    std::string product;
    static const char* KindName(Kind k)
    {
        return k == Kind::MpqFolder ? "mpqfolder" : k == Kind::MpqFile ? "mpq" : k == Kind::Casc ? "casc" : "folder";
    }
    static Kind KindFrom(const std::string& s)
    {
        return s == "mpq" ? Kind::MpqFile : s == "folder" ? Kind::Folder : s == "casc" ? Kind::Casc : Kind::MpqFolder;
    }
};

/// What a folder holds, as layers: every .mpq (one layer each, lowest first in the client's load order) and every
/// folder that is the top of unpacked game files (holds World, DBFilesClient, Textures, Interface...; not a client
/// folder, which holds Data) as one unpacked layer above them, by path. Each layer's `from` is `root`.
struct LayerScan
{
    std::vector<MpqLayer> layers;
    std::vector<std::string> strays;   // files outside any unpacked tree (first few, relative to root)
    size_t strayCount = 0;
};
LayerScan ScanForLayers(const std::filesystem::path& root);

/// `layers` with the scan of `root` again: layers still found keep their place and settings, layers no longer
/// found go, new ones are added on top.
std::vector<MpqLayer> RescanLayers(const std::vector<MpqLayer>& layers, const std::string& root, LayerScan* scan = nullptr);

/// Read-only view over layers of game files (reads are thread-safe), searched highest priority first the way the client
/// resolves a file several archives contain: later layers above earlier ones, a folder of MPQs in client load order.
class MpqChain
{
public:
    MpqChain() = default;
    MpqChain(const MpqChain&) = delete;
    MpqChain& operator=(const MpqChain&) = delete;
    ~MpqChain();

    /// Opens the enabled layers, lowest priority first (later layers win); returns how many archives (an unpacked
    /// folder counts as one) opened. LayerReport() says what each layer gave.
    size_t Open(const std::vector<MpqLayer>& layers);
    /// One client Data folder (or a client folder holding Data), or "<install>*<product>" for a CASC storage.
    size_t Open(const std::string& dataDir)
    {
        if (const size_t star = dataDir.find('*'); star != std::string::npos)
            return Open(std::vector<MpqLayer>{ { MpqLayer::Kind::Casc, dataDir.substr(0, star), true, false, "", "", dataDir.substr(star + 1) } });
        return Open(std::vector<MpqLayer>{ { MpqLayer::Kind::MpqFolder, dataDir } });
    }
    void Close();

    /// What each layer of the last Open gave: archives, files (unpacked folders), and a note when it looks wrong.
    struct LayerReport { size_t archives = 0, files = 0; std::string note; double ms = 0; };
    const std::vector<LayerReport>& Report() const { return m_report; }

    /// The file from the highest-priority archive that has it, else from the fallback chains (other clients),
    /// so assets pasted from another client still load.
    std::optional<std::vector<uint8_t>> Read(const std::string& name) const;
    /// True when this chain's own archives have the file (fallbacks not consulted).
    bool HasOwn(const std::string& name) const;
    /// The layer (index into what Open was given) whose archive serves the file; overlay and fallbacks not consulted.
    std::optional<size_t> LayerOf(const std::string& name) const;
    /// True when a layer players have (MpqLayer::installed) has the file: what a patch need not carry.
    bool HasInstalled(const std::string& name) const;
    void SetFallbacks(std::vector<const MpqChain*> fallbacks) { m_fallbacks = std::move(fallbacks); }
    /// A map's own files (World\Maps\<map>\...) come from the lowest layer holding its WDT, the plain client's copy,
    /// not a mod's version stacked over it; other files resolve top-first as usual. On for the project's base: mods'
    /// copies of a map stay other versions of it. Set before reading.
    void SetMapsFromLowestLayer(bool on) { m_mapsFromLowest = on; std::lock_guard lock(m_homeLock); m_mapHomes.clear(); }
    /// The first archive of the layer a map path comes from under that rule; none when it is off or no layer has the map.
    std::optional<size_t> MapHome(const std::string& name) const;
    /// Loose files above every archive: <dir>/<game path, '/' separators>, looked up for World\Maps\ paths only
    /// (the project's added map tiles and the WDTs that list them). Empty = none. Set while no loader is reading.
    void SetOverlay(std::filesystem::path dir) { m_overlay = std::move(dir); }
    /// Where a game path lives in the overlay (empty when there is no overlay or the path is not a map file).
    std::filesystem::path OverlayPath(const std::string& name) const;
    /// The file as one archive holds it (index into Names()), ignoring every other archive.
    std::optional<std::vector<uint8_t>> ReadFrom(size_t archive, const std::string& name) const;
    /// The file from `archive` or the archives after it in the same layer: that layer resolved on its own.
    std::optional<std::vector<uint8_t>> ReadFromLayer(size_t archive, const std::string& name) const;
    /// The layer an archive came from (index into what Open was given).
    size_t ArchiveLayer(size_t archive) const { return archive < m_archives.size() ? m_archives[archive].layer : 0; }
    bool Has(size_t archive, const std::string& name) const;

    /// Every file name the archives' listfiles know, once each (case-insensitive), with the archive it resolves to.
    struct Entry { std::string name; size_t archive; };
    std::vector<Entry> List() const;

    /// The game path of a FileDataID, from the first CASC layer's listfile names; empty when none knows it.
    std::string NameOf(uint32_t fileDataId) const;

    /// Archive names, highest priority first (an unpacked folder: its folder name and a slash).
    const std::vector<std::string>& Names() const { return m_names; }

private:
    struct Archive
    {
        void* handle = nullptr;                                         // StormLib HANDLE; null for an unpacked folder
        void* casc = nullptr;                                           // CascLib storage HANDLE (Casc layer)
        std::unordered_map<std::string, uint32_t> ids;                  // Casc: lower-case game path -> FileDataID
        std::unordered_set<uint32_t> present;                           // Casc: FileDataIDs this build has (on disk or on the CDN)
        std::unordered_map<uint32_t, size_t> byId;                      // Casc: FileDataID -> index into `listed` (its name)
        std::shared_ptr<CdnFetcher> cdn;                                // Casc: files not on disk, from Blizzard's CDN
        std::unordered_map<std::string, std::filesystem::path> files;   // unpacked folder: lower-case game path -> file
        std::vector<std::string> listed;                                // unpacked folder: game paths as found
        bool installed = true;
        size_t layer = 0;                                               // index into the layers Open was given
    };
    /// One CASC file by FileDataID: from disk, else the CDN. Caller holds m_lock.
    std::optional<std::vector<uint8_t>> ReadCasc(const Archive& a, uint32_t id) const;
    // StormLib handles are not safe to share between threads: every call into them takes this lock (the tile loader
    // reads on its own thread). Open/Close must only run while no loader is reading.
    mutable std::mutex m_lock;
    bool m_mapsFromLowest = false;
    mutable std::mutex m_homeLock;
    mutable std::unordered_map<std::string, std::optional<size_t>> m_mapHomes;   // lower-case map folder -> MapHome
    std::vector<Archive> m_archives;   // highest priority first
    std::vector<const MpqChain*> m_fallbacks;
    std::vector<std::string> m_names;
    std::vector<LayerReport> m_report;
    std::filesystem::path m_overlay;
};
