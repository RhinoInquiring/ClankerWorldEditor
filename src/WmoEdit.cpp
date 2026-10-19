#include "WmoEdit.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

using namespace DirectX;

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

    /// Chunks between begin and end: magic, body offset, body size. A size past `end` is cut to it (some shipped
    /// groups state a MOGP size past the end of the file).
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

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }

    /// The MOGP body of a group file (offset, size), or none.
    std::optional<Chunk> Mogp(const std::vector<uint8_t>& g)
    {
        for (const Chunk& c : Chunks(g, 0, g.size()))
            if (c.tag == Tag("MOGP") && c.size >= 68) return c;
        return std::nullopt;
    }

    const XMMATRIX kWowToEditor = XMMatrixSet(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1);   // v_editor = v_wow * C
}

std::optional<WmoDoodads> ReadWmoDoodads(const std::vector<uint8_t>& root, const std::vector<std::vector<uint8_t>>& groups, std::string& error)
{
    std::optional<Chunk> mods, modn, modd;
    bool mohd = false;
    for (const Chunk& c : Chunks(root, 0, root.size()))
    {
        if (c.tag == Tag("MODI") || c.tag == Tag("GFID")) { error = "a newer client's WMO (doodads or groups by FileDataID): export converts it, it cannot be edited here yet"; return std::nullopt; }
        if (c.tag == Tag("MOHD")) mohd = c.size >= 64;
        if (c.tag == Tag("MODS")) mods = c;
        if (c.tag == Tag("MODN")) modn = c;
        if (c.tag == Tag("MODD")) modd = c;
    }
    if (!mohd) { error = "not a WMO root (no MOHD)"; return std::nullopt; }

    WmoDoodads out;
    uint32_t groupCount = 0;
    float bounds[6];
    WmoRootInfo(root, groupCount, bounds);
    for (uint32_t g = 0; g < groupCount && g < 512; ++g) out.slots[g] = WmoSlot{ "", uint16_t(g) };   // its own groups, as they are
    auto name = [&](uint32_t at) {
        if (!modn || at >= modn->size) return std::string();
        const char* s = reinterpret_cast<const char*>(root.data() + modn->off + at);
        return std::string(s, strnlen(s, modn->size - at));
    };
    // MODD: 40 bytes each (name offset in the low 24 bits + flags, position, quaternion x y z w, scale, BGRA colour).
    std::vector<WmoDoodads::Doodad> listed;
    for (size_t i = 0; modd && i + 40 <= modd->size; i += 40)
    {
        const size_t at = modd->off + i;
        WmoDoodads::Doodad d;
        uint32_t nameAndFlags = 0;
        ReadAt(root, at, nameAndFlags);
        d.model = name(nameAndFlags & 0xFFFFFF);
        d.flags = uint8_t(nameAndFlags >> 24);
        for (int k = 0; k < 3; ++k) ReadAt(root, at + 4 + size_t(k) * 4, d.pos[k]);
        for (int k = 0; k < 4; ++k) ReadAt(root, at + 16 + size_t(k) * 4, d.rot[k]);
        ReadAt(root, at + 32, d.scale);
        ReadAt(root, at + 36, d.color);
        listed.push_back(std::move(d));
    }
    // MODS: name[20], first, count, pad. A doodad in two sets' ranges becomes one per set (the copy computes its groups).
    // ponytail: doodads in no set are dropped (the game never shows them).
    uint32_t extra = uint32_t(listed.size());
    std::vector<bool> used(listed.size());
    for (size_t i = 0; mods && i + 32 <= mods->size; i += 32)
    {
        const char* s = reinterpret_cast<const char*>(root.data() + mods->off + i);
        out.sets.emplace_back(s, strnlen(s, 20));
        uint32_t first = 0, count = 0;
        ReadAt(root, mods->off + i + 20, first);
        ReadAt(root, mods->off + i + 24, count);
        for (uint32_t k = first; k < first + count && k < listed.size(); ++k)
        {
            WmoDoodads::Doodad d = listed[k];
            d.set = uint32_t(out.sets.size() - 1);
            out.doodads[used[k] ? extra++ : k] = std::move(d);
            used[k] = true;
        }
    }
    if (out.sets.empty()) out.sets.push_back("Set_$DefaultGlobal");
    // Each group's MODR: the doodads drawn (and lit) with that group.
    for (size_t g = 0; g < groups.size(); ++g)
    {
        const auto mogp = Mogp(groups[g]);
        if (!mogp) continue;
        for (const Chunk& c : Chunks(groups[g], mogp->off + 68, mogp->off + mogp->size))
            if (c.tag == Tag("MODR"))
                for (size_t i = 0; i + 2 <= c.size; i += 2)
                {
                    uint16_t index = 0;
                    ReadAt(groups[g], c.off + i, index);
                    if (const auto it = out.doodads.find(index); it != out.doodads.end()) it->second.groups.push_back(uint16_t(g));
                }
    }
    return out;
}

std::vector<WmoBox> WmoGroupBoxes(const std::vector<std::vector<uint8_t>>& groups)
{
    std::vector<WmoBox> out(groups.size(), WmoBox{});
    for (size_t g = 0; g < groups.size(); ++g)
        if (const auto mogp = Mogp(groups[g]))
            for (int k = 0; k < 6; ++k) ReadAt(groups[g], mogp->off + 0x0C + size_t(k) * 4, out[g][k]);
    return out;
}

void MeshDoodads(const WmoDoodads& d, std::vector<ModelMesh::Doodad>& doodads, std::vector<ModelMesh::DoodadSet>& sets, std::vector<uint32_t>& ids)
{
    doodads.clear();
    sets.clear();
    ids.clear();
    for (uint32_t s = 0; s < d.sets.size(); ++s)
    {
        ModelMesh::DoodadSet set{ uint32_t(doodads.size()), 0, d.sets[s] };
        for (const auto& [id, e] : d.doodads)
        {
            if (e.set != s) continue;
            doodads.push_back({ e.model, { e.pos[0], e.pos[1], e.pos[2] }, { e.rot[0], e.rot[1], e.rot[2], e.rot[3] }, e.scale });
            ids.push_back(id);
            ++set.count;
        }
        sets.push_back(std::move(set));
    }
}

uint32_t WmoDoodads::NextBatch() const
{
    uint32_t next = 1;
    for (const auto& [id, s] : slots) next = std::max(next, s.batch + 1);
    return next;
}

void TransformDoodad(WmoDoodads::Doodad& d, const WmoTransform& t)
{
    t.Point(d.pos);
    const XMVECTOR turn = XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), float(t.turns & 3) * XM_PIDIV2);
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(d.rot), XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(d.rot)), turn)));
}

int BoxGroupOf(const WmoDoodads::Doodad& e, const std::vector<WmoBox>& boxes)
{
    int best = -1;
    float bestVolume = INFINITY, bestGap = INFINITY;
    for (size_t g = 0; g < boxes.size(); ++g)
    {
        const WmoBox& b = boxes[g];
        float gap = 0;
        for (int k = 0; k < 3; ++k) gap += std::pow(std::max({ b[k] - e.pos[k], e.pos[k] - b[3 + k], 0.0f }), 2.0f);
        const float volume = (b[3] - b[0]) * (b[4] - b[1]) * (b[5] - b[2]);
        if (gap < bestGap || (gap == 0 && bestGap == 0 && volume < bestVolume)) { best = int(g); bestGap = gap; bestVolume = volume; }
    }
    return best;
}

void WmoDoodadChunks(const WmoDoodads& d, const std::vector<WmoBox>& boxes, const std::function<int(uint16_t)>& indexOf, std::vector<uint8_t>& mods,
                     std::vector<uint8_t>& modn, std::vector<uint8_t>& modd, std::vector<std::vector<uint16_t>>& modr, uint32_t counts[3])
{
    std::vector<ModelMesh::Doodad> order;
    std::vector<ModelMesh::DoodadSet> sets;
    std::vector<uint32_t> ids;
    MeshDoodads(d, order, sets, ids);

    // MODN: each name once, zero-ended, padded to four bytes as the game's files are.
    std::map<std::string, uint32_t> nameAt;
    modr.assign(boxes.size(), {});
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const WmoDoodads::Doodad& e = d.doodads.at(ids[i]);
        auto [it, fresh] = nameAt.try_emplace(Lower(e.model), uint32_t(modn.size()));
        if (fresh)
        {
            modn.insert(modn.end(), e.model.begin(), e.model.end());
            do modn.push_back(0); while (modn.size() % 4);
        }
        Put(modd, (it->second & 0xFFFFFF) | uint32_t(e.flags) << 24);
        for (float v : e.pos) Put(modd, v);
        for (float v : e.rot) Put(modd, v);
        Put(modd, e.scale);
        Put(modd, e.color);

        // Its groups: as the source listed them (still there), else the smallest box holding it, else the nearest box.
        std::vector<int> in;
        for (uint16_t slot : e.groups)
            if (const int g = indexOf(slot); g >= 0 && size_t(g) < boxes.size()) in.push_back(g);
        if (in.empty())
            if (const int g = BoxGroupOf(e, boxes); g >= 0) in.push_back(g);
        for (int g : in) modr[size_t(g)].push_back(uint16_t(i));   // ponytail: over 65535 doodads wraps (no shipped WMO comes close)
    }
    for (const auto& s : sets)
    {
        char name[20] = {};
        std::memcpy(name, s.name.data(), std::min<size_t>(s.name.size(), 19));
        mods.insert(mods.end(), name, name + 20);
        Put(mods, s.first);
        Put(mods, s.count);
        Put(mods, uint32_t(0));
    }
    counts[0] = uint32_t(nameAt.size());
    counts[1] = uint32_t(ids.size());
    counts[2] = uint32_t(sets.size());
}

std::vector<uint8_t> WriteWmoRoot(const std::vector<uint8_t>& root, const WmoDoodads& d, const std::vector<WmoBox>& boxes,
                                  std::vector<std::vector<uint16_t>>& modr)
{
    std::vector<uint8_t> modn, modd, mods;
    uint32_t counts[3] = {};
    WmoDoodadChunks(d, boxes, [](uint16_t slot) { return int(slot); }, mods, modn, modd, modr, counts);

    // The same chunks in the same order, the three doodad chunks replaced (added before MFOG when the file had none).
    std::vector<uint8_t> out;
    bool wrote = false;
    auto doodadChunks = [&] {
        PutChunk(out, Tag("MODS"), mods);
        PutChunk(out, Tag("MODN"), modn);
        PutChunk(out, Tag("MODD"), modd);
        wrote = true;
    };
    for (const Chunk& c : Chunks(root, 0, root.size()))
    {
        if (c.tag == Tag("MODS") || c.tag == Tag("MODN") || c.tag == Tag("MODD"))
        {
            if (c.tag == Tag("MODS")) doodadChunks();
            continue;
        }
        if (c.tag == Tag("MFOG") && !wrote) doodadChunks();
        if (c.tag == Tag("MOHD"))
        {
            std::vector<uint8_t> header(root.begin() + std::ptrdiff_t(c.off), root.begin() + std::ptrdiff_t(c.off + c.size));
            WriteAt(header, 0x10, counts[0]);
            WriteAt(header, 0x14, counts[1]);
            WriteAt(header, 0x18, counts[2]);
            PutChunk(out, c.tag, header);
            continue;
        }
        PutChunk(out, c.tag, root.data() + c.off, c.size);
    }
    if (!wrote) doodadChunks();
    return out;
}

std::vector<uint8_t> WriteWmoGroupModr(const std::vector<uint8_t>& group, const std::vector<uint16_t>& modr)
{
    std::vector<uint8_t> body;
    for (uint16_t i : modr) Put(body, i);
    std::vector<uint8_t> out;
    for (const Chunk& top : Chunks(group, 0, group.size()))
    {
        if (top.tag != Tag("MOGP") || top.size < 68) { PutChunk(out, top.tag, group.data() + top.off, top.size); continue; }
        std::vector<uint8_t> inner(group.begin() + std::ptrdiff_t(top.off), group.begin() + std::ptrdiff_t(top.off + 68));
        uint32_t flags = 0;
        ReadAt(inner, 8, flags);
        WriteAt(inner, 8, modr.empty() ? flags & ~0x800u : flags | 0x800u);
        // 3.3.5a reads the optional chunks in flag order without checking names: MODR right after MOBA and MOLR.
        bool afterBatches = false, wrote = modr.empty();
        for (const Chunk& c : Chunks(group, top.off + 68, top.off + top.size))
        {
            if (c.tag == Tag("MODR")) continue;
            if (afterBatches && !wrote && c.tag != Tag("MOLR")) { PutChunk(inner, Tag("MODR"), body); wrote = true; }
            if (c.tag == Tag("MOBA")) afterBatches = true;
            PutChunk(inner, c.tag, group.data() + c.off, c.size);
        }
        if (!wrote) PutChunk(inner, Tag("MODR"), body);
        PutChunk(out, Tag("MOGP"), inner);
    }
    return out;
}

XMMATRIX DoodadMatrix(const WmoDoodads::Doodad& d)
{
    ModelMesh::Doodad m{ d.model, { d.pos[0], d.pos[1], d.pos[2] }, { d.rot[0], d.rot[1], d.rot[2], d.rot[3] }, d.scale };
    return WmoDoodadMatrix(m);
}

void SetDoodadMatrix(WmoDoodads::Doodad& d, FXMMATRIX m)
{
    // WmoDoodadMatrix is C^-1 * local * C, so the WoW-axes matrix is C * m * C^-1.
    XMVECTOR s, q, t;
    if (!XMMatrixDecompose(&s, &q, &t, kWowToEditor * m * XMMatrixTranspose(kWowToEditor))) return;
    XMFLOAT3 scale, pos;
    XMFLOAT4 rot;
    XMStoreFloat3(&scale, s);
    XMStoreFloat3(&pos, t);
    XMStoreFloat4(&rot, XMQuaternionNormalize(q));
    d.pos[0] = pos.x; d.pos[1] = pos.y; d.pos[2] = pos.z;
    d.rot[0] = rot.x; d.rot[1] = rot.y; d.rot[2] = rot.z; d.rot[3] = rot.w;
    d.scale = std::clamp((scale.x + scale.y + scale.z) / 3, 1.0f / 1024.0f, 63.0f);
}

nlohmann::json ToJson(const WmoDoodads::Doodad& d)
{
    nlohmann::json j = { { "m", d.model }, { "p", { d.pos[0], d.pos[1], d.pos[2] } }, { "q", { d.rot[0], d.rot[1], d.rot[2], d.rot[3] } },
                         { "s", d.scale }, { "c", d.color }, { "f", d.flags }, { "set", d.set } };
    if (!d.groups.empty()) j["g"] = d.groups;
    return j;
}

WmoDoodads::Doodad DoodadFromJson(const nlohmann::json& j)
{
    WmoDoodads::Doodad d;
    d.model = j.value("m", std::string());
    for (int k = 0; k < 3; ++k) d.pos[k] = j.at("p").at(k).get<float>();
    for (int k = 0; k < 4; ++k) d.rot[k] = j.at("q").at(k).get<float>();
    d.scale = j.value("s", 1.0f);
    d.color = j.value("c", 0xFF7F7F7Fu);
    d.flags = j.value("f", uint8_t(0));
    d.set = j.value("set", 0u);
    if (const auto g = j.find("g"); g != j.end()) d.groups = g->get<std::vector<uint16_t>>();
    return d;
}

std::string WmoCopyName(const std::string& root, const std::function<bool(const std::string&)>& taken)
{
    const size_t dot = root.find_last_of('.');
    const std::string stem = dot == std::string::npos ? root : root.substr(0, dot), ext = dot == std::string::npos ? ".wmo" : root.substr(dot);
    for (int n = 1;; ++n)
        if (const std::string name = stem + "_c" + std::to_string(n) + ext; !taken(name)) return name;
}


// ---------------------------------------------------------------------------------------------- the adapter

void WmoEditAdapter::Reset(const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read, const std::filesystem::path& dir)
{
    for (const auto& [key, e] : m_wmos) m_changed.insert(e.name);
    m_wmos.clear();
    m_roots.clear();
    m_written.clear();
    m_sources.clear();
    m_infos.clear();
    m_read = read;
    m_dir = dir;
    std::error_code ec;
    if (!dir.empty()) std::filesystem::remove_all(dir, ec);
    m_filesChanged = true;
}

const WmoEditAdapter::Edited* WmoEditAdapter::Find(const std::string& name) const
{
    const auto it = m_wmos.find(Lower(name));
    return it == m_wmos.end() ? nullptr : &it->second;
}

std::filesystem::path WmoEditAdapter::PathOf(const std::string& game) const
{
    std::string rel = game;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return m_dir / std::filesystem::path(rel);
}

const std::vector<uint8_t>* WmoEditAdapter::SourceRoot(const std::string& source)
{
    auto [it, fresh] = m_sources.try_emplace(Lower(source));
    if (fresh && m_read) it->second = m_read(source);
    return it->second ? &*it->second : nullptr;
}

const WmoGroupInfo* WmoEditAdapter::GroupInfo(const std::string& source, uint16_t group)
{
    auto [it, fresh] = m_infos.try_emplace(Lower(source) + "|" + std::to_string(group));
    if (fresh)
        if (const auto* root = SourceRoot(source))
            if (const auto bytes = m_read(WmoGroupFile(source, *root, group)))
            {
                it->second = ReadWmoGroupInfo(*bytes);
                if (it->second)   // its name, from the root's MOGN
                    for (const Chunk& c : Chunks(*root, 0, root->size()))
                        if (c.tag == Tag("MOGN") && it->second->nameOffset < c.size)
                        {
                            const char* s = reinterpret_cast<const char*>(root->data() + c.off + it->second->nameOffset);
                            it->second->name.assign(s, strnlen(s, c.size - it->second->nameOffset));
                        }
            }
    return it->second ? &*it->second : nullptr;
}

std::optional<WmoBox> WmoEditAdapter::SlotBox(const std::string& base, const WmoSlot& s)
{
    const WmoGroupInfo* info = GroupInfo(s.from.empty() ? base : s.from, s.group);
    if (!info) return std::nullopt;
    return s.at.Box(info->box);
}

std::optional<Change> WmoEditAdapter::MakeCreate(const std::string& name, const std::string& from, std::string& error)
{
    const auto root = m_read ? m_read(from) : std::nullopt;
    if (!root) { error = "cannot read " + from; return std::nullopt; }
    if (!ReadWmoDoodads(*root, {}, error)) return std::nullopt;
    Change c;
    c.domain = Domain();
    c.target = name;
    const auto file = [](const std::string& s) { return s.substr(s.find_last_of("\\/") + 1); };
    c.label = Lower(name) == Lower(from) ? "Edit " + file(from) + " in place" : "Copy " + file(from) + " as " + file(name);
    c.data = { { "wmo", name }, { "from", from }, { "create", true } };
    return c;
}

std::optional<Change> WmoEditAdapter::MakeEdit(const std::string& name, const WmoDoodads& before, const WmoDoodads& after, const std::string& label) const
{
    nlohmann::json sets = nlohmann::json::array(), doodads = nlohmann::json::array(), slots = nlohmann::json::array();
    for (size_t i = 0; i < std::min(before.sets.size(), after.sets.size()); ++i)
        if (before.sets[i] != after.sets[i]) sets.push_back({ i, before.sets[i], after.sets[i] });
    for (size_t i = before.sets.size(); i < after.sets.size(); ++i) sets.push_back({ i, nullptr, after.sets[i] });
    for (size_t i = before.sets.size(); i-- > after.sets.size();) sets.push_back({ i, before.sets[i], nullptr });   // last first
    auto diff = [](const auto& b, const auto& a, nlohmann::json& out) {
        std::set<uint32_t> ids;
        for (const auto& [id, x] : b) ids.insert(id);
        for (const auto& [id, x] : a) ids.insert(id);
        for (uint32_t id : ids)
        {
            const auto bi = b.find(id), ai = a.find(id);
            const nlohmann::json bj = bi == b.end() ? nlohmann::json() : ToJson(bi->second);
            const nlohmann::json aj = ai == a.end() ? nlohmann::json() : ToJson(ai->second);
            if (bj != aj) out.push_back({ id, bj, aj });
        }
    };
    diff(before.doodads, after.doodads, doodads);
    diff(before.slots, after.slots, slots);
    if (sets.empty() && doodads.empty() && slots.empty()) return std::nullopt;
    Change c;
    c.domain = Domain();
    c.target = name;
    c.label = label;
    c.data = { { "wmo", name }, { "sets", sets }, { "doodads", doodads } };
    if (!slots.empty()) c.data["slots"] = slots;
    return c;
}

void WmoEditAdapter::Apply(const Change& change)
{
    const std::string name = change.data.at("wmo"), key = Lower(name);
    if (change.data.value("create", false))
    {
        const std::string from = change.data.at("from");
        const auto* root = SourceRoot(from);
        uint32_t count = 0;
        float bounds[6];
        if (!root || !WmoRootInfo(*root, count, bounds)) return;
        std::vector<std::vector<uint8_t>> groups;
        for (uint32_t g = 0; g < count && g < 512; ++g) groups.push_back(m_read(WmoGroupFile(from, *root, g)).value_or(std::vector<uint8_t>{}));
        std::string error;
        auto doodads = ReadWmoDoodads(*root, groups, error);
        if (!doodads) return;
        m_roots[key] = *root;
        Edited& e = m_wmos[key] = Edited{ name, from, std::move(*doodads) };
        m_written.erase(key);
        Write(e);
        return;
    }
    const auto it = m_wmos.find(key);
    if (it == m_wmos.end()) return;
    WmoDoodads& d = it->second.doodads;
    for (const auto& s : ChangeStore::List(change.data, "sets"))
    {
        const size_t i = s.at(0).get<size_t>();
        if (s.at(2).is_null()) { if (i + 1 == d.sets.size() && i > 0) d.sets.pop_back(); }
        else if (s.at(1).is_null()) { if (i == d.sets.size()) d.sets.push_back(s.at(2)); }
        else if (i < d.sets.size()) d.sets[i] = s.at(2);
    }
    for (const auto& s : ChangeStore::List(change.data, "slots"))
    {
        const uint32_t id = s.at(0).get<uint32_t>();
        if (s.at(2).is_null()) d.slots.erase(id);
        else d.slots[id] = SlotFromJson(s.at(2));
    }
    for (const auto& e : ChangeStore::List(change.data, "doodads"))
    {
        const uint32_t id = e.at(0).get<uint32_t>();
        if (e.at(2).is_null()) d.doodads.erase(id);
        else d.doodads[id] = DoodadFromJson(e.at(2));
    }
    Write(it->second);
}

void WmoEditAdapter::Revert(const Change& change)
{
    const std::string key = Lower(change.data.at("wmo").get<std::string>());
    if (change.data.value("create", false)) { Remove(key); return; }
    const auto it = m_wmos.find(key);
    if (it == m_wmos.end()) return;
    WmoDoodads& d = it->second.doodads;
    const auto& doodads = ChangeStore::List(change.data, "doodads");
    for (auto e = doodads.rbegin(); e != doodads.rend(); ++e)
    {
        const uint32_t id = e->at(0).get<uint32_t>();
        if (e->at(1).is_null()) d.doodads.erase(id);
        else d.doodads[id] = DoodadFromJson(e->at(1));
    }
    const auto& slots = ChangeStore::List(change.data, "slots");
    for (auto s = slots.rbegin(); s != slots.rend(); ++s)
    {
        const uint32_t id = s->at(0).get<uint32_t>();
        if (s->at(1).is_null()) d.slots.erase(id);
        else d.slots[id] = SlotFromJson(s->at(1));
    }
    const auto& sets = ChangeStore::List(change.data, "sets");
    for (auto s = sets.rbegin(); s != sets.rend(); ++s)
    {
        const size_t i = s->at(0).get<size_t>();
        if (s->at(1).is_null()) { if (i + 1 == d.sets.size() && i > 0) d.sets.pop_back(); }
        else if (s->at(2).is_null()) { if (i == d.sets.size()) d.sets.push_back(s->at(1)); }
        else if (i < d.sets.size()) d.sets[i] = s->at(1);
    }
    Write(it->second);
}

namespace
{
    bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return bool(f);
    }
}

void WmoEditAdapter::Write(Edited& e)
{
    const std::string key = Lower(e.name);
    const WmoComposed out = ComposeWmo(m_roots.at(key), e.from, e.doodads.slots, e.doodads, [this](const std::string& s) { return SourceRoot(s); },
                                       [this](const std::string& s, uint16_t g) { return GroupInfo(s, g); });
    std::error_code ec;
    if (!std::filesystem::exists(PathOf(e.name), ec)) m_filesChanged = true;   // a new file: the file layer lists it
    WriteBytes(PathOf(e.name), out.root);
    // Groups only when what decides their bytes changed (big buildings have hundreds); a change beyond their doodad
    // lists means the building's shape changed and the renderer reads it again.
    Written& w = m_written[key];
    const bool first = w.keys.empty();
    bool reshaped = out.groups.size() != w.keys.size() && !first;
    w.keys.resize(out.groups.size());
    w.shapes.resize(out.groups.size());
    for (size_t g = 0; g < out.groups.size(); ++g)
    {
        const WmoGroupPlan& plan = out.groups[g];
        WmoGroupPlan shape = plan;
        shape.modr.clear();
        const std::string planKey = plan.Key(), shapeKey = shape.Key();
        if (w.keys[g] == planKey) continue;
        if (!first && w.shapes[g] != shapeKey) reshaped = true;
        const auto* root = SourceRoot(plan.source);
        const auto source = root ? m_read(WmoGroupFile(plan.source, *root, plan.group)) : std::nullopt;
        if (!source) continue;
        const auto path = PathOf(WmoGroupName(e.name, uint32_t(g)));
        if (!std::filesystem::exists(path, ec)) m_filesChanged = true;
        WriteBytes(path, EmitWmoGroup(plan, *source));
        w.keys[g] = planKey;
        w.shapes[g] = shapeKey;
    }
    for (size_t g = out.groups.size(); g < w.count; ++g)   // groups cut since the last write
    {
        std::filesystem::remove(PathOf(WmoGroupName(e.name, uint32_t(g))), ec);
        m_filesChanged = true;
    }
    w.count = out.groups.size();
    e.boxes = out.boxes;
    e.order = out.slots;
    e.notes = out.notes;
    m_changed.insert(e.name);
    if (reshaped) m_rebuilt.insert(e.name);
}

void WmoEditAdapter::Remove(const std::string& key)
{
    const auto it = m_wmos.find(key);
    if (it == m_wmos.end()) return;
    std::error_code ec;
    std::filesystem::remove(PathOf(it->second.name), ec);
    for (size_t g = 0; g < m_written[key].count; ++g) std::filesystem::remove(PathOf(WmoGroupName(it->second.name, uint32_t(g))), ec);
    m_changed.insert(it->second.name);
    m_rebuilt.insert(it->second.name);   // an original edited in place has its own shape again
    m_filesChanged = true;
    m_wmos.erase(it);
    m_roots.erase(key);
    m_written.erase(key);
}

// ---------------------------------------------------------------------------------------------- self test

bool WmoEditSelfTest()
{
    // A root with one set of two doodads, and one group whose box holds both and lists them in MODR.
    auto chunk = [](std::vector<uint8_t>& out, const char (&tag)[5], const std::vector<uint8_t>& body) { PutChunk(out, Tag(tag), body); };
    std::vector<uint8_t> root, header(64), mods, modn = { 'a', '.', 'm', 'd', 'x', 0, 0, 0 }, modd, ver = { 17, 0, 0, 0 };
    WriteAt(header, 4, uint32_t(1));
    char setName[20] = "Set_$DefaultGlobal";
    mods.insert(mods.end(), setName, setName + 20);
    Put(mods, uint32_t(0));
    Put(mods, uint32_t(2));
    Put(mods, uint32_t(0));
    for (int i = 0; i < 2; ++i)
    {
        Put(modd, uint32_t(0));
        for (float v : { float(i), 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f }) Put(modd, v);
        Put(modd, 0xFF102030u);
    }
    chunk(root, "MVER", ver);
    chunk(root, "MOHD", header);
    chunk(root, "MODS", mods);
    chunk(root, "MODN", modn);
    chunk(root, "MODD", modd);
    chunk(root, "MFOG", {});
    std::vector<uint8_t> group, mogp(68), modr0;
    for (int k = 0; k < 6; ++k) WriteAt(mogp, 0x0C + size_t(k) * 4, k < 3 ? -10.0f : 10.0f);
    WriteAt(mogp, 8, uint32_t(0x800));
    PutChunk(mogp, Tag("MOPY"), {});
    PutChunk(mogp, Tag("MOBA"), {});
    Put(modr0, uint16_t(0));
    Put(modr0, uint16_t(1));
    PutChunk(mogp, Tag("MODR"), modr0);
    PutChunk(mogp, Tag("MOCV"), { 1, 2, 3, 4 });
    chunk(group, "MVER", ver);
    PutChunk(group, Tag("MOGP"), mogp);

    std::string error;
    auto d = ReadWmoDoodads(root, { group }, error);
    if (!d || d->sets.size() != 1 || d->doodads.size() != 2 || d->doodads[1].pos[0] != 1 || d->doodads[1].color != 0xFF102030u ||
        d->doodads[0].groups != std::vector<uint16_t>{ 0 })
        return false;

    // Move one (it keeps no groups: the box decides), add a set with a new doodad far outside the box (nearest box).
    WmoDoodads edited = *d;
    edited.doodads[1].pos[0] = 5;
    edited.doodads[1].groups.clear();
    edited.sets.push_back("Extra");
    WmoDoodads::Doodad added = edited.doodads[0];
    added.model = "World\\b.m2";
    added.set = 1;
    added.pos[0] = 50;
    added.groups.clear();
    edited.doodads[edited.NextId()] = added;
    std::vector<std::vector<uint16_t>> modr;
    const auto boxes = WmoGroupBoxes({ group });
    const auto newRoot = WriteWmoRoot(root, edited, boxes, modr);
    const auto newGroup = WriteWmoGroupModr(group, modr[0]);
    const auto back = ReadWmoDoodads(newRoot, { newGroup }, error);
    if (!back || back->sets.size() != 2 || back->sets[1] != "Extra" || back->doodads.size() != 3 || back->doodads.at(2).model != "World\\b.m2" ||
        back->doodads.at(1).pos[0] != 5 || back->doodads.at(2).set != 1 || modr[0] != std::vector<uint16_t>{ 0, 1, 2 })
        return false;
    uint32_t count = 0;
    float bounds[6];
    if (!WmoRootInfo(newRoot, count, bounds) || count != 1) return false;
    // The group keeps its other chunks in order, MODR before MOCV; with no doodads MODR goes and so does the flag.
    const auto bare = WriteWmoGroupModr(group, {});
    uint32_t flags = 0;
    if (!ReadAt(bare, 12 + 8 + 8, flags) || (flags & 0x800) || bare.size() != group.size() - 8 - 4) return false;
    {
        const auto mogpNew = Mogp(newGroup);
        std::vector<uint32_t> tags;
        for (const Chunk& c : Chunks(newGroup, mogpNew->off + 68, mogpNew->off + mogpNew->size)) tags.push_back(c.tag);
        if (tags != std::vector<uint32_t>{ Tag("MOPY"), Tag("MOBA"), Tag("MODR"), Tag("MOCV") }) return false;
    }
    // Matrices: a doodad turned and scaled survives its matrix.
    WmoDoodads::Doodad turned = added;
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(turned.rot), XMQuaternionRotationRollPitchYaw(0.3f, 1.1f, -0.4f));
    turned.scale = 1.5f;
    WmoDoodads::Doodad again = turned;
    SetDoodadMatrix(again, DoodadMatrix(turned));
    for (int k = 0; k < 3; ++k)
        if (std::fabs(again.pos[k] - turned.pos[k]) > 1e-3f) return false;
    const float dot = std::fabs(again.rot[0] * turned.rot[0] + again.rot[1] * turned.rot[1] + again.rot[2] * turned.rot[2] + again.rot[3] * turned.rot[3]);
    if (dot < 0.9999f || std::fabs(again.scale - 1.5f) > 1e-3f) return false;
    if (WmoCopyName("World\\wmo\\Inn.wmo", [](const std::string& n) { return n == "World\\wmo\\Inn_c1.wmo"; }) != "World\\wmo\\Inn_c2.wmo") return false;

    // The adapter: create a copy, edit it, undo, undo the copy (its files go).
    const auto dir = std::filesystem::temp_directory_path() / "wwe-wmo-edit-check";
    std::map<std::string, std::vector<uint8_t>> files = { { "w\\inn.wmo", root }, { "w\\inn_000.wmo", group } };
    WmoEditAdapter adapter;
    adapter.Reset([&](const std::string& n) -> std::optional<std::vector<uint8_t>> {
        const auto it = files.find(Lower(n));
        return it == files.end() ? std::nullopt : std::optional(it->second);
    }, dir);
    const auto create = adapter.MakeCreate("w\\Inn_c1.wmo", "w\\Inn.wmo", error);
    if (!create) return false;
    adapter.Apply(*create);
    const auto* e = adapter.Find("W\\INN_C1.WMO");
    if (!e || e->doodads.doodads.size() != 2 || !std::filesystem::exists(dir / "w" / "Inn_c1_000.wmo")) return false;
    const auto edit = adapter.MakeEdit(e->name, e->doodads, edited, "test");
    if (!edit) return false;
    adapter.Apply(*edit);
    const auto written = ReadFileBytes(dir / "w" / "Inn_c1.wmo");
    const auto writtenGroup = ReadFileBytes(dir / "w" / "Inn_c1_000.wmo");
    const auto reread = written && writtenGroup ? ReadWmoDoodads(*written, { *writtenGroup }, error) : std::nullopt;
    if (!reread || reread->doodads.size() != 3 || reread->sets.size() != 2) return false;
    adapter.Revert(*edit);
    if (adapter.Find(e->name)->doodads.doodads.size() != 2 || adapter.Find(e->name)->doodads.sets.size() != 1) return false;
    adapter.Revert(*create);
    const bool gone = !adapter.Find("w\\Inn_c1.wmo") && !std::filesystem::exists(dir / "w" / "Inn_c1.wmo");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return gone;
}
