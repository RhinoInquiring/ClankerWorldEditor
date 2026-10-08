#include "Mpq.hpp"

#include "Formats.hpp"
#include "Server.hpp"

#include <StormLib.h>
#include <CascLib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <tuple>
#include <unordered_set>

namespace
{
    /// MPQ name hashes treat '/' and '\' as different characters; the client accepts both (custom ADTs often use '/').
    std::string Backslashes(std::string s)
    {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    /// Load order of a 3.3.5 client: base, locale base, then patches (patch-N above patch, letters
    /// above digits). Later in this order wins. Unknown archives sit just above the base set.
    std::tuple<int, int> Rank(const std::string& fileName)
    {
        static const char* kBase[] = { "common.mpq", "common-2.mpq", "expansion.mpq", "lichking.mpq" };
        const std::string n = Lower(fileName);
        for (int i = 0; i < 4; ++i)
            if (n == kBase[i]) return { 0, i };
        if (n.rfind("locale-", 0) == 0) return { 1, 0 };
        if (n.rfind("expansion-locale-", 0) == 0) return { 1, 1 };
        if (n.rfind("lichking-locale-", 0) == 0) return { 1, 2 };
        if (n == "patch.mpq") return { 3, 0 };

        // patch-<x>.mpq or patch-<locale>.mpq / patch-<locale>-<x>.mpq
        std::string rest = n.substr(0, n.size() - 4);
        if (rest.rfind("patch-", 0) != 0) return { 2, 0 };
        rest = rest.substr(6);
        const bool locale = rest.size() >= 4 && std::isalpha(static_cast<unsigned char>(rest[0])) && rest.size() != 1;
        if (locale)
        {
            const size_t dash = rest.find('-');
            if (dash == std::string::npos) return { 3, 1 };          // patch-enUS.mpq
            rest = rest.substr(dash + 1);
        }
        if (rest.size() != 1) return { 2, 0 };
        const char c = rest[0];
        const int order = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : 10 + (c - 'a');
        return { 4, order * 2 + (locale ? 1 : 0) };
    }

    std::filesystem::path EditorDir()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return std::filesystem::path(exe).parent_path();
    }

    /// The community listfile CASC layers name their files from: listfile.csv beside the editor.
    std::filesystem::path ListfilePath() { return EditorDir() / "listfile.csv"; }

    /// The FileDataID a CASC name stands for: a listfile name, or CascLib's FILE%08X.dat for unnamed files.
    std::optional<uint32_t> CascId(const std::unordered_map<std::string, uint32_t>& ids, const std::string& lower)
    {
        if (const auto it = ids.find(lower); it != ids.end()) return it->second;
        unsigned id = 0;
        char tail = 0;
        if (lower.size() == 16 && std::sscanf(lower.c_str(), "file%8x.da%c", &id, &tail) == 2 && tail == 't') return id;
        return std::nullopt;
    }
}

namespace
{
    std::atomic<bool> g_cdnAsync{ false };
    std::atomic<uint64_t> g_cdnArrivals{ 0 };

    /// One file of a CASC storage by FileDataID; none when not on disk (local) or encrypted with a key we lack.
    std::optional<std::vector<uint8_t>> CascReadId(HANDLE storage, uint32_t id)
    {
        HANDLE file = nullptr;
        if (!CascOpenFile(storage, CASC_FILE_DATA_ID(id), 0, CASC_OPEN_BY_FILEID, &file)) return std::nullopt;
        ULONGLONG size = 0;
        std::optional<std::vector<uint8_t>> out;
        if (CascGetFileSize64(file, &size) && size < (1ull << 31))
        {
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            DWORD got = 0;
            if (bytes.empty() || (CascReadFile(file, bytes.data(), DWORD(bytes.size()), &got) && got == bytes.size())) out = std::move(bytes);
        }
        CascCloseFile(file);
        return out;
    }
}

namespace
{
    std::atomic<uint64_t> g_fallbackReads{ 0 }, g_fallbackHits{ 0 }, g_cascReads{ 0 }, g_cascLocalMisses{ 0 }, g_cdnQueued{ 0 }, g_fallbackMicros{ 0 };
    std::mutex g_namesLock;
    std::vector<std::string> g_fallbackNames;
}

MpqStats GetMpqStats()
{
    MpqStats s{ g_fallbackReads, g_fallbackHits, g_cascReads, g_cascLocalMisses, g_cdnQueued, g_fallbackMicros };
    std::lock_guard l(g_namesLock);
    s.fallbackNames = g_fallbackNames;
    return s;
}

void SetCdnAsync(bool on) { g_cdnAsync = on; }
uint64_t CdnArrivals() { return g_cdnArrivals; }

/// The CDN side of a CASC layer: the same build opened online (on first use: slow once, it fetches the build's
/// indexes), downloads kept in the cache folder, and a worker thread that fetches queued files in the background.
struct CdnFetcher
{
    std::string product, buildKey;
    std::mutex lock;   // queue and sets
    std::condition_variable wake;
    std::deque<uint32_t> queue;
    std::unordered_set<uint32_t> queued, fetched, failed;
    std::thread worker;
    bool stop = false;
    std::mutex storageLock;   // the online storage, one reader at a time
    HANDLE online = nullptr;
    bool tried = false;

    ~CdnFetcher()
    {
        {
            std::lock_guard l(lock);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        if (online) CascCloseStorage(online);
    }

    std::optional<std::vector<uint8_t>> Read(uint32_t id)
    {
        std::lock_guard l(storageLock);
        // The cache folder is not safe for two processes at once (one reads an archive the other is still writing):
        // every editor process (window, command-line checks) takes this system-wide lock around its CDN use.
        static const HANDLE processes = CreateMutexW(nullptr, FALSE, L"Local\\wow-world-editor-casc-cdn");
        if (processes) WaitForSingleObject(processes, INFINITE);
        struct Release { ~Release() { if (processes) ReleaseMutex(processes); } } release;
        if (!tried)
        {
            tried = true;
            // Where: the folder named in <settings>\casc-cache.txt (first line), else casc-cache beside the editor.
            std::filesystem::path cache = EditorDir() / "casc-cache";
            if (std::ifstream f(SettingsDir() / "casc-cache.txt"); f)
                if (std::string line; std::getline(f, line) && !line.empty()) cache = line;
            std::error_code ec;
            std::filesystem::create_directories(cache, ec);
            const std::string cacheDir = cache.string();
            CASC_OPEN_STORAGE_ARGS args{};
            args.Size = sizeof(args);
            args.szLocalPath = cacheDir.c_str();
            args.szCodeName = product.c_str();
            args.szRegion = "us";
            args.szBuildKey = buildKey.empty() ? nullptr : buildKey.c_str();
            args.dwLocaleMask = CASC_LOCALE_ENUS | CASC_LOCALE_ENGB;
            if (!CascOpenStorageEx(nullptr, &args, true, &online)) online = nullptr;
        }
        auto out = online ? CascReadId(online, id) : std::nullopt;
        std::lock_guard l2(lock);
        (out ? fetched : failed).insert(id);
        return out;
    }

    void Run()
    {
        for (;;)
        {
            uint32_t id = 0;
            {
                std::unique_lock l(lock);
                wake.wait(l, [&] { return stop || !queue.empty(); });
                if (stop) return;
                id = queue.front();
                queue.pop_front();
            }
            Read(id);
            ++g_cdnArrivals;
        }
    }
};

MpqChain::~MpqChain() { Close(); }

bool LoadsAfter(const std::string& a, const std::string& b) { return Rank(a) > Rank(b); }

namespace
{
    /// .build.info as rows of column name -> cell: a '|' separated table whose first line names the columns
    /// ("Product!STRING:0"; the part before '!' is the name).
    std::vector<std::unordered_map<std::string, std::string>> BuildInfoRows(const std::filesystem::path& install)
    {
        std::ifstream f(install / ".build.info");
        auto split = [](const std::string& s) {
            std::vector<std::string> cells(1);
            for (char c : s)
                if (c == '|') cells.emplace_back();
                else if (c != '\r') cells.back() += c;
            return cells;
        };
        std::string line;
        std::vector<std::unordered_map<std::string, std::string>> rows;
        if (!std::getline(f, line)) return rows;
        std::vector<std::string> head = split(line);
        for (std::string& h : head) h = h.substr(0, h.find('!'));
        while (std::getline(f, line))
        {
            const auto cells = split(line);
            auto& row = rows.emplace_back();
            for (size_t i = 0; i < head.size() && i < cells.size(); ++i) row[head[i]] = cells[i];
        }
        return rows;
    }
}

std::vector<std::string> CascProducts(const std::filesystem::path& install)
{
    std::vector<std::string> out;
    for (auto& row : BuildInfoRows(install))
        if (const std::string& p = row["Product"]; !p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

std::string CascBuildInfo(const std::filesystem::path& install, const std::string& product, const std::string& column)
{
    for (auto& row : BuildInfoRows(install))
        if (row["Product"] == product) return row[column];
    return {};
}

bool WriteMpq(const std::filesystem::path& archive, const std::filesystem::path& root, std::string& error, size_t* files)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::path, std::string>> list;   // file on disk, archived name
    for (const auto& entry : fs::recursive_directory_iterator(root, ec))
        if (entry.is_regular_file(ec))
            list.push_back({ entry.path(), Backslashes(fs::relative(entry.path(), root, ec).string()) });
    if (ec) { error = "Cannot read " + root.string() + ": " + ec.message(); return false; }
    if (list.empty()) { error = "Nothing to pack in " + root.string(); return false; }
    DWORD slots = 16;   // hash table: a power of two with room to spare
    while (slots < list.size() * 2 + 16) slots *= 2;

    const fs::path temp = archive.string() + ".partial";
    fs::create_directories(archive.parent_path(), ec);
    fs::remove(temp, ec);
    HANDLE h = nullptr;
    if (!SFileCreateArchive(temp.string().c_str(), MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES | MPQ_CREATE_ARCHIVE_V1, slots, &h))
    {
        error = "Cannot create " + temp.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    for (const auto& [path, name] : list)
        if (!SFileAddFileEx(h, path.string().c_str(), name.c_str(), MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, MPQ_COMPRESSION_ZLIB,
                            MPQ_COMPRESSION_NEXT_SAME))
        {
            error = "Cannot add " + name + " (error " + std::to_string(GetLastError()) + ")";
            SFileCloseArchive(h);
            fs::remove(temp, ec);
            return false;
        }
    if (!SFileCloseArchive(h)) { error = "Cannot finish " + temp.string(); fs::remove(temp, ec); return false; }
    fs::rename(temp, archive, ec);
    if (ec)
    {
        error = "Cannot replace " + archive.string() + ": " + ec.message() + " (is a client running with it?)";
        fs::remove(temp, ec);
        return false;
    }
    if (files) *files = list.size();
    return true;
}

bool WriteMpqFrom(const std::filesystem::path& archive, const std::vector<std::string>& names,
                  const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read, std::string& error, size_t* written)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    DWORD slots = 16;
    while (slots < names.size() * 2 + 16) slots *= 2;
    fs::create_directories(archive.parent_path(), ec);
    fs::remove(archive, ec);
    HANDLE h = nullptr;
    if (!SFileCreateArchive(archive.string().c_str(), MPQ_CREATE_LISTFILE | MPQ_CREATE_ARCHIVE_V1, slots, &h))
    {
        error = "Cannot create " + archive.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    size_t count = 0;
    for (const std::string& name : names)
    {
        const auto bytes = read(name);
        if (!bytes) continue;   // the caller listed what might be there
        HANDLE f = nullptr;
        const std::string inside = Backslashes(name);
        if (!SFileCreateFile(h, inside.c_str(), 0, DWORD(bytes->size()), 0, MPQ_FILE_REPLACEEXISTING, &f) ||
            (!bytes->empty() && !SFileWriteFile(f, bytes->data(), DWORD(bytes->size()), 0)) || !SFileFinishFile(f))
        {
            error = "Cannot add " + inside + " (error " + std::to_string(GetLastError()) + ")";
            SFileCloseArchive(h);
            fs::remove(archive, ec);
            return false;
        }
        ++count;
    }
    if (!SFileCloseArchive(h)) { error = "Cannot finish " + archive.string(); return false; }
    if (written) *written = count;
    return true;
}

void MpqChain::Close()
{
    for (const Archive& a : m_archives)
    {
        if (a.handle) SFileCloseArchive(a.handle);
        if (a.casc) CascCloseStorage(a.casc);
    }
    m_archives.clear();
    m_names.clear();
    m_report.clear();
    m_fallbacks.clear();
    std::lock_guard lock(m_homeLock);
    m_mapHomes.clear();
}

namespace
{
    /// Folders a game-file tree has at its top; an unpacked layer without any was likely picked one level too high or low.
    bool LooksLikeGameFiles(const std::filesystem::path& dir)
    {
        static const char* kRoots[] = { "world", "dbfilesclient", "textures", "interface", "sound", "character", "creature", "item",
                                        "spells", "dungeons", "tileset", "environments", "xtextures", "particles", "cameras", "fonts" };
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_directory(ec))
                for (const char* r : kRoots)
                    if (Lower(e.path().filename().string()) == r) return true;
        return false;
    }
}

LayerScan ScanForLayers(const std::filesystem::path& root)
{
    namespace fs = std::filesystem;
    LayerScan scan;
    std::vector<fs::path> mpqs, trees;
    std::error_code ec;
    std::vector<fs::path> todo{ root };
    while (!todo.empty())
    {
        const fs::path dir = todo.back();
        todo.pop_back();
        // The top of an unpacked tree: one layer; its archives (if any) are layers of their own.
        const bool tree = LooksLikeGameFiles(dir) && !fs::is_directory(dir / "Data", ec);
        if (tree) trees.push_back(dir);
        for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        {
            if (e.is_regular_file(ec))
            {
                if (Lower(e.path().extension().string()) == ".mpq") mpqs.push_back(e.path());
                else if (!tree && ++scan.strayCount <= 8) scan.strays.push_back(fs::relative(e.path(), root, ec).string());
            }
            else if (e.is_directory(ec))
            {
                if (!tree) todo.push_back(e.path());
                else   // archives anywhere inside the tree still count (a mod folder shipping a patch beside its loose files)
                    for (const auto& f : fs::recursive_directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec))
                        if (f.is_regular_file(ec) && Lower(f.path().extension().string()) == ".mpq") mpqs.push_back(f.path());
            }
        }
    }
    // Lowest priority first: archives in the client's order (path breaks ties), then the unpacked trees by path.
    std::sort(mpqs.begin(), mpqs.end(), [](const fs::path& a, const fs::path& b) {
        const auto ra = Rank(a.filename().string()), rb = Rank(b.filename().string());
        return ra != rb ? ra < rb : a < b;
    });
    std::sort(trees.begin(), trees.end());
    for (const auto& p : mpqs) scan.layers.push_back({ MpqLayer::Kind::MpqFile, p.string(), true, true, root.string() });
    for (const auto& p : trees) scan.layers.push_back({ MpqLayer::Kind::Folder, p.string(), true, true, root.string() });
    return scan;
}

std::vector<MpqLayer> RescanLayers(const std::vector<MpqLayer>& layers, const std::string& root, LayerScan* report)
{
    LayerScan scan = ScanForLayers(root);
    auto same = [](const MpqLayer& a, const MpqLayer& b) { return a.kind == b.kind && Lower(a.path) == Lower(b.path); };
    std::vector<MpqLayer> out;
    for (const MpqLayer& l : layers)   // kept in place with their settings, unless the scan no longer finds them
        if (l.from != root || std::any_of(scan.layers.begin(), scan.layers.end(), [&](const MpqLayer& s) { return same(s, l); }))
            out.push_back(l);
    for (const MpqLayer& s : scan.layers)   // new ones on top
        if (std::none_of(out.begin(), out.end(), [&](const MpqLayer& l) { return same(s, l); })) out.push_back(s);
    if (report) *report = std::move(scan);
    return out;
}

size_t MpqChain::Open(const std::vector<MpqLayer>& layers)
{
    namespace fs = std::filesystem;
    Close();
    m_report.resize(layers.size());
    // Highest priority first: the last layer's archives lead. CASC layers open in a second pass: opened before the
    // MPQs, their million-name index made StormLib's opening 3x slower (4 s -> 14 s for a stock client).
    for (int pass = 0; pass < 2; ++pass)
    for (size_t li = layers.size(); li-- > 0;)
    {
        const MpqLayer& layer = layers[li];
        if ((layer.kind == MpqLayer::Kind::Casc) != (pass == 1)) continue;
        LayerReport& report = m_report[li];
        struct Timer   // how long the layer took to open, however this iteration ends
        {
            LayerReport& r;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~Timer() { r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
        } timer{ report };
        if (!layer.enabled) { report.note = "disabled"; continue; }
        std::error_code ec;
        if (layer.kind == MpqLayer::Kind::Casc)
        {
            CASC_OPEN_STORAGE_ARGS args{};
            args.Size = sizeof(args);
            args.szLocalPath = layer.path.c_str();
            args.szCodeName = layer.product.empty() ? nullptr : layer.product.c_str();
            args.dwLocaleMask = CASC_LOCALE_ENUS | CASC_LOCALE_ENGB;
            HANDLE h = nullptr;
            if (!CascOpenStorageEx(nullptr, &args, false, &h))
            {
                report.note = "could not open the CASC storage (error " + std::to_string(GetCascError()) + ")";
                continue;
            }
            Archive a;
            a.casc = h;
            a.installed = layer.installed;
            a.layer = li;
            a.cdn = std::make_shared<CdnFetcher>();
            a.cdn->product = layer.product;
            a.cdn->buildKey = CascBuildInfo(layer.path, layer.product, "Build Key");
            // Every file of this build (the listfile names files of every product): those not on disk come from the CDN.
            const fs::path listfile = ListfilePath();
            const bool named = fs::is_regular_file(listfile, ec);
            CASC_FIND_DATA fd{};
            HANDLE find = CascFindFirstFile(h, "*", &fd, named ? listfile.string().c_str() : nullptr);
            if (find)
            {
                do
                {
                    if (fd.dwFileDataId == CASC_INVALID_ID) continue;
                    const std::string name = Backslashes(fd.szFileName);
                    if (a.ids.emplace(Lower(name), fd.dwFileDataId).second)
                    {
                        a.byId.try_emplace(fd.dwFileDataId, a.listed.size());
                        a.listed.push_back(name);
                    }
                    a.present.insert(fd.dwFileDataId);
                } while (CascFindNextFile(find, &fd));
                CascFindClose(find);
            }
            report.files = a.ids.size();
            if (!named) report.note = "no listfile.csv beside the editor: files are named FILExxxxxxxx.dat";
            m_names.push_back(fs::path(layer.path).filename().string() + ":" + layer.product);
            m_archives.push_back(std::move(a));
            report.archives = 1;
            continue;
        }
        if (layer.kind == MpqLayer::Kind::Folder)
        {
            Archive a;
            a.installed = layer.installed;
            a.layer = li;
            const fs::path root = layer.path;
            for (const auto& e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
                if (e.is_regular_file(ec) && Lower(e.path().extension().string()) != ".mpq")
                {
                    const std::string rel = Backslashes(fs::relative(e.path(), root, ec).string());
                    if (a.files.emplace(Lower(rel), e.path()).second) a.listed.push_back(rel);
                }
            report.files = a.files.size();
            if (a.files.empty()) { report.note = "no files"; continue; }
            if (!LooksLikeGameFiles(root))
                report.note = "no game folders (World, DBFilesClient, Textures...) at its top: pick the folder that holds them";
            m_names.push_back(root.filename().string() + "/");
            m_archives.push_back(std::move(a));
            report.archives = 1;
            continue;
        }
        std::vector<fs::path> found;
        if (layer.kind == MpqLayer::Kind::MpqFile)
        {
            if (fs::is_regular_file(layer.path, ec)) found.push_back(layer.path);
            else report.note = "file not found";
        }
        else
        {
            fs::path dir = layer.path;
            if (fs::is_directory(dir / "Data", ec)) dir /= "Data";   // a client folder: its Data
            for (const auto& entry : fs::directory_iterator(dir, ec))
            {
                if (entry.is_regular_file(ec) && Lower(entry.path().extension().string()) == ".mpq")
                    found.push_back(entry.path());
                else if (entry.is_directory(ec))   // locale folders (enUS, deDE, ...)
                    for (const auto& sub : fs::directory_iterator(entry.path(), ec))
                        if (sub.is_regular_file(ec) && Lower(sub.path().extension().string()) == ".mpq")
                            found.push_back(sub.path());
            }
            std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
                return Rank(a.filename().string()) > Rank(b.filename().string());
            });
            if (found.empty()) report.note = "no MPQ archives here";
        }
        for (const auto& path : found)
        {
            HANDLE h = nullptr;
            if (SFileOpenArchive(path.string().c_str(), 0, MPQ_OPEN_READ_ONLY, &h))
            {
                Archive a;
                a.handle = h;
                a.installed = layer.installed;
                a.layer = li;
            a.layer = li;
                m_archives.push_back(std::move(a));
                m_names.push_back(path.filename().string());
                ++report.archives;
            }
            else if (report.note.empty())
                report.note = "could not open " + path.filename().string();
        }
    }
    // A CASC storage (a newer client) only fills in what the 3.3.5a layers lack, wherever it sits in the list: its
    // copies of shared names are newer formats, and half its files may need fetching from the CDN.
    std::vector<size_t> order(m_archives.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_partition(order.begin(), order.end(), [&](size_t i) { return !m_archives[i].casc; });
    std::vector<Archive> archives;
    std::vector<std::string> names;
    for (size_t i : order)
    {
        archives.push_back(std::move(m_archives[i]));
        names.push_back(std::move(m_names[i]));
    }
    m_archives = std::move(archives);
    m_names = std::move(names);
    return m_archives.size();
}

std::vector<MpqChain::Entry> MpqChain::List() const
{
    std::lock_guard lock(m_lock);
    std::vector<Entry> out;
    std::unordered_set<std::string> seen;   // lower-case names already listed
    for (size_t a = 0; a < m_archives.size(); ++a)   // highest priority first: the first archive listing a name wins
    {
        if (!m_archives[a].handle)
        {
            for (const std::string& name : m_archives[a].listed)
                if (seen.insert(Lower(name)).second) out.push_back({ name, a });
            continue;
        }
        SFILE_FIND_DATA fd{};
        HANDLE find = SFileFindFirstFile(m_archives[a].handle, "*", &fd, nullptr);
        if (!find) continue;
        do
        {
            const std::string name = fd.cFileName;
            if (!name.empty() && name[0] != '(' && seen.insert(Lower(name)).second) out.push_back({ name, a });   // skip (listfile) etc.
        } while (SFileFindNextFile(find, &fd));
        SFileFindClose(find);
    }
    return out;
}

std::filesystem::path MpqChain::OverlayPath(const std::string& name) const
{
    if (m_overlay.empty() || Lower(Backslashes(name)).rfind("world\\maps\\", 0) != 0) return {};
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return m_overlay / std::filesystem::path(rel);
}

std::optional<size_t> MpqChain::MapHome(const std::string& name) const
{
    if (!m_mapsFromLowest) return std::nullopt;
    const std::string lower = Lower(Backslashes(name));
    static const std::string prefix = "world\\maps\\", mapDbc = "dbfilesclient\\map.dbc";
    // Map.dbc too: the list of maps is the client's; packs' Map.dbc only say which maps they add (App::RefreshMapList).
    std::string dir, wdt;
    if (lower == mapDbc) dir = wdt = mapDbc;
    else
    {
        if (lower.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
        const size_t slash = lower.find('\\', prefix.size());
        if (slash == std::string::npos) return std::nullopt;
        dir = lower.substr(prefix.size(), slash - prefix.size());
        wdt = prefix + dir + "\\" + dir + ".wdt";
    }
    {
        std::lock_guard lock(m_homeLock);
        if (const auto it = m_mapHomes.find(dir); it != m_mapHomes.end()) return it->second;
    }
    // The lowest layer holding the map's WDT; its first (highest-priority) archive starts the layer's own lookup.
    // A CASC storage is the map's home only when no 3.3.5a layer has it (CASC archives sort last: see Open).
    std::optional<size_t> home;
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (Has(a, wdt) && (!home || (!m_archives[a].casc && (m_archives[*home].casc || m_archives[a].layer < m_archives[*home].layer)))) home = a;
    if (home)
        while (*home > 0 && m_archives[*home - 1].layer == m_archives[*home].layer) --*home;
    std::lock_guard lock(m_homeLock);
    m_mapHomes[dir] = home;
    return home;
}

std::optional<std::vector<uint8_t>> MpqChain::Read(const std::string& name) const
{
    if (const auto path = OverlayPath(name); !path.empty())
        if (std::ifstream f(path, std::ios::binary); f)
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (const auto home = MapHome(name))   // a map's own files: from the layer the map comes from
        if (auto bytes = ReadFromLayer(*home, name)) return bytes;
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (auto bytes = ReadFrom(a, name)) return bytes;
    if (m_fallbacks.empty()) return std::nullopt;
    ++g_fallbackReads;
    {
        std::lock_guard l(g_namesLock);
        if (g_fallbackNames.size() < 40) g_fallbackNames.push_back(name);
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::optional<std::vector<uint8_t>> out;
    for (const MpqChain* f : m_fallbacks)
        if ((out = f->Read(name))) break;
    g_fallbackMicros += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    if (out) ++g_fallbackHits;
    return out;
}

bool MpqChain::HasOwn(const std::string& name) const
{
    if (const auto path = OverlayPath(name); !path.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return true;
    }
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (Has(a, name)) return true;
    return false;
}

std::optional<std::vector<uint8_t>> MpqChain::ReadFromLayer(size_t archive, const std::string& name) const
{
    for (size_t a = archive; a < m_archives.size() && m_archives[a].layer == ArchiveLayer(archive); ++a)
        if (auto bytes = ReadFrom(a, name)) return bytes;
    return std::nullopt;
}

std::optional<size_t> MpqChain::LayerOf(const std::string& name) const
{
    if (const auto home = MapHome(name))
        for (size_t a = *home; a < m_archives.size() && m_archives[a].layer == m_archives[*home].layer; ++a)
            if (Has(a, name)) return m_archives[a].layer;
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (Has(a, name)) return m_archives[a].layer;
    return std::nullopt;
}

bool MpqChain::HasInstalled(const std::string& name) const
{
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (m_archives[a].installed && Has(a, name)) return true;
    return false;
}

std::string MpqChain::NameOf(uint32_t fileDataId) const
{
    for (const Archive& a : m_archives)
        if (const auto it = a.byId.find(fileDataId); a.casc && it != a.byId.end()) return a.listed[it->second];
    for (const MpqChain* f : m_fallbacks)
        if (std::string n = f->NameOf(fileDataId); !n.empty()) return n;
    return {};
}

std::optional<std::vector<uint8_t>> MpqChain::ReadCasc(const Archive& a, uint32_t id) const
{
    auto read = [&](HANDLE storage) { return CascReadId(storage, id); };
    ++g_cascReads;
    {
        std::lock_guard lock(m_lock);
        if (auto bytes = read(a.casc)) return bytes;
    }
    ++g_cascLocalMisses;
    // Not on disk: Blizzard's CDN. Waiting mode downloads here; otherwise the file is queued for the background
    // fetcher and this read misses (CdnArrivals() counts what lands, so the editor asks again).
    CdnFetcher& cdn = *a.cdn;
    {
        std::lock_guard lock(cdn.lock);
        if (cdn.failed.count(id)) return std::nullopt;
        if (g_cdnAsync && !cdn.fetched.count(id))
        {
            if (cdn.queued.insert(id).second) { cdn.queue.push_back(id); ++g_cdnQueued; }
            if (!cdn.worker.joinable()) cdn.worker = std::thread([&cdn] { cdn.Run(); });
            cdn.wake.notify_one();
            return std::nullopt;
        }
    }
    return cdn.Read(id);   // already downloaded (from the cache, quick) or waiting mode
}

std::optional<std::vector<uint8_t>> MpqChain::ReadFrom(size_t archive, const std::string& name) const
{
    if (archive >= m_archives.size()) return std::nullopt;
    const Archive& a = m_archives[archive];
    if (a.casc)
    {
        const std::string lower = Lower(Backslashes(name));
        const auto id = CascId(a.ids, lower);
        if (!id || !a.present.count(*id)) return std::nullopt;
        auto bytes = ReadCasc(a, *id);   // locks what it uses
        // A split tile (root + _tex0 + _obj0) is served as the one 3.3.5 ADT under the root's name.
        if (bytes && lower.ends_with(".adt") && lower.find("_obj") == std::string::npos && lower.find("_tex") == std::string::npos &&
            lower.find("_lod") == std::string::npos)
        {
            const std::string stem = lower.substr(0, lower.size() - 4);
            const auto tex = a.ids.find(stem + "_tex0.adt"), obj = a.ids.find(stem + "_obj0.adt");
            if (tex != a.ids.end() && obj != a.ids.end())
            {
                const auto t = ReadCasc(a, tex->second), o = ReadCasc(a, obj->second);
                auto merged = t && o ? MergeSplitAdt(*bytes, *t, *o, [&](uint32_t fdid) {
                    const auto it = a.byId.find(fdid);
                    return it == a.byId.end() ? std::string() : a.listed[it->second];
                }) : std::vector<uint8_t>{};
                if (merged.empty()) return std::nullopt;
                return merged;
            }
        }
        return bytes;
    }
    if (!a.handle)
    {
        const auto it = a.files.find(Lower(Backslashes(name)));
        if (it == a.files.end()) return std::nullopt;
        std::ifstream f(it->second, std::ios::binary);
        if (!f) return std::nullopt;
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    std::lock_guard lock(m_lock);
    HANDLE file = nullptr;
    if (!SFileOpenFileEx(a.handle, Backslashes(name).c_str(), SFILE_OPEN_FROM_MPQ, &file)) return std::nullopt;
    std::vector<uint8_t> bytes(SFileGetFileSize(file, nullptr));
    DWORD read = 0;
    const bool ok = bytes.empty() || SFileReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    SFileCloseFile(file);
    if (ok && read == bytes.size()) return bytes;
    return std::nullopt;
}

bool MpqChain::Has(size_t archive, const std::string& name) const
{
    if (archive >= m_archives.size()) return false;
    const Archive& a = m_archives[archive];
    if (a.casc)
    {
        const auto id = CascId(a.ids, Lower(Backslashes(name)));
        return id && a.present.count(*id);
    }
    if (!a.handle) return a.files.count(Lower(Backslashes(name))) != 0;
    std::lock_guard lock(m_lock);
    return SFileHasFile(a.handle, Backslashes(name).c_str());
}
