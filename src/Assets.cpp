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

std::vector<std::string> FileReferences(const MpqChain& mpq, const std::string& path, const std::vector<uint8_t>& bytes,
                                        const std::function<std::string(uint32_t)>& nameOf)
{
    if (!IsNewerFormat(path, bytes)) return AssetReferences(mpq, path, bytes);
    std::vector<std::string> ignored;
    std::vector<std::string> refs = AssetReferences(mpq, path, Downport(path, bytes, nameOf, [&](const std::string& n) { return mpq.Read(n); }, ignored));
    for (uint32_t id : WxlFileIds(path, bytes))
        if (std::string n = nameOf(id); !n.empty()) refs.push_back(std::move(n));
    return refs;
}

AssetReport CopyMissingAssets(const MpqChain& mpq, const std::vector<fs::path>& adtFiles, const fs::path& outDir, bool resume, ExportTarget target)
{
    AssetReport report;
    std::set<std::string> seen;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> queue;   // files whose references still need following
    const auto nameOf = [&](uint32_t id) { std::string n = mpq.NameOf(id); std::replace(n.begin(), n.end(), '/', '\\'); return n; };
    const auto read = [&](const std::string& n) { return mpq.Read(n); };
    // WXL: models and WMO roots shipped as they are; their skins, .anim files and groups go with them unchanged.
    std::set<std::string> kept;
    auto keptWith = [&](std::string lower) {
        std::replace(lower.begin(), lower.end(), '/', '\\');
        if (lower.ends_with(".skin") && lower.size() > 7) return kept.count(lower.substr(0, lower.size() - 7) + ".m2") > 0;          // Name00.skin
        if (lower.ends_with(".anim") && lower.size() > 12) return kept.count(lower.substr(0, lower.size() - 12) + ".m2") > 0;        // Name0000-00.anim
        if (lower.ends_with(".wmo") && lower.size() > 8 && lower[lower.size() - 8] == '_') return kept.count(lower.substr(0, lower.size() - 8) + ".wmo") > 0;   // Name_000.wmo
        return false;
    };
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
        bool asIs = false;
        if (bytes && target == ExportTarget::Wxl)
        {
            const std::string lower = Catalog::Normalize(name);
            asIs = keptWith(lower);
            if (!asIs && IsNewerFormat(name, *bytes) && WxlReadsAsIs(name, *bytes, read))
            {
                asIs = true;
                std::string key = lower;
                std::replace(key.begin(), key.end(), '/', '\\');
                kept.insert(key);
                for (uint32_t id : WxlFileIds(name, *bytes))
                    if (std::string path = nameOf(id); !path.empty()) report.fileIds[id] = std::move(path);
                    else report.notes.push_back(name + ": no name for file " + std::to_string(id) + " (newer listfile needed); WXL cannot find it");
            }
            if (asIs) report.kept.push_back(name);
        }
        if (bytes && !asIs && (IsNewerFormat(name, *bytes) || Catalog::Normalize(name).ends_with(".skin")))
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
        for (const std::string& r : FileReferences(mpq, name, bytes, nameOf)) visit(r);
    }
    return report;
}

// ---- FileDataID tables (wxl-db2) ------------------------------------------------------------------------------------

namespace
{
    constexpr uint32_t kWdc1 = 0x31434457;   // 'WDC1'
    constexpr size_t kWdc1Header = 84;

    void PutU16(std::vector<uint8_t>& d, uint16_t v) { d.insert(d.end(), { uint8_t(v), uint8_t(v >> 8) }); }
    void PutU32(std::vector<uint8_t>& d, uint32_t v) { d.insert(d.end(), { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) }); }
}

std::vector<uint8_t> WriteFileIdTable(const std::map<uint32_t, std::string>& rows)
{
    // Strings: offset 0 is the empty string, each path once.
    std::vector<uint8_t> strings(1, 0);
    std::map<std::string, uint32_t> offsets;
    std::vector<uint8_t> records;
    for (const auto& [id, path] : rows)
    {
        auto [it, added] = offsets.try_emplace(path, uint32_t(strings.size()));
        if (added) { strings.insert(strings.end(), path.begin(), path.end()); strings.push_back(0); }
        PutU32(records, id);
        PutU32(records, it->second);
    }
    std::vector<uint8_t> d;
    PutU32(d, kWdc1);
    PutU32(d, uint32_t(rows.size()));                           // records
    PutU32(d, 2);                                               // fields: id, path
    PutU32(d, 8);                                               // record size
    PutU32(d, uint32_t(strings.size()));
    PutU32(d, 0); PutU32(d, 0);                                 // table and layout hash
    PutU32(d, rows.empty() ? 0 : rows.begin()->first);          // min id
    PutU32(d, rows.empty() ? 0 : rows.rbegin()->first);         // max id
    PutU32(d, 0);                                               // locale
    PutU32(d, 0);                                               // copy table size
    PutU16(d, 0);                                               // flags: fixed records, no offset map
    PutU16(d, 0);                                               // id index: field 0, inline
    PutU32(d, 2);                                               // total fields
    PutU32(d, 8);                                               // bitpacked data offset (= record size: none)
    PutU32(d, 0); PutU32(d, 0); PutU32(d, 0);                   // lookup columns, offset map, id list size
    PutU32(d, 2 * 24);                                          // field storage info size
    PutU32(d, 0); PutU32(d, 0); PutU32(d, 0);                   // common, pallet, relationship data
    for (uint16_t offset : { 0, 4 }) { PutU16(d, 0); PutU16(d, offset); }   // field structure: 32 bits each
    d.insert(d.end(), records.begin(), records.end());
    d.insert(d.end(), strings.begin(), strings.end());
    for (uint16_t bits : { 0, 32 })   // field storage info: plain 32-bit fields
    {
        PutU16(d, bits);
        PutU16(d, 32);
        for (int i = 0; i < 5; ++i) PutU32(d, 0);
    }
    return d;
}

std::optional<std::map<uint32_t, std::string>> ReadFileIdTable(const std::vector<uint8_t>& d)
{
    uint32_t magic = 0, count = 0, recordSize = 0, stringSize = 0, copySize = 0, totalFields = 0, idList = 0;
    uint16_t flags = 0;
    if (!ReadAt(d, 0, magic) || magic != kWdc1 || !ReadAt(d, 4, count) || !ReadAt(d, 12, recordSize) || !ReadAt(d, 16, stringSize) ||
        !ReadAt(d, 40, copySize) || !ReadAt(d, 44, flags) || !ReadAt(d, 48, totalFields) || !ReadAt(d, 64, idList))
        return std::nullopt;
    if (recordSize != 8 || flags || copySize || idList || totalFields != 2) return std::nullopt;
    const size_t records = kWdc1Header + size_t(totalFields) * 4, strings = records + size_t(count) * 8;
    if (strings + stringSize > d.size()) return std::nullopt;
    std::map<uint32_t, std::string> rows;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t id = 0, offset = 0;
        ReadAt(d, records + size_t(i) * 8, id);
        ReadAt(d, records + size_t(i) * 8 + 4, offset);
        if (offset >= stringSize) continue;
        const char* s = reinterpret_cast<const char*>(d.data() + strings + offset);
        if (const size_t n = strnlen(s, stringSize - offset)) rows[id] = std::string(s, n);
    }
    return rows;
}

std::vector<std::string> WriteFileIdTables(const MpqChain& mpq, const std::map<uint32_t, std::string>& fileIds, const fs::path& outDir,
                                           std::vector<std::string>& notes)
{
    std::vector<std::string> written;
    for (const char* table : { "TextureFilePath.db2", "ModelFilePath.db2" })
    {
        const bool textures = table[0] == 'T';
        std::map<uint32_t, std::string> rows;
        for (const auto& [id, path] : fileIds)
            if (Catalog::Normalize(path).ends_with(".blp") == textures) rows[id] = path;
        if (rows.empty()) continue;   // the client's own table, if any, stays as it is
        const std::string game = std::string("DBFilesClient\\") + table;
        if (mpq.HasInstalled(game))
            if (const auto bytes = mpq.Read(game))
            {
                if (const auto own = ReadFileIdTable(*bytes)) rows.insert(own->begin(), own->end());   // ours win: insert keeps them
                else notes.push_back(game + ": the client's table has another layout; the export's holds only the project's ids, so WXL loses the client's");
            }
        const std::vector<uint8_t> bytes = WriteFileIdTable(rows);
        std::error_code ec;
        fs::create_directories(outDir / "DBFilesClient", ec);
        std::ofstream(outDir / "DBFilesClient" / table, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        written.push_back(game);
    }
    return written;
}
