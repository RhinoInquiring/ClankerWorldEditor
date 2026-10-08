#include "Downport.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

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

    void Put(std::vector<uint8_t>& d, const void* p, size_t n) { d.insert(d.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
    void PutU32(std::vector<uint8_t>& d, uint32_t v) { Put(d, &v, 4); }

    /// M2, SKIN and .anim chunk magics are stored in reading order.
    bool Magic(const std::vector<uint8_t>& d, size_t at, const char (&m)[5]) { return d.size() >= at + 4 && std::memcmp(d.data() + at, m, 4) == 0; }

    /// WMO chunk magics are stored reversed: as a little-endian uint32 they read as the name's big-endian value.
    constexpr uint32_t Tag(const char (&s)[5])
    {
        return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
    }

    uint32_t TagOf(const char* s)
    {
        return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }

    std::string Backslashes(std::string s)
    {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    /// A FileDataID as the game path the 3.3.5a file must name; empty (and a note) when unknown.
    std::string PathOf(uint32_t id, const FileIdName& nameOf, std::vector<std::string>& notes, const std::string& owner)
    {
        if (!id) return {};
        std::string n = nameOf ? nameOf(id) : std::string();
        if (n.empty()) notes.push_back(owner + ": no name for file " + std::to_string(id) + " (newer listfile needed)");
        return Backslashes(n);
    }

    struct M2Array { uint32_t count, offset; };

    /// Each element's offset of the M2Array at `header`, when the array lies inside the data.
    template <class Fn>
    void Each(const std::vector<uint8_t>& d, size_t header, size_t stride, Fn&& fn)
    {
        M2Array a{};
        if (!ReadAt(d, header, a) || a.offset > d.size() || (d.size() - a.offset) / stride < a.count) return;
        for (uint32_t i = 0; i < a.count; ++i) fn(a.offset + size_t(i) * stride, i);
    }

    // ---- M2 -----------------------------------------------------------------------------------------------------

    constexpr size_t kGlobalFlags = 0x10, kSequences = 0x1C, kBones = 0x2C, kViews = 0x44, kTextures = 0x50, kMaterials = 0x70,
                     kCameras = 0x110, kCameraLookup = 0x118, kParticles = 0x128;

    std::vector<uint8_t> DownportM2(const std::vector<uint8_t>& file, const FileIdName& nameOf, std::vector<std::string>& notes, const std::string& name)
    {
        std::vector<uint8_t> d;
        std::vector<uint32_t> txid;
        bool skeletonFile = false;
        if (Magic(file, 0, "MD21"))
            for (size_t pos = 0; pos + 8 <= file.size();)
            {
                uint32_t size = 0;
                ReadAt(file, pos + 4, size);
                if (size > file.size() - pos - 8) break;
                const auto body = file.begin() + std::ptrdiff_t(pos + 8);
                if (Magic(file, pos, "MD21")) d.assign(body, body + size);
                if (Magic(file, pos, "TXID")) { txid.resize(size / 4); std::memcpy(txid.data(), &*body, txid.size() * 4); }
                if (Magic(file, pos, "SKID")) skeletonFile = true;
                pos += 8 + size;
            }
        else
            d = file;
        uint32_t version = 0;
        if (!Magic(d, 0, "MD20") || d.size() < 0x130 || !ReadAt(d, 4, version)) { notes.push_back(name + ": not an M2"); return {}; }
        if (version <= 264) return d;

        WriteAt<uint32_t>(d, 4, 264);
        uint32_t flags = 0;
        ReadAt(d, kGlobalFlags, flags);
        WriteAt(d, kGlobalFlags, flags & 0xB);   // tilt x/y, blend overrides (texture combiner combos): what 3.3.5a knows
        uint32_t views = 0;
        if (ReadAt(d, kViews, views) && views > 4) WriteAt<uint32_t>(d, kViews, 4);

        // Sequences (64 bytes): Legion split the blend time into in/out halves (uint16 each); 3.3.5a has one uint32.
        Each(d, kSequences, 64, [&](size_t at, uint32_t) {
            uint32_t seqFlags = 0;
            ReadAt(d, at + 12, seqFlags);
            WriteAt(d, at + 12, seqFlags & 0xEF);
            if (version >= 272)
            {
                uint16_t in = 0, out = 0;
                ReadAt(d, at + 28, in);
                ReadAt(d, at + 30, out);
                WriteAt<uint32_t>(d, at + 28, std::max(in, out));
            }
        });
        Each(d, kBones, 88, [&](size_t at, uint32_t) {
            uint32_t f = 0;
            ReadAt(d, at + 4, f);
            WriteAt(d, at + 4, f & 0x3FF);
        });
        // Materials: flags 3.3.5a knows; blend mode 7 (Legion's blend-add) is closest to add.
        size_t blends = 0;
        Each(d, kMaterials, 4, [&](size_t at, uint32_t) {
            uint16_t f = 0, blend = 0;
            ReadAt(d, at, f);
            ReadAt(d, at + 2, blend);
            WriteAt<uint16_t>(d, at, f & 0x1F);
            if (blend > 6) { WriteAt<uint16_t>(d, at + 2, blend == 7 ? 4 : 0); ++blends; }
        });
        if (blends) notes.push_back(name + ": " + std::to_string(blends) + " material(s) used a newer blend mode, folded to add/opaque");

        // Textures (16 bytes: type, flags, name): names back from TXID, appended after the data; types past 15 folded.
        std::vector<std::pair<size_t, std::string>> names;   // entry offset -> name to write
        Each(d, kTextures, 16, [&](size_t at, uint32_t i) {
            uint32_t type = 0, texFlags = 0;
            M2Array n{};
            ReadAt(d, at, type);
            ReadAt(d, at + 4, texFlags);
            ReadAt(d, at + 8, n);
            WriteAt(d, at + 4, texFlags & 0x3);   // wrap x / wrap y
            const uint32_t id = i < txid.size() ? txid[i] : 0;
            if (type > 15) type = id || n.count > 1 ? 0 : 11;   // a file named: hard-coded; else an empty creature skin slot
            if (type == 0 && n.count <= 1)
            {
                const std::string path = PathOf(id, nameOf, notes, name);
                if (path.empty()) type = 11;
                else names.push_back({ at, path });
            }
            WriteAt(d, at, type);
        });
        for (const auto& [at, path] : names)
        {
            while (d.size() % 16) d.push_back(0);
            const M2Array n{ uint32_t(path.size() + 1), uint32_t(d.size()) };
            Put(d, path.c_str(), path.size() + 1);
            WriteAt(d, at + 8, n);
        }

        // Cameras: 272+ moved the field of view into a spline track at the end (116 bytes); 3.3.5a has one float
        // after the type (100 bytes). Rebuilt after the data with the track's first value.
        if (version >= 272)
        {
            std::vector<uint8_t> cams;
            Each(d, kCameras, 116, [&](size_t at, uint32_t) {
                float fov = 0.9749309f;   // the client's default
                M2Array values{}, first{};   // the fov track's values: per animation, spline keys (value first)
                if (ReadAt(d, at + 96 + 12, values) && values.count && ReadAt(d, values.offset, first) && first.count) ReadAt(d, first.offset, fov);
                Put(cams, d.data() + at, 4);                // type
                Put(cams, &fov, 4);
                Put(cams, d.data() + at + 4, 92);           // far, near, position track + base, target track + base, roll track
            });
            M2Array a{};
            if (ReadAt(d, kCameras, a) && a.count && cams.size() == size_t(a.count) * 100)
            {
                while (d.size() % 16) d.push_back(0);
                WriteAt(d, kCameras, M2Array{ a.count, uint32_t(d.size()) });
                d.insert(d.end(), cams.begin(), cams.end());
            }
            else if (a.count)
            {
                WriteAt(d, kCameras, M2Array{ 0, 0 });
                WriteAt(d, kCameraLookup, M2Array{ 0, 0 });
                notes.push_back(name + ": cameras unreadable, dropped (no portrait camera)");
            }
        }
        // Particle emitters changed shape after 3.3.5a (476 -> 492 bytes).
        // ponytail: dropped; map them field by field when particles matter.
        if (M2Array p{}; ReadAt(d, kParticles, p) && p.count)
        {
            WriteAt(d, kParticles, M2Array{ 0, 0 });
            notes.push_back(name + ": " + std::to_string(p.count) + " particle emitter(s) dropped (newer layout)");
        }
        if (M2Array b{}; skeletonFile && ReadAt(d, kBones, b) && !b.count)
            notes.push_back(name + ": bones live in a .skel file, not carried over: the model is static");
        return d;
    }

    /// Skin: batches whose shader 3.3.5a lacks go to the default; at most 2 texture units.
    std::vector<uint8_t> DownportSkin(std::vector<uint8_t> d, bool combinerCombos, std::vector<std::string>& notes, const std::string& name)
    {
        if (!Magic(d, 0, "SKIN")) { notes.push_back(name + ": not a skin"); return {}; }
        size_t changed = 0;
        Each(d, 36, 24, [&](size_t at, uint32_t) {
            uint16_t shader = 0, count = 0;
            ReadAt(d, at + 2, shader);
            ReadAt(d, at + 14, count);
            if (((shader & 0x8000) && !combinerCombos) || (!(shader & 0x8000) && shader > 0xFF)) { WriteAt<uint16_t>(d, at + 2, 0); ++changed; }
            if (count > 2) { WriteAt<uint16_t>(d, at + 14, 2); ++changed; }
        });
        if (changed) notes.push_back(name + ": " + std::to_string(changed) + " batch shader/texture setting(s) reduced to what 3.3.5a has");
        return d;
    }

    // ---- WMO ----------------------------------------------------------------------------------------------------

    using Chunks = std::multimap<uint32_t, std::vector<uint8_t>>;

    Chunks ReadChunks(const std::vector<uint8_t>& d, size_t begin, size_t end)
    {
        Chunks c;
        for (size_t pos = begin; pos + 8 <= end;)
        {
            uint32_t magic = 0, size = 0;
            ReadAt(d, pos, magic);
            ReadAt(d, pos + 4, size);
            const size_t clamped = std::min<size_t>(size, end - pos - 8);   // some shipped groups overstate MOGP
            c.emplace(magic, std::vector<uint8_t>(d.begin() + std::ptrdiff_t(pos + 8), d.begin() + std::ptrdiff_t(pos + 8 + clamped)));
            pos += 8 + clamped;
        }
        return c;
    }

    const std::vector<uint8_t>* First(const Chunks& c, uint32_t tag)
    {
        const auto it = c.find(tag);
        return it == c.end() ? nullptr : &it->second;
    }

    void PutChunk(std::vector<uint8_t>& out, uint32_t tag, const std::vector<uint8_t>& body)
    {
        PutU32(out, tag);
        PutU32(out, uint32_t(body.size()));
        out.insert(out.end(), body.begin(), body.end());
    }

    /// A name block (MOTX, MODN) built as names are added; each name 4-byte aligned like Blizzard's.
    struct NameBlock
    {
        std::vector<uint8_t> bytes;
        std::map<std::string, uint32_t> at;
        uint32_t Add(const std::string& s)
        {
            if (const auto it = at.find(Lower(s)); it != at.end()) return it->second;
            const uint32_t off = uint32_t(bytes.size());
            Put(bytes, s.c_str(), s.size() + 1);
            while (bytes.size() % 4) bytes.push_back(0);
            at[Lower(s)] = off;
            return off;
        }
    };

    std::string BlockString(const std::vector<uint8_t>* block, uint32_t off)
    {
        if (!block || off >= block->size()) return {};
        const char* p = reinterpret_cast<const char*>(block->data() + off);
        return std::string(p, strnlen(p, block->size() - off));
    }

    std::vector<uint8_t> DownportWmoRoot(const std::vector<uint8_t>& d, const FileIdName& nameOf, std::vector<std::string>& notes, const std::string& name)
    {
        const Chunks c = ReadChunks(d, 0, d.size());
        const auto* mohd = First(c, Tag("MOHD"));
        if (!mohd || mohd->size() < 64) { notes.push_back(name + ": no MOHD"); return {}; }
        const auto* motx = First(c, Tag("MOTX"));   // none: MOMT names textures by FileDataID
        auto BlockOf = [&](const char* t) { const auto* b = First(c, TagOf(t)); return b ? *b : std::vector<uint8_t>{}; };
        auto block = BlockOf;

        // Textures: an empty name first, so 0 means "none" for unused texture slots.
        NameBlock textures;
        textures.Add("");
        std::vector<uint8_t> momt = block("MOMT");
        size_t shaders = 0;
        for (size_t at = 0; at + 64 <= momt.size(); at += 64)
        {
            uint32_t flags = 0, shader = 0, blend = 0;
            ReadAt(momt, at, flags);
            ReadAt(momt, at + 4, shader);
            ReadAt(momt, at + 8, blend);
            WriteAt(momt, at, flags & 0x1FF);
            if (shader > 6) { WriteAt<uint32_t>(momt, at + 4, 0); ++shaders; }
            if (blend > 6) WriteAt<uint32_t>(momt, at + 8, 1);
            for (size_t field : { size_t(12), size_t(24), size_t(36) })   // texture 1, 2, 3
            {
                uint32_t ref = 0;
                ReadAt(momt, at + field, ref);
                const std::string tex = motx ? BlockString(motx, ref) : PathOf(ref, nameOf, notes, name);
                WriteAt(momt, at + field, textures.Add(tex));
            }
        }
        if (shaders) notes.push_back(name + ": " + std::to_string(shaders) + " material(s) used a newer shader, drawn with the default");

        // Doodads: MODI (FileDataIDs) or MODN (names) -> MODN, MODD name offsets rewritten.
        NameBlock doodads;
        const auto* modn = First(c, Tag("MODN"));
        const auto* modi = First(c, Tag("MODI"));
        std::vector<uint8_t> modd = block("MODD");
        for (size_t at = 0; at + 40 <= modd.size(); at += 40)
        {
            uint32_t v = 0;
            ReadAt(modd, at, v);
            const uint32_t ref = v & 0xFFFFFF;
            uint32_t id = 0;
            const std::string model = modi ? (ReadAt(*modi, size_t(ref) * 4, id) ? PathOf(id, nameOf, notes, name) : std::string()) : BlockString(modn, ref);
            WriteAt(modd, at, (doodads.Add(model) & 0xFFFFFF) | (v & 0x0F000000));
        }
        // Skybox: MOSI (FileDataID) -> MOSB (name).
        std::vector<uint8_t> mosb = block("MOSB");
        if (const auto* mosi = First(c, Tag("MOSI")); mosi && mosb.empty())
            if (uint32_t id = 0; ReadAt(*mosi, 0, id) && id)
            {
                const std::string sky = PathOf(id, nameOf, notes, name);
                Put(mosb, sky.c_str(), sky.size() + 1);
            }
        if (mosb.empty()) mosb.assign(4, 0);

        std::vector<uint8_t> header = *mohd;
        header.resize(64);
        WriteAt<uint32_t>(header, 0, uint32_t(textures.at.size() - 1));
        WriteAt<uint32_t>(header, 16, uint32_t(doodads.at.size()));
        uint16_t flags = 0;
        ReadAt(header, 60, flags);
        WriteAt<uint16_t>(header, 60, flags & 0xF);
        WriteAt<uint16_t>(header, 62, 0);   // LOD count: 3.3.5a has no WMO LODs

        // The order 3.3.5a reads them in, every one present (empty when the source had none).
        std::vector<uint8_t> out;
        std::vector<uint8_t> ver(4);
        WriteAt<uint32_t>(ver, 0, 17);
        PutChunk(out, Tag("MVER"), ver);
        PutChunk(out, Tag("MOHD"), header);
        PutChunk(out, Tag("MOTX"), textures.bytes);
        PutChunk(out, Tag("MOMT"), momt);
        for (const char* t : { "MOGN", "MOGI" }) PutChunk(out, TagOf(t), BlockOf(t));
        PutChunk(out, Tag("MOSB"), mosb);
        for (const char* t : { "MOPV", "MOPT", "MOPR", "MOVV", "MOVB", "MOLT", "MODS" })
            PutChunk(out, TagOf(t), BlockOf(t));
        PutChunk(out, Tag("MODN"), doodads.bytes);
        PutChunk(out, Tag("MODD"), modd);
        PutChunk(out, Tag("MFOG"), block("MFOG"));
        if (const auto* mcvp = First(c, Tag("MCVP"))) PutChunk(out, Tag("MCVP"), *mcvp);
        return out;
    }

    std::vector<uint8_t> DownportWmoGroup(const std::vector<uint8_t>& d, std::vector<std::string>& notes, const std::string& name)
    {
        const Chunks top = ReadChunks(d, 0, d.size());
        const auto* mogp = First(top, Tag("MOGP"));
        if (!mogp || mogp->size() < 68) { notes.push_back(name + ": no MOGP"); return {}; }
        std::vector<uint8_t> header(mogp->begin(), mogp->begin() + 68);
        const Chunks c = ReadChunks(*mogp, 68, mogp->size());
        auto nth = [&](uint32_t tag, size_t n) -> const std::vector<uint8_t>* {
            auto [b, e] = c.equal_range(tag);
            for (; b != e && n; ++b, --n) {}
            return b == e ? nullptr : &b->second;
        };
        auto body = [&](uint32_t tag) { const auto* b = nth(tag, 0); return b ? *b : std::vector<uint8_t>{}; };

        // Triangles: MOVX (32-bit) -> MOVI (16-bit) when every index fits; MPY2 (16-bit flags/material) -> MOPY.
        std::vector<uint8_t> movi = body(Tag("MOVI"));
        if (const auto* movx = nth(Tag("MOVX"), 0); movx && movi.empty())
            for (size_t at = 0; at + 4 <= movx->size(); at += 4)
            {
                uint32_t i = 0;
                ReadAt(*movx, at, i);
                if (i > 0xFFFF) { notes.push_back(name + ": more than 65535 vertices in one group, cannot be 3.3.5a"); return {}; }
                const uint16_t s = uint16_t(i);
                Put(movi, &s, 2);
            }
        std::vector<uint8_t> mopy = body(Tag("MOPY"));
        if (const auto* mpy2 = nth(Tag("MPY2"), 0); mpy2 && mopy.empty())
            for (size_t at = 0; at + 4 <= mpy2->size(); at += 4)
            {
                uint16_t f = 0, m = 0;
                ReadAt(*mpy2, at, f);
                ReadAt(*mpy2, at + 2, m);
                mopy.push_back(uint8_t(f));
                mopy.push_back(m > 0xFF ? 0xFF : uint8_t(m));   // 0xFF: collision only
            }
        const std::vector<uint8_t> movt = body(Tag("MOVT"));

        // Batches: Shadowlands put a wide material id in the bounding box (flag 0x2); back into the byte, box rebuilt.
        std::vector<uint8_t> moba = body(Tag("MOBA"));
        for (size_t at = 0; at + 24 <= moba.size(); at += 24)
        {
            uint8_t flags = moba[at + 22];
            if (!(flags & 0x2)) continue;
            uint16_t material = 0, minIndex = 0, maxIndex = 0;
            ReadAt(moba, at + 10, material);
            ReadAt(moba, at + 16, minIndex);
            ReadAt(moba, at + 18, maxIndex);
            moba[at + 23] = material > 0xFF ? 0xFF : uint8_t(material);
            moba[at + 22] = flags & ~0x2;
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (uint32_t v = minIndex; v <= maxIndex && (v + 1) * 12 <= movt.size(); ++v)
                for (int k = 0; k < 3; ++k)
                {
                    float p = 0;
                    ReadAt(movt, v * 12 + k * 4, p);
                    lo[k] = std::min(lo[k], p);
                    hi[k] = std::max(hi[k], p);
                }
            for (int k = 0; k < 3; ++k)
            {
                WriteAt<int16_t>(moba, at + k * 2, int16_t(std::clamp(lo[k], -32768.0f, 32767.0f)));
                WriteAt<int16_t>(moba, at + 6 + k * 2, int16_t(std::clamp(hi[k] + 1.0f, -32768.0f, 32767.0f)));
            }
        }

        // Optional chunks 3.3.5a reads wherever their flag is set (without checking the name), in this order.
        uint32_t flags = 0;
        ReadAt(header, 8, flags);
        flags &= 0x07FFFFFF;
        std::vector<std::pair<uint32_t, const std::vector<uint8_t>*>> optional;
        auto option = [&](uint32_t bit, std::initializer_list<std::pair<uint32_t, const std::vector<uint8_t>*>> parts) {
            bool all = true;
            for (const auto& p : parts) all &= p.second != nullptr;
            flags = all ? flags | bit : flags & ~bit;
            if (all) optional.insert(optional.end(), parts.begin(), parts.end());
        };
        option(0x200, { { Tag("MOLR"), nth(Tag("MOLR"), 0) } });
        option(0x800, { { Tag("MODR"), nth(Tag("MODR"), 0) } });
        option(0x1, { { Tag("MOBN"), nth(Tag("MOBN"), 0) }, { Tag("MOBR"), nth(Tag("MOBR"), 0) } });
        flags &= ~0x400u;   // portal batches (MPBV...): 3.3.5a shipped none
        option(0x4, { { Tag("MOCV"), nth(Tag("MOCV"), 0) } });
        option(0x1000, { { Tag("MLIQ"), nth(Tag("MLIQ"), 0) } });
        option(0x2000000, { { Tag("MOTV"), nth(Tag("MOTV"), 1) } });
        option(0x1000000, { { Tag("MOCV"), nth(Tag("MOCV"), 1) } });
        flags &= ~0x20000u;   // MORI/MORB
        WriteAt(header, 8, flags);

        std::vector<uint8_t> inner = header;
        PutChunk(inner, Tag("MOPY"), mopy);
        PutChunk(inner, Tag("MOVI"), movi);
        PutChunk(inner, Tag("MOVT"), movt);
        PutChunk(inner, Tag("MONR"), body(Tag("MONR")));
        PutChunk(inner, Tag("MOTV"), body(Tag("MOTV")));
        PutChunk(inner, Tag("MOBA"), moba);
        for (const auto& [tag, b] : optional) PutChunk(inner, tag, *b);
        std::vector<uint8_t> out, ver(4);
        WriteAt<uint32_t>(ver, 0, 17);
        PutChunk(out, Tag("MVER"), ver);
        PutChunk(out, Tag("MOGP"), inner);
        return out;
    }

    bool IsWmoGroup(const std::vector<uint8_t>& d)
    {
        uint32_t magic = 0;
        return ReadAt(d, 12, magic) && magic == Tag("MOGP");
    }

    bool IsNewerWmoRoot(const Chunks& c)
    {
        return !First(c, Tag("MOTX")) || First(c, Tag("GFID")) || First(c, Tag("MODI")) || First(c, Tag("MOSI"));
    }
}

bool IsNewerFormat(const std::string& name, const std::vector<uint8_t>& d)
{
    const std::string lower = Lower(name);
    auto ends = [&](const char* s) { return lower.size() >= std::strlen(s) && lower.compare(lower.size() - std::strlen(s), std::string::npos, s) == 0; };
    if (ends(".m2"))
    {
        uint32_t version = 0;
        return Magic(d, 0, "MD21") || (Magic(d, 0, "MD20") && ReadAt(d, 4, version) && version > 264);
    }
    if (ends(".anim")) return Magic(d, 0, "AFM2");
    if (ends(".skin")) return false;   // judged with its model (Downport)
    if (ends(".wmo"))
    {
        if (IsWmoGroup(d))
        {
            const auto top = ReadChunks(d, 0, d.size());
            const auto* mogp = First(top, Tag("MOGP"));
            if (!mogp || mogp->size() < 68) return false;
            const auto c = ReadChunks(*mogp, 68, mogp->size());
            for (const char* t : { "MOVX", "MPY2", "MOLP", "MOLS", "MOPB", "MOTA", "MDAL", "MOC2" })
                if (First(c, TagOf(t))) return true;
            // Judged by chunks only, so 3.3.5a groups from other clients pass through untouched whatever their flags.
            if (const auto* moba = First(c, Tag("MOBA")))
                for (size_t at = 0; at + 24 <= moba->size(); at += 24)
                    if ((*moba)[at + 22] & 0x2) return true;
            return false;
        }
        return IsNewerWmoRoot(ReadChunks(d, 0, d.size()));
    }
    return false;
}

std::vector<uint8_t> Downport(const std::string& name, std::vector<uint8_t> bytes, const FileIdName& nameOf, const ReadGameFile& read,
                              std::vector<std::string>& notes)
{
    const std::string lower = Lower(name);
    auto ends = [&](const char* s) { return lower.size() >= std::strlen(s) && lower.compare(lower.size() - std::strlen(s), std::string::npos, s) == 0; };
    if (ends(".m2")) return IsNewerFormat(name, bytes) ? DownportM2(bytes, nameOf, notes, name) : bytes;
    if (ends(".anim"))
    {
        if (!Magic(bytes, 0, "AFM2")) return bytes;
        uint32_t size = 0;   // the old layout, offsets relative to the AFM2 data
        ReadAt(bytes, 4, size);
        return std::vector<uint8_t>(bytes.begin() + 8, bytes.begin() + 8 + std::min<size_t>(size, bytes.size() - 8));
    }
    if (ends(".skin"))
    {
        // Its model says whether it is newer (and whether batches may use texture combiner combos): Name00.skin -> Name.m2.
        const auto model = lower.size() > 7 && read ? read(name.substr(0, name.size() - 7) + ".m2") : std::nullopt;
        if (!model || !IsNewerFormat(name.substr(0, name.size() - 7) + ".m2", *model)) return bytes;
        std::vector<std::string> ignored;
        const auto md20 = DownportM2(*model, nullptr, ignored, name);
        uint32_t flags = 0;
        ReadAt(md20, kGlobalFlags, flags);
        return DownportSkin(std::move(bytes), (flags & 0x8) != 0, notes, name);
    }
    if (ends(".wmo")) return !IsNewerFormat(name, bytes) ? bytes : IsWmoGroup(bytes) ? DownportWmoGroup(bytes, notes, name) : DownportWmoRoot(bytes, nameOf, notes, name);
    return bytes;
}

bool DownportSelfTest()
{
    std::vector<std::string> notes;
    // M2: an MD21 holding a v274 MD20 with one texture named only by TXID and a 272-style sequence.
    std::vector<uint8_t> md20(0x140, 0);
    std::memcpy(md20.data(), "MD20", 4);
    WriteAt<uint32_t>(md20, 4, 274);
    WriteAt(md20, kTextures, M2Array{ 1, 0x140 });
    md20.resize(0x150, 0);   // texture entry: type 0, no name
    WriteAt(md20, kSequences, M2Array{ 1, 0x150 });
    md20.resize(0x190, 0);
    WriteAt<uint16_t>(md20, 0x150 + 28, 150);
    WriteAt<uint16_t>(md20, 0x150 + 30, 300);
    std::vector<uint8_t> file;
    Put(file, "MD21", 4);
    PutU32(file, uint32_t(md20.size()));
    file.insert(file.end(), md20.begin(), md20.end());
    Put(file, "TXID", 4);
    PutU32(file, 4);
    PutU32(file, 1234);
    const FileIdName names = [](uint32_t id) { return id == 1234 ? std::string("world/foo/bar.blp") : std::string(); };
    const auto m2 = Downport("x.m2", file, names, nullptr, notes);
    uint32_t version = 0, blend = 0;
    M2Array tex{};
    if (!ReadAt(m2, 4, version) || version != 264 || !ReadAt(m2, 0x140 + 8, tex) || tex.count != 18) return false;
    if (std::string(reinterpret_cast<const char*>(m2.data() + tex.offset)) != "world\\foo\\bar.blp") return false;
    if (!ReadAt(m2, 0x150 + 28, blend) || blend != 300) return false;
    if (IsNewerFormat("x.m2", m2)) return false;

    // WMO root: MOMT texture by FileDataID, doodad by MODI -> names in MOTX / MODN, in 3.3.5a chunk order.
    std::vector<uint8_t> root, mohd(64, 0), momt(64, 0), modd(40, 0), modi(4, 0), ver(4, 0);
    WriteAt<uint32_t>(ver, 0, 17);
    WriteAt<uint32_t>(momt, 12, 1234);
    WriteAt<uint32_t>(momt, 4, 20);   // a newer shader
    WriteAt<uint32_t>(modi, 0, 1234);
    PutChunk(root, Tag("MVER"), ver);
    PutChunk(root, Tag("MOHD"), mohd);
    PutChunk(root, Tag("MOMT"), momt);
    PutChunk(root, Tag("MODI"), modi);
    PutChunk(root, Tag("MODD"), modd);
    if (!IsNewerFormat("a.wmo", root)) return false;
    const auto w = Downport("a.wmo", root, names, nullptr, notes);
    const Chunks c = ReadChunks(w, 0, w.size());
    const auto* motx = First(c, Tag("MOTX"));
    const auto* outMomt = First(c, Tag("MOMT"));
    uint32_t ref = 0, shader = 1;
    if (!motx || !outMomt || !ReadAt(*outMomt, 12, ref) || BlockString(motx, ref) != "world\\foo\\bar.blp" || !ReadAt(*outMomt, 4, shader) || shader) return false;
    uint32_t tag = 0;
    if (!ReadAt(w, 12, tag) || tag != Tag("MOHD") || IsNewerFormat("a.wmo", w)) return false;
    return true;
}
