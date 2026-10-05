#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/// Packs every file under `root` into a new MPQ at `archive` (archived names: paths relative to root with '\'
/// separators), zlib-compressed, with (listfile) and (attributes). Built beside it first, then moved over an older one;
/// false (and `error`) when it cannot be written, e.g. a running client holds the old one open.
bool WriteMpq(const std::filesystem::path& archive, const std::filesystem::path& root, std::string& error, size_t* files = nullptr);

/// True when the client loads archive `a` after archive `b` (file names, e.g. "patch-enUS-Z.MPQ"): `a`'s files win.
bool LoadsAfter(const std::string& a, const std::string& b);

/// Read-only view over a 3.3.5 client's Data folder (reads are thread-safe): every MPQ, searched highest priority first,
/// the way the client resolves a file that several archives contain.
class MpqChain
{
public:
    MpqChain() = default;
    MpqChain(const MpqChain&) = delete;
    MpqChain& operator=(const MpqChain&) = delete;
    ~MpqChain();

    /// Opens every archive in dataDir and its locale subfolders; returns how many opened.
    size_t Open(const std::string& dataDir);
    void Close();

    /// The file from the highest-priority archive that has it, else from the fallback chains (other clients),
    /// so assets pasted from another client still load.
    std::optional<std::vector<uint8_t>> Read(const std::string& name) const;
    /// True when this chain's own archives have the file (fallbacks not consulted).
    bool HasOwn(const std::string& name) const;
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

    /// Archive file names, highest priority first.
    const std::vector<std::string>& Names() const { return m_names; }

private:
    // StormLib handles are not safe to share between threads: every call into them takes this lock (the tile loader
    // reads on its own thread). Open/Close must only run while no loader is reading.
    mutable std::mutex m_lock;
    std::vector<void*> m_archives;   // HANDLEs, highest priority first
    std::vector<const MpqChain*> m_fallbacks;
    std::vector<std::string> m_names;
    std::filesystem::path m_overlay;
};
