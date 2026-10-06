#include "Formats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <map>

namespace
{
    constexpr uint32_t Tag(const char (&s)[5])
    {
        return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 |
               uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
    }

    template <class T>
    bool ReadAt(const std::vector<uint8_t>& d, size_t off, T& out)
    {
        if (off > d.size() || d.size() - off < sizeof(T)) return false;
        std::memcpy(&out, d.data() + off, sizeof(T));
        return true;
    }

    /// Calls fn(magic, bodyOffset, bodySize) for each top-level chunk, stopping at the first bad one.
    template <class Fn>
    void ForEachChunk(const std::vector<uint8_t>& d, size_t begin, size_t end, Fn&& fn)
    {
        size_t pos = begin;
        while (pos + 8 <= end)
        {
            uint32_t magic = 0, size = 0;
            ReadAt(d, pos, magic);
            ReadAt(d, pos + 4, size);
            if (size > end - pos - 8) break;
            fn(magic, pos + 8, size_t(size));
            pos += 8 + size;
        }
    }

    std::string CString(const std::vector<uint8_t>& d, size_t blockOff, size_t blockSize, uint32_t off)
    {
        if (off >= blockSize) return {};
        const char* p = reinterpret_cast<const char*>(d.data() + blockOff + off);
        return std::string(p, strnlen(p, blockSize - off));
    }

    struct McnkHeader
    {
        uint32_t flags, indexX, indexY, nLayers, nDoodadRefs, ofsHeight, ofsNormal, ofsLayer, ofsRefs,
                 ofsAlpha, sizeAlpha, ofsShadow, sizeShadow, areaId, nMapObjRefs;
        uint16_t holes, pad;
        uint8_t lowQualityTextureMap[16];
        uint32_t predTex, noEffectDoodad, ofsSndEmitters, nSndEmitters, ofsLiquid, sizeLiquid;
        float position[3];
        uint32_t ofsMccv, ofsMclv, unused;
    };
    static_assert(sizeof(McnkHeader) == 128);

    struct MclyEntry { uint32_t textureId, flags, offsetInMcal, effectId; };
    constexpr uint32_t kMclyUseAlpha = 0x100, kMclyCompressed = 0x200;
    constexpr uint32_t kMcnkDoNotFixAlpha = 0x8000;

    struct MddfEntry { uint32_t nameId, uniqueId; float pos[3], rot[3]; uint16_t scale, flags; };
    struct ModfEntry { uint32_t nameId, uniqueId; float pos[3], rot[3], ext[6]; uint16_t flags, doodadSet, nameSet, pad; };
    static_assert(sizeof(MddfEntry) == 36 && sizeof(ModfEntry) == 64);

    /// Offset of the data of the sub-chunk at chunkStart + ofs, when its magic matches.
    std::optional<std::pair<size_t, size_t>> SubChunk(const std::vector<uint8_t>& d, size_t chunkStart,
                                                      size_t chunkEnd, uint32_t ofs, uint32_t magic)
    {
        uint32_t m = 0, size = 0;
        const size_t at = chunkStart + ofs;
        if (ofs == 0 || at + 8 > chunkEnd || !ReadAt(d, at, m) || !ReadAt(d, at + 4, size) || m != magic) return std::nullopt;
        if (size > chunkEnd - at - 8) size = uint32_t(chunkEnd - at - 8);
        return std::make_pair(at + 8, size_t(size));
    }

    bool IsMcnkSubTag(uint32_t m)
    {
        for (uint32_t t : { Tag("MCVT"), Tag("MCCV"), Tag("MCLV"), Tag("MCNR"), Tag("MCLY"), Tag("MCRF"), Tag("MCSH"), Tag("MCAL"), Tag("MCLQ"), Tag("MCSE") })
            if (m == t) return true;
        return false;
    }

    /// One sub-chunk of an MCNK: its magic, where its 8-byte header starts and where the next one starts.
    struct SubSpan { uint32_t tag; size_t start, end; };

    /// The MCNK's sub-chunks the way the 3.3.5 client reads them: one after another from the end of the 128-byte
    /// header, each advancing by its size field (the client never uses the header's ofs* fields). Two stock quirks:
    /// 13 padding bytes follow MCNR's 435; an MCAL whose size field is short (compressed-alpha tiles say 512) runs
    /// for the header's sizeAlpha; an MCLQ whose size field is 0 runs for the header's sizeLiquid.
    /// `start` = offset of the "MCNK" magic. Stops at the first span that does not lead to a sub-chunk or the end.
    std::vector<SubSpan> WalkSubChunks(const std::vector<uint8_t>& d, size_t start, size_t chunkEnd, uint32_t sizeAlpha, uint32_t sizeLiquid)
    {
        std::vector<SubSpan> spans;
        auto tagAt = [&](size_t at) { uint32_t m = 0; return at + 8 <= chunkEnd && ReadAt(d, at, m) ? m : 0u; };
        size_t q = start + 8 + sizeof(McnkHeader);
        while (q + 8 <= chunkEnd)
        {
            const uint32_t tag = tagAt(q);
            uint32_t size = 0;
            ReadAt(d, q + 4, size);
            if (!IsMcnkSubTag(tag) || size > chunkEnd - q - 8) break;
            size_t next = q + 8 + size;
            if (next < chunkEnd && !IsMcnkSubTag(tagAt(next)))
            {
                if (tag == Tag("MCNR") && IsMcnkSubTag(tagAt(next + 13))) next += 13;
                else if (tag == Tag("MCAL") && sizeAlpha > 8 && q + sizeAlpha <= chunkEnd) next = q + sizeAlpha;
                else if (tag == Tag("MCLQ") && sizeLiquid > 8 && q + sizeLiquid <= chunkEnd) next = q + sizeLiquid;
            }
            if (tag == Tag("MCNR") && next + 13 == chunkEnd) next = chunkEnd;   // padding at the very end
            spans.push_back({ tag, q, next });
            q = next;
        }
        return spans;
    }

    /// MCNK header field holding each sub-chunk's offset (from the "MCNK" magic).
    constexpr std::pair<uint32_t, size_t> kSubOffsetFields[] = {
        { Tag("MCVT"), offsetof(McnkHeader, ofsHeight) }, { Tag("MCNR"), offsetof(McnkHeader, ofsNormal) },
        { Tag("MCLY"), offsetof(McnkHeader, ofsLayer) },  { Tag("MCRF"), offsetof(McnkHeader, ofsRefs) },
        { Tag("MCAL"), offsetof(McnkHeader, ofsAlpha) },  { Tag("MCSH"), offsetof(McnkHeader, ofsShadow) },
        { Tag("MCSE"), offsetof(McnkHeader, ofsSndEmitters) }, { Tag("MCLQ"), offsetof(McnkHeader, ofsLiquid) },
        { Tag("MCCV"), offsetof(McnkHeader, ofsMccv) },   { Tag("MCLV"), offsetof(McnkHeader, ofsMclv) } };

    /// One layer's 64x64 alpha from MCAL into channel `channel` of the RGBA map.
    void DecodeAlpha(const uint8_t* src, size_t avail, uint32_t flags, bool bigAlpha, uint8_t* rgba, int channel)
    {
        uint8_t a[4096] = {};
        if (flags & kMclyCompressed)
        {
            size_t in = 0, out = 0;
            while (out < 4096 && in < avail)
            {
                const uint8_t b = src[in++];
                const size_t count = std::min<size_t>(b & 0x7F, 4096 - out);
                if (b & 0x80)
                {
                    if (in >= avail) break;
                    std::fill_n(a + out, count, src[in++]);
                }
                else
                {
                    const size_t n = std::min(count, avail - in);
                    std::memcpy(a + out, src + in, n);
                    in += n;
                }
                out += count;
            }
        }
        else if (bigAlpha)
            std::memcpy(a, src, std::min<size_t>(avail, 4096));
        else
            for (size_t i = 0; i < 2048 && i < avail; ++i)
            {
                a[i * 2]     = uint8_t((src[i] & 0x0F) * 17);
                a[i * 2 + 1] = uint8_t((src[i] >> 4) * 17);
            }
        for (int i = 0; i < 4096; ++i) rgba[i * 4 + channel] = a[i];
    }

    /// 4-bit maps hold 63x63 meaningful texels; unless the chunk says otherwise, the last row and
    /// column repeat their neighbours.
    void FixAlphaEdge(uint8_t* rgba)
    {
        for (int i = 0; i < 64; ++i)
            for (int c = 0; c < 4; ++c)
            {
                rgba[(i * 64 + 63) * 4 + c] = rgba[(i * 64 + 62) * 4 + c];
                rgba[(63 * 64 + i) * 4 + c] = rgba[(62 * 64 + i) * 4 + c];
            }
    }

    /// MH2O: 256 chunk headers {instances offset, count, attributes offset}; offsets are relative to MH2O's data.
    void ParseMh2o(const std::vector<uint8_t>& d, size_t base, size_t size, Adt& adt)
    {
        struct Instance { uint16_t type, format; float minHeight, maxHeight; uint8_t x, y, w, h; uint32_t ofsExists, ofsVertices; };
        for (const AdtChunk& c : adt.chunks)
        {
            const size_t header = base + size_t(c.indexY * 16 + c.indexX) * 12;
            uint32_t ofsInstances = 0, count = 0, ofsAttributes = 0;
            if (header + 12 > base + size || !ReadAt(d, header, ofsInstances) || !ReadAt(d, header + 4, count)) continue;
            uint64_t fishable = ~0ull, deep = 0;
            if (ReadAt(d, header + 8, ofsAttributes) && ofsAttributes)
            {
                ReadAt(d, base + ofsAttributes, fishable);
                ReadAt(d, base + ofsAttributes + 8, deep);
            }
            for (uint32_t i = 0; i < count && ofsInstances; ++i)
            {
                Instance in{};
                if (!ReadAt(d, base + ofsInstances + size_t(i) * sizeof(Instance), in) || in.w == 0 || in.h == 0 || in.x + in.w > 8 || in.y + in.h > 8) continue;
                AdtLiquid l;
                l.type = in.type;
                l.format = in.format;
                l.cornerX = c.baseX;
                l.cornerZ = c.baseZ;
                l.x = in.x; l.y = in.y; l.w = in.w; l.h = in.h;
                const size_t verts = size_t(in.w + 1) * (in.h + 1);
                l.heights.assign(verts, in.minHeight);
                if (in.ofsVertices && in.format != 2)   // formats 0, 1, 3 start with heights
                    for (size_t v = 0; v < verts; ++v) ReadAt(d, base + in.ofsVertices + v * 4, l.heights[v]);
                const size_t heightBytes = in.format != 2 ? verts * 4 : 0, extra = verts * LiquidExtraPerVertex(in.format);
                if (in.ofsVertices && base + in.ofsVertices + heightBytes + extra <= d.size())
                    l.extra.assign(d.begin() + std::ptrdiff_t(base + in.ofsVertices + heightBytes),
                                   d.begin() + std::ptrdiff_t(base + in.ofsVertices + heightBytes + extra));
                l.fishable = fishable;
                l.deep = deep;
                l.exists.assign(size_t(in.w) * in.h, true);
                if (in.ofsExists)
                    for (size_t b = 0; b < l.exists.size(); ++b)
                    {
                        uint8_t byte = 0;
                        ReadAt(d, base + in.ofsExists + b / 8, byte);
                        l.exists[b] = (byte >> (b % 8)) & 1;
                    }
                adt.liquids.push_back(std::move(l));
            }
        }
    }

    void ParseMcnk(const std::vector<uint8_t>& d, size_t start, size_t end, bool bigAlpha, Adt& adt)
    {
        McnkHeader h{};
        if (!ReadAt(d, start + 8, h)) return;

        AdtChunk c;
        c.baseX = kZeroPoint - h.position[1];
        c.baseZ = kZeroPoint - h.position[0];
        c.baseY = h.position[2];
        c.mcnkOffset = start;
        c.indexX = h.indexX;
        c.indexY = h.indexY;
        c.holes = h.holes;
        c.areaId = h.areaId;

        if (auto mcvt = SubChunk(d, start, end, h.ofsHeight, Tag("MCVT")); mcvt && mcvt->second >= 145 * 4)
        {
            std::memcpy(c.heights.data(), d.data() + mcvt->first, 145 * 4);
            c.mcvtOffset = mcvt->first;
        }
        if (auto mcnr = SubChunk(d, start, end, h.ofsNormal, Tag("MCNR")); mcnr && mcnr->second >= 145 * 3)
        {
            c.mcnrOffset = mcnr->first;
        }

        c.alpha.assign(64 * 64 * 4, 0);
        auto mcly = SubChunk(d, start, end, h.ofsLayer, Tag("MCLY"));
        auto mcal = SubChunk(d, start, end, h.ofsAlpha, Tag("MCAL"));
        if (mcal && h.sizeAlpha > 8)   // the client trusts sizeAlpha over MCAL's own (sometimes short) size field
            mcal->second = std::max(mcal->second, std::min<size_t>(h.sizeAlpha - 8, end - mcal->first));
        const uint32_t layers = mcly ? std::min<uint32_t>({ h.nLayers, 4u, uint32_t(mcly->second / sizeof(MclyEntry)) }) : 0;
        for (uint32_t i = 0; i < layers; ++i)
        {
            MclyEntry e{};
            ReadAt(d, mcly->first + i * sizeof(MclyEntry), e);
            c.textureIds[i] = e.textureId;
            c.layerFlags[i] = e.flags;
            c.effectIds[i] = e.effectId;
            if (i == 0 || !(e.flags & kMclyUseAlpha) || !mcal || e.offsetInMcal >= mcal->second) continue;
            DecodeAlpha(d.data() + mcal->first + e.offsetInMcal, mcal->second - e.offsetInMcal, e.flags, bigAlpha,
                        c.alpha.data(), int(i - 1));
        }
        c.layerCount = layers;
        if (auto mcrf = SubChunk(d, start, end, h.ofsRefs, Tag("MCRF")))
            for (uint32_t k = 0; k < h.nDoodadRefs + h.nMapObjRefs && (k + 1) * 4 <= mcrf->second; ++k)
            {
                uint32_t v = 0;
                ReadAt(d, mcrf->first + k * 4, v);
                (k < h.nDoodadRefs ? c.doodadRefs : c.wmoRefs).push_back(v);
            }
        if (!bigAlpha && !(h.flags & kMcnkDoNotFixAlpha)) FixAlphaEdge(c.alpha.data());

        // Old-style liquid (MCLQ, pre-MH2O; ParseAdt drops it when the tile has MH2O, as the client does). Flags
        // 0x4 river, 0x8 ocean, 0x10 magma, 0x20 slime. Data: min/max height, 9 x 9 vertices of 8 bytes (height
        // last), 8 x 8 tile bytes (low nibble 0xF = nothing there).
        if (const uint32_t kinds = h.flags & 0x3C; kinds && h.sizeLiquid > 8)
            if (auto mclq = SubChunk(d, start, end, h.ofsLiquid, Tag("MCLQ")))
            {
                const size_t data = mclq->first, need = 8 + 81 * 8 + 64;
                if (data + need <= end)
                {
                    AdtLiquid l;
                    l.type = kinds & 0x4 ? 1 : kinds & 0x8 ? 2 : kinds & 0x10 ? 3 : 4;   // LiquidType: water, ocean, magma, slime
                    l.cornerX = c.baseX;
                    l.cornerZ = c.baseZ;
                    l.w = l.h = 8;
                    l.fromMclq = true;
                    l.heights.resize(81);
                    for (size_t v = 0; v < 81; ++v) ReadAt(d, data + 8 + v * 8 + 4, l.heights[v]);
                    // The 4 bytes before each height: water {depth, flow, flow, filler}, magma and slime {u16 s, t}.
                    l.format = l.type >= 3 ? 1 : 0;
                    for (size_t v = 0; v < 81; ++v)
                    {
                        const uint8_t* p = d.data() + data + 8 + v * 8;
                        if (l.format == 1) l.extra.insert(l.extra.end(), p, p + 4);
                        else l.extra.push_back(p[0]);
                    }
                    l.exists.resize(64);
                    for (size_t t = 0; t < 64; ++t) l.exists[t] = (d[data + 8 + 81 * 8 + t] & 0x0F) != 0x0F;
                    adt.liquids.push_back(std::move(l));
                }
            }
        adt.chunks.push_back(std::move(c));
    }
}

std::optional<Adt> ParseAdt(const std::vector<uint8_t>& d, bool bigAlpha)
{
    Adt adt;
    size_t mmdxOff = 0, mmdxSize = 0, mwmoOff = 0, mwmoSize = 0;
    std::vector<uint32_t> mmid, mwid;
    std::vector<MddfEntry> mddf;
    std::vector<ModfEntry> modf;
    size_t mh2oOff = 0, mh2oSize = 0;

    ForEachChunk(d, 0, d.size(), [&](uint32_t magic, size_t off, size_t size) {
        switch (magic)
        {
        case Tag("MTEX"):
            for (size_t p = off; p < off + size;)
            {
                std::string name = CString(d, off, size, uint32_t(p - off));
                p += name.size() + 1;
                if (!name.empty()) adt.textures.push_back(std::move(name));
            }
            break;
        case Tag("MMDX"): mmdxOff = off; mmdxSize = size; break;
        case Tag("MWMO"): mwmoOff = off; mwmoSize = size; break;
        case Tag("MMID"): mmid.resize(size / 4); std::memcpy(mmid.data(), d.data() + off, mmid.size() * 4); break;
        case Tag("MWID"): mwid.resize(size / 4); std::memcpy(mwid.data(), d.data() + off, mwid.size() * 4); break;
        case Tag("MDDF"): mddf.resize(size / sizeof(MddfEntry)); std::memcpy(mddf.data(), d.data() + off, mddf.size() * sizeof(MddfEntry)); break;
        case Tag("MODF"): modf.resize(size / sizeof(ModfEntry)); std::memcpy(modf.data(), d.data() + off, modf.size() * sizeof(ModfEntry)); break;
        case Tag("MCNK"): ParseMcnk(d, off - 8, off + size, bigAlpha, adt); break;
        case Tag("MH2O"): mh2oOff = off; mh2oSize = size; break;
        default: break;
        }
    });
    if (adt.chunks.empty()) return std::nullopt;
    if (mh2oSize)
    {
        std::erase_if(adt.liquids, [](const AdtLiquid& l) { return l.fromMclq; });   // MH2O replaces MCLQ
        ParseMh2o(d, mh2oOff, mh2oSize, adt);
    }

    for (const auto& e : mddf)
    {
        DoodadPlacement p{ e.nameId < mmid.size() ? CString(d, mmdxOff, mmdxSize, mmid[e.nameId]) : std::string(),
                           { e.pos[0], e.pos[1], e.pos[2] }, { e.rot[0], e.rot[1], e.rot[2] }, e.scale / 1024.0f, e.uniqueId, e.flags };
        adt.doodads.push_back(std::move(p));
    }
    for (const auto& e : modf)
    {
        WmoPlacement p{ e.nameId < mwid.size() ? CString(d, mwmoOff, mwmoSize, mwid[e.nameId]) : std::string(),
                        { e.pos[0], e.pos[1], e.pos[2] }, { e.rot[0], e.rot[1], e.rot[2] },
                        { e.ext[0], e.ext[1], e.ext[2] }, { e.ext[3], e.ext[4], e.ext[5] }, e.uniqueId, e.doodadSet, e.flags, e.nameSet };
        adt.wmos.push_back(std::move(p));
    }
    return adt;
}

bool WdtBigAlpha(const std::vector<uint8_t>& wdt)
{
    bool big = false;
    ForEachChunk(wdt, 0, wdt.size(), [&](uint32_t magic, size_t off, size_t size) {
        uint32_t flags = 0;
        if (magic == Tag("MPHD") && size >= 4 && ReadAt(wdt, off, flags)) big = (flags & (0x4 | 0x80)) != 0;
    });
    return big;
}

bool WdtHasTile(const std::vector<uint8_t>& wdt, int x, int y)
{
    bool has = false;
    if (x < 0 || x > 63 || y < 0 || y > 63) return false;
    ForEachChunk(wdt, 0, wdt.size(), [&](uint32_t magic, size_t off, size_t size) {
        uint32_t flags = 0;
        if (magic == Tag("MAIN") && size >= 64 * 64 * 8 && ReadAt(wdt, off + size_t(y * 64 + x) * 8, flags)) has = (flags & 1) != 0;
    });
    return has;
}

std::optional<WmoPlacement> WdtGlobalWmo(const std::vector<uint8_t>& wdt)
{
    uint32_t flags = 0;
    std::string name;
    std::optional<ModfEntry> modf;
    ForEachChunk(wdt, 0, wdt.size(), [&](uint32_t magic, size_t off, size_t size) {
        if (magic == Tag("MPHD") && size >= 4) ReadAt(wdt, off, flags);
        else if (magic == Tag("MWMO") && size) name = CString(wdt, off, size, 0);
        else if (magic == Tag("MODF") && size >= sizeof(ModfEntry))
        {
            modf.emplace();
            std::memcpy(&*modf, wdt.data() + off, sizeof(ModfEntry));
        }
    });
    if (!(flags & 0x1) || name.empty() || !modf) return std::nullopt;
    // A WDT's MODF counts from the middle of the map, an ADT's from its corner: shift it into the ADTs' space.
    const ModfEntry& e = *modf;
    return WmoPlacement{ name, { e.pos[0] + kZeroPoint, e.pos[1], e.pos[2] + kZeroPoint }, { e.rot[0], e.rot[1], e.rot[2] },
                         { e.ext[0] + kZeroPoint, e.ext[1], e.ext[2] + kZeroPoint }, { e.ext[3] + kZeroPoint, e.ext[4], e.ext[5] + kZeroPoint },
                         e.uniqueId, e.doodadSet, e.flags, e.nameSet };
}

std::vector<bool> WdtTiles(const std::vector<uint8_t>& wdt)
{
    std::vector<bool> tiles(64 * 64, false);
    ForEachChunk(wdt, 0, wdt.size(), [&](uint32_t magic, size_t off, size_t size) {
        if (magic != Tag("MAIN") || size < 64 * 64 * 8) return;
        for (size_t i = 0; i < 64 * 64; ++i)
        {
            uint32_t flags = 0;
            ReadAt(wdt, off + i * 8, flags);
            tiles[i] = (flags & 1) != 0;
        }
    });
    return tiles;
}

bool SetUniqueIds(std::vector<uint8_t>& adt, const std::vector<uint32_t>& doodads, const std::vector<uint32_t>& wmos)
{
    bool ok = true;
    ForEachChunk(adt, 0, adt.size(), [&](uint32_t magic, size_t off, size_t size) {
        const bool d = magic == Tag("MDDF"), w = magic == Tag("MODF");
        if (!d && !w) return;
        const size_t entry = d ? sizeof(MddfEntry) : sizeof(ModfEntry);
        const auto& ids = d ? doodads : wmos;
        if (size / entry != ids.size()) { ok = false; return; }
        for (size_t i = 0; i < ids.size(); ++i) std::memcpy(adt.data() + off + i * entry + 4, &ids[i], 4);   // after the name index
    });
    return ok;
}

std::vector<uint8_t> WdtSetTile(std::vector<uint8_t> wdt, int x, int y, bool present)
{
    bool done = false;
    ForEachChunk(wdt, 0, wdt.size(), [&](uint32_t magic, size_t off, size_t size) {
        if (done || magic != Tag("MAIN") || size < 64 * 64 * 8) return;
        uint32_t flags = 0;
        const size_t at = off + size_t(y * 64 + x) * 8;
        ReadAt(wdt, at, flags);
        flags = present ? flags | 1u : flags & ~1u;
        std::memcpy(wdt.data() + at, &flags, 4);
        done = true;
    });
    return done ? wdt : std::vector<uint8_t>{};
}

std::vector<MapEntry> ParseMapDbc(const std::vector<uint8_t>& d)
{
    struct Header { char magic[4]; uint32_t records, fields, recordSize, stringSize; };
    Header h{};
    std::vector<MapEntry> maps;
    if (!ReadAt(d, 0, h) || std::memcmp(h.magic, "WDBC", 4) != 0 || h.fields < 6) return maps;
    const size_t strings = 20 + size_t(h.records) * h.recordSize;
    if (strings + h.stringSize > d.size()) return maps;
    for (uint32_t i = 0; i < h.records; ++i)
    {
        const size_t row = 20 + size_t(i) * h.recordSize;
        uint32_t id = 0, dirOff = 0, nameOff = 0;
        ReadAt(d, row, id);
        ReadAt(d, row + 4, dirOff);
        ReadAt(d, row + 5 * 4, nameOff);
        maps.push_back({ id, CString(d, strings, h.stringSize, dirOff), CString(d, strings, h.stringSize, nameOff) });
    }
    return maps;
}

std::vector<uint8_t> BlpPixels(const BlpImage& b)
{
    if (b.mips.empty() || !b.width || !b.height) return {};
    std::vector<uint8_t> out(size_t(b.width) * b.height * 4, 255);
    const std::vector<uint8_t>& m = b.mips[0];
    if (b.format == BlpImage::Format::RGBA8) return m.size() >= out.size() ? std::vector<uint8_t>(m.begin(), m.begin() + std::ptrdiff_t(out.size())) : out;
    const size_t block = b.format == BlpImage::Format::BC1 ? 8 : 16;
    auto rgb565 = [](uint16_t c) { return std::array<int, 3>{ (c >> 11) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31 }; };
    const uint32_t bw = (b.width + 3) / 4, bh = (b.height + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const size_t start = (size_t(by) * bw + bx) * block, at = start + (block - 8);   // the colour half comes last
            if (at + 8 > m.size()) continue;
            const uint16_t c0 = uint16_t(m[at] | m[at + 1] << 8), c1 = uint16_t(m[at + 2] | m[at + 3] << 8);
            const auto a = rgb565(c0), z = rgb565(c1);
            const bool four = c0 > c1 || block == 16;   // BC1 with c0 <= c1: three colours and transparent black
            std::array<std::array<int, 4>, 4> pal{};
            for (size_t k = 0; k < 3; ++k)
            {
                pal[0][k] = a[k];
                pal[1][k] = z[k];
                pal[2][k] = four ? (2 * a[k] + z[k]) / 3 : (a[k] + z[k]) / 2;
                pal[3][k] = four ? (a[k] + 2 * z[k]) / 3 : 0;
            }
            for (auto& c : pal) c[3] = 255;
            if (!four) pal[3][3] = 0;
            // Alpha: BC2 4 bits per texel; BC3 two end points and 3-bit indices.
            uint8_t alpha[16];
            std::fill(std::begin(alpha), std::end(alpha), uint8_t(255));
            if (b.format == BlpImage::Format::BC2)
                for (int t = 0; t < 16; ++t) alpha[t] = uint8_t(((m[start + size_t(t / 2)] >> ((t & 1) * 4)) & 15) * 17);
            else if (b.format == BlpImage::Format::BC3)
            {
                const int a0 = m[start], a1 = m[start + 1];
                int ap[8] = { a0, a1 };
                for (int k = 2; k < 8; ++k)
                    ap[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : k == 6 ? 0 : 255;
                uint64_t bits = 0;
                for (int k = 0; k < 6; ++k) bits |= uint64_t(m[start + 2 + size_t(k)]) << (8 * k);
                for (int t = 0; t < 16; ++t) alpha[t] = uint8_t(ap[(bits >> (3 * t)) & 7]);
            }
            const uint32_t bits = uint32_t(m[at + 4] | m[at + 5] << 8 | m[at + 6] << 16 | uint32_t(m[at + 7]) << 24);
            for (uint32_t py = 0; py < 4; ++py)
                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= b.width || y >= b.height) continue;
                    const int t = int(py * 4 + px);
                    const auto& c = pal[(bits >> (2 * t)) & 3];
                    uint8_t* o = &out[(size_t(y) * b.width + x) * 4];
                    o[0] = uint8_t(c[0]);
                    o[1] = uint8_t(c[1]);
                    o[2] = uint8_t(c[2]);
                    o[3] = uint8_t(block == 8 ? c[3] : alpha[t]);
                }
        }
    return out;
}

std::optional<BlpImage> ParseBlp(const std::vector<uint8_t>& d)
{
    struct Header
    {
        char magic[4];
        uint32_t type;
        uint8_t compression, alphaDepth, alphaType, hasMips;
        uint32_t width, height, mipOffsets[16], mipSizes[16];
    };
    static_assert(sizeof(Header) == 148);
    Header h{};
    if (!ReadAt(d, 0, h) || std::memcmp(h.magic, "BLP2", 4) != 0 || h.width == 0 || h.height == 0) return std::nullopt;

    BlpImage img;
    img.width = h.width;
    img.height = h.height;
    uint32_t palette[256] = {};
    if (h.compression == 1 && d.size() >= 148 + 1024) std::memcpy(palette, d.data() + 148, 1024);

    if (h.compression == 2)
        img.format = h.alphaType == 7 ? BlpImage::Format::BC3 : h.alphaType == 1 ? BlpImage::Format::BC2 : BlpImage::Format::BC1;
    else if (h.compression != 1 && h.compression != 3)
        return std::nullopt;

    const uint32_t blockBytes = img.format == BlpImage::Format::BC1 ? 8 : 16;
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t w = std::max(1u, h.width >> i), hh = std::max(1u, h.height >> i);
        const uint32_t off = h.mipOffsets[i], size = h.mipSizes[i];
        if (off == 0 || size == 0 || off > d.size() || size > d.size() - off) break;
        const uint8_t* src = d.data() + off;

        if (h.compression == 2)
        {
            const size_t need = size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (hh + 3) / 4) * blockBytes;
            if (size < need) break;
            img.mips.emplace_back(src, src + need);
        }
        else if (h.compression == 3)
        {
            if (size < size_t(w) * hh * 4) break;
            std::vector<uint8_t> rgba(size_t(w) * hh * 4);
            for (size_t p = 0; p < size_t(w) * hh; ++p)   // BGRA -> RGBA
            {
                rgba[p * 4 + 0] = src[p * 4 + 2];
                rgba[p * 4 + 1] = src[p * 4 + 1];
                rgba[p * 4 + 2] = src[p * 4 + 0];
                rgba[p * 4 + 3] = src[p * 4 + 3];
            }
            img.mips.push_back(std::move(rgba));
        }
        else
        {
            const size_t px = size_t(w) * hh;
            const size_t alphaBytes = (px * h.alphaDepth + 7) / 8;
            if (size < px + alphaBytes) break;
            std::vector<uint8_t> rgba(px * 4);
            for (size_t p = 0; p < px; ++p)
            {
                const uint32_t c = palette[src[p]];   // BGRA
                rgba[p * 4 + 0] = uint8_t(c >> 16);
                rgba[p * 4 + 1] = uint8_t(c >> 8);
                rgba[p * 4 + 2] = uint8_t(c);
                const uint8_t* a = src + px;
                switch (h.alphaDepth)
                {
                case 1: rgba[p * 4 + 3] = (a[p / 8] >> (p % 8)) & 1 ? 255 : 0; break;
                case 4: rgba[p * 4 + 3] = uint8_t(((a[p / 2] >> ((p % 2) * 4)) & 0xF) * 17); break;
                case 8: rgba[p * 4 + 3] = a[p]; break;
                default: rgba[p * 4 + 3] = 255; break;
                }
            }
            img.mips.push_back(std::move(rgba));
        }
        if (!h.hasMips || (w == 1 && hh == 1)) break;
    }
    if (img.mips.empty()) return std::nullopt;
    return img;
}

namespace
{
    void PutU32(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); }
    void SetU32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); }

    void PutChunk(std::vector<uint8_t>& v, uint32_t magic, const std::vector<uint8_t>& body)
    {
        PutU32(v, magic);
        PutU32(v, uint32_t(body.size()));
        v.insert(v.end(), body.begin(), body.end());
    }

    /// One layer's alpha (a channel of the RGBA map) in the map's MCAL encoding, uncompressed.
    void EncodeAlpha(const std::vector<uint8_t>& rgba, int channel, bool bigAlpha, std::vector<uint8_t>& out)
    {
        if (bigAlpha)
            for (int i = 0; i < 4096; ++i) out.push_back(rgba[size_t(i) * 4 + channel]);
        else
            for (int i = 0; i < 4096; i += 2)
            {
                const auto nibble = [&](int p) { return uint8_t((rgba[size_t(p) * 4 + channel] * 15 + 127) / 255); };
                out.push_back(uint8_t(nibble(i) | nibble(i + 1) << 4));
            }
    }
}

size_t LiquidExtraPerVertex(uint16_t format)
{
    switch (format)
    {
    case 0: return 1;   // depth
    case 1: return 4;   // uv
    case 2: return 1;   // depth only (no heights)
    case 3: return 5;   // uv, then depth
    default: return 0;
    }
}

std::vector<uint8_t> WriteMh2o(const Adt& adt)
{
    // 256 headers {instances, count, attributes}, then per chunk with liquid: attributes, instances, and each
    // instance's existence bits and vertex data. Offsets are relative to the start of the body.
    std::vector<uint8_t> out(256 * 12, 0);
    auto align = [&] { while (out.size() % 4) out.push_back(0); };
    for (const AdtChunk& c : adt.chunks)
    {
        std::vector<const AdtLiquid*> here;
        for (const AdtLiquid& l : adt.liquids)
            if (l.w && l.h && std::fabs(l.cornerX - c.baseX) < 1.0f && std::fabs(l.cornerZ - c.baseZ) < 1.0f) here.push_back(&l);
        if (here.empty() || c.indexX > 15 || c.indexY > 15) continue;
        uint64_t fishable = 0, deep = 0;
        for (const AdtLiquid* l : here) { fishable |= l->fishable; deep |= l->deep; }
        const uint32_t attributes = uint32_t(out.size());
        PutU32(out, uint32_t(fishable)); PutU32(out, uint32_t(fishable >> 32));
        PutU32(out, uint32_t(deep)); PutU32(out, uint32_t(deep >> 32));
        const size_t instances = out.size();
        out.resize(out.size() + here.size() * 24);
        for (size_t i = 0; i < here.size(); ++i)
        {
            const AdtLiquid& l = *here[i];
            const size_t verts = size_t(l.w + 1) * (l.h + 1), cells = size_t(l.w) * l.h;
            const uint32_t ofsExists = uint32_t(out.size());
            std::vector<uint8_t> bits((cells + 7) / 8, 0);
            for (size_t b = 0; b < cells; ++b)
                if (b >= l.exists.size() || l.exists[b]) bits[b / 8] |= uint8_t(1u << (b % 8));
            out.insert(out.end(), bits.begin(), bits.end());
            align();
            float lo = l.heights.empty() ? 0.0f : l.heights[0], hi = lo;
            for (float h : l.heights) { lo = std::min(lo, h); hi = std::max(hi, h); }
            uint32_t ofsVertices = 0;
            std::vector<uint8_t> extra = l.extra;
            if (l.format != 2 || !extra.empty()) extra.resize(verts * LiquidExtraPerVertex(l.format), 0);   // flat ocean may have no data at all
            if (l.format != 2 || !extra.empty())
            {
                ofsVertices = uint32_t(out.size());
                if (l.format != 2)
                    for (size_t v = 0; v < verts; ++v)
                    {
                        const float h = v < l.heights.size() ? l.heights[v] : lo;
                        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h) + 4);
                    }
                out.insert(out.end(), extra.begin(), extra.end());
                align();
            }
            uint8_t* p = out.data() + instances + i * 24;
            std::memcpy(p, &l.type, 2);
            std::memcpy(p + 2, &l.format, 2);
            std::memcpy(p + 4, &lo, 4);
            std::memcpy(p + 8, &hi, 4);
            p[12] = l.x; p[13] = l.y; p[14] = l.w; p[15] = l.h;
            std::memcpy(p + 16, &ofsExists, 4);
            std::memcpy(p + 20, &ofsVertices, 4);
        }
        const size_t header = size_t(c.indexY * 16 + c.indexX) * 12;
        SetU32(out, header, uint32_t(instances));
        SetU32(out, header + 4, uint32_t(here.size()));
        SetU32(out, header + 8, attributes);
    }
    return out;
}

std::vector<uint8_t> RewriteAdt(const std::vector<uint8_t>& d, const Adt& adt, const std::set<size_t>& chunks, bool bigAlpha)
{
    // MHDR field index -> the chunk it points at (field 0 is flags).
    static const uint32_t kMhdrFields[] = { 0, Tag("MCIN"), Tag("MTEX"), Tag("MMDX"), Tag("MMID"), Tag("MWMO"),
                                            Tag("MWID"), Tag("MDDF"), Tag("MODF"), Tag("MFBO"), Tag("MH2O"), Tag("MTXF") };
    std::vector<uint8_t> out;
    out.reserve(d.size() + chunks.size() * 8192);
    std::map<uint32_t, size_t> placed;                 // first occurrence of each top-level chunk -> new offset
    std::vector<std::pair<size_t, size_t>> mcnks;      // new offset, total size (header included)
    size_t mcnkIndex = 0;

    // Objects, matched to the original file by unique id: unchanged ones keep their chunk references (MCRF),
    // moved and new ones are referenced by the chunks they stand on.
    const auto original = ParseAdt(d, bigAlpha);
    if (!original) return {};
    auto same = [](const auto& a, const auto& b) {
        return a.model == b.model && a.uniqueId == b.uniqueId && !std::memcmp(a.pos, b.pos, sizeof a.pos) && !std::memcmp(a.rot, b.rot, sizeof a.rot);
    };
    auto sameDoodad = [&](const DoodadPlacement& a, const DoodadPlacement& b) { return same(a, b) && a.scale == b.scale && a.flags == b.flags; };
    auto sameWmo = [&](const WmoPlacement& a, const WmoPlacement& b) {
        return same(a, b) && !std::memcmp(a.extMin, b.extMin, sizeof a.extMin) && !std::memcmp(a.extMax, b.extMax, sizeof a.extMax) &&
               a.doodadSet == b.doodadSet && a.flags == b.flags && a.nameSet == b.nameSet;
    };
    bool objects = adt.doodads.size() != original->doodads.size() || adt.wmos.size() != original->wmos.size();
    for (size_t i = 0; !objects && i < adt.doodads.size(); ++i) objects = !sameDoodad(adt.doodads[i], original->doodads[i]);
    for (size_t i = 0; !objects && i < adt.wmos.size(); ++i) objects = !sameWmo(adt.wmos[i], original->wmos[i]);

    // Unique id -> new index for the objects that stay put; everything else is placed afresh.
    std::map<uint32_t, uint32_t> keptDoodads, keptWmos;
    std::map<size_t, std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> placedRefs;   // chunk -> doodad, wmo indices
    auto chunksOver = [&](float x0, float z0, float x1, float z1, auto&& fn) {
        bool any = false;
        for (size_t ci = 0; ci < adt.chunks.size(); ++ci)
        {
            const AdtChunk& c = adt.chunks[ci];
            if (x1 >= c.baseX && x0 < c.baseX + kChunkSize && z1 >= c.baseZ && z0 < c.baseZ + kChunkSize) { fn(ci); any = true; }
        }
        if (any || adt.chunks.empty()) return;
        size_t best = 0;   // outside the tile: the nearest chunk, so the client still draws it
        float bestDist = 1e30f;
        for (size_t ci = 0; ci < adt.chunks.size(); ++ci)
        {
            const float dx = adt.chunks[ci].baseX + kChunkSize / 2 - (x0 + x1) / 2, dz = adt.chunks[ci].baseZ + kChunkSize / 2 - (z0 + z1) / 2;
            if (dx * dx + dz * dz < bestDist) { bestDist = dx * dx + dz * dz; best = ci; }
        }
        fn(best);
    };
    if (objects)
    {
        std::map<uint32_t, const DoodadPlacement*> oldD;
        std::map<uint32_t, const WmoPlacement*> oldW;
        for (const auto& p : original->doodads) oldD.try_emplace(p.uniqueId, &p);
        for (const auto& p : original->wmos) oldW.try_emplace(p.uniqueId, &p);
        for (size_t i = 0; i < adt.doodads.size(); ++i)
        {
            const DoodadPlacement& p = adt.doodads[i];
            if (auto it = oldD.find(p.uniqueId); it != oldD.end() && sameDoodad(*it->second, p) && keptDoodads.try_emplace(p.uniqueId, uint32_t(i)).second) continue;
            chunksOver(p.pos[0], p.pos[2], p.pos[0], p.pos[2], [&](size_t ci) { placedRefs[ci].first.push_back(uint32_t(i)); });
            // ponytail: a doodad is referenced by the chunk under its origin only; use model bounds if big trees pop at chunk edges.
        }
        for (size_t i = 0; i < adt.wmos.size(); ++i)
        {
            const WmoPlacement& p = adt.wmos[i];
            if (auto it = oldW.find(p.uniqueId); it != oldW.end() && sameWmo(*it->second, p) && keptWmos.try_emplace(p.uniqueId, uint32_t(i)).second) continue;
            chunksOver(p.extMin[0], p.extMin[2], p.extMax[0], p.extMax[2], [&](size_t ci) { placedRefs[ci].second.push_back(uint32_t(i)); });
        }
    }

    // Name blocks (MMDX/MWMO: zero-terminated strings; MMID/MWID: their offsets) and the placement lists.
    auto names = [](const auto& list, std::vector<uint8_t>& block, std::vector<uint8_t>& ids) {
        std::map<std::string, uint32_t> index;
        std::vector<uint32_t> nameIds;
        for (const auto& p : list)
        {
            auto [it, added] = index.try_emplace(p.model, uint32_t(index.size()));
            if (added)
            {
                PutU32(ids, uint32_t(block.size()));
                block.insert(block.end(), p.model.begin(), p.model.end());
                block.push_back(0);
            }
            nameIds.push_back(it->second);
        }
        return nameIds;
    };
    std::vector<uint8_t> mmdx, mmid, mddf, mwmo, mwid, modf;
    if (objects)
    {
        const auto dIds = names(adt.doodads, mmdx, mmid);
        for (size_t i = 0; i < adt.doodads.size(); ++i)
        {
            const DoodadPlacement& p = adt.doodads[i];
            MddfEntry e{ dIds[i], p.uniqueId, { p.pos[0], p.pos[1], p.pos[2] }, { p.rot[0], p.rot[1], p.rot[2] },
                         uint16_t(std::clamp(std::lround(p.scale * 1024.0f), 1L, 65535L)), p.flags };
            mddf.insert(mddf.end(), reinterpret_cast<uint8_t*>(&e), reinterpret_cast<uint8_t*>(&e) + sizeof e);
        }
        const auto wIds = names(adt.wmos, mwmo, mwid);
        for (size_t i = 0; i < adt.wmos.size(); ++i)
        {
            const WmoPlacement& p = adt.wmos[i];
            ModfEntry e{ wIds[i], p.uniqueId, { p.pos[0], p.pos[1], p.pos[2] }, { p.rot[0], p.rot[1], p.rot[2] },
                         { p.extMin[0], p.extMin[1], p.extMin[2], p.extMax[0], p.extMax[1], p.extMax[2] }, p.flags, p.doodadSet, p.nameSet, 0 };
            modf.insert(modf.end(), reinterpret_cast<uint8_t*>(&e), reinterpret_cast<uint8_t*>(&e) + sizeof e);
        }
        for (uint32_t tag : { Tag("MMDX"), Tag("MMID"), Tag("MWMO"), Tag("MWID"), Tag("MDDF"), Tag("MODF") })
        {
            bool found = false;
            ForEachChunk(d, 0, d.size(), [&](uint32_t magic, size_t, size_t) { found |= magic == tag; });
            if (!found) return {};   // ponytail: 3.3.5 ADTs always carry these; insert missing ones if a custom ADT lacks them
        }
    }
    const std::map<uint32_t, const std::vector<uint8_t>*> replaced = { { Tag("MMDX"), &mmdx }, { Tag("MMID"), &mmid }, { Tag("MWMO"), &mwmo },
                                                                       { Tag("MWID"), &mwid }, { Tag("MDDF"), &mddf }, { Tag("MODF"), &modf } };

    // Liquids: MH2O written afresh when they differ from the file's.
    auto sameLiquid = [](const AdtLiquid& a, const AdtLiquid& b) {
        return a.type == b.type && a.format == b.format && a.cornerX == b.cornerX && a.cornerZ == b.cornerZ && a.x == b.x && a.y == b.y &&
               a.w == b.w && a.h == b.h && a.heights == b.heights && a.exists == b.exists && a.extra == b.extra && a.fishable == b.fishable &&
               a.deep == b.deep && a.fromMclq == b.fromMclq;
    };
    bool liquids = adt.liquids.size() != original->liquids.size();
    for (size_t i = 0; !liquids && i < adt.liquids.size(); ++i) liquids = !sameLiquid(adt.liquids[i], original->liquids[i]);
    const std::vector<uint8_t> mh2o = liquids ? WriteMh2o(adt) : std::vector<uint8_t>{};
    bool hadMh2o = false;
    ForEachChunk(d, 0, d.size(), [&](uint32_t magic, size_t, size_t) { hadMh2o |= magic == Tag("MH2O"); });

    bool failed = false;
    ForEachChunk(d, 0, d.size(), [&](uint32_t magic, size_t off, size_t size) {
        if (failed) return;
        if (liquids && !hadMh2o && magic == Tag("MCNK") && !placed.count(Tag("MH2O")))   // a file without MH2O gets one before its chunks
        {
            placed.emplace(Tag("MH2O"), out.size());
            PutChunk(out, Tag("MH2O"), mh2o);
        }
        const size_t newOff = out.size();
        if (magic == Tag("MH2O") && liquids)
        {
            placed.try_emplace(magic, newOff);
            PutChunk(out, magic, mh2o);
            return;
        }
        placed.try_emplace(magic, newOff);
        if (magic == Tag("MTEX"))
        {
            std::vector<uint8_t> body;
            for (const std::string& t : adt.textures)
            {
                body.insert(body.end(), t.begin(), t.end());
                body.push_back(0);
            }
            PutChunk(out, magic, body);
            return;
        }
        if (auto r = replaced.find(magic); objects && r != replaced.end())
        {
            PutChunk(out, magic, *r->second);
            return;
        }
        const size_t ci = magic == Tag("MCNK") ? mcnkIndex++ : 0;
        const bool layersChanged = chunks.count(ci) && ci < adt.chunks.size();
        // This chunk's references after the edit: kept objects under their new index, then the placed ones.
        bool refsChanged = false;
        std::vector<uint32_t> doodadRefs, wmoRefs;
        if (magic == Tag("MCNK") && objects)
        {
            McnkHeader h{};
            ReadAt(d, off, h);
            std::vector<uint32_t> oldDoodadRefs, oldWmoRefs;
            if (auto old = SubChunk(d, off - 8, off + size, h.ofsRefs, Tag("MCRF")))
                for (uint32_t k = 0; k < h.nDoodadRefs + h.nMapObjRefs && (k + 1) * 4 <= old->second; ++k)
                {
                    uint32_t v = 0;
                    ReadAt(d, old->first + k * 4, v);
                    (k < h.nDoodadRefs ? oldDoodadRefs : oldWmoRefs).push_back(v);
                }
            for (uint32_t v : oldDoodadRefs)
                if (v < original->doodads.size())
                    if (auto it = keptDoodads.find(original->doodads[v].uniqueId); it != keptDoodads.end()) doodadRefs.push_back(it->second);
            for (uint32_t v : oldWmoRefs)
                if (v < original->wmos.size())
                    if (auto it = keptWmos.find(original->wmos[v].uniqueId); it != keptWmos.end()) wmoRefs.push_back(it->second);
            if (auto placedHere = placedRefs.find(ci); placedHere != placedRefs.end())
            {
                doodadRefs.insert(doodadRefs.end(), placedHere->second.first.begin(), placedHere->second.first.end());
                wmoRefs.insert(wmoRefs.end(), placedHere->second.second.begin(), placedHere->second.second.end());
            }
            refsChanged = doodadRefs != oldDoodadRefs || wmoRefs != oldWmoRefs;
        }
        if (magic != Tag("MCNK") || (!layersChanged && !refsChanged))
        {
            out.insert(out.end(), d.begin() + std::ptrdiff_t(off - 8), d.begin() + std::ptrdiff_t(off + size));
            if (magic == Tag("MCNK")) mcnks.push_back({ newOff, size + 8 });
            return;
        }

        // Changed chunk: rebuilt sub-chunk by sub-chunk in its original order, MCLY / MCAL / MCRF replaced in place.
        // The client walks the sub-chunks one after another (it ignores the header offsets), so each must appear
        // exactly once; appending new copies at the end derails that walk and crashes the client.
        McnkHeader h{};
        std::memcpy(&h, d.data() + off, sizeof h);
        const size_t start = off - 8, chunkEnd = off + size;
        const auto spans = WalkSubChunks(d, start, chunkEnd, h.sizeAlpha, h.sizeLiquid);   // with the original sizes
        if (spans.empty() || spans.back().end != chunkEnd) { failed = true; return; }   // not a layout we can rebuild safely
        std::map<uint32_t, std::vector<uint8_t>> bodies;   // replacement bodies by sub-chunk magic
        if (layersChanged)
        {
            const AdtChunk& c = adt.chunks[ci];
            std::vector<uint8_t>& mcly = bodies[Tag("MCLY")];
            std::vector<uint8_t>& mcal = bodies[Tag("MCAL")];
            const uint32_t layers = std::min<uint32_t>(c.layerCount, 4);
            for (uint32_t l = 0; l < layers; ++l)
            {
                MclyEntry e{ c.textureIds[l], c.layerFlags[l] & ~(kMclyCompressed | kMclyUseAlpha), 0, c.effectIds[l] };
                if (l > 0)
                {
                    e.flags |= kMclyUseAlpha;
                    e.offsetInMcal = uint32_t(mcal.size());
                    EncodeAlpha(c.alpha, int(l - 1), bigAlpha, mcal);
                }
                mcly.insert(mcly.end(), reinterpret_cast<uint8_t*>(&e), reinterpret_cast<uint8_t*>(&e) + sizeof e);
            }
            h.nLayers = layers;
            h.sizeAlpha = uint32_t(mcal.size() + 8);
        }
        if (refsChanged)
        {
            // MCRF: doodad indices, then WMO indices.
            std::vector<uint8_t>& mcrf = bodies[Tag("MCRF")];
            for (uint32_t v : doodadRefs) PutU32(mcrf, v);
            for (uint32_t v : wmoRefs) PutU32(mcrf, v);
            h.nDoodadRefs = uint32_t(doodadRefs.size());
            h.nMapObjRefs = uint32_t(wmoRefs.size());
        }

        std::vector<uint8_t> chunk(d.begin() + std::ptrdiff_t(start), d.begin() + std::ptrdiff_t(start + 8 + sizeof h));
        std::map<uint32_t, uint32_t> moved;   // sub-chunk magic -> new offset from the MCNK magic
        for (const SubSpan& s : spans)
        {
            moved.try_emplace(s.tag, uint32_t(chunk.size()));
            if (auto r = bodies.find(s.tag); r != bodies.end())
            {
                PutChunk(chunk, s.tag, r->second);
                bodies.erase(r);   // a duplicate later in the file keeps its bytes; the header points at the first
            }
            else
                chunk.insert(chunk.end(), d.begin() + std::ptrdiff_t(s.start), d.begin() + std::ptrdiff_t(s.end));
        }
        for (const auto& [tag, body] : bodies)   // a sub-chunk the original lacked goes at the end
        {
            moved.try_emplace(tag, uint32_t(chunk.size()));
            PutChunk(chunk, tag, body);
        }
        std::memcpy(chunk.data() + 8, &h, sizeof h);
        // Offsets the original set (or that point at a sub-chunk written here) follow their sub-chunk.
        for (const auto& [tag, field] : kSubOffsetFields)
        {
            uint32_t was = 0;
            ReadAt(d, off + field, was);
            if (auto m = moved.find(tag); m != moved.end() && (was || tag == Tag("MCLY") || tag == Tag("MCAL") || tag == Tag("MCRF")))
                SetU32(chunk, 8 + field, m->second);
        }
        SetU32(chunk, 4, uint32_t(chunk.size() - 8));
        out.insert(out.end(), chunk.begin(), chunk.end());
        mcnks.push_back({ newOff, chunk.size() });
    });

    auto mhdr = placed.find(Tag("MHDR"));
    auto mcin = placed.find(Tag("MCIN"));
    if (failed || mhdr == placed.end() || mcin == placed.end()) return {};

    // MHDR offsets are relative to MHDR's data; keep zero fields zero.
    const size_t mhdrData = mhdr->second + 8;
    for (size_t field = 1; field < std::size(kMhdrFields); ++field)
    {
        uint32_t original = 0;
        ReadAt(out, mhdrData + field * 4, original);
        auto it = placed.find(kMhdrFields[field]);
        if ((original != 0 || (liquids && kMhdrFields[field] == Tag("MH2O"))) && it != placed.end())
            SetU32(out, mhdrData + field * 4, uint32_t(it->second - mhdrData));
    }

    // MCIN: 256 x { offset, size, flags, asyncId }, absolute offsets, sizes include the chunk header.
    const size_t mcinData = mcin->second + 8;
    for (size_t i = 0; i < mcnks.size() && i < 256; ++i)
    {
        SetU32(out, mcinData + i * 16, uint32_t(mcnks[i].first));
        SetU32(out, mcinData + i * 16 + 4, uint32_t(mcnks[i].second));
    }
    return out;
}

std::array<std::array<float, 3>, 145> ChunkNormals(const Adt& adt, size_t chunk,
                                                   const std::function<std::optional<float>(int gx, int gz)>& beyond)
{
    // The tile's outer vertices as one 129 x 129 grid (chunk edges shared), then gradients on it.
    constexpr int N = 129;
    std::vector<float> grid(size_t(N) * N, 0.0f);
    std::vector<char> known(size_t(N) * N, 0);
    float originX = 1e30f, originZ = 1e30f;
    for (const AdtChunk& c : adt.chunks) { originX = std::min(originX, c.baseX); originZ = std::min(originZ, c.baseZ); }
    for (const AdtChunk& c : adt.chunks)
    {
        const int cx = int(std::lround((c.baseX - originX) / kChunkSize)), cz = int(std::lround((c.baseZ - originZ) / kChunkSize));
        for (int row = 0; row <= 8; ++row)
            for (int col = 0; col <= 8; ++col)
            {
                const int gx = cx * 8 + col, gz = cz * 8 + row;
                if (gx < 0 || gz < 0 || gx >= N || gz >= N) continue;
                grid[size_t(gz) * N + gx] = c.baseY + c.heights[size_t(row * 17 + col)];
                known[size_t(gz) * N + gx] = 1;
            }
    }
    auto outside = [&](int gx, int gz) { return gx < 0 || gz < 0 || gx >= N || gz >= N; };
    auto at = [&](int gx, int gz, float fallback) {
        if (outside(gx, gz)) return beyond ? beyond(gx, gz).value_or(fallback) : fallback;
        return known[size_t(gz) * N + gx] ? grid[size_t(gz) * N + gx] : fallback;
    };
    auto normal = [](float dhdx, float dhdz) {
        const float len = std::sqrt(dhdx * dhdx + 1 + dhdz * dhdz);
        return std::array<float, 3>{ -dhdx / len, 1 / len, -dhdz / len };
    };

    std::array<std::array<float, 3>, 145> out{};
    const AdtChunk& c = adt.chunks[chunk];
    const int cx = int(std::lround((c.baseX - originX) / kChunkSize)), cz = int(std::lround((c.baseZ - originZ) / kChunkSize));
    for (int row = 0; row <= 8; ++row)
        for (int col = 0; col <= 8; ++col)
        {
            const int gx = cx * 8 + col, gz = cz * 8 + row;
            const float h = at(gx, gz, 0);
            const float l = at(gx - 1, gz, h), r = at(gx + 1, gz, h), u = at(gx, gz - 1, h), d = at(gx, gz + 1, h);
            auto has = [&](int x, int z) {
                if (outside(x, z)) return beyond && beyond(x, z) ? 1.0f : 0.0f;
                return known[size_t(z) * N + x] ? 1.0f : 0.0f;
            };
            const float spanX = has(gx - 1, gz) + has(gx + 1, gz), spanZ = has(gx, gz - 1) + has(gx, gz + 1);   // 2 inside, 1 at the border
            out[size_t(row * 17 + col)] = normal((r - l) / (std::max(spanX, 1.0f) * kUnitSize), (d - u) / (std::max(spanZ, 1.0f) * kUnitSize));
        }
    for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 8; ++col)
        {
            // Inner vertex: the slope of its cell, from the four corners.
            const float tl = c.heights[size_t(row * 17 + col)], tr = c.heights[size_t(row * 17 + col + 1)];
            const float bl = c.heights[size_t((row + 1) * 17 + col)], br = c.heights[size_t((row + 1) * 17 + col + 1)];
            out[size_t(row * 17 + 9 + col)] = normal(((tr + br) - (tl + bl)) / (2 * kUnitSize), ((bl + br) - (tl + tr)) / (2 * kUnitSize));
        }
    return out;
}

std::vector<std::string> ValidateAdt(const std::vector<uint8_t>& d, bool bigAlpha, std::string* summary)
{
    std::vector<std::string> problems;
    auto problem = [&](const std::string& p) { if (problems.size() < 40) problems.push_back(p); };
    auto tagName = [](uint32_t t) { return std::string{ char(t >> 24), char(t >> 16), char(t >> 8), char(t) }; };
    auto magicAt = [&](size_t at) { uint32_t m = 0; ReadAt(d, at, m); return m; };

    // Top level: where each chunk is, and the counts the MCNKs refer to.
    std::map<uint32_t, std::pair<size_t, size_t>> top;   // first of each kind: body offset, size
    std::vector<size_t> mcnkStarts;
    size_t end = 0;
    ForEachChunk(d, 0, d.size(), [&](uint32_t magic, size_t off, size_t size) {
        top.try_emplace(magic, off, size);
        if (magic == Tag("MCNK")) mcnkStarts.push_back(off - 8);
        end = off + size;
    });
    if (end != d.size()) problem("chunk walk stops at " + std::to_string(end) + " of " + std::to_string(d.size()) + " bytes");
    for (uint32_t t : { Tag("MVER"), Tag("MHDR"), Tag("MCIN"), Tag("MTEX"), Tag("MMDX"), Tag("MMID"), Tag("MWMO"), Tag("MWID"), Tag("MDDF"), Tag("MODF") })
        if (!top.count(t)) problem("no " + tagName(t));
    if (mcnkStarts.size() != 256) problem(std::to_string(mcnkStarts.size()) + " MCNKs (256 expected)");

    size_t textures = 0;
    if (top.count(Tag("MTEX")))
        for (size_t p = top[Tag("MTEX")].first, e = p + top[Tag("MTEX")].second; p < e; ++textures)
            p += strnlen(reinterpret_cast<const char*>(d.data() + p), e - p) + 1;
    const size_t mmid = top.count(Tag("MMID")) ? top[Tag("MMID")].second / 4 : 0, mwid = top.count(Tag("MWID")) ? top[Tag("MWID")].second / 4 : 0;
    const size_t doodads = top.count(Tag("MDDF")) ? top[Tag("MDDF")].second / sizeof(MddfEntry) : 0;
    const size_t wmos = top.count(Tag("MODF")) ? top[Tag("MODF")].second / sizeof(ModfEntry) : 0;
    if (summary)
        *summary = std::to_string(d.size()) + " bytes, " + std::to_string(textures) + " textures, " + std::to_string(mmid) + " models / " +
                   std::to_string(doodads) + " doodads, " + std::to_string(mwid) + " WMOs / " + std::to_string(wmos) + " placements";

    // MHDR: each non-zero field points at its chunk's header.
    static const uint32_t kFields[] = { 0, Tag("MCIN"), Tag("MTEX"), Tag("MMDX"), Tag("MMID"), Tag("MWMO"), Tag("MWID"), Tag("MDDF"), Tag("MODF"),
                                        Tag("MFBO"), Tag("MH2O"), Tag("MTXF") };
    if (top.count(Tag("MHDR")))
    {
        const size_t base = top[Tag("MHDR")].first;
        for (size_t f = 1; f < std::size(kFields); ++f)
        {
            uint32_t ofs = 0;
            ReadAt(d, base + f * 4, ofs);
            if (ofs && magicAt(base + ofs) != kFields[f]) problem("MHDR field " + tagName(kFields[f]) + " points at " + tagName(magicAt(base + ofs)));
        }
    }
    // MCIN: 256 entries pointing at the MCNKs, sizes including the 8-byte header.
    if (top.count(Tag("MCIN")))
        for (size_t i = 0; i < 256 && i < mcnkStarts.size(); ++i)
        {
            uint32_t ofs = 0, size = 0, chunkSize = 0;
            ReadAt(d, top[Tag("MCIN")].first + i * 16, ofs);
            ReadAt(d, top[Tag("MCIN")].first + i * 16 + 4, size);
            ReadAt(d, mcnkStarts[i] + 4, chunkSize);
            if (ofs != mcnkStarts[i] || size != chunkSize + 8) problem("MCIN " + std::to_string(i) + " does not match its MCNK");
        }
    // Placements: name indices in range.
    for (size_t i = 0; i < doodads; ++i)
    {
        MddfEntry e{};
        ReadAt(d, top[Tag("MDDF")].first + i * sizeof e, e);
        if (e.nameId >= mmid) { problem("MDDF " + std::to_string(i) + " name " + std::to_string(e.nameId) + " >= " + std::to_string(mmid)); break; }
    }
    for (size_t i = 0; i < wmos; ++i)
    {
        ModfEntry e{};
        ReadAt(d, top[Tag("MODF")].first + i * sizeof e, e);
        if (e.nameId >= mwid) { problem("MODF " + std::to_string(i) + " name " + std::to_string(e.nameId) + " >= " + std::to_string(mwid)); break; }
    }

    // Every MCNK: sub-chunk offsets land on the right magic inside the chunk; layers, alpha and refs in range.
    for (size_t i = 0; i < mcnkStarts.size(); ++i)
    {
        const size_t start = mcnkStarts[i];
        uint32_t size = 0;
        ReadAt(d, start + 4, size);
        const size_t chunkEnd = start + 8 + size;
        McnkHeader h{};
        ReadAt(d, start + 8, h);
        const std::string where = "MCNK " + std::to_string(i) + ": ";
        auto sub = [&](uint32_t ofs, uint32_t magic, bool required) -> std::optional<std::pair<size_t, size_t>> {
            if (!ofs) { if (required) problem(where + tagName(magic) + " offset is 0"); return std::nullopt; }
            const auto s = SubChunk(d, start, chunkEnd, ofs, magic);
            if (!s) problem(where + tagName(magic) + " offset " + std::to_string(ofs) + " lands on " + tagName(magicAt(start + ofs)));
            else
            {
                uint32_t declared = 0;
                ReadAt(d, start + ofs + 4, declared);
                if (start + ofs + 8 + declared > chunkEnd) problem(where + tagName(magic) + " runs past the end of the MCNK");
            }
            return s;
        };
        // The client's own reading: sub-chunks one after another (WalkSubChunks). Each must appear once, the walk
        // must end exactly at the chunk end, and the header offsets must name what the walk finds.
        const auto spans = WalkSubChunks(d, start, chunkEnd, h.sizeAlpha, h.sizeLiquid);
        if (spans.empty() || spans.back().end != chunkEnd)
            problem(where + "sub-chunk walk stops " + std::to_string(chunkEnd - (spans.empty() ? start + 8 + sizeof h : spans.back().end)) +
                    " bytes before the end (the client would read garbage)");
        std::map<uint32_t, size_t> walked;
        for (const SubSpan& s : spans)
            if (!walked.try_emplace(s.tag, s.start - start).second) problem(where + tagName(s.tag) + " appears twice (the client reads both in turn)");
        for (const auto& [tag, field] : kSubOffsetFields)
        {
            uint32_t ofs = 0;
            ReadAt(d, start + 8 + field, ofs);
            if (auto w = walked.find(tag); ofs && w != walked.end() && w->second != ofs)
                problem(where + tagName(tag) + " header offset " + std::to_string(ofs) + " but the client finds it at " + std::to_string(w->second));
        }
        sub(h.ofsHeight, Tag("MCVT"), true);
        sub(h.ofsNormal, Tag("MCNR"), true);
        const auto mcly = sub(h.ofsLayer, Tag("MCLY"), h.nLayers > 0);
        const auto mcrf = sub(h.ofsRefs, Tag("MCRF"), h.nDoodadRefs + h.nMapObjRefs > 0);
        auto mcal = h.sizeAlpha > 8 ? sub(h.ofsAlpha, Tag("MCAL"), true) : std::nullopt;
        if (mcal) mcal->second = std::max(mcal->second, std::min<size_t>(h.sizeAlpha - 8, chunkEnd - mcal->first));   // as the client reads it
        if (h.sizeShadow > 8) sub(h.ofsShadow, Tag("MCSH"), true);
        if (h.nSndEmitters) sub(h.ofsSndEmitters, Tag("MCSE"), true);
        if (h.sizeLiquid > 8) sub(h.ofsLiquid, Tag("MCLQ"), true);
        if (h.ofsMccv) sub(h.ofsMccv, Tag("MCCV"), false);
        if (h.nLayers > 4) problem(where + std::to_string(h.nLayers) + " layers");
        if (mcal && h.sizeAlpha > mcal->second + 8) problem(where + "sizeAlpha " + std::to_string(h.sizeAlpha) + " runs past the MCNK");
        if (mcly)
            for (uint32_t l = 0; l < h.nLayers && (l + 1) * sizeof(MclyEntry) <= mcly->second; ++l)
            {
                MclyEntry e{};
                ReadAt(d, mcly->first + l * sizeof e, e);
                if (e.textureId >= textures) problem(where + "layer " + std::to_string(l) + " texture " + std::to_string(e.textureId) + " >= " + std::to_string(textures));
                if (l > 0 && (e.flags & kMclyUseAlpha))
                {
                    const size_t need = (e.flags & kMclyCompressed) ? 1 : bigAlpha ? 4096 : 2048;
                    if (!mcal || e.offsetInMcal + need > mcal->second) problem(where + "layer " + std::to_string(l) + " alpha outside MCAL");
                }
            }
        if (mcrf)
            for (uint32_t k = 0; k < h.nDoodadRefs + h.nMapObjRefs; ++k)
            {
                uint32_t v = 0;
                if ((k + 1) * 4 > mcrf->second) { problem(where + "MCRF shorter than its counts"); break; }
                ReadAt(d, mcrf->first + k * 4, v);
                if (k < h.nDoodadRefs ? v >= doodads : v >= wmos)
                {
                    problem(where + (k < h.nDoodadRefs ? "doodad" : "WMO") + " ref " + std::to_string(v) + " out of range");
                    break;
                }
            }
    }
    return problems;
}

std::vector<std::vector<int16_t>> ParseWdl(const std::vector<uint8_t>& d)
{
    std::vector<std::vector<int16_t>> tiles(4096);
    size_t maof = 0;
    ForEachChunk(d, 0, d.size(), [&](uint32_t magic, size_t off, size_t size) { if (magic == Tag("MAOF") && size >= 4096 * 4) maof = off; });
    if (!maof) return tiles;
    for (size_t i = 0; i < 4096; ++i)
    {
        uint32_t at = 0, magic = 0, size = 0;   // MAOF: absolute offset of each tile's MARE chunk, 0 = none
        ReadAt(d, maof + i * 4, at);
        if (!at || !ReadAt(d, at, magic) || !ReadAt(d, at + 4, size) || magic != Tag("MARE") || size < 289 * 2 || at + 8 + 289 * 2 > d.size()) continue;
        tiles[i].resize(289);   // the 17 x 17 outer heights come first (the 16 x 16 inner ones follow, unused)
        std::memcpy(tiles[i].data(), d.data() + at + 8, 289 * 2);
    }
    return tiles;
}

std::vector<uint8_t> WdlSetTile(std::vector<uint8_t> wdl, int x, int y, const Adt* adt)
{
    size_t maof = 0;
    ForEachChunk(wdl, 0, wdl.size(), [&](uint32_t magic, size_t off, size_t size) { if (magic == Tag("MAOF") && size >= 4096 * 4) maof = off; });
    if (!maof || x < 0 || y < 0 || x > 63 || y > 63) return {};
    const size_t slot = maof + size_t(y * 64 + x) * 4;
    if (!adt) { SetU32(wdl, slot, 0); return wdl; }
    // Chunks by their place in the tile (row along z, column along x, as the grid is everywhere else).
    std::array<const AdtChunk*, 256> grid{};
    for (const AdtChunk& c : adt->chunks)
    {
        const int col = int(std::lround(c.baseX / kChunkSize)) - x * 16, row = int(std::lround(c.baseZ / kChunkSize)) - y * 16;
        if (col >= 0 && col < 16 && row >= 0 && row < 16) grid[size_t(row * 16 + col)] = &c;
    }
    auto height = [&](int row, int col, int vrow, int vcol) -> int16_t {
        const AdtChunk* c = grid[size_t(row * 16 + col)];
        if (!c) return 0;
        return int16_t(std::clamp(std::lround(c->baseY + c->heights[size_t(vrow * 17 + vcol)]), -32768L, 32767L));
    };
    std::vector<uint8_t> mare;
    auto put = [&](int16_t v) { mare.insert(mare.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 2); };
    for (int r = 0; r <= 16; ++r)   // chunk corners; the last row and column are the far edges of the last chunks
        for (int c = 0; c <= 16; ++c) put(height(std::min(r, 15), std::min(c, 15), r == 16 ? 8 : 0, c == 16 ? 8 : 0));
    for (int r = 0; r < 16; ++r)    // chunk centres
        for (int c = 0; c < 16; ++c) put(height(r, c, 4, 4));
    SetU32(wdl, slot, uint32_t(wdl.size()));   // absolute offset of the MARE chunk
    PutChunk(wdl, Tag("MARE"), mare);
    PutChunk(wdl, Tag("MAHO"), std::vector<uint8_t>(32, 0));   // no low-detail holes
    return wdl;
}

std::optional<std::string> TrsLookup(const std::vector<uint8_t>& trs, const std::string& map, int x, int y)
{
    const std::string key = map + "\\map" + std::to_string(x) + "_" + std::to_string(y) + ".blp";
    auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };
    const std::string want = lower(key);
    size_t at = 0;
    const std::string text(trs.begin(), trs.end());
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (const size_t tab = line.find('\t'); tab != std::string::npos && lower(line.substr(0, tab)) == want) return line.substr(tab + 1);
        at = end + 1;
    }
    return std::nullopt;
}

std::vector<uint8_t> TrsSet(const std::vector<uint8_t>& trs, const std::string& map, int x, int y, const std::string& file)
{
    auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };
    const std::string key = map + "\\map" + std::to_string(x) + "_" + std::to_string(y) + ".blp", entry = key + "\t" + file;
    std::vector<std::string> lines;
    const std::string text(trs.begin(), trs.end());
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        at = end + 1;
    }
    bool done = false;
    for (std::string& line : lines)
        if (const size_t tab = line.find('\t'); tab != std::string::npos && lower(line.substr(0, tab)) == lower(key)) { line = entry; done = true; }
    if (!done)
    {
        const std::string header = "dir: " + lower(map);
        auto it = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return lower(l) == header; });
        if (it == lines.end()) { lines.push_back("dir: " + map); lines.push_back(entry); }
        else
        {
            auto next = std::find_if(it + 1, lines.end(), [](const std::string& l) { return l.rfind("dir: ", 0) == 0; });
            lines.insert(next, entry);
        }
    }
    std::vector<uint8_t> out;
    for (const std::string& l : lines)
    {
        out.insert(out.end(), l.begin(), l.end());
        out.push_back('\r');
        out.push_back('\n');
    }
    return out;
}

std::vector<uint8_t> MapPreview(const std::vector<std::vector<int16_t>>& wdl, int px)
{
    const int side = 64 * px;
    std::vector<uint8_t> rgba(size_t(side) * side * 4, 0);
    struct Rgb { float r, g, b; };
    auto mix = [](Rgb a, Rgb b, float t) { t = std::clamp(t, 0.0f, 1.0f); return Rgb{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; };
    const Rgb shore{ 45, 45, 250 }, deep{ 34, 30, 28 }, grass{ 52, 150, 38 }, earth{ 140, 104, 54 }, rock{ 196, 186, 170 };
    auto colour = [&](float h) {
        if (h < 0)
        {
            const float t = std::min(-h / 90.0f, 1.0f);
            return mix(shore, deep, t * t * (3 - 2 * t));   // smooth: a bright rim along the coast
        }
        return h < 350 ? mix(grass, earth, h / 350.0f) : mix(earth, rock, (h - 700) / 700.0f);   // highlands stay brown, peaks go pale
    };
    for (int ty = 0; ty < 64; ++ty)
        for (int tx = 0; tx < 64; ++tx)
        {
            const auto& g = wdl.size() == 4096 ? wdl[size_t(ty) * 64 + tx] : std::vector<int16_t>{};
            if (g.size() < 289) continue;
            for (int py = 0; py < px; ++py)
                for (int pxl = 0; pxl < px; ++pxl)
                {
                    // Bilinear over the 17 x 17 outer grid (row = z, column = x).
                    const float fx = (pxl + 0.5f) / px * 16, fy = (py + 0.5f) / px * 16;
                    const int x0 = std::min(int(fx), 15), y0 = std::min(int(fy), 15);
                    const float ax = fx - x0, ay = fy - y0;
                    auto at = [&](int r, int c) { return float(g[size_t(r) * 17 + c]); };
                    const float h = (at(y0, x0) * (1 - ax) + at(y0, x0 + 1) * ax) * (1 - ay) + (at(y0 + 1, x0) * (1 - ax) + at(y0 + 1, x0 + 1) * ax) * ay;
                    const Rgb c = colour(h);
                    uint8_t* p = &rgba[(size_t(ty * px + py) * side + tx * px + pxl) * 4];
                    p[0] = uint8_t(c.r);
                    p[1] = uint8_t(c.g);
                    p[2] = uint8_t(c.b);
                    p[3] = 255;
                }
        }
    return rgba;
}

std::set<uint32_t> DbcIds(const std::vector<uint8_t>& d)
{
    std::set<uint32_t> ids;
    uint32_t magic = 0, records = 0, recordSize = 0;
    if (!ReadAt(d, 0, magic) || magic != 0x43424457 /* WDBC */ || !ReadAt(d, 4, records) || !ReadAt(d, 12, recordSize)) return ids;
    for (uint32_t r = 0; r < records; ++r)
    {
        uint32_t id = 0;
        if (ReadAt(d, 20 + size_t(r) * recordSize, id)) ids.insert(id);
    }
    return ids;
}

bool Dbc::Load(std::vector<uint8_t> data)
{
    m_data = std::move(data);
    m_index.clear();
    m_records = 0;
    uint32_t magic = 0;
    if (!ReadAt(m_data, 0, magic) || magic != 0x43424457 /* WDBC */ || !ReadAt(m_data, 4, m_records) || !ReadAt(m_data, 8, m_fields) ||
        !ReadAt(m_data, 12, m_recordSize) || !ReadAt(m_data, 16, m_stringSize) ||
        20 + uint64_t(m_records) * m_recordSize + m_stringSize > m_data.size())
    {
        m_records = 0;
        return false;
    }
    for (uint32_t r = 0; r < m_records; ++r) m_index.emplace_back(U32(r, 0), r);
    std::sort(m_index.begin(), m_index.end());
    return true;
}

uint32_t Dbc::U32(uint32_t row, uint32_t field) const
{
    uint32_t v = 0;
    if (row < m_records && field < m_fields && (field + 1) * 4 <= m_recordSize) ReadAt(m_data, 20 + size_t(row) * m_recordSize + field * 4, v);
    return v;
}

float Dbc::F32(uint32_t row, uint32_t field) const
{
    const uint32_t bits = U32(row, field);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::string Dbc::Str(uint32_t row, uint32_t field) const
{
    return CString(m_data, 20 + size_t(m_records) * m_recordSize, m_stringSize, U32(row, field));
}

std::optional<uint32_t> Dbc::Find(uint32_t id) const
{
    auto it = std::lower_bound(m_index.begin(), m_index.end(), std::pair<uint32_t, uint32_t>{ id, 0 });
    if (it == m_index.end() || it->first != id) return std::nullopt;
    return it->second;
}

std::string Base64Encode(const uint8_t* data, size_t size)
{
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3)
    {
        const uint32_t n = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < size ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& text)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text)
    {
        const int v = value(c);
        if (v < 0) continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

bool FormatsSelfTest()
{
    {   // BLP writer round trip through the parser
        const uint8_t px[2 * 2 * 4] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160 };
        const auto img = ParseBlp(WriteBlp(2, 2, px));
        if (!img || img->width != 2 || img->mips.empty() || !std::equal(px, px + 16, img->mips[0].begin())) return false;
    }
    auto put = [](std::vector<uint8_t>& v, const void* p, size_t n) { v.insert(v.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    auto chunk = [&](std::vector<uint8_t>& v, uint32_t magic, const std::vector<uint8_t>& body) {
        uint32_t size = uint32_t(body.size());
        put(v, &magic, 4); put(v, &size, 4); put(v, body.data(), body.size());
    };

    // One MCNK: heights 0..144, two layers, the second with an uncompressed 4-bit alpha of 0xF/0x0.
    std::vector<uint8_t> mcvt, mcly, mcal, sub;
    for (int i = 0; i < 145; ++i) { float f = float(i); put(mcvt, &f, 4); }
    MclyEntry l0{ 0, 0, 0, 0 }, l1{ 1, kMclyUseAlpha, 0, 0 };
    put(mcly, &l0, sizeof l0); put(mcly, &l1, sizeof l1);
    mcal.assign(2048, 0x0F);   // low nibble 15, high nibble 0 -> 255, 0, 255, 0 ...

    McnkHeader h{};
    h.nLayers = 2;
    h.position[0] = kZeroPoint - 100.0f;   // -> baseZ 100
    h.position[1] = kZeroPoint - 200.0f;   // -> baseX 200
    h.position[2] = 50.0f;
    h.ofsHeight = 8 + 128;
    h.ofsLayer = h.ofsHeight + 8 + uint32_t(mcvt.size());
    h.ofsAlpha = h.ofsLayer + 8 + uint32_t(mcly.size());
    put(sub, &h, sizeof h);
    chunk(sub, Tag("MCVT"), mcvt);
    chunk(sub, Tag("MCLY"), mcly);
    chunk(sub, Tag("MCAL"), mcal);
    std::vector<uint8_t> mcnkBody(sub.begin(), sub.end());

    std::vector<uint8_t> file;
    chunk(file, Tag("MTEX"), { 'a', '.', 'b', 'l', 'p', 0, 'b', '.', 'b', 'l', 'p', 0 });
    chunk(file, Tag("MCNK"), mcnkBody);

    const auto adt = ParseAdt(file, false);
    if (!adt || adt->chunks.size() != 1 || adt->textures.size() != 2 || adt->textures[1] != "b.blp") return false;
    const AdtChunk& c = adt->chunks[0];
    if (c.baseX != 200.0f || c.baseZ != 100.0f || c.baseY != 50.0f || c.heights[144] != 144.0f) return false;
    if (c.layerCount != 2 || c.textureIds[1] != 1) return false;
    if (c.alpha[0] != 255 || c.alpha[4] != 0) return false;          // texels 0,1 of layer 1 (red)
    if (c.alpha[(5 * 64 + 63) * 4] != c.alpha[(5 * 64 + 62) * 4]) return false;   // edge fixed
    if (c.alpha[1] != 0) return false;                                 // layer 2 channel untouched

    // Old MCLQ liquid: an ocean chunk (flag 0x8) whose MCLQ size field is 0 (as shipped: sizeLiquid holds it),
    // 9 x 9 heights 10.., tile 5 marked empty.
    {
        std::vector<uint8_t> mclq, body;
        const float lo = 0, hi = 100;
        put(mclq, &lo, 4); put(mclq, &hi, 4);
        for (int v = 0; v < 81; ++v) { const uint32_t depth = 0; const float height = 10.0f + v; put(mclq, &depth, 4); put(mclq, &height, 4); }
        for (int t = 0; t < 64; ++t) mclq.push_back(t == 5 ? 0x0F : 0x00);
        McnkHeader w{};
        w.flags = 0x8;
        w.ofsLiquid = 8 + 128;
        w.sizeLiquid = 8 + uint32_t(mclq.size());
        put(body, &w, sizeof w);
        const uint32_t tag = Tag("MCLQ"), zero = 0;
        put(body, &tag, 4); put(body, &zero, 4); put(body, mclq.data(), mclq.size());
        std::vector<uint8_t> water;
        chunk(water, Tag("MCNK"), body);
        const auto wa = ParseAdt(water, false);
        if (!wa || wa->liquids.size() != 1) return false;
        const AdtLiquid& l = wa->liquids[0];
        if (!l.fromMclq || l.type != 2 || l.w != 8 || l.heights[80] != 90.0f || l.exists[5] || !l.exists[6]) return false;
    }

    // Compressed alpha: fill 64 x 0x80, then copy 2 literal bytes.
    uint8_t rle[] = { 0x80 | 64, 0x80, 2, 7, 9 };
    uint8_t rgba[64 * 64 * 4] = {};
    DecodeAlpha(rle, sizeof rle, kMclyCompressed, false, rgba, 0);
    if (!(rgba[63 * 4] == 0x80 && rgba[64 * 4] == 7 && rgba[65 * 4] == 9)) return false;

    // Rewrite: MVER, MHDR, MCIN, MTEX, MCNK, MFBO. Give chunk 0 three layers and a new texture.
    std::vector<uint8_t> full;
    chunk(full, Tag("MVER"), { 18, 0, 0, 0 });
    const size_t mhdrData = full.size() + 8;
    chunk(full, Tag("MHDR"), std::vector<uint8_t>(64, 0));
    const size_t mcinPos = full.size();
    chunk(full, Tag("MCIN"), std::vector<uint8_t>(256 * 16, 0));
    const size_t mtexPos = full.size();
    chunk(full, Tag("MTEX"), { 'a', '.', 'b', 'l', 'p', 0, 'b', '.', 'b', 'l', 'p', 0 });
    const size_t mcnkPos = full.size();
    chunk(full, Tag("MCNK"), mcnkBody);
    const size_t mfboPos = full.size();
    chunk(full, Tag("MFBO"), std::vector<uint8_t>(36, 0));
    auto set32 = [&](size_t at, uint32_t v) { std::memcpy(full.data() + at, &v, 4); };
    set32(mhdrData + 4, uint32_t(mcinPos - mhdrData));
    set32(mhdrData + 8, uint32_t(mtexPos - mhdrData));
    set32(mhdrData + 36, uint32_t(mfboPos - mhdrData));
    set32(mcinPos + 8, uint32_t(mcnkPos));
    set32(mcinPos + 12, uint32_t(mcnkBody.size() + 8));

    auto edited = ParseAdt(full, false);
    if (!edited) return false;
    edited->textures.push_back("c.blp");
    AdtChunk& e0 = edited->chunks[0];
    e0.layerCount = 3;
    e0.textureIds = { 0, 1, 2, 0 };
    for (size_t i = 0; i < 4096; ++i) e0.alpha[i * 4 + 1] = 128;
    const auto rewritten = RewriteAdt(full, *edited, { 0 }, false);
    const auto back = rewritten.empty() ? std::nullopt : ParseAdt(rewritten, false);
    if (!back || back->textures.size() != 3 || back->textures[2] != "c.blp") return false;
    const AdtChunk& b0 = back->chunks[0];
    if (b0.layerCount != 3 || b0.textureIds[2] != 2 || b0.heights[144] != 144.0f) return false;
    if (b0.alpha[1] != 136 || b0.alpha[4] != 0) return false;   // 128 -> nibble 8 -> 136; layer 1 still 255/0 pairs
    uint32_t mcinOfs = 0, mfboRel = 0, magic = 0;
    ReadAt(rewritten, mcinPos + 8, mcinOfs);
    ReadAt(rewritten, mcinOfs, magic);
    ReadAt(rewritten, mhdrData + 36, mfboRel);
    uint32_t mfboMagic = 0;
    ReadAt(rewritten, mhdrData + mfboRel, mfboMagic);
    return magic == Tag("MCNK") && mfboMagic == Tag("MFBO") && Base64Decode(Base64Encode((const uint8_t*)"hello", 5)).size() == 5;
}

std::vector<uint8_t> WriteBlp(uint32_t width, uint32_t height, const uint8_t* rgba)
{
    // Header (148 bytes) and the palette every BLP2 carries (1024, unused here), then mip 0 as BGRA.
    const size_t pixels = size_t(width) * height, dataAt = 148 + 1024;
    std::vector<uint8_t> out(dataAt + pixels * 4, 0);
    std::memcpy(out.data(), "BLP2", 4);
    const uint32_t type = 1, size = uint32_t(pixels * 4), offset = uint32_t(dataAt);
    std::memcpy(out.data() + 4, &type, 4);
    out[8] = 3;    // compression: ARGB8888
    out[9] = 8;    // alpha depth
    out[10] = 8;   // alpha type
    out[11] = 0;   // no mipmaps
    std::memcpy(out.data() + 12, &width, 4);
    std::memcpy(out.data() + 16, &height, 4);
    std::memcpy(out.data() + 20, &offset, 4);        // mipOffsets[0]
    std::memcpy(out.data() + 20 + 64, &size, 4);     // mipSizes[0]
    for (size_t p = 0; p < pixels; ++p)
    {
        out[dataAt + p * 4 + 0] = rgba[p * 4 + 2];
        out[dataAt + p * 4 + 1] = rgba[p * 4 + 1];
        out[dataAt + p * 4 + 2] = rgba[p * 4 + 0];
        out[dataAt + p * 4 + 3] = rgba[p * 4 + 3];
    }
    return out;
}
