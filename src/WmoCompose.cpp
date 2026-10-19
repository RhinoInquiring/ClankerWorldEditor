#include "WmoCompose.hpp"

#include "WmoEdit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace
{
    template <class T>
    bool ReadAt(const std::vector<uint8_t>& d, size_t off, T& out)
    {
        if (off > d.size() || d.size() - off < sizeof(T)) return false;
        std::memcpy(&out, d.data() + off, sizeof(T));
        return true;
    }

    template <class T>
    void WriteAt(std::vector<uint8_t>& d, size_t off, T v)
    {
        if (off <= d.size() && d.size() - off >= sizeof(T)) std::memcpy(d.data() + off, &v, sizeof(T));
    }

    template <class T> void Put(std::vector<uint8_t>& d, T v) { d.insert(d.end(), reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + sizeof(T)); }

    /// WMO chunk magics are stored reversed: as a little-endian uint32 they read as the name's big-endian value.
    constexpr uint32_t Tag(const char (&s)[5])
    {
        return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
    }

    void PutChunk(std::vector<uint8_t>& out, uint32_t tag, const uint8_t* body, size_t size)
    {
        Put(out, tag);
        Put(out, uint32_t(size));
        out.insert(out.end(), body, body + size);
    }
    void PutChunk(std::vector<uint8_t>& out, uint32_t tag, const std::vector<uint8_t>& body) { PutChunk(out, tag, body.data(), body.size()); }

    struct Chunk { uint32_t tag; size_t off, size; };
    std::vector<Chunk> Chunks(const std::vector<uint8_t>& d, size_t begin, size_t end)
    {
        std::vector<Chunk> out;
        for (size_t pos = begin; pos + 8 <= end;)
        {
            uint32_t tag = 0, size = 0;
            ReadAt(d, pos, tag);
            ReadAt(d, pos + 4, size);
            const size_t clamped = std::min<size_t>(size, end - pos - 8);
            out.push_back({ tag, pos + 8, clamped });
            pos += 8 + clamped;
        }
        return out;
    }

    /// The body of a root's first chunk with this tag (empty when it has none).
    std::vector<uint8_t> Body(const std::vector<uint8_t>& root, uint32_t tag)
    {
        for (const Chunk& c : Chunks(root, 0, root.size()))
            if (c.tag == tag) return { root.begin() + std::ptrdiff_t(c.off), root.begin() + std::ptrdiff_t(c.off + c.size) };
        return {};
    }

    std::optional<Chunk> Mogp(const std::vector<uint8_t>& g)
    {
        for (const Chunk& c : Chunks(g, 0, g.size()))
            if (c.tag == Tag("MOGP") && c.size >= 68) return c;
        return std::nullopt;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }

    std::string CString(const std::vector<uint8_t>& pool, size_t at)
    {
        if (at >= pool.size()) return {};
        const char* s = reinterpret_cast<const char*>(pool.data() + at);
        return std::string(s, strnlen(s, pool.size() - at));
    }

    /// A string pool (MOTX, MOGN): strings by offset, new ones appended zero-ended and padded to four bytes.
    struct Pool
    {
        std::vector<uint8_t> bytes;
        std::map<std::string, uint32_t> at;   // lower-case -> first offset
        explicit Pool(std::vector<uint8_t> b) : bytes(std::move(b))
        {
            for (size_t i = 0; i < bytes.size();)
            {
                if (!bytes[i]) { ++i; continue; }
                const std::string s = CString(bytes, i);
                at.try_emplace(Lower(s), uint32_t(i));
                i += s.size();
            }
        }
        uint32_t Add(const std::string& s)
        {
            if (const auto it = at.find(Lower(s)); it != at.end()) return it->second;
            const uint32_t off = uint32_t(bytes.size());
            bytes.insert(bytes.end(), s.begin(), s.end());
            do bytes.push_back(0); while (bytes.size() % 4);
            at.emplace(Lower(s), off);
            return off;
        }
    };

    constexpr float kLiquidUnit = 1600.0f / 3.0f / 128.0f;   // one ADT vertex step: a liquid tile
}

// ---------------------------------------------------------------------------------------------- transforms

void WmoTransform::Vector(float v[3]) const
{
    for (int k = 0; k < (turns & 3); ++k)
    {
        const float x = v[0];
        v[0] = -v[1];
        v[1] = x;
    }
}

void WmoTransform::Point(float p[3]) const
{
    if (Identity()) return;   // not even + 0: a -0.0 in the file stays as it was
    Vector(p);
    for (int k = 0; k < 3; ++k) p[k] += offset[k];
}

WmoBox WmoTransform::Box(const WmoBox& b) const
{
    float a[3] = { b[0], b[1], b[2] }, c[3] = { b[3], b[4], b[5] };
    Point(a);
    Point(c);
    return { std::min(a[0], c[0]), std::min(a[1], c[1]), std::min(a[2], c[2]), std::max(a[0], c[0]), std::max(a[1], c[1]), std::max(a[2], c[2]) };
}

WmoTransform WmoTransform::Then(const WmoTransform& next) const
{
    WmoTransform out;
    out.turns = (turns + next.turns) & 3;
    std::copy(offset, offset + 3, out.offset);
    next.Point(out.offset);
    return out;
}

bool WmoTransform::operator==(const WmoTransform& o) const
{
    return (turns & 3) == (o.turns & 3) && offset[0] == o.offset[0] && offset[1] == o.offset[1] && offset[2] == o.offset[2];
}

std::string WmoTransform::Key() const
{
    uint32_t bits[3];
    std::memcpy(bits, offset, sizeof bits);
    return std::to_string(turns & 3) + ":" + std::to_string(bits[0]) + ":" + std::to_string(bits[1]) + ":" + std::to_string(bits[2]);
}

nlohmann::json ToJson(const WmoSlot& s)
{
    nlohmann::json j = { { "g", s.group }, { "b", s.batch }, { "t", s.at.turns & 3 }, { "o", { s.at.offset[0], s.at.offset[1], s.at.offset[2] } } };
    if (!s.from.empty()) j["from"] = s.from;
    if (s.outdoor >= 0) j["out"] = s.outdoor;
    return j;
}

WmoSlot SlotFromJson(const nlohmann::json& j)
{
    WmoSlot s;
    s.from = j.value("from", std::string());
    s.group = j.value("g", uint16_t(0));
    s.batch = j.value("b", 0u);
    s.at.turns = j.value("t", 0);
    if (const auto o = j.find("o"); o != j.end())
        for (int k = 0; k < 3; ++k) s.at.offset[k] = o->at(k).get<float>();
    s.outdoor = j.value("out", int8_t(-1));
    return s;
}

void TransformBsp(std::vector<uint8_t>& mobn, const WmoTransform& t)
{
    // Nodes: flags (axis 0 x, 1 y, 2 z; 0x4 leaf), negative child, positive child, face count, first face, split distance.
    for (size_t at = 0; at + 16 <= mobn.size(); at += 16)
    {
        uint16_t flags = 0;
        ReadAt(mobn, at, flags);
        if (flags & 0x4) continue;
        int16_t neg = 0, pos = 0;
        float dist = 0;
        ReadAt(mobn, at + 2, neg);
        ReadAt(mobn, at + 4, pos);
        ReadAt(mobn, at + 12, dist);
        uint16_t axis = flags & 0x3;
        for (int k = 0; k < (t.turns & 3); ++k)   // (x, y) -> (-y, x): x splits become y splits; y splits flip onto x
        {
            if (axis == 0) axis = 1;
            else if (axis == 1)
            {
                axis = 0;
                dist = -dist;
                std::swap(neg, pos);
            }
        }
        if (axis < 3) dist += t.offset[axis];
        WriteAt(mobn, at, uint16_t((flags & ~0x3) | axis));
        WriteAt(mobn, at + 2, neg);
        WriteAt(mobn, at + 4, pos);
        WriteAt(mobn, at + 12, dist);
    }
}

std::vector<uint16_t> BspFacesAt(const std::vector<uint8_t>& mobn, const std::vector<uint8_t>& mobr, const float p[3])
{
    int16_t node = 0;
    for (int steps = 0; steps < 4096 && node >= 0 && size_t(node) * 16 + 16 <= mobn.size(); ++steps)
    {
        const size_t at = size_t(node) * 16;
        uint16_t flags = 0;
        ReadAt(mobn, at, flags);
        if (flags & 0x4)
        {
            uint16_t count = 0;
            uint32_t first = 0;
            ReadAt(mobn, at + 6, count);
            ReadAt(mobn, at + 8, first);
            std::vector<uint16_t> faces;
            for (uint32_t i = first; i < first + count && (i + 1) * 2 <= mobr.size(); ++i)
            {
                uint16_t f = 0;
                ReadAt(mobr, size_t(i) * 2, f);
                faces.push_back(f);
            }
            return faces;
        }
        float dist = 0;
        ReadAt(mobn, at + 12, dist);
        int16_t neg = -1, pos = -1;
        ReadAt(mobn, at + 2, neg);
        ReadAt(mobn, at + 4, pos);
        const int axis = flags & 0x3;
        node = axis < 3 && p[axis] < dist ? neg : pos;
    }
    return {};
}

void TransformLiquid(std::vector<uint8_t>& mliq, const WmoTransform& t)
{
    // Header: x verts, y verts, x tiles, y tiles, corner, material; then x * y vertices of 8 bytes (height last), one
    // byte per tile.
    int32_t xv = 0, yv = 0, xt = 0, yt = 0;
    float corner[3] = {};
    if (!ReadAt(mliq, 0, xv) || !ReadAt(mliq, 4, yv) || !ReadAt(mliq, 8, xt) || !ReadAt(mliq, 12, yt) || xv <= 0 || yv <= 0 || xt < 0 || yt < 0) return;
    for (int k = 0; k < 3; ++k) ReadAt(mliq, 16 + size_t(k) * 4, corner[k]);
    const size_t verts = 30, tiles = verts + size_t(xv) * yv * 8, end = tiles + size_t(xt) * yt;
    if (end > mliq.size()) return;
    for (int k = 0; k < (t.turns & 3); ++k)
    {
        // A point (i, j) of the old grid lands on (yv - 1 - j, i) of the new one; the new corner is the old far-y corner turned.
        std::vector<uint8_t> v(size_t(xv) * yv * 8), tl(size_t(xt) * yt);
        for (int nj = 0; nj < xv; ++nj)
            for (int ni = 0; ni < yv; ++ni)
                std::memcpy(v.data() + (size_t(nj) * yv + ni) * 8, mliq.data() + verts + (size_t(yv - 1 - ni) * xv + nj) * 8, 8);
        for (int nj = 0; nj < xt; ++nj)
            for (int ni = 0; ni < yt; ++ni) tl[size_t(nj) * yt + ni] = mliq[tiles + size_t(yt - 1 - ni) * xt + nj];
        std::swap(xv, yv);
        std::swap(xt, yt);
        const float cx = corner[0];
        corner[0] = -(corner[1] + float(xv - 1) * kLiquidUnit);   // xv is the old yv now
        corner[1] = cx;
        std::copy(v.begin(), v.end(), mliq.begin() + std::ptrdiff_t(verts));
        std::copy(tl.begin(), tl.end(), mliq.begin() + std::ptrdiff_t(verts + v.size()));
    }
    for (int k = 0; k < 3; ++k) corner[k] += t.offset[k];
    WriteAt(mliq, 0, xv);
    WriteAt(mliq, 4, yv);
    WriteAt(mliq, 8, xt);
    WriteAt(mliq, 12, yt);
    for (int k = 0; k < 3; ++k) WriteAt(mliq, 16 + size_t(k) * 4, corner[k]);
    for (size_t i = 0; i < size_t(xv) * yv; ++i)
    {
        float h = 0;
        ReadAt(mliq, verts + i * 8 + 4, h);
        WriteAt(mliq, verts + i * 8 + 4, h + t.offset[2]);
    }
}

// ---------------------------------------------------------------------------------------------- group files

std::optional<WmoGroupInfo> ReadWmoGroupInfo(const std::vector<uint8_t>& g)
{
    const auto mogp = Mogp(g);
    if (!mogp) return std::nullopt;
    WmoGroupInfo info;
    const size_t h = mogp->off;
    ReadAt(g, h, info.nameOffset);
    ReadAt(g, h + 4, info.descOffset);
    ReadAt(g, h + 8, info.flags);
    for (int k = 0; k < 6; ++k) ReadAt(g, h + 0x0C + size_t(k) * 4, info.box[k]);
    ReadAt(g, h + 0x24, info.portalStart);
    ReadAt(g, h + 0x26, info.portalCount);
    for (int k = 0; k < 4; ++k) info.fogs[k] = g[h + 0x30 + size_t(k)];
    ReadAt(g, h + 0x34, info.liquid);
    ReadAt(g, h + 0x38, info.uniqueId);
    for (const Chunk& c : Chunks(g, h + 68, h + mogp->size))
    {
        if (c.tag == Tag("MOPY"))
            for (size_t i = 1; i < c.size; i += 2)
                if (g[c.off + i] != 0xFF) info.materials.insert(g[c.off + i]);
        if (c.tag == Tag("MOBA"))
            for (size_t i = 0; i + 24 <= c.size; i += 24) info.materials.insert(g[c.off + i + 23]);
        if (c.tag == Tag("MOLR"))
            for (size_t i = 0; i + 2 <= c.size; i += 2)
            {
                uint16_t l = 0;
                ReadAt(g, c.off + i, l);
                info.lights.push_back(l);
            }
        if (c.tag == Tag("MLIQ") && c.size >= 30)
        {
            uint16_t m = 0;
            ReadAt(g, c.off + 28, m);
            if (m != 0xFFFF) info.materials.insert(m);
            info.hasLiquid = true;
        }
    }
    return info;
}

std::string WmoGroupPlan::Key() const
{
    const nlohmann::json j = { source, group, at.Key(), outdoor, name, descName, materials, lights, fogs ? nlohmann::json(*fogs) : nlohmann::json(),
                               portalStart, portalCount, uniqueId ? nlohmann::json(*uniqueId) : nlohmann::json(), modr };
    return j.dump();
}

std::vector<uint8_t> EmitWmoGroup(const WmoGroupPlan& p, const std::vector<uint8_t>& g)
{
    auto material = [&](uint16_t m) -> uint16_t { return m < p.materials.size() && p.materials[m] >= 0 ? uint16_t(p.materials[m]) : m; };
    std::vector<uint8_t> modr;
    for (uint16_t i : p.modr) Put(modr, i);
    std::vector<uint8_t> out;
    for (const Chunk& top : Chunks(g, 0, g.size()))
    {
        if (top.tag != Tag("MOGP") || top.size < 68) { PutChunk(out, top.tag, g.data() + top.off, top.size); continue; }
        std::vector<uint8_t> inner(g.begin() + std::ptrdiff_t(top.off), g.begin() + std::ptrdiff_t(top.off + 68));
        if (p.name >= 0) WriteAt(inner, 0, uint32_t(p.name));
        if (p.descName >= 0) WriteAt(inner, 4, uint32_t(p.descName));
        uint32_t flags = 0;
        ReadAt(inner, 8, flags);
        if (p.outdoor == 1) flags = (flags | 0x8) & ~0x2000u;
        if (p.outdoor == 0) flags = (flags | 0x2000) & ~0x8u;
        flags = modr.empty() ? flags & ~0x800u : flags | 0x800u;
        WriteAt(inner, 8, flags);
        WmoBox box{};
        for (int k = 0; k < 6; ++k) ReadAt(inner, 0x0C + size_t(k) * 4, box[k]);
        box = p.at.Box(box);
        for (int k = 0; k < 6; ++k) WriteAt(inner, 0x0C + size_t(k) * 4, box[k]);
        WriteAt(inner, 0x24, p.portalStart);
        WriteAt(inner, 0x26, p.portalCount);
        if (p.fogs)
            for (int k = 0; k < 4; ++k) inner[0x30 + size_t(k)] = (*p.fogs)[k];
        if (p.uniqueId) WriteAt(inner, 0x38, *p.uniqueId);

        bool afterBatches = false, wrote = modr.empty();
        for (const Chunk& c : Chunks(g, top.off + 68, top.off + top.size))
        {
            if (c.tag == Tag("MODR")) continue;
            if (afterBatches && !wrote && c.tag != Tag("MOLR")) { PutChunk(inner, Tag("MODR"), modr); wrote = true; }
            std::vector<uint8_t> b(g.begin() + std::ptrdiff_t(c.off), g.begin() + std::ptrdiff_t(c.off + c.size));
            if (c.tag == Tag("MOPY"))
                for (size_t i = 1; i < b.size(); i += 2)
                    if (b[i] != 0xFF) b[i] = uint8_t(std::min<uint16_t>(material(b[i]), 0xFE));
            if (c.tag == Tag("MOVT") || c.tag == Tag("MONR"))
                for (size_t i = 0; i + 12 <= b.size(); i += 12)
                {
                    float v[3];
                    std::memcpy(v, b.data() + i, 12);
                    if (c.tag == Tag("MOVT")) p.at.Point(v); else p.at.Vector(v);
                    std::memcpy(b.data() + i, v, 12);
                }
            if (c.tag == Tag("MOBA"))
                for (size_t i = 0; i + 24 <= b.size(); i += 24)
                {
                    int16_t s[6];
                    std::memcpy(s, b.data() + i, 12);
                    const WmoBox moved = p.at.Box({ float(s[0]), float(s[1]), float(s[2]), float(s[3]), float(s[4]), float(s[5]) });
                    for (int k = 0; k < 6; ++k)
                        s[k] = int16_t(std::clamp(k < 3 ? std::floor(moved[k]) : std::ceil(moved[k]), -32768.0f, 32767.0f));
                    std::memcpy(b.data() + i, s, 12);
                    if (b[i + 23] != 0xFF) b[i + 23] = uint8_t(std::min<uint16_t>(material(b[i + 23]), 0xFE));
                }
            if (c.tag == Tag("MOLR") && !p.lights.empty())
                for (size_t i = 0; i + 2 <= b.size(); i += 2)
                {
                    uint16_t l = 0;
                    ReadAt(b, i, l);
                    if (l < p.lights.size() && p.lights[l] >= 0) WriteAt(b, i, uint16_t(p.lights[l]));
                }
            if (c.tag == Tag("MOBN")) TransformBsp(b, p.at);
            if (c.tag == Tag("MLIQ"))
            {
                TransformLiquid(b, p.at);
                if (uint16_t m = 0; ReadAt(b, 28, m) && m != 0xFFFF) WriteAt(b, 28, material(m));
            }
            if (c.tag == Tag("MOBA")) afterBatches = true;
            PutChunk(inner, c.tag, b);
        }
        if (!wrote) PutChunk(inner, Tag("MODR"), modr);
        PutChunk(out, Tag("MOGP"), inner);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- the root

WmoComposed ComposeWmo(const std::vector<uint8_t>& base, const std::string& baseName, const std::map<uint32_t, WmoSlot>& slots, const WmoDoodads& doodads,
                       const std::function<const std::vector<uint8_t>*(const std::string&)>& rootOf,
                       const std::function<const WmoGroupInfo*(const std::string&, uint16_t)>& infoOf)
{
    WmoComposed out;
    const std::vector<uint8_t> header = Body(base, Tag("MOHD"));
    uint32_t baseGroups = 0;
    uint16_t baseFlags = 0;
    ReadAt(header, 4, baseGroups);
    ReadAt(header, 0x3C, baseFlags);
    Pool textures(Body(base, Tag("MOTX"))), names(Body(base, Tag("MOGN")));
    std::vector<uint8_t> momt = Body(base, Tag("MOMT")), molt = Body(base, Tag("MOLT")), mfog = Body(base, Tag("MFOG"));
    const std::vector<uint8_t> baseMogi = Body(base, Tag("MOGI"));
    std::vector<uint8_t> mogi, mopv, mopt, mopr;
    uint32_t baseMaxId = 0;
    for (uint32_t g = 0; g < baseGroups; ++g)
        if (const WmoGroupInfo* i = infoOf(baseName, uint16_t(g))) baseMaxId = std::max(baseMaxId, i->uniqueId);

    // Each source's root parts, read once.
    struct Source { const std::vector<uint8_t>* root; std::vector<uint8_t> motx, momt, mogn, mogi, molt, mfog, mopv, mopt, mopr; uint16_t flags = 0; };
    std::map<std::string, Source> sources;
    auto source = [&](const std::string& name) -> Source* {
        auto [it, fresh] = sources.try_emplace(Lower(name));
        if (fresh)
        {
            const std::vector<uint8_t>* r = Lower(name) == Lower(baseName) ? &base : rootOf(name);
            it->second.root = r;
            if (r)
            {
                it->second.motx = Body(*r, Tag("MOTX"));
                it->second.momt = Body(*r, Tag("MOMT"));
                it->second.mogn = Body(*r, Tag("MOGN"));
                it->second.mogi = Body(*r, Tag("MOGI"));
                it->second.molt = Body(*r, Tag("MOLT"));
                it->second.mfog = Body(*r, Tag("MFOG"));
                it->second.mopv = Body(*r, Tag("MOPV"));
                it->second.mopt = Body(*r, Tag("MOPT"));
                it->second.mopr = Body(*r, Tag("MOPR"));
                ReadAt(Body(*r, Tag("MOHD")), 0x3C, it->second.flags);
            }
        }
        return it->second.root ? &it->second : nullptr;
    };

    // Output groups: slots in id order whose source and group can be read; groups pasted together form a family.
    struct Out { uint32_t slot; const WmoSlot* s; std::string src; bool base; const WmoGroupInfo* info; Source* from; std::string family; };
    std::vector<Out> outs;
    bool allBaseAsIs = true;
    for (const auto& [id, s] : slots)
    {
        const std::string src = s.from.empty() ? baseName : s.from;
        Source* from = source(src);
        const WmoGroupInfo* info = from ? infoOf(src, s.group) : nullptr;
        if (!info) { out.notes.push_back("group " + std::to_string(s.group) + " of " + src + " cannot be read: left out"); continue; }
        const bool isBase = Lower(src) == Lower(baseName);
        allBaseAsIs = allBaseAsIs && isBase && s.at.Identity() && s.outdoor < 0 && s.group == id;
        outs.push_back({ id, &s, src, isBase, info, from, Lower(src) + "|" + std::to_string(s.batch) + "|" + s.at.Key() });
    }
    allBaseAsIs = allBaseAsIs && outs.size() == baseGroups;
    std::map<std::pair<std::string, uint16_t>, uint16_t> indexOf;   // (family, source group) -> output group
    for (size_t i = 0; i < outs.size(); ++i) indexOf[{ outs[i].family, outs[i].s->group }] = uint16_t(i);

    std::map<std::string, int> copiedLights, copiedFogs;   // family | index -> output index
    std::set<std::string> noted;
    for (size_t i = 0; i < outs.size(); ++i)
    {
        const Out& o = outs[i];
        const WmoSlot& s = *o.s;
        WmoGroupPlan plan;
        plan.source = o.src;
        plan.group = s.group;
        plan.at = s.at;
        plan.outdoor = s.outdoor;
        const bool asIs = o.base && s.at.Identity();
        if (!o.base)
        {
            // Materials it uses, their textures added to this root's pool; identical ones shared.
            for (uint16_t m : o.info->materials)
            {
                if (size_t(m) * 64 + 64 > o.from->momt.size()) continue;
                std::vector<uint8_t> entry(o.from->momt.begin() + std::ptrdiff_t(m) * 64, o.from->momt.begin() + std::ptrdiff_t(m) * 64 + 64);
                for (const size_t at : { size_t(12), size_t(24), size_t(36) })
                {
                    uint32_t off = 0;
                    ReadAt(entry, at, off);
                    WriteAt(entry, at, off < o.from->motx.size() && o.from->motx[off] ? textures.Add(CString(o.from->motx, off)) : 0u);
                }
                int index = -1;
                for (size_t k = 0; k + 64 <= momt.size() && index < 0; k += 64)
                    if (std::memcmp(momt.data() + k, entry.data(), 64) == 0) index = int(k / 64);
                if (index < 0)
                {
                    index = int(momt.size() / 64);
                    momt.insert(momt.end(), entry.begin(), entry.end());
                }
                if (index > 254 && noted.insert("materials").second) out.notes.push_back("more than 255 materials: some groups show the wrong one");
                if (plan.materials.size() <= m) plan.materials.resize(size_t(m) + 1, -1);
                plan.materials[m] = index;
            }
            plan.name = names.Add(CString(o.from->mogn, o.info->nameOffset));
            plan.descName = o.info->descOffset ? int64_t(names.Add(CString(o.from->mogn, o.info->descOffset))) : 0;
            plan.uniqueId = baseMaxId + 1 + o.slot;
            if (((o.from->flags ^ baseFlags) & 0x4) && o.info->hasLiquid && noted.insert("liquid " + o.src).second)
                out.notes.push_back(o.src + " names its liquids another way: its water may change kind here");
            if (((o.from->flags ^ baseFlags) & 0xA) && noted.insert("colours " + o.src).second)
                out.notes.push_back(o.src + " lights vertex colours another way: its groups may look brighter or darker here");
        }
        if (!asIs)
        {
            // Lights and fogs it names come along, moved with it (once per family).
            for (uint16_t l : o.info->lights)
            {
                if (size_t(l) * 48 + 48 > o.from->molt.size()) continue;
                auto [it, fresh] = copiedLights.try_emplace(o.family + "|" + std::to_string(l), int(molt.size() / 48));
                if (fresh)
                {
                    std::vector<uint8_t> e(o.from->molt.begin() + std::ptrdiff_t(l) * 48, o.from->molt.begin() + std::ptrdiff_t(l) * 48 + 48);
                    float p[3];
                    std::memcpy(p, e.data() + 8, 12);
                    s.at.Point(p);
                    std::memcpy(e.data() + 8, p, 12);
                    molt.insert(molt.end(), e.begin(), e.end());
                }
                if (plan.lights.size() <= l) plan.lights.resize(size_t(l) + 1, -1);
                plan.lights[l] = it->second;
            }
            std::array<uint8_t, 4> fogs{};
            for (int k = 0; k < 4; ++k)
            {
                const uint8_t f = o.info->fogs[k];
                if (f == 0 || size_t(f) * 48 + 48 > o.from->mfog.size()) continue;   // 0: the WMO's own default fog
                auto [it, fresh] = copiedFogs.try_emplace(o.family + "|" + std::to_string(f), int(mfog.size() / 48));
                if (fresh)
                {
                    std::vector<uint8_t> e(o.from->mfog.begin() + std::ptrdiff_t(f) * 48, o.from->mfog.begin() + std::ptrdiff_t(f) * 48 + 48);
                    float p[3];
                    std::memcpy(p, e.data() + 4, 12);
                    s.at.Point(p);
                    std::memcpy(e.data() + 4, p, 12);
                    mfog.insert(mfog.end(), e.begin(), e.end());
                }
                fogs[k] = uint8_t(std::min(it->second, 255));
            }
            plan.fogs = fogs;
        }
        // Its MOGI entry: flags as the group's, its box moved, its name.
        std::vector<uint8_t> info(32);
        if (size_t(s.group) * 32 + 32 <= o.from->mogi.size()) std::copy_n(o.from->mogi.begin() + std::ptrdiff_t(s.group) * 32, 32, info.begin());
        uint32_t flags = 0;
        ReadAt(info, 0, flags);
        if (s.outdoor == 1) flags = (flags | 0x8) & ~0x2000u;
        if (s.outdoor == 0) flags = (flags | 0x2000) & ~0x8u;
        WriteAt(info, 0, flags);
        WmoBox gbox{};
        for (int k = 0; k < 6; ++k) ReadAt(info, 4 + size_t(k) * 4, gbox[k]);
        gbox = s.at.Box(gbox);
        for (int k = 0; k < 6; ++k) WriteAt(info, 4 + size_t(k) * 4, gbox[k]);
        if (plan.name >= 0) WriteAt(info, 28, int32_t(plan.name));
        mogi.insert(mogi.end(), info.begin(), info.end());
        out.boxes.push_back(s.at.Box(o.info->box));
        out.slots.push_back(o.slot);
        out.groups.push_back(std::move(plan));
    }

    // Portals: one kept when every group it joins came in with the same family (moved the same way). A building with
    // only its own groups as they were keeps its portal tables as the file has them.
    std::map<std::string, std::map<uint16_t, uint16_t>> portalOf;   // family -> source portal -> output portal
    if (allBaseAsIs)
    {
        mopv = Body(base, Tag("MOPV"));
        mopt = Body(base, Tag("MOPT"));
        mopr = Body(base, Tag("MOPR"));
        for (size_t i = 0; i < outs.size(); ++i)
        {
            out.groups[i].portalStart = outs[i].info->portalStart;
            out.groups[i].portalCount = outs[i].info->portalCount;
        }
    }
    else
    {
        std::map<std::string, std::vector<size_t>> families;
        for (size_t i = 0; i < outs.size(); ++i) families[outs[i].family].push_back(i);
        std::vector<std::string> order;   // families by their first output group
        for (const Out& o : outs)
            if (std::find(order.begin(), order.end(), o.family) == order.end()) order.push_back(o.family);
        for (const std::string& family : order)
        {
            const Source& src = *outs[families[family][0]].from;
            const WmoTransform& at = outs[families[family][0]].s->at;
            const size_t portals = src.mopt.size() / 20, refs = src.mopr.size() / 8;
            std::vector<int> total(portals), inside(portals);
            for (size_t r = 0; r < refs; ++r)
                if (uint16_t p = 0; ReadAt(src.mopr, r * 8, p) && p < portals) ++total[p];
            for (size_t i : families[family])
            {
                const WmoGroupInfo& gi = *outs[i].info;
                for (size_t r = gi.portalStart; r < size_t(gi.portalStart) + gi.portalCount && r < refs; ++r)
                {
                    uint16_t p = 0, other = 0;
                    ReadAt(src.mopr, r * 8, p);
                    ReadAt(src.mopr, r * 8 + 2, other);
                    if (p < portals) inside[p] += indexOf.count({ family, other }) ? 1 : -100000;
                }
            }
            for (size_t p = 0; p < portals; ++p)
            {
                const bool unmoved = outs[families[family][0]].base && at.Identity();   // the building as it was keeps even portals no group names
                if (total[p] == 0 ? !unmoved : inside[p] != total[p]) continue;
                uint16_t first = 0, count = 0;
                ReadAt(src.mopt, p * 20, first);
                ReadAt(src.mopt, p * 20 + 2, count);
                float n[3], d = 0, v0[3] = {};
                std::memcpy(n, src.mopt.data() + p * 20 + 4, 12);
                ReadAt(src.mopt, p * 20 + 16, d);
                std::vector<uint8_t> entry(20);
                WriteAt(entry, 0, uint16_t(mopv.size() / 12));
                WriteAt(entry, 2, count);
                for (uint16_t k = 0; k < count && (size_t(first) + k) * 12 + 12 <= src.mopv.size(); ++k)
                {
                    float v[3];
                    std::memcpy(v, src.mopv.data() + (size_t(first) + k) * 12, 12);
                    if (k == 0) std::copy(v, v + 3, v0);
                    at.Point(v);
                    mopv.insert(mopv.end(), reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 12);
                }
                // The plane turns with it; its distance moves by the offset along the turned normal, in the file's sign
                // convention (n . v = d, or n . v + d = 0: told apart on its first vertex).
                const float before = n[0] * v0[0] + n[1] * v0[1] + n[2] * v0[2];
                const float sign = count == 0 || std::fabs(before - d) <= std::fabs(before + d) ? 1.0f : -1.0f;
                at.Vector(n);
                std::memcpy(entry.data() + 4, n, 12);
                WriteAt(entry, 16, d + sign * (n[0] * at.offset[0] + n[1] * at.offset[1] + n[2] * at.offset[2]));
                portalOf[family][uint16_t(p)] = uint16_t(mopt.size() / 20);
                mopt.insert(mopt.end(), entry.begin(), entry.end());
            }
        }
    }
    for (size_t i = 0; i < outs.size() && !allBaseAsIs; ++i)
    {
        const Out& o = outs[i];
        WmoGroupPlan& plan = out.groups[i];
        plan.portalStart = uint16_t(mopr.size() / 8);
        const auto& kept = portalOf[o.family];
        for (size_t r = o.info->portalStart; r < size_t(o.info->portalStart) + o.info->portalCount && r * 8 + 8 <= o.from->mopr.size(); ++r)
        {
            uint16_t p = 0, other = 0;
            ReadAt(o.from->mopr, r * 8, p);
            ReadAt(o.from->mopr, r * 8 + 2, other);
            const auto k = kept.find(p);
            const auto to = indexOf.find({ o.family, other });
            if (k == kept.end() || to == indexOf.end()) continue;
            std::vector<uint8_t> e(o.from->mopr.begin() + std::ptrdiff_t(r) * 8, o.from->mopr.begin() + std::ptrdiff_t(r) * 8 + 8);
            WriteAt(e, 0, k->second);
            WriteAt(e, 2, to->second);
            mopr.insert(mopr.end(), e.begin(), e.end());
        }
        plan.portalCount = uint16_t(mopr.size() / 8 - plan.portalStart);
    }

    // Doodads: their slots to output groups.
    std::map<uint32_t, int> outOfSlot;
    for (size_t i = 0; i < out.slots.size(); ++i) outOfSlot[out.slots[i]] = int(i);
    std::vector<uint8_t> mods, modn, modd;
    std::vector<std::vector<uint16_t>> modr;
    uint32_t counts[3] = {};
    WmoDoodadChunks(doodads, out.boxes, [&](uint16_t slot) { const auto it = outOfSlot.find(slot); return it == outOfSlot.end() ? -1 : it->second; },
                    mods, modn, modd, modr, counts);
    for (size_t i = 0; i < out.groups.size(); ++i) out.groups[i].modr = modr[i];

    std::vector<uint8_t> mohd = header;
    mohd.resize(std::max<size_t>(mohd.size(), 64));
    WriteAt(mohd, 0x00, uint32_t(momt.size() / 64));
    WriteAt(mohd, 0x04, uint32_t(out.groups.size()));
    WriteAt(mohd, 0x08, uint32_t(mopt.size() / 20));
    WriteAt(mohd, 0x0C, uint32_t(molt.size() / 48));
    WriteAt(mohd, 0x10, counts[0]);
    WriteAt(mohd, 0x14, counts[1]);
    WriteAt(mohd, 0x18, counts[2]);
    if (!allBaseAsIs && !out.boxes.empty())
    {
        WmoBox all = out.boxes[0];
        for (const WmoBox& b : out.boxes)
            for (int k = 0; k < 3; ++k) { all[k] = std::min(all[k], b[k]); all[3 + k] = std::max(all[3 + k], b[3 + k]); }
        for (int k = 0; k < 6; ++k) WriteAt(mohd, 0x24 + size_t(k) * 4, all[k]);
    }

    // The base's chunks in its own order with these bodies; any of them it lacks in the client's order.
    const std::map<uint32_t, const std::vector<uint8_t>*> bodies = {
        { Tag("MOHD"), &mohd }, { Tag("MOTX"), &textures.bytes }, { Tag("MOMT"), &momt }, { Tag("MOGN"), &names.bytes }, { Tag("MOGI"), &mogi },
        { Tag("MOPV"), &mopv }, { Tag("MOPT"), &mopt }, { Tag("MOPR"), &mopr }, { Tag("MOLT"), &molt }, { Tag("MODS"), &mods },
        { Tag("MODN"), &modn }, { Tag("MODD"), &modd }, { Tag("MFOG"), &mfog } };
    const auto chunks = Chunks(base, 0, base.size());
    bool complete = true;
    for (const auto& [tag, body] : bodies)
        complete = complete && std::any_of(chunks.begin(), chunks.end(), [tag = tag](const Chunk& c) { return c.tag == tag; });
    std::set<uint32_t> done;
    if (complete)
        for (const Chunk& c : chunks)
        {
            const auto b = bodies.find(c.tag);
            if (b == bodies.end()) PutChunk(out.root, c.tag, base.data() + c.off, c.size);
            else if (done.insert(c.tag).second) PutChunk(out.root, c.tag, *b->second);
        }
    else
    {
        static const char* kOrder[] = { "MVER", "MOHD", "MOTX", "MOMT", "MOGN", "MOGI", "MOSB", "MOPV", "MOPT", "MOPR", "MOVV", "MOVB", "MOLT", "MODS", "MODN", "MODD", "MFOG" };
        for (const char* name : kOrder)
        {
            const uint32_t tag = uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 | uint32_t(uint8_t(name[2])) << 8 | uint32_t(uint8_t(name[3]));
            const auto b = bodies.find(tag);
            PutChunk(out.root, tag, b != bodies.end() ? *b->second : Body(base, tag));
            done.insert(tag);
        }
        for (const Chunk& c : chunks)
            if (!done.count(c.tag)) PutChunk(out.root, c.tag, base.data() + c.off, c.size);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- self test

namespace
{
    /// A small WMO: `groups` boxes side by side along x (10 yd each), one portal between each neighbour pair, one
    /// material, one light named by group 0. Each group: a floor of two triangles and a two-leaf collision tree.
    struct TestWmo { std::vector<uint8_t> root; std::vector<std::vector<uint8_t>> groups; };
    TestWmo MakeTestWmo(int count, const std::string& texture, float x0)
    {
        TestWmo w;
        auto chunk = [](std::vector<uint8_t>& out, const char (&tag)[5], const std::vector<uint8_t>& body) { PutChunk(out, Tag(tag), body); };
        std::vector<uint8_t> ver = { 17, 0, 0, 0 }, mohd(64), motx(texture.begin(), texture.end()), momt(64), mogn = { 0, 0 }, mogi, mopv, mopt, mopr, molt(48), mfog(48);
        do motx.push_back(0); while (motx.size() % 4);
        WriteAt(mohd, 0, uint32_t(1));
        WriteAt(mohd, 4, uint32_t(count));
        WriteAt(mohd, 8, uint32_t(count - 1));
        WriteAt(mohd, 0xC, uint32_t(1));
        WriteAt(molt, 8, x0 + 5.0f);
        std::vector<uint32_t> nameAt;
        for (int g = 0; g < count; ++g)
        {
            nameAt.push_back(uint32_t(mogn.size()));
            const std::string n = "room" + std::to_string(g);
            mogn.insert(mogn.end(), n.begin(), n.end());
            mogn.push_back(0);
        }
        for (int p = 0; p + 1 < count; ++p)   // a square in the plane x = x0 + 10 (p + 1)
        {
            const float x = x0 + 10.0f * float(p + 1);
            std::vector<uint8_t> e(20);
            WriteAt(e, 0, uint16_t(mopv.size() / 12));
            WriteAt(e, 2, uint16_t(4));
            for (const float yz : { 0.0f, 1.0f, 2.0f, 3.0f })
                for (const float v : { x, yz < 2 ? 0.0f : 10.0f, (int(yz) % 2) ? 5.0f : 0.0f }) Put(mopv, v);
            WriteAt(e, 4, 1.0f);   // normal +x, distance x (n . v = d)
            WriteAt(e, 16, x);
            mopt.insert(mopt.end(), e.begin(), e.end());
        }
        std::vector<int> start;
        for (int g = 0; g < count; ++g)
        {
            start.push_back(int(mopr.size() / 8));
            for (int p : { g - 1, g })   // the portal on its left (side -1) and on its right (side 1)
                if (p >= 0 && p + 1 < count)
                {
                    Put(mopr, uint16_t(p));
                    Put(mopr, uint16_t(p == g ? g + 1 : g - 1));
                    Put(mopr, int16_t(p == g ? 1 : -1));
                    Put(mopr, uint16_t(0));
                }
        }
        for (int g = 0; g < count; ++g)
        {
            const float a = x0 + 10.0f * float(g), b = a + 10.0f;
            std::vector<uint8_t> info(32);
            WriteAt(info, 0, uint32_t(0x2000));
            for (int k = 0; k < 6; ++k) WriteAt(info, 4 + size_t(k) * 4, k == 0 ? a : k == 3 ? b : k < 3 ? 0.0f : 10.0f);
            WriteAt(info, 28, int32_t(nameAt[size_t(g)]));
            mogi.insert(mogi.end(), info.begin(), info.end());

            std::vector<uint8_t> mogp(68), movt, mopy = { 0, 0, 0, 0 }, movi, moba(24), mobn, mobr, molr;
            WriteAt(mogp, 0, nameAt[size_t(g)]);
            WriteAt(mogp, 8, uint32_t(0x2000 | (g == 0 ? 0x200 : 0) | 0x1));
            for (int k = 0; k < 6; ++k) WriteAt(mogp, 0x0C + size_t(k) * 4, k == 0 ? a : k == 3 ? b : k < 3 ? 0.0f : 10.0f);
            WriteAt(mogp, 0x24, uint16_t(start[size_t(g)]));
            WriteAt(mogp, 0x26, uint16_t((g > 0) + (g + 1 < count)));
            WriteAt(mogp, 0x38, uint32_t(100 + g));
            for (const float v : { a, 0.0f, 0.0f, b, 0.0f, 0.0f, b, 10.0f, 0.0f, a, 10.0f, 0.0f }) Put(movt, v);
            for (const uint16_t i : { 0, 1, 2, 0, 2, 3 }) Put(movi, i);
            WriteAt(moba, 12, uint32_t(0));
            WriteAt(moba, 16, uint16_t(6));
            WriteAt(moba, 20, uint16_t(3));
            moba[23] = 0;
            for (int k = 0; k < 6; ++k) WriteAt(moba, size_t(k) * 2, int16_t(k == 0 ? a : k == 3 ? b : k < 3 ? 0 : 10));
            // Root: split x at the middle; left leaf face 0, right leaf face 1.
            auto node = [&](uint16_t flags, int16_t neg, int16_t pos, uint16_t faces, uint32_t first, float dist) {
                Put(mobn, flags); Put(mobn, neg); Put(mobn, pos); Put(mobn, faces); Put(mobn, first); Put(mobn, dist);
            };
            node(0, 1, 2, 0, 0, (a + b) / 2);
            node(4, -1, -1, 1, 0, 0);
            node(4, -1, -1, 1, 1, 0);
            Put(mobr, uint16_t(0));
            Put(mobr, uint16_t(1));
            Put(molr, uint16_t(0));
            PutChunk(mogp, Tag("MOPY"), mopy);
            PutChunk(mogp, Tag("MOVI"), movi);
            PutChunk(mogp, Tag("MOVT"), movt);
            PutChunk(mogp, Tag("MONR"), std::vector<uint8_t>(movt.size()));
            PutChunk(mogp, Tag("MOTV"), std::vector<uint8_t>(movt.size() / 12 * 8));
            PutChunk(mogp, Tag("MOBA"), moba);
            if (g == 0) PutChunk(mogp, Tag("MOLR"), molr);
            PutChunk(mogp, Tag("MOBN"), mobn);
            PutChunk(mogp, Tag("MOBR"), mobr);
            std::vector<uint8_t> file;
            chunk(file, "MVER", ver);
            PutChunk(file, Tag("MOGP"), mogp);
            w.groups.push_back(std::move(file));
        }
        WriteAt(momt, 12, uint32_t(0));
        for (const auto& [tag, body] : std::vector<std::pair<const char*, const std::vector<uint8_t>*>>{
                 { "MVER", &ver }, { "MOHD", &mohd }, { "MOTX", &motx }, { "MOMT", &momt }, { "MOGN", &mogn }, { "MOGI", &mogi } })
            PutChunk(w.root, uint32_t(uint8_t(tag[0])) << 24 | uint32_t(uint8_t(tag[1])) << 16 | uint32_t(uint8_t(tag[2])) << 8 | uint32_t(uint8_t(tag[3])), *body);
        chunk(w.root, "MOSB", {});
        chunk(w.root, "MOPV", mopv);
        chunk(w.root, "MOPT", mopt);
        chunk(w.root, "MOPR", mopr);
        chunk(w.root, "MOVV", {});
        chunk(w.root, "MOVB", {});
        chunk(w.root, "MOLT", molt);
        chunk(w.root, "MODS", {});
        chunk(w.root, "MODN", {});
        chunk(w.root, "MODD", {});
        chunk(w.root, "MFOG", mfog);
        return w;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
}

bool WmoComposeSelfTest()
{
    // Transforms compose: a then b is a.Then(b).
    const WmoTransform a{ 1, { 1, 2, 3 } }, b{ 3, { 5, 0, -1 } };
    float p[3] = { 4, -7, 2 }, q[3] = { 4, -7, 2 };
    a.Point(p);
    b.Point(p);
    a.Then(b).Point(q);
    if (!Near(p[0], q[0]) || !Near(p[1], q[1]) || !Near(p[2], q[2])) return false;

    // A collision tree still sends each point to its leaf after any turn and move; for x and y splits.
    for (uint16_t axis : { uint16_t(0), uint16_t(1) })
        for (int turns = 0; turns < 4; ++turns)
        {
            std::vector<uint8_t> mobn, mobr;
            auto node = [&](uint16_t flags, int16_t neg, int16_t pos, uint16_t faces, uint32_t first, float dist) {
                Put(mobn, flags); Put(mobn, neg); Put(mobn, pos); Put(mobn, faces); Put(mobn, first); Put(mobn, dist);
            };
            node(axis, 1, 2, 0, 0, 5);
            node(4, -1, -1, 1, 0, 0);
            node(4, -1, -1, 1, 1, 0);
            Put(mobr, uint16_t(7));
            Put(mobr, uint16_t(9));
            const WmoTransform t{ turns, { 10, 20, 3 } };
            std::vector<uint8_t> moved = mobn;
            TransformBsp(moved, t);
            for (const float c : { 2.0f, 8.0f })
            {
                float at[3] = { axis == 0 ? c : 1.0f, axis == 1 ? c : 1.0f, 0.5f }, there[3] = { at[0], at[1], at[2] };
                t.Point(there);
                if (BspFacesAt(mobn, mobr, at) != BspFacesAt(moved, mobr, there) || BspFacesAt(mobn, mobr, at).empty()) return false;
            }
        }

    // A liquid grid turned and moved: every vertex keeps its height at its moved place.
    {
        const int xv = 3, yv = 2;
        std::vector<uint8_t> mliq(30);
        WriteAt(mliq, 0, int32_t(xv));
        WriteAt(mliq, 4, int32_t(yv));
        WriteAt(mliq, 8, int32_t(xv - 1));
        WriteAt(mliq, 12, int32_t(yv - 1));
        WriteAt(mliq, 16, 100.0f);
        WriteAt(mliq, 20, 50.0f);
        WriteAt(mliq, 24, 7.0f);
        for (int i = 0; i < xv * yv; ++i) { Put(mliq, uint32_t(0)); Put(mliq, float(i)); }
        mliq.push_back(1);
        mliq.push_back(2);
        auto places = [](const std::vector<uint8_t>& m) {
            int32_t x = 0, y = 0;
            float c[3];
            ReadAt(m, 0, x);
            ReadAt(m, 4, y);
            for (int k = 0; k < 3; ++k) ReadAt(m, 16 + size_t(k) * 4, c[k]);
            std::map<std::pair<long, long>, float> out;
            for (int j = 0; j < y; ++j)
                for (int i = 0; i < x; ++i)
                {
                    float h = 0;
                    ReadAt(m, 30 + (size_t(j) * x + i) * 8 + 4, h);
                    out[{ std::lround((c[0] + float(i) * kLiquidUnit) * 10), std::lround((c[1] + float(j) * kLiquidUnit) * 10) }] = h;
                }
            return out;
        };
        for (int turns = 1; turns < 4; ++turns)
        {
            const WmoTransform t{ turns, { 3, 4, 2 } };
            std::vector<uint8_t> moved = mliq;
            TransformLiquid(moved, t);
            std::map<std::pair<long, long>, float> want;
            for (const auto& [at, h] : places(mliq))
            {
                float v[3] = { float(at.first) / 10, float(at.second) / 10, 0 };
                t.Point(v);
                want[{ std::lround(v[0] * 10), std::lround(v[1] * 10) }] = h + 2;
            }
            if (places(moved) != want) return false;
        }
    }

    // Compose: a base of two rooms, then the three rooms of another WMO pasted turned and moved (one family), then
    // only two of them (the portal to the third goes).
    const TestWmo base = MakeTestWmo(2, "base.blp", 0), other = MakeTestWmo(3, "other.blp", 100);
    std::map<std::string, std::map<uint16_t, WmoGroupInfo>> infos;
    auto infoOf = [&](const std::string& src, uint16_t g) -> const WmoGroupInfo* {
        const TestWmo& w = src == "base.wmo" ? base : other;
        if (g >= w.groups.size()) return nullptr;
        auto& slot = infos[src][g];
        slot = *ReadWmoGroupInfo(w.groups[g]);
        return &slot;
    };
    auto rootOf = [&](const std::string& src) -> const std::vector<uint8_t>* { return src == "other.wmo" ? &other.root : nullptr; };
    WmoDoodads d;
    d.sets = { "Set_$DefaultGlobal" };
    d.slots = { { 0, WmoSlot{ "", 0 } }, { 1, WmoSlot{ "", 1 } } };
    const WmoTransform t{ 1, { 0, 50, 2 } };
    for (uint16_t g = 0; g < 3; ++g) d.slots[2 + g] = WmoSlot{ "other.wmo", g, 1, t };
    const WmoComposed all = ComposeWmo(base.root, "base.wmo", d.slots, d, rootOf, infoOf);
    uint32_t n[4] = {};
    for (int k = 0; k < 4; ++k) ReadAt(Body(all.root, Tag("MOHD")), size_t(k) * 4, n[k]);
    if (n[0] != 2 || n[1] != 5 || n[2] != 3 || n[3] != 2 || all.groups.size() != 5 || !all.notes.empty()) return false;
    const WmoGroupPlan& pasted0 = all.groups[2];
    if (pasted0.materials.size() != 1 || pasted0.materials[0] != 1 || pasted0.lights != std::vector<int>{ 1 } || !pasted0.uniqueId || pasted0.portalCount != 1)
        return false;
    // The pasted room's references point at the other pasted rooms, its floor moved, its texture and light carried.
    const std::vector<uint8_t> mopr = Body(all.root, Tag("MOPR"));
    uint16_t portal = 0, to = 0;
    ReadAt(mopr, size_t(pasted0.portalStart) * 8, portal);
    ReadAt(mopr, size_t(pasted0.portalStart) * 8 + 2, to);
    if (portal != 1 || to != 3) return false;
    const auto emitted = EmitWmoGroup(pasted0, other.groups[0]);
    const auto back = ReadWmoGroupInfo(emitted);
    float moved[3] = { 100, 0, 0 };
    t.Point(moved);
    if (!back || back->materials != std::set<uint16_t>{ 1 } || back->lights != std::vector<uint16_t>{ 1 } || !Near(back->box[1], moved[1] - 0.0f) ||
        !Near(back->box[3], moved[0]))
        return false;
    {
        // The pasted room's collision tree finds its faces where its floor is now.
        const auto mogp = Mogp(emitted);
        std::vector<uint8_t> mobn, mobr;
        for (const Chunk& c : Chunks(emitted, mogp->off + 68, mogp->off + mogp->size))
        {
            if (c.tag == Tag("MOBN")) mobn.assign(emitted.begin() + std::ptrdiff_t(c.off), emitted.begin() + std::ptrdiff_t(c.off + c.size));
            if (c.tag == Tag("MOBR")) mobr.assign(emitted.begin() + std::ptrdiff_t(c.off), emitted.begin() + std::ptrdiff_t(c.off + c.size));
        }
        float left[3] = { 102, 5, 0 }, right[3] = { 108, 5, 0 };
        t.Point(left);
        t.Point(right);
        if (BspFacesAt(mobn, mobr, left) != std::vector<uint16_t>{ 0 } || BspFacesAt(mobn, mobr, right) != std::vector<uint16_t>{ 1 }) return false;
    }
    if (Body(all.root, Tag("MOTX")).size() <= Body(base.root, Tag("MOTX")).size()) return false;   // other.blp added

    WmoDoodads some = d;
    some.slots.erase(4);
    const WmoComposed part = ComposeWmo(base.root, "base.wmo", some.slots, some, rootOf, infoOf);
    uint32_t portals = 0;
    ReadAt(Body(part.root, Tag("MOHD")), 8, portals);
    if (portals != 2 || part.groups[3].portalCount != 1) return false;   // the base's, and the pair still together
    // Moving one of a family on its own drops the portals it shared.
    some.slots[3].at = t.Then(WmoTransform{ 0, { 1, 0, 0 } });
    const WmoComposed apart = ComposeWmo(base.root, "base.wmo", some.slots, some, rootOf, infoOf);
    ReadAt(Body(apart.root, Tag("MOHD")), 8, portals);
    return portals == 1;
}
