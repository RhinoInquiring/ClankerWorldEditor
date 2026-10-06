#include "Models.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

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

    struct M2Array { uint32_t count, offset; };

    /// Elements of an M2Array, or empty when it points outside the file.
    template <class T>
    std::vector<T> Elements(const std::vector<uint8_t>& d, size_t at)
    {
        M2Array a{};
        if (!ReadAt(d, at, a) || a.count == 0 || a.offset > d.size() || (d.size() - a.offset) / sizeof(T) < a.count) return {};
        std::vector<T> out(a.count);
        std::memcpy(out.data(), d.data() + a.offset, size_t(a.count) * sizeof(T));
        return out;
    }

    constexpr uint32_t Tag(const char (&s)[5])
    {
        return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
    }

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

    void Grow(ModelMesh& m, const XMFLOAT3& p)
    {
        if (m.vertices.size() == 1) { m.boundsMin = m.boundsMax = p; return; }
        m.boundsMin = { std::min(m.boundsMin.x, p.x), std::min(m.boundsMin.y, p.y), std::min(m.boundsMin.z, p.z) };
        m.boundsMax = { std::max(m.boundsMax.x, p.x), std::max(m.boundsMax.y, p.y), std::max(m.boundsMax.z, p.z) };
    }

    // M2 (version 264) layout, offsets into the MD20 header.
    constexpr size_t kM2Vertices = 0x3C, kM2Textures = 0x50, kM2RenderFlags = 0x70, kM2TexLookup = 0x80;

    struct M2Vertex { float pos[3]; uint8_t weights[4], bones[4]; float normal[3]; float uv[2][2]; };
    struct M2Texture { uint32_t type, flags; M2Array name; };
    struct M2RenderFlag { uint16_t flags, blend; };
    struct SkinSubmesh
    {
        uint16_t id, level, vertexStart, vertexCount, indexStart, indexCount, boneCount, boneCombo, boneInfluences, centerBone;
        float center[3], sortCenter[3], sortRadius;
    };
    struct SkinBatch
    {
        uint8_t flags; int8_t priority; uint16_t shader, submesh, geoset; int16_t color;
        uint16_t material, layer, textureCount, textureCombo, uvCombo, weightCombo, transformCombo;
    };
    static_assert(sizeof(M2Vertex) == 48 && sizeof(SkinSubmesh) == 48 && sizeof(SkinBatch) == 24);

    // WMO v17.
    struct MomtEntry { uint32_t flags, shader, blend, texture1, color1, flags1, texture2, color2, groundType, texture3, color3, flags3, runtime[4]; };
    struct MobaEntry { int16_t box[6]; uint32_t startIndex; uint16_t count, minIndex, maxIndex; uint8_t flags, material; };
    static_assert(sizeof(MomtEntry) == 64 && sizeof(MobaEntry) == 24);

    // M2 animation (version 264).
    constexpr size_t kM2GlobalLoops = 0x14, kM2Sequences = 0x1C, kM2Bones = 0x2C, kM2Attachments = 0xF0;
    constexpr size_t kM2Colors = 0x48, kM2TexWeights = 0x58, kM2TexTransforms = 0x60, kM2TransparencyLookup = 0x90, kM2TexTransformLookup = 0x98;
    constexpr size_t kSequenceSize = 64, kBoneSize = 88, kAttachmentSize = 40;
    constexpr uint32_t kSequenceInM2 = 0x20, kSequenceAlias = 0x40;

    /// One M2Track (interpolation, global sequence, per-animation timestamp and value arrays) for animation
    /// `anim`, whose arrays live in `data` (the .m2, or the sequence's .anim file).
    template <class Raw, class T, class Convert>
    ModelTrack<T> ReadTrack(const std::vector<uint8_t>& m2, size_t at, uint32_t anim, const std::vector<uint8_t>& data, Convert convert)
    {
        ModelTrack<T> track;
        uint16_t interpolation = 0;
        ReadAt(m2, at, interpolation);
        ReadAt(m2, at + 2, track.globalSequence);
        track.linear = interpolation != 0;
        // A global-sequence track keeps its keys in the first array, in the .m2 itself.
        const uint32_t index = track.globalSequence >= 0 ? 0 : anim;
        const std::vector<uint8_t>& source = track.globalSequence >= 0 ? m2 : data;
        M2Array times{}, values{};
        if (!ReadAt(m2, at + 4, times) || !ReadAt(m2, at + 12, values) || index >= times.count || index >= values.count) return track;
        M2Array t{}, v{};
        if (!ReadAt(m2, times.offset + size_t(index) * 8, t) || !ReadAt(m2, values.offset + size_t(index) * 8, v) || t.count != v.count) return track;
        if (t.count == 0 || t.offset > source.size() || (source.size() - t.offset) / 4 < t.count ||
            v.offset > source.size() || (source.size() - v.offset) / sizeof(Raw) < v.count)
            return track;
        track.times.resize(t.count);
        std::memcpy(track.times.data(), source.data() + t.offset, size_t(t.count) * 4);
        track.values.reserve(v.count);
        for (uint32_t i = 0; i < v.count; ++i)
        {
            Raw raw;
            std::memcpy(&raw, source.data() + v.offset + size_t(i) * sizeof(Raw), sizeof(Raw));
            track.values.push_back(convert(raw));
        }
        return track;
    }

    struct Vec3Raw { float v[3]; };
    struct QuatRaw { int16_t v[4]; };
    float Unpack(int16_t v) { return (v < 0 ? v + 32768 : v - 32767) / 32767.0f; }

    /// Bones, an animation (`sequence`, or -1: Stand, else the first) and attachments; null when the model has no bones.
    std::shared_ptr<const ModelSkeleton> ParseSkeleton(const std::vector<uint8_t>& d, const FileReader& anim, const std::string& name, int sequence = -1)
    {
        M2Array bones{}, sequences{};
        if (!ReadAt(d, kM2Bones, bones) || bones.count == 0 || bones.count > 4096 || bones.offset > d.size() ||
            (d.size() - bones.offset) / kBoneSize < bones.count)
            return nullptr;
        auto skeleton = std::make_shared<ModelSkeleton>();
        skeleton->globalSequences = Elements<uint32_t>(d, kM2GlobalLoops);

        // The animation: the one asked for, else Stand (id 0) if there is one; aliases followed.
        uint32_t chosen = UINT32_MAX;
        ReadAt(d, kM2Sequences, sequences);
        if (sequences.offset > d.size() || (d.size() - sequences.offset) / kSequenceSize < sequences.count) sequences.count = 0;
        auto seq = [&](uint32_t i, size_t field, auto& out) { return i < sequences.count && ReadAt(d, sequences.offset + size_t(i) * kSequenceSize + field, out); };
        for (uint32_t i = 0; i < sequences.count; ++i)
        {
            ModelSequence q;
            seq(i, 0, q.id);
            seq(i, 2, q.variation);
            seq(i, 4, q.duration);
            seq(i, 12, q.flags);
            seq(i, 62, q.alias);
            skeleton->sequences.push_back(q);
        }
        if (sequence >= 0 && uint32_t(sequence) < sequences.count) chosen = uint32_t(sequence);
        for (uint32_t i = 0; i < sequences.count && chosen == UINT32_MAX; ++i)
            if (uint16_t id = 1; seq(i, 0, id) && id == 0) chosen = i;
        if (chosen == UINT32_MAX && sequences.count) chosen = 0;
        for (int hops = 0; hops < 8 && chosen != UINT32_MAX; ++hops)
            if (uint32_t flags = 0; seq(chosen, 12, flags) && (flags & kSequenceAlias))
            {
                uint16_t next = 0;
                seq(chosen, 62, next);
                chosen = next;
            }
            else break;

        std::vector<uint8_t> external;   // the .anim file of a sequence not kept in the .m2
        const std::vector<uint8_t>* data = &d;
        if (chosen != UINT32_MAX)
        {
            seq(chosen, 4, skeleton->duration);
            uint32_t flags = 0;
            uint16_t id = 0, variation = 0;
            seq(chosen, 12, flags);
            seq(chosen, 0, id);
            seq(chosen, 2, variation);
            if (!(flags & kSequenceInM2))
            {
                char suffix[32];
                snprintf(suffix, sizeof suffix, "%04u-%02u.anim", id, variation);
                const size_t dot = name.find_last_of('.');
                if (anim && !name.empty())
                    if (auto file = anim((dot == std::string::npos ? name : name.substr(0, dot)) + suffix)) external = std::move(*file);
                data = &external;   // empty when missing: the animation's own tracks read nothing
            }
        }
        const uint32_t animIndex = chosen == UINT32_MAX ? 0 : chosen;
        skeleton->sequence = chosen == UINT32_MAX ? -1 : int(chosen);

        skeleton->bones.resize(bones.count);
        for (uint32_t i = 0; i < bones.count; ++i)
        {
            const size_t at = bones.offset + size_t(i) * kBoneSize;
            ModelSkeleton::Bone& b = skeleton->bones[i];
            ReadAt(d, at + 8, b.parent);
            float pivot[3] = {};
            for (int k = 0; k < 3; ++k) ReadAt(d, at + 76 + size_t(k) * 4, pivot[k]);
            b.pivot = { pivot[0], pivot[1], pivot[2] };
            auto vec3 = [](const Vec3Raw& r) { return XMFLOAT3{ r.v[0], r.v[1], r.v[2] }; };
            b.translation = ReadTrack<Vec3Raw, XMFLOAT3>(d, at + 16, animIndex, *data, vec3);
            b.rotation = ReadTrack<QuatRaw, XMFLOAT4>(d, at + 36, animIndex, *data,
                                                      [](const QuatRaw& r) { return XMFLOAT4{ Unpack(r.v[0]), Unpack(r.v[1]), Unpack(r.v[2]), Unpack(r.v[3]) }; });
            b.scale = ReadTrack<Vec3Raw, XMFLOAT3>(d, at + 56, animIndex, *data, vec3);
            if (b.parent >= int16_t(bones.count)) b.parent = -1;
            skeleton->animated |= !b.translation.times.empty() || !b.rotation.times.empty() || !b.scale.times.empty();
        }

        // Texture animation: transforms (3 tracks, 60 bytes: translation, rotation, scale), transparency weights
        // (one fixed16 track each) and colours (vec3 colour track + fixed16 alpha track, 40 bytes).
        auto fixed16 = [](const int16_t& v) { return std::clamp(v / 32767.0f, 0.0f, 1.0f); };
        auto vec3 = [](const Vec3Raw& r) { return XMFLOAT3{ r.v[0], r.v[1], r.v[2] }; };
        auto tracks = [&](size_t header, size_t stride, auto&& each) {
            M2Array a{};
            if (!ReadAt(d, header, a) || a.offset > d.size() || a.count > 4096 || (d.size() - a.offset) / stride < a.count) return;
            for (uint32_t i = 0; i < a.count; ++i) each(a.offset + size_t(i) * stride);
        };
        tracks(kM2TexTransforms, 60, [&](size_t at) {
            ModelSkeleton::UvAnim u;
            u.translation = ReadTrack<Vec3Raw, XMFLOAT3>(d, at, animIndex, *data, vec3);
            u.scale = ReadTrack<Vec3Raw, XMFLOAT3>(d, at + 40, animIndex, *data, vec3);
            skeleton->animated |= !u.translation.times.empty() || !u.scale.times.empty();
            skeleton->uvAnims.push_back(std::move(u));
        });
        tracks(kM2TexWeights, 20, [&](size_t at) { skeleton->weights.push_back(ReadTrack<int16_t, float>(d, at, animIndex, *data, fixed16)); });
        tracks(kM2Colors, 40, [&](size_t at) { skeleton->colorAlphas.push_back(ReadTrack<int16_t, float>(d, at + 20, animIndex, *data, fixed16)); });

        M2Array attachments{};
        if (ReadAt(d, kM2Attachments, attachments) && attachments.offset <= d.size() && (d.size() - attachments.offset) / kAttachmentSize >= attachments.count)
            for (uint32_t i = 0; i < attachments.count; ++i)
            {
                const size_t at = attachments.offset + size_t(i) * kAttachmentSize;
                ModelSkeleton::Attachment a{};
                float p[3] = {};
                ReadAt(d, at, a.id);
                ReadAt(d, at + 4, a.bone);
                for (int k = 0; k < 3; ++k) ReadAt(d, at + 8 + size_t(k) * 4, p[k]);
                a.pos = { p[0], p[1], p[2] };
                if (a.bone < bones.count) skeleton->attachments.push_back(a);
            }
        return skeleton;
    }

    /// The track's value at `t` ms into the animation (`now` for global sequences).
    template <class T, class Lerp>
    T Sample(const ModelTrack<T>& track, const ModelSkeleton& s, uint32_t animTime, uint32_t now, T fallback, Lerp lerp)
    {
        if (track.times.empty()) return fallback;
        uint32_t t = animTime;
        if (track.globalSequence >= 0)
        {
            const uint32_t length = size_t(track.globalSequence) < s.globalSequences.size() ? s.globalSequences[track.globalSequence] : 0;
            t = length ? now % length : 0;
        }
        if (track.times.size() == 1 || t <= track.times.front()) return track.values.front();
        if (t >= track.times.back()) return track.values.back();
        const size_t hi = std::upper_bound(track.times.begin(), track.times.end(), t) - track.times.begin();
        const size_t lo = hi - 1;
        if (!track.linear) return track.values[lo];
        const float f = float(t - track.times[lo]) / float(std::max<uint32_t>(track.times[hi] - track.times[lo], 1));
        return lerp(track.values[lo], track.values[hi], f);
    }

    std::string CString(const std::vector<uint8_t>& d, size_t blockOff, size_t blockSize, uint32_t off)
    {
        if (off >= blockSize) return {};
        const char* p = reinterpret_cast<const char*>(d.data() + blockOff + off);
        return std::string(p, strnlen(p, blockSize - off));
    }
}

XMMATRIX PlacementMatrix(const float pos[3], const float rot[3], float scale)
{
    // Rotations about the editor's x, z then y axes, as the client applies them to MDDF/MODF entries.
    return XMMatrixScaling(scale, scale, scale) * XMMatrixRotationX(XMConvertToRadians(rot[2])) *
           XMMatrixRotationZ(XMConvertToRadians(-rot[0])) * XMMatrixRotationY(XMConvertToRadians(rot[1] - 90.0f)) *
           XMMatrixTranslation(pos[0], pos[1], pos[2]);
}

void DecomposePlacement(FXMMATRIX m, float pos[3], float rot[3], float& scale)
{
    // Rotation part R = Rx(a) * Rz(b) * Ry(c) (row vectors) with a = rot[2], b = -rot[0], c = rot[1] - 90. Its rows:
    //   [ cb cc,                 sb,     -cb sc ]
    //   [ -ca sb cc + sa sc,     ca cb,  ca sb sc + sa cc ]
    //   [ sa sb cc + ca sc,     -sa cb, -sa sb sc + ca cc ]
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);
    const float lengths[3] = { std::sqrt(f._11 * f._11 + f._12 * f._12 + f._13 * f._13), std::sqrt(f._21 * f._21 + f._22 * f._22 + f._23 * f._23),
                               std::sqrt(f._31 * f._31 + f._32 * f._32 + f._33 * f._33) };
    scale = (lengths[0] + lengths[1] + lengths[2]) / 3;
    float r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = f.m[i][j] / (lengths[i] > 1e-6f ? lengths[i] : 1.0f);
    const float b = std::asin(std::clamp(r[0][1], -1.0f, 1.0f));
    float a, c;
    if (std::fabs(r[0][1]) < 0.9999f)
    {
        c = std::atan2(-r[0][2], r[0][0]);
        a = std::atan2(-r[2][1], r[1][1]);
    }
    else   // gimbal lock: only a + c (or a - c) is defined; put it all in a
    {
        c = 0;
        a = std::atan2(r[1][2], r[2][2]);
    }
    auto wrap = [](float deg) { return std::fmod(deg + 720.0f, 360.0f); };
    rot[2] = wrap(XMConvertToDegrees(a));
    rot[0] = wrap(-XMConvertToDegrees(b));
    rot[1] = wrap(XMConvertToDegrees(c) + 90.0f);
    pos[0] = f._41; pos[1] = f._42; pos[2] = f._43;
}

std::string M2Name(const std::string& name)
{
    std::string n = name;
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos)
    {
        std::string ext = n.substr(dot);
        for (char& c : ext) c = char(std::tolower((unsigned char)c));
        if (ext == ".mdx" || ext == ".mdl") n = n.substr(0, dot) + ".m2";
    }
    return n;
}

std::string M2SkinName(const std::string& m2Name)
{
    const size_t dot = m2Name.find_last_of('.');
    return (dot == std::string::npos ? m2Name : m2Name.substr(0, dot)) + "00.skin";
}

std::string WmoGroupName(const std::string& rootName, uint32_t group)
{
    const size_t dot = rootName.find_last_of('.');
    char suffix[16];
    snprintf(suffix, sizeof suffix, "_%03u.wmo", group);
    return (dot == std::string::npos ? rootName : rootName.substr(0, dot)) + suffix;
}

std::optional<ModelMesh> ParseM2(const std::vector<uint8_t>& d, const std::vector<uint8_t>& skin, const FileReader& anim, const std::string& m2Name)
{
    if (d.size() < 0x130 || std::memcmp(d.data(), "MD20", 4) != 0) return std::nullopt;
    if (skin.size() < 48 || std::memcmp(skin.data(), "SKIN", 4) != 0) return std::nullopt;

    const auto verts = Elements<M2Vertex>(d, kM2Vertices);
    const auto textures = Elements<M2Texture>(d, kM2Textures);
    const auto flags = Elements<M2RenderFlag>(d, kM2RenderFlags);
    const auto texLookup = Elements<uint16_t>(d, kM2TexLookup);
    const auto weightLookup = Elements<uint16_t>(d, kM2TransparencyLookup), uvLookup = Elements<uint16_t>(d, kM2TexTransformLookup);
    const auto skinVerts = Elements<uint16_t>(skin, 4);
    const auto triangles = Elements<uint16_t>(skin, 12);
    const auto submeshes = Elements<SkinSubmesh>(skin, 28);
    const auto batches = Elements<SkinBatch>(skin, 36);
    if (verts.empty() || skinVerts.empty() || triangles.empty()) return std::nullopt;

    ModelMesh mesh;
    for (uint16_t v : skinVerts)
    {
        if (v >= verts.size()) return std::nullopt;
        const M2Vertex& src = verts[v];
        ModelVertex out{ FromWowAxes(src.pos[0], src.pos[1], src.pos[2]), FromWowAxes(src.normal[0], src.normal[1], src.normal[2]), { src.uv[0][0], src.uv[0][1] } };
        for (int k = 0; k < 4; ++k) { out.bones[k] = src.bones[k]; out.weights[k] = src.weights[k]; }
        mesh.vertices.push_back(out);
        Grow(mesh, mesh.vertices.back().pos);
    }
    for (uint16_t t : triangles)
    {
        if (t >= mesh.vertices.size()) return std::nullopt;
        mesh.indices.push_back(t);
    }

    for (const SkinBatch& b : batches)
    {
        if (b.submesh >= submeshes.size()) continue;
        const SkinSubmesh& s = submeshes[b.submesh];
        ModelMesh::Batch out;
        out.indexStart = uint32_t(s.indexStart) | uint32_t(s.level) << 16;
        out.indexCount = s.indexCount;
        if (out.indexStart + out.indexCount > mesh.indices.size()) continue;
        out.geoset = s.id;
        if (b.textureCombo < texLookup.size() && texLookup[b.textureCombo] < textures.size())
        {
            const M2Texture& t = textures[texLookup[b.textureCombo]];
            out.textureType = t.type;
            if (t.type == 0 && t.name.count > 1 && t.name.offset < d.size() && d.size() - t.name.offset >= t.name.count)
                out.texture.assign(reinterpret_cast<const char*>(d.data() + t.name.offset), strnlen(reinterpret_cast<const char*>(d.data() + t.name.offset), t.name.count));
        }
        // Texture animation links (0xFFFF in a lookup = none): flowing water, waterfalls, glows.
        out.color = b.color;
        if (b.weightCombo < weightLookup.size() && weightLookup[b.weightCombo] != 0xFFFF) out.weight = int16_t(weightLookup[b.weightCombo]);
        if (b.transformCombo < uvLookup.size() && uvLookup[b.transformCombo] != 0xFFFF) out.uvAnim = int16_t(uvLookup[b.transformCombo]);
        if (b.material < flags.size())
        {
            const M2RenderFlag& f = flags[b.material];
            out.twoSided = (f.flags & 0x4) != 0;
            out.blend = f.blend == 0 ? ModelMesh::Blend::Opaque : f.blend == 1 ? ModelMesh::Blend::AlphaTest
                      : f.blend == 2 ? ModelMesh::Blend::AlphaBlend : ModelMesh::Blend::Additive;
        }
        mesh.batches.push_back(std::move(out));
    }
    if (mesh.batches.empty()) return std::nullopt;
    mesh.skeleton = ParseSkeleton(d, anim, m2Name);
    for (const ModelVertex& v : mesh.vertices)   // bone indices past the skeleton: draw unskinned
        for (int k = 0; k < 4; ++k)
            if (v.weights[k] && (!mesh.skeleton || v.bones[k] >= mesh.skeleton->bones.size())) { mesh.skeleton = nullptr; break; }
    if (!mesh.skeleton)
        for (ModelVertex& v : mesh.vertices) { std::fill(std::begin(v.bones), std::end(v.bones), 0); v.weights[0] = 255; v.weights[1] = v.weights[2] = v.weights[3] = 0; }
    return mesh;
}

std::shared_ptr<const ModelSkeleton> LoadSkeleton(const std::string& name, const FileReader& read, int sequence)
{
    const auto m2 = read(name);
    return m2 ? ParseSkeleton(*m2, read, name, sequence) : nullptr;
}

std::optional<ModelMesh> LoadM2(const std::string& name, const FileReader& read)
{
    const auto m2 = read(name);
    const auto skin = m2 ? read(M2SkinName(name)) : std::nullopt;
    if (!m2 || !skin) return std::nullopt;
    return ParseM2(*m2, *skin, read, name);
}

void PoseBones(const ModelSkeleton& s, uint32_t now, std::vector<XMFLOAT4X4>& palette)
{
    const uint32_t animTime = s.duration ? now % s.duration : 0;
    const size_t count = s.bones.size();
    std::vector<XMFLOAT4X4> world(count);   // WoW axes
    std::vector<bool> done(count, false);
    auto lerp3 = [](const XMFLOAT3& a, const XMFLOAT3& b, float f) {
        XMFLOAT3 r;
        XMStoreFloat3(&r, XMVectorLerp(XMLoadFloat3(&a), XMLoadFloat3(&b), f));
        return r;
    };
    auto slerp = [](const XMFLOAT4& a, const XMFLOAT4& b, float f) {
        XMFLOAT4 r;
        XMStoreFloat4(&r, XMQuaternionSlerp(XMLoadFloat4(&a), XMLoadFloat4(&b), f));
        return r;
    };
    // Each bone: about its pivot scale, rotate, translate; then its parent's transform (row vectors).
    auto pose = [&](auto&& self, size_t i, int depth) -> XMMATRIX {
        if (done[i]) return XMLoadFloat4x4(&world[i]);
        const ModelSkeleton::Bone& b = s.bones[i];
        const XMFLOAT3 t = Sample(b.translation, s, animTime, now, XMFLOAT3{ 0, 0, 0 }, lerp3);
        const XMFLOAT4 q = Sample(b.rotation, s, animTime, now, XMFLOAT4{ 0, 0, 0, 1 }, slerp);
        const XMFLOAT3 sc = Sample(b.scale, s, animTime, now, XMFLOAT3{ 1, 1, 1 }, lerp3);
        XMMATRIX m = XMMatrixTranslation(-b.pivot.x, -b.pivot.y, -b.pivot.z) * XMMatrixScaling(sc.x, sc.y, sc.z) *
                     XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&q))) * XMMatrixTranslation(t.x, t.y, t.z) *
                     XMMatrixTranslation(b.pivot.x, b.pivot.y, b.pivot.z);
        if (b.parent >= 0 && size_t(b.parent) != i && depth < 128) m = m * self(self, size_t(b.parent), depth + 1);
        XMStoreFloat4x4(&world[i], m);
        done[i] = true;
        return m;
    };
    // Editor axes: v_editor = v_wow * C, so the palette is C^-1 * M * C.
    const XMMATRIX c = XMMatrixSet(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    const XMMATRIX cInv = XMMatrixTranspose(c);
    palette.resize(count);
    for (size_t i = 0; i < count; ++i) XMStoreFloat4x4(&palette[i], cInv * pose(pose, i, 0) * c);
}

bool WmoRootInfo(const std::vector<uint8_t>& root, uint32_t& groups, float bounds[6])
{
    bool found = false;
    ForEachChunk(root, 0, root.size(), [&](uint32_t magic, size_t off, size_t size) {
        if (magic != Tag("MOHD") || size < 64) return;
        ReadAt(root, off + 4, groups);
        for (int i = 0; i < 6; ++i) ReadAt(root, off + 0x24 + size_t(i) * 4, bounds[i]);
        found = true;
    });
    return found;
}

std::optional<WmoAreaKeys> ReadWmoAreaKeys(const std::string& rootName, const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read)
{
    const auto root = read(rootName);
    if (!root) return std::nullopt;
    WmoAreaKeys keys;
    uint32_t groups = 0;
    size_t mognOff = 0, mognSize = 0;
    bool found = false;
    ForEachChunk(*root, 0, root->size(), [&](uint32_t magic, size_t off, size_t size) {
        if (magic == Tag("MOGN")) { mognOff = off; mognSize = size; }
        if (magic != Tag("MOHD") || size < 64) return;
        ReadAt(*root, off + 4, groups);
        ReadAt(*root, off + 0x20, keys.wmoId);
        found = true;
    });
    if (!found) return std::nullopt;
    for (uint32_t i = 0; i < groups && i < 512; ++i)
    {
        const auto g = read(WmoGroupName(rootName, i));
        if (!g) continue;
        // MVER (4 bytes), then MOGP: name offset into the root's MOGN at +0, the WMOAreaTable group id at +0x38. Only the
        // header is read, so a MOGP that states a size past the end of the file (some shipped groups do) still counts.
        uint32_t magic = 0, name = 0, id = 0;
        if (!ReadAt(*g, 12, magic) || magic != Tag("MOGP") || !ReadAt(*g, 20, name) || !ReadAt(*g, 20 + 0x38, id)) continue;
        keys.groups.push_back({ id, CString(*root, mognOff, mognSize, name) });
    }
    return keys;
}

std::optional<ModelMesh> ParseWmo(const std::vector<uint8_t>& root, const std::vector<std::vector<uint8_t>>& groups)
{
    size_t motxOff = 0, motxSize = 0, modnOff = 0, modnSize = 0, modsOff = 0, modsSize = 0, moddOff = 0, moddSize = 0;
    uint16_t rootFlags = 0;   // MOHD flags; 0x4: groups name their liquid by LiquidType id
    std::vector<MomtEntry> materials;
    ForEachChunk(root, 0, root.size(), [&](uint32_t magic, size_t off, size_t size) {
        if (magic == Tag("MOTX")) { motxOff = off; motxSize = size; }
        if (magic == Tag("MODN")) { modnOff = off; modnSize = size; }
        if (magic == Tag("MODS")) { modsOff = off; modsSize = size; }
        if (magic == Tag("MODD")) { moddOff = off; moddSize = size; }
        if (magic == Tag("MOHD") && size >= 62) ReadAt(root, off + 60, rootFlags);
        if (magic == Tag("MOMT"))
        {
            materials.resize(size / sizeof(MomtEntry));
            std::memcpy(materials.data(), root.data() + off, materials.size() * sizeof(MomtEntry));
        }
    });

    ModelMesh mesh;
    // Doodad sets (MODS: name[20], first, count, pad) over the doodad list (MODD, 40 bytes: name offset into MODN in
    // the low 24 bits + flags, position, quaternion x y z w, scale, colour), all in the WMO's own (WoW) axes.
    for (size_t i = 0; i + 32 <= modsSize; i += 32)
    {
        ModelMesh::DoodadSet set;
        ReadAt(root, modsOff + i + 20, set.first);
        ReadAt(root, modsOff + i + 24, set.count);
        mesh.doodadSets.push_back(set);
    }
    for (size_t i = 0; i + 40 <= moddSize; i += 40)
    {
        uint32_t nameAndFlags = 0;
        ModelMesh::Doodad d;
        ReadAt(root, moddOff + i, nameAndFlags);
        for (int k = 0; k < 3; ++k) ReadAt(root, moddOff + i + 4 + size_t(k) * 4, (&d.pos.x)[k]);
        for (int k = 0; k < 4; ++k) ReadAt(root, moddOff + i + 16 + size_t(k) * 4, (&d.rot.x)[k]);
        ReadAt(root, moddOff + i + 32, d.scale);
        d.model = CString(root, modnOff, modnSize, nameAndFlags & 0xFFFFFF);
        mesh.doodads.push_back(std::move(d));
    }
    for (const auto& g : groups)
    {
        // A group file: MVER, then one MOGP whose data is a 68-byte header followed by sub-chunks. Some shipped
        // groups state a MOGP size past the end of the file; read up to the end instead of skipping the group.
        auto groupChunks = [&](auto&& fn) {
            for (size_t pos = 0; pos + 8 <= g.size();)
            {
                uint32_t magic = 0, size = 0;
                ReadAt(g, pos, magic);
                ReadAt(g, pos + 4, size);
                const size_t clamped = std::min<size_t>(size, g.size() - pos - 8);
                fn(magic, pos + 8, clamped);
                pos += 8 + clamped;
            }
        };
        groupChunks([&](uint32_t magic, size_t off, size_t size) {
            if (magic != Tag("MOGP") || size < 68) return;
            std::vector<float> pos, nrm, uv;
            std::vector<uint16_t> idx;
            std::vector<MobaEntry> moba;
            size_t mliqOff = 0, mliqSize = 0;
            ForEachChunk(g, off + 68, off + size, [&](uint32_t sub, size_t so, size_t ss) {
                auto grab = [&](auto& vec) {
                    using T = typename std::decay_t<decltype(vec)>::value_type;
                    vec.resize(ss / sizeof(T));
                    std::memcpy(vec.data(), g.data() + so, vec.size() * sizeof(T));
                };
                if (sub == Tag("MOVT")) grab(pos);
                else if (sub == Tag("MONR")) grab(nrm);
                else if (sub == Tag("MOTV") && uv.empty()) grab(uv);   // first UV set only
                else if (sub == Tag("MOVI")) grab(idx);
                else if (sub == Tag("MOBA")) grab(moba);
                else if (sub == Tag("MLIQ")) { mliqOff = so; mliqSize = ss; }
            });

            const uint32_t base = uint32_t(mesh.vertices.size());
            const size_t count = pos.size() / 3;
            for (size_t i = 0; i < count; ++i)
            {
                const XMFLOAT3 n = nrm.size() >= (i + 1) * 3 ? FromWowAxes(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]) : XMFLOAT3{ 0, 1, 0 };
                const XMFLOAT2 t = uv.size() >= (i + 1) * 2 ? XMFLOAT2{ uv[i * 2], uv[i * 2 + 1] } : XMFLOAT2{ 0, 0 };
                mesh.vertices.push_back({ FromWowAxes(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]), n, t });
                Grow(mesh, mesh.vertices.back().pos);
            }
            for (const MobaEntry& b : moba)
            {
                if (size_t(b.startIndex) + b.count > idx.size()) continue;
                ModelMesh::Batch out;
                out.indexStart = uint32_t(mesh.indices.size());
                out.indexCount = b.count;
                for (uint32_t k = 0; k < b.count; ++k) mesh.indices.push_back(base + std::min<uint32_t>(idx[b.startIndex + k], uint32_t(count ? count - 1 : 0)));
                if (b.material < materials.size())
                {
                    const MomtEntry& m = materials[b.material];
                    out.texture = CString(root, motxOff, motxSize, m.texture1);
                    out.twoSided = (m.flags & 0x4) != 0;
                    out.blend = m.blend == 0 ? ModelMesh::Blend::Opaque : m.blend == 1 ? ModelMesh::Blend::AlphaTest : ModelMesh::Blend::AlphaBlend;
                }
                mesh.batches.push_back(std::move(out));
            }

            // The group's liquid (MLIQ): xverts, yverts, xtiles, ytiles, corner (WoW axes), material; then xverts *
            // yverts vertices of 8 bytes (height last) and one byte per tile (low nibble 0xF = no liquid there).
            int32_t xv = 0, yv = 0, xt = 0, yt = 0;
            float corner[3] = {};
            if (mliqSize >= 30 && ReadAt(g, mliqOff, xv) && ReadAt(g, mliqOff + 4, yv) && ReadAt(g, mliqOff + 8, xt) && ReadAt(g, mliqOff + 12, yt) &&
                xv > 1 && yv > 1 && xt == xv - 1 && yt == yv - 1 && 30 + size_t(xv) * yv * 8 + size_t(xt) * yt <= mliqSize)
            {
                for (int k = 0; k < 3; ++k) ReadAt(g, mliqOff + 16 + size_t(k) * 4, corner[k]);
                const size_t verts = mliqOff + 30, tiles = verts + size_t(xv) * yv * 8;
                constexpr float kUnit = 1600.0f / 3.0f / 128.0f;   // one ADT vertex step, 4.17 yd
                // The liquid's type: a LiquidType id when the root says so, else the basic kind of its first tile
                // (water, ocean, magma, slime -> the stock WMO liquid types).
                uint32_t groupLiquid = 0;
                ReadAt(g, off + 0x34, groupLiquid);
                uint8_t firstTile = 0x0F;
                for (int t = 0; t < xt * yt && (firstTile & 0x0F) == 0x0F; ++t) firstTile = g[tiles + t];
                static const uint16_t kBasic[4] = { 13, 14, 19, 20 };
                const uint16_t type = (rootFlags & 0x4) && groupLiquid ? uint16_t(groupLiquid) : kBasic[firstTile & 0x3];

                const uint32_t first = uint32_t(mesh.vertices.size());
                for (int j = 0; j < yv; ++j)
                    for (int i = 0; i < xv; ++i)
                    {
                        float h = corner[2];
                        ReadAt(g, verts + (size_t(j) * xv + i) * 8 + 4, h);
                        mesh.vertices.push_back({ FromWowAxes(corner[0] + i * kUnit, corner[1] + j * kUnit, h), { 0, 1, 0 }, { i * 0.5f, j * 0.5f } });
                        Grow(mesh, mesh.vertices.back().pos);
                    }
                ModelMesh::Batch out;
                out.indexStart = uint32_t(mesh.indices.size());
                for (int j = 0; j < yt; ++j)
                    for (int i = 0; i < xt; ++i)
                    {
                        if ((g[tiles + size_t(j) * xt + i] & 0x0F) == 0x0F) continue;
                        const uint32_t a = first + j * xv + i, b = a + 1, c = a + xv, d = c + 1;
                        mesh.indices.insert(mesh.indices.end(), { a, b, d, a, d, c });
                    }
                out.indexCount = uint32_t(mesh.indices.size()) - out.indexStart;
                out.blend = ModelMesh::Blend::AlphaBlend;
                out.twoSided = true;
                out.liquid = type;
                if (out.indexCount) mesh.batches.push_back(std::move(out));
            }
        });
    }
    if (mesh.batches.empty()) return std::nullopt;
    return mesh;
}

XMMATRIX WmoDoodadMatrix(const ModelMesh::Doodad& d)
{
    // Scale, rotate, move in the WMO's WoW axes; then the same change of axes as the meshes (v_editor = v_wow * C).
    const XMMATRIX c = XMMatrixSet(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    const XMMATRIX local = XMMatrixScaling(d.scale, d.scale, d.scale) * XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&d.rot))) *
                           XMMatrixTranslation(d.pos.x, d.pos.y, d.pos.z);
    return XMMatrixTranspose(c) * local * c;
}

XMFLOAT4 UvTransform(const ModelSkeleton& s, int index, uint32_t now)
{
    if (index < 0 || size_t(index) >= s.uvAnims.size()) return { 1, 1, 0, 0 };
    const uint32_t animTime = s.duration ? now % s.duration : 0;
    auto lerp3 = [](const XMFLOAT3& a, const XMFLOAT3& b, float f) { return XMFLOAT3{ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f }; };
    const auto& u = s.uvAnims[size_t(index)];
    const XMFLOAT3 t = Sample(u.translation, s, animTime, now, XMFLOAT3{ 0, 0, 0 }, lerp3);
    const XMFLOAT3 k = Sample(u.scale, s, animTime, now, XMFLOAT3{ 1, 1, 1 }, lerp3);
    return { k.x, k.y, t.x, t.y };
}

float BatchAlpha(const ModelSkeleton& s, int weight, int color, uint32_t now)
{
    const uint32_t animTime = s.duration ? now % s.duration : 0;
    auto lerp = [](float a, float b, float f) { return a + (b - a) * f; };
    float alpha = 1;
    if (weight >= 0 && size_t(weight) < s.weights.size()) alpha *= Sample(s.weights[size_t(weight)], s, animTime, now, 1.0f, lerp);
    if (color >= 0 && size_t(color) < s.colorAlphas.size()) alpha *= Sample(s.colorAlphas[size_t(color)], s, animTime, now, 1.0f, lerp);
    return alpha;
}
