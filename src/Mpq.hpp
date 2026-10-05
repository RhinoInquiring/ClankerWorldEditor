#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/// Packs every file under `root` into a new MPQ at `archive` (archived names: paths relative to root with '\'
/// separators), zlib-compressed, with (listfile) and (attributes). Built beside it first, then moved over an older one;
/// false (and `error`) when it cannot be written, e.g. a running client holds the old one open.
bool WriteMpq(const std::filesystem::path& archive, const std::filesystem::path& root, std::string& error, size_t* files = nullptr);

/// True when the client loads archive `a` after archive `b` (file names, e.g. "patch-enUS-Z.MPQ"): `a`'s files win.
bool LoadsAfter(const std::string& a, const std::string& b);

/// One layer of game files: a folder of MPQs (a client's Data folder: its archives in the client's load order, locale
/// folders included), a single MPQ, or an unpacked folder laid out by game path (World\Maps\..., DBFilesClient\...).
struct MpqLayer
{
    enum class Kind { MpqFolder, MpqFile, Folder };
    Kind kind = Kind::MpqFolder;
    std::string path;
    bool enabled = true;
    /// Players' clients have this layer (a client's Data, installed module patches): files only it has are not copied
    /// into the project's patch. Off for an unpacked folder of new art: the patch carries what the edits use from it.
    bool installed = true;
    /// The folder a scan found this layer in (ScanForLayers), so a rescan can update it; empty when added by hand.
    std::string from;
    static const char* KindName(Kind k) { return k == Kind::MpqFolder ? "mpqfolder" : k == Kind::MpqFile ? "mpq" : "folder"; }
    static Kind KindFrom(const std::string& s) { return s == "mpq" ? Kind::MpqFile : s == "folder" ? Kind::Folder : Kind::MpqFolder; }
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
    /// One client Data folder (or a client folder holding Data).
    size_t Open(const std::string& dataDir) { return Open(std::vector<MpqLayer>{ { MpqLayer::Kind::MpqFolder, dataDir } }); }
    void Close();

    /// What each layer of the last Open gave: archives, files (unpacked folders), and a note when it looks wrong.
    struct LayerReport { size_t archives = 0, files = 0; std::string note; };
    const std::vector<LayerReport>& Report() const { return m_report; }

    /// The file from the highest-priority archive that has it, else from the fallback chains (other clients),
    /// so assets pasted from another client still load.
    std::optional<std::vector<uint8_t>> Read(const std::string& name) const;
    /// True when this chain's own archives have the file (fallbacks not consulted).
    bool HasOwn(const std::string& name) const;
    /// True when a layer players have (MpqLayer::installed) has the file: what a patch need not carry.
    bool HasInstalled(const std::string& name) const;
    void SetFallbacks(std::vector<const MpqChain*> fallbacks) { m_fallbacks = std::move(fallbacks); }
    /// Loose files above every archive: <dir>/<game path, '/' separators>, looked up for World\Maps\ paths only
    /// (the project's added map tiles and the WDTs that list them). Empty = none. Set while no loader is reading.
    void SetOverlay(std::filesystem::path dir) { m_overlay = std::move(dir); }
    /// Where a game path lives in the overlay (empty when there is no overlay or the path is not a map file).
    std::filesystem::path OverlayPath(const std::string& name) const;
    /// The file as one archive holds it (index into Names()), ignoring every other archive.
    std::optional<std::vector<uint8_t>> ReadFrom(size_t archive, const std::string& name) const;
    bool Has(size_t archive, const std::string& name) const;

    /// Every file name the archives' listfiles know, once each (case-insensitive), with the archive it resolves to.
    struct Entry { std::string name; size_t archive; };
    std::vector<Entry> List() const;

    /// Archive names, highest priority first (an unpacked folder: its folder name and a slash).
    const std::vector<std::string>& Names() const { return m_names; }

private:
    struct Archive
    {
        void* handle = nullptr;                                         // StormLib HANDLE; null for an unpacked folder
        std::unordered_map<std::string, std::filesystem::path> files;   // unpacked folder: lower-case game path -> file
        std::vector<std::string> listed;                                // unpacked folder: game paths as found
        bool installed = true;
    };
    // StormLib handles are not safe to share between threads: every call into them takes this lock (the tile loader
    // reads on its own thread). Open/Close must only run while no loader is reading.
    mutable std::mutex m_lock;
    std::vector<Archive> m_archives;   // highest priority first
    std::vector<const MpqChain*> m_fallbacks;
    std::vector<std::string> m_names;
    std::vector<LayerReport> m_report;
    std::filesystem::path m_overlay;
};
