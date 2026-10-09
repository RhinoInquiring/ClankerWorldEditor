#include "Assets.hpp"

#include "Catalog.hpp"
#include "Downport.hpp"
#include "Formats.hpp"
#include "Models.hpp"
#include "Mpq.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{
    template <class T>
    bool ReadAt(const std::vector<uint8_t>& d, size_t off, T& out)
    {
        if (off > d.size() || d.size() - off < sizeof(T)) return false;
        std::memcpy(&out, d.data() + off, sizeof(T));
        return true;
    }

    std::string Extension(const std::string& lower)
    {
        const size_t dot = lower.find_last_of('.');
        return dot == std::string::npos ? std::string() : lower.substr(dot);
    }

    /// Zero-terminated names packed in a block (MOTX, MODN).
    void Strings(const std::vector<uint8_t>& d, size_t off, size_t size, std::vector<std::string>& out)
    {
        for (size_t p = off; p < off + size;)
        {
            const char* s = reinterpret_cast<const char*>(d.data() + p);
            const size_t n = strnlen(s, off + size - p);
            if (n) out.emplace_back(s, n);
            p += n + 1;
        }
    }
}

std::vector<std::string> AssetReferences(const MpqChain& mpq, const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::vector<std::string> refs;
    const std::string lower = Catalog::Normalize(path);
    const std::string ext = Extension(lower);
    if (ext == ".adt")
    {
        if (const auto adt = ParseAdt(bytes, false))
        {
            for (const auto& t : adt->textures)
            {
                refs.push_back(t);
                refs.push_back(t.substr(0, t.size() - 4) + "_s.blp");   // specular companion, when there is one
            }
            for (const auto& d : adt->doodads) refs.push_back(M2Name(d.model));
            for (const auto& w : adt->wmos) refs.push_back(w.model);
        }
    }
    else if (ext == ".wdt")   // a WMO-only map's building
    {
        if (const auto w = WdtGlobalWmo(bytes)) refs.push_back(w->model);
    }
    else if (ext == ".m2")
    {
        for (int i = 0; i < 4; ++i)
        {
            std::string skin = M2SkinName(path);
            skin[skin.size() - 6] = char('0' + i);   // Name00.skin .. Name03.skin
            refs.push_back(skin);
        }
        // Textures with a file name (type 0): header count/offset at 0x50, 16-byte entries.
        uint32_t count = 0, offset = 0;
        ReadAt(bytes, 0x50, count);
        ReadAt(bytes, 0x54, offset);
        for (uint32_t i = 0; i < count && i < 256; ++i)
        {
            uint32_t type = 1, flags = 0, nameLen = 0, nameOfs = 0;
            const size_t at = size_t(offset) + i * 16;
            if (!ReadAt(bytes, at, type) || !ReadAt(bytes, at + 4, flags) || !ReadAt(bytes, at + 8, nameLen) || !ReadAt(bytes, at + 12, nameOfs)) break;
            if (type == 0 && nameLen > 1 && size_t(nameOfs) + nameLen <= bytes.size())
                refs.emplace_back(reinterpret_cast<const char*>(bytes.data() + nameOfs), strnlen(reinterpret_cast<const char*>(bytes.data() + nameOfs), nameLen));
        }
        // Animations kept outside the model (sequence flag 0x20 off, not an alias 0x40): Name<id>-<variation>.anim.
        uint32_t seqCount = 0, seqOffset = 0;
        ReadAt(bytes, 0x1C, seqCount);
        ReadAt(bytes, 0x20, seqOffset);
        for (uint32_t i = 0; i < seqCount && i < 4096 && size_t(seqOffset) + (i + 1) * 64 <= bytes.size(); ++i)
        {
            uint16_t id = 0, variation = 0;
            uint32_t flags = 0;
            ReadAt(bytes, size_t(seqOffset) + i * 64, id);
            ReadAt(bytes, size_t(seqOffset) + i * 64 + 2, variation);
            ReadAt(bytes, size_t(seqOffset) + i * 64 + 12, flags);
            if (flags & 0x60) continue;
            char suffix[24];
            snprintf(suffix, sizeof suffix, "%04u-%02u.anim", id, variation);
            refs.push_back(path.substr(0, path.size() - 3) + suffix);
        }
    }
    else if (ext == ".wmo")
    {
        uint32_t groups = 0;
        float bounds[6];
        if (WmoRootInfo(bytes, groups, bounds))   // a root: its groups, textures, doodads and skybox
        {
            for (uint32_t g = 0; g < groups; ++g) refs.push_back(WmoGroupFile(path, bytes, g));
            std::vector<std::string> names;
            for (size_t pos = 12; pos + 8 <= bytes.size();)   // after MVER
            {
                uint32_t magic = 0, size = 0;
                ReadAt(bytes, pos, magic);
                ReadAt(bytes, pos + 4, size);
                if (size > bytes.size() - pos - 8) break;
                const char tag[5] = { char(magic >> 24), char(magic >> 16), char(magic >> 8), char(magic), 0 };
                if (!std::strcmp(tag, "MOTX") || !std::strcmp(tag, "MOSB")) Strings(bytes, pos + 8, size, refs);
                if (!std::strcmp(tag, "MODN")) Strings(bytes, pos + 8, size, names);
                pos += 8 + size;
            }
            for (const auto& n : names) refs.push_back(M2Name(n));
        }
    }
    (void)mpq;
    return refs;
}

AssetReport CopyMissingAssets(const MpqChain& mpq, const std::vector<fs::path>& adtFiles, const fs::path& outDir, bool resume)
{
    AssetReport report;
    std::set<std::string> seen;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> queue;   // files whose references still need following
    auto visit = [&](const std::string& name) {
        if (name.empty() || !seen.insert(Catalog::Normalize(name)).second) return;
        if (mpq.HasInstalled(name)) return;   // players have it (and everything it refers to)
        if (resume)
        {
            std::string rel = name;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            std::error_code ec;
            if (const fs::path done = outDir / fs::path(rel); fs::is_regular_file(done, ec))
            {
                queue.push_back({ name, ReadFileBytes(done).value_or(std::vector<uint8_t>{}) });
                return;
            }
        }
        auto bytes = mpq.Read(name);    // from another client
        if (bytes && (IsNewerFormat(name, *bytes) || Catalog::Normalize(name).ends_with(".skin")))
        {
            const size_t notesBefore = report.notes.size();
            auto converted = Downport(name, *bytes, [&](uint32_t id) { return mpq.NameOf(id); },
                                      [&](const std::string& n) { return mpq.Read(n); }, report.notes);
            if (converted != *bytes) report.converted.push_back(name);
            if (converted.empty()) { report.missing.push_back(name + " (cannot convert: " + (report.notes.size() > notesBefore ? report.notes.back() : "unknown") + ")"); return; }
            bytes = std::move(converted);
        }
        const bool optional = name.size() > 6 && Catalog::Normalize(name).ends_with("_s.blp");
        if (!bytes)
        {
            const std::string lower = Catalog::Normalize(name);
            const bool optionalSkin = lower.ends_with("01.skin") || lower.ends_with("02.skin") || lower.ends_with("03.skin") || lower.ends_with(".anim");
            if (!optional && !optionalSkin) report.missing.push_back(name);
            return;
        }
        std::string rel = name;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const fs::path out = outDir / fs::path(rel);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
        report.copied.push_back(name);
        queue.push_back({ name, std::move(*bytes) });
    };

    for (const fs::path& adt : adtFiles)
    {
        const std::vector<uint8_t> bytes = ReadFileBytes(adt).value_or(std::vector<uint8_t>{});
        for (const std::string& r : AssetReferences(mpq, adt.filename().string(), bytes)) visit(r);
    }
    while (!queue.empty())
    {
        auto [name, bytes] = std::move(queue.front());
        queue.pop_front();
        for (const std::string& r : AssetReferences(mpq, name, bytes)) visit(r);
    }
    return report;
}
