#include "Mpq.hpp"

#include <StormLib.h>

#include <algorithm>
#include <cctype>
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
}

MpqChain::~MpqChain() { Close(); }

bool LoadsAfter(const std::string& a, const std::string& b) { return Rank(a) > Rank(b); }

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
        if (a.handle) SFileCloseArchive(a.handle);
    m_archives.clear();
    m_names.clear();
    m_report.clear();
    m_fallbacks.clear();
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
    // Highest priority first: the last layer's archives lead.
    for (size_t li = layers.size(); li-- > 0;)
    {
        const MpqLayer& layer = layers[li];
        LayerReport& report = m_report[li];
        if (!layer.enabled) { report.note = "disabled"; continue; }
        std::error_code ec;
        if (layer.kind == MpqLayer::Kind::Folder)
        {
            Archive a;
            a.installed = layer.installed;
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
                m_archives.push_back(std::move(a));
                m_names.push_back(path.filename().string());
                ++report.archives;
            }
            else if (report.note.empty())
                report.note = "could not open " + path.filename().string();
        }
    }
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

std::optional<std::vector<uint8_t>> MpqChain::Read(const std::string& name) const
{
    if (const auto path = OverlayPath(name); !path.empty())
        if (std::ifstream f(path, std::ios::binary); f)
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (auto bytes = ReadFrom(a, name)) return bytes;
    for (const MpqChain* f : m_fallbacks)
        if (auto bytes = f->Read(name)) return bytes;
    return std::nullopt;
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

bool MpqChain::HasInstalled(const std::string& name) const
{
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (m_archives[a].installed && Has(a, name)) return true;
    return false;
}

std::optional<std::vector<uint8_t>> MpqChain::ReadFrom(size_t archive, const std::string& name) const
{
    if (archive >= m_archives.size()) return std::nullopt;
    const Archive& a = m_archives[archive];
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
    if (!a.handle) return a.files.count(Lower(Backslashes(name))) != 0;
    std::lock_guard lock(m_lock);
    return SFileHasFile(a.handle, Backslashes(name).c_str());
}
