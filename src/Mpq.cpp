#include "Mpq.hpp"

#include <StormLib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
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

void MpqChain::Close()
{
    for (void* h : m_archives) SFileCloseArchive(h);
    m_archives.clear();
    m_names.clear();
    m_fallbacks.clear();
}

size_t MpqChain::Open(const std::string& dataDir)
{
    namespace fs = std::filesystem;
    Close();

    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dataDir, ec))
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

    for (const auto& path : found)
    {
        HANDLE h = nullptr;
        if (SFileOpenArchive(path.string().c_str(), 0, MPQ_OPEN_READ_ONLY, &h))
        {
            m_archives.push_back(h);
            m_names.push_back(path.filename().string());
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
        SFILE_FIND_DATA fd{};
        HANDLE find = SFileFindFirstFile(m_archives[a], "*", &fd, nullptr);
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

std::optional<std::vector<uint8_t>> MpqChain::Read(const std::string& name) const
{
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (auto bytes = ReadFrom(a, name)) return bytes;
    for (const MpqChain* f : m_fallbacks)
        if (auto bytes = f->Read(name)) return bytes;
    return std::nullopt;
}

bool MpqChain::HasOwn(const std::string& name) const
{
    for (size_t a = 0; a < m_archives.size(); ++a)
        if (Has(a, name)) return true;
    return false;
}

std::optional<std::vector<uint8_t>> MpqChain::ReadFrom(size_t archive, const std::string& name) const
{
    std::lock_guard lock(m_lock);
    if (archive >= m_archives.size()) return std::nullopt;
    HANDLE file = nullptr;
    if (!SFileOpenFileEx(m_archives[archive], Backslashes(name).c_str(), SFILE_OPEN_FROM_MPQ, &file)) return std::nullopt;
    std::vector<uint8_t> bytes(SFileGetFileSize(file, nullptr));
    DWORD read = 0;
    const bool ok = bytes.empty() || SFileReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    SFileCloseFile(file);
    if (ok && read == bytes.size()) return bytes;
    return std::nullopt;
}

bool MpqChain::Has(size_t archive, const std::string& name) const
{
    std::lock_guard lock(m_lock);
    return archive < m_archives.size() && SFileHasFile(m_archives[archive], Backslashes(name).c_str());
}
