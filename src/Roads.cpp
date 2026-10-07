#include "Roads.hpp"

#include "Blend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace
{
    float Smooth(float v) { v = std::clamp(v, 0.0f, 1.0f); return v * v * (3 - 2 * v); }

    /// Value noise in [-1, 1] on the ground, about `scale` yards per cell.
    float Noise(float x, float z, float scale)
    {
        auto hash = [](int a, int b) {
            uint32_t h = uint32_t(a) * 374761393u + uint32_t(b) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return float((h ^ (h >> 16)) & 0xFFFF) / 32767.5f - 1.0f;
        };
        x /= scale;
        z /= scale;
        const int ix = int(std::floor(x)), iz = int(std::floor(z));
        const float fx = Smooth(x - ix), fz = Smooth(z - iz);
        const float a = hash(ix, iz) + (hash(ix + 1, iz) - hash(ix, iz)) * fx;
        const float b = hash(ix, iz + 1) + (hash(ix + 1, iz + 1) - hash(ix, iz + 1)) * fx;
        return a + (b - a) * fz;
    }

    /// Position of MCVT vertex j inside its chunk, in vertex steps (as Terrain.cpp's VertexXZ).
    void VertexXZ(size_t j, float& x, float& z)
    {
        const size_t row = j / 17, col = j % 17;
        x = col >= 9 ? float(col - 9) + 0.5f : float(col);
        z = col >= 9 ? float(row) + 0.5f : float(row);
    }

    /// The nearest point of a road's centre line to (x, z): squared distance, height and width there.
    struct Nearest { float d2 = 1e30f, y = 0, width = 0; };
    Nearest NearestOn(const std::vector<std::pair<const RoadSample*, const RoadSample*>>& segs, float x, float z)
    {
        Nearest best;
        for (const auto& [a, b] : segs)
        {
            const float ex = b->pos.x - a->pos.x, ez = b->pos.z - a->pos.z, len2 = ex * ex + ez * ez;
            const float u = len2 > 1e-6f ? std::clamp(((x - a->pos.x) * ex + (z - a->pos.z) * ez) / len2, 0.0f, 1.0f) : 0.0f;
            const float px = a->pos.x + ex * u - x, pz = a->pos.z + ez * u - z, d2 = px * px + pz * pz;
            if (d2 < best.d2) best = { d2, a->pos.y + (b->pos.y - a->pos.y) * u, a->width + (b->width - a->width) * u };
        }
        return best;
    }

    /// Raises `name`'s share of each texel to at least `amount` (0..1), the other layers giving way. Adds the texture as
    /// a layer when it is missing (a fifth replaces the weakest layer that is not in `keep`).
    void RaiseShare(AdtChunk& c, std::vector<std::string>& textures, const std::string& name, const float* amount, const std::vector<std::string>& keep)
    {
        if (c.alpha.size() != 4096 * 4) c.alpha.assign(4096 * 4, 0);
        uint32_t n = std::min<uint32_t>(c.layerCount, 4);
        auto weights = [&](size_t i) {
            return LayerWeights(n > 1 ? c.alpha[i * 4] / 255.0f : 0, n > 2 ? c.alpha[i * 4 + 1] / 255.0f : 0, n > 3 ? c.alpha[i * 4 + 2] / 255.0f : 0);
        };
        auto store = [&](size_t i, const std::array<float, 4>& w) {
            const auto a = LayerAlphas(w);
            for (int ch = 0; ch < 3; ++ch) c.alpha[i * 4 + size_t(ch)] = uint8_t(std::lround(a[size_t(ch)] * 255.0f));
        };
        int target = -1;
        for (uint32_t l = 0; l < n; ++l)
            if (c.textureIds[l] < textures.size() && textures[c.textureIds[l]] == name) target = int(l);
        if (target < 0)
        {
            auto it = std::find(textures.begin(), textures.end(), name);
            if (it == textures.end()) it = textures.insert(textures.end(), name);
            const uint32_t id = uint32_t(it - textures.begin());
            if (n == 0) target = int(n++);
            else if (n < 4)
            {
                target = int(n++);
                for (size_t i = 0; i < 4096; ++i) c.alpha[i * 4 + size_t(target - 1)] = 0;
            }
            else
            {
                // The weakest layer the road does not use gives its place; its share goes to the others.
                std::array<double, 4> totals{};
                for (size_t i = 0; i < 4096; ++i)
                {
                    const auto w = weights(i);
                    for (int l = 0; l < 4; ++l) totals[size_t(l)] += w[size_t(l)];
                }
                for (int l = 0; l < 4; ++l)
                    if (c.textureIds[l] < textures.size() && std::find(keep.begin(), keep.end(), textures[c.textureIds[l]]) != keep.end()) totals[size_t(l)] = 1e30;
                target = int(std::min_element(totals.begin(), totals.end()) - totals.begin());
                for (size_t i = 0; i < 4096; ++i)
                {
                    auto w = weights(i);
                    w[size_t(target)] = 0;
                    float sum = w[0] + w[1] + w[2] + w[3];
                    if (sum <= 1e-6f) { w = { 0, 0, 0, 0 }; w[target == 0 ? 1 : 0] = 1; sum = 1; }
                    for (float& v : w) v /= sum;
                    store(i, w);
                }
            }
            c.textureIds[size_t(target)] = id;
            c.layerFlags[size_t(target)] = 0;
            c.effectIds[size_t(target)] = 0;   // no grass or pebbles on a road
            c.layerCount = n;
            if (n == 1) return void(std::fill(c.alpha.begin(), c.alpha.end(), uint8_t(0)));   // the only layer: everywhere already
        }
        for (size_t i = 0; i < 4096; ++i)
        {
            if (amount[i] <= 0) continue;
            auto w = weights(i);
            const float t = w[size_t(target)];
            if (amount[i] <= t) continue;
            const float rest = 1.0f - t, keepShare = rest > 1e-6f ? (1.0f - amount[i]) / rest : 0.0f;
            for (int l = 0; l < 4; ++l)
                if (l != target) w[size_t(l)] *= keepShare;
            w[size_t(target)] = amount[i];
            store(i, w);
        }
    }
}

nlohmann::json Road::ToJson() const
{
    nlohmann::json pts = nlohmann::json::array();
    for (const RoadPoint& p : points) pts.push_back({ p.pos.x, p.pos.y, p.pos.z, p.width });
    return { { "id", id }, { "name", name }, { "map", map }, { "points", pts }, { "width", width }, { "texture", texture },
             { "shoulderTexture", shoulderTexture }, { "shoulder", shoulder }, { "noise", noise }, { "grade", grade }, { "sink", sink },
             { "follow", followGround } };
}

Road Road::FromJson(const nlohmann::json& j)
{
    Road r;
    r.id = j.value("id", 0u);
    r.name = j.value("name", std::string());
    r.map = j.value("map", std::string());
    for (const auto& p : ChangeStore::List(j, "points")) r.points.push_back({ { p[0], p[1], p[2] }, p.size() > 3 ? float(p[3]) : 0.0f });
    r.width = j.value("width", 6.0f);
    r.texture = j.value("texture", std::string());
    r.shoulderTexture = j.value("shoulderTexture", std::string());
    r.shoulder = j.value("shoulder", 3.0f);
    r.noise = j.value("noise", 0.5f);
    r.grade = j.value("grade", 0.5f);
    r.sink = j.value("sink", 0.2f);
    r.followGround = j.value("follow", true);
    return r;
}

float RoadReach(const Road& road)
{
    float widest = road.width;
    for (const RoadPoint& p : road.points) widest = std::max(widest, p.width);
    return widest / 2 + std::max(road.shoulder, 0.0f) + 4.0f + 2.0f;   // grade fades 4 yd past the shoulder; noise up to ~2
}

std::vector<RoadSample> SampleRoad(const Road& road, const RoadGround& ground)
{
    std::vector<RoadSample> out;
    const size_t n = road.points.size();
    if (n < 2) return out;
    auto P = [&](int i) { return XMLoadFloat3(&road.points[size_t(i)].pos); };
    auto widthAt = [&](size_t i) { return road.points[i].width > 0 ? road.points[i].width : road.width; };
    for (size_t i = 0; i + 1 < n; ++i)
    {
        // Centripetal Catmull-Rom through p1 -> p2 (no loops or cusps when points bunch up); ends mirrored.
        const XMVECTOR p1 = P(int(i)), p2 = P(int(i + 1));
        const XMVECTOR p0 = i > 0 ? P(int(i - 1)) : XMVectorSubtract(XMVectorScale(p1, 2), p2);
        const XMVECTOR p3 = i + 2 < n ? P(int(i + 2)) : XMVectorSubtract(XMVectorScale(p2, 2), p1);
        auto knot = [](FXMVECTOR a, FXMVECTOR b) { return std::max(std::sqrt(XMVectorGetX(XMVector3Length(XMVectorSubtract(b, a)))), 1e-3f); };
        const float t0 = 0, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
        const float len = XMVectorGetX(XMVector3Length(XMVectorSubtract(p2, p1)));
        const int steps = std::max(2, int(std::ceil(len / 1.0f)));
        for (int s = 0; s < steps || (i + 2 == n && s == steps); ++s)
        {
            const float f = float(s) / steps, t = t1 + (t2 - t1) * f;
            const XMVECTOR a1 = XMVectorAdd(XMVectorScale(p0, (t1 - t) / (t1 - t0)), XMVectorScale(p1, (t - t0) / (t1 - t0)));
            const XMVECTOR a2 = XMVectorAdd(XMVectorScale(p1, (t2 - t) / (t2 - t1)), XMVectorScale(p2, (t - t1) / (t2 - t1)));
            const XMVECTOR a3 = XMVectorAdd(XMVectorScale(p2, (t3 - t) / (t3 - t2)), XMVectorScale(p3, (t - t2) / (t3 - t2)));
            const XMVECTOR b1 = XMVectorAdd(XMVectorScale(a1, (t2 - t) / (t2 - t0)), XMVectorScale(a2, (t - t0) / (t2 - t0)));
            const XMVECTOR b2 = XMVectorAdd(XMVectorScale(a2, (t3 - t) / (t3 - t1)), XMVectorScale(a3, (t - t1) / (t3 - t1)));
            const XMVECTOR c = XMVectorAdd(XMVectorScale(b1, (t2 - t) / (t2 - t1)), XMVectorScale(b2, (t - t1) / (t2 - t1)));
            RoadSample sample;
            XMStoreFloat3(&sample.pos, c);
            sample.width = widthAt(i) + (widthAt(i + 1) - widthAt(i)) * Smooth(f);
            out.push_back(sample);
        }
    }
    if (road.followGround && ground && !out.empty())
    {
        // The ground under the line, averaged over a stretch of road (samples are about a yard apart): bumps go, hills
        // stay. Where the ground is unknown the line keeps its own height.
        std::vector<double> sum(out.size() + 1, 0.0);
        for (size_t i = 0; i < out.size(); ++i) sum[i + 1] = sum[i] + ground(out[i].pos.x, out[i].pos.z).value_or(out[i].pos.y);
        const size_t half = size_t(std::max(road.width * 1.5f, 8.0f));
        for (size_t i = 0; i < out.size(); ++i)
        {
            const size_t a = i > half ? i - half : 0, b = std::min(out.size(), i + half + 1);
            out[i].pos.y = float((sum[b] - sum[a]) / double(b - a));
        }
    }
    return out;
}

bool ApplyRoads(const std::vector<const Road*>& roads, AdtChunk& c, std::vector<std::string>& textures, const RoadGround& ground, bool paint,
                const RoadLines& lines)
{
    bool changed = false;
    const float x0 = c.baseX, z0 = c.baseZ, x1 = c.baseX + kChunkSize, z1 = c.baseZ + kChunkSize;
    for (const Road* road : roads)
    {
        if (road->points.size() < 2) continue;
        const float reach = RoadReach(*road);
        // Quick reject on the control points (the spline stays close to them).
        float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
        for (const RoadPoint& p : road->points)
        {
            minX = std::min(minX, p.pos.x); maxX = std::max(maxX, p.pos.x);
            minZ = std::min(minZ, p.pos.z); maxZ = std::max(maxZ, p.pos.z);
        }
        const float slack = reach + 20.0f;
        if (maxX + slack < x0 || minX - slack > x1 || maxZ + slack < z0 || minZ - slack > z1) continue;
        std::vector<RoadSample> own;
        if (!lines) own = SampleRoad(*road, ground);
        const std::vector<RoadSample>& samples = lines ? lines(*road) : own;
        std::vector<std::pair<const RoadSample*, const RoadSample*>> segs;
        for (size_t i = 0; i + 1 < samples.size(); ++i)
        {
            const RoadSample &a = samples[i], &b = samples[i + 1];
            if (std::max(a.pos.x, b.pos.x) + reach < x0 || std::min(a.pos.x, b.pos.x) - reach > x1 ||
                std::max(a.pos.z, b.pos.z) + reach < z0 || std::min(a.pos.z, b.pos.z) - reach > z1)
                continue;
            segs.push_back({ &a, &b });
        }
        if (segs.empty()) continue;
        changed = true;
        const float shoulder = std::max(road->shoulder, 0.0f);

        // Heights: pulled towards the centre line's height (level side to side), sunk in the middle, fading out past
        // the shoulder.
        for (size_t j = 0; j < 145; ++j)
        {
            float vx, vz;
            VertexXZ(j, vx, vz);
            const float x = c.baseX + vx * kUnitSize, z = c.baseZ + vz * kUnitSize;
            const Nearest nb = NearestOn(segs, x, z);
            const float d = std::sqrt(nb.d2), half = nb.width / 2, outer = half + shoulder + 4.0f;
            if (d >= outer) continue;
            const float pull = d <= half ? 1.0f : Smooth(1.0f - (d - half) / (outer - half));
            const float sunk = d <= half ? 1.0f : Smooth(1.0f - (d - half) / std::max(shoulder, 1.0f));
            const float h = c.baseY + c.heights[j], target = nb.y - road->sink * sunk;
            c.heights[j] = h + (target - h) * std::clamp(road->grade, 0.0f, 1.0f) * pull - c.baseY;
        }

        if (!paint) continue;
        // Textures: the shoulder first, the centre over it, both with ragged edges.
        std::vector<float> centre(4096, 0.0f), side(4096, 0.0f);
        bool anyCentre = false, anySide = false;
        // Distance to the line and width there on a 33 x 33 grid (about a yard apart), bilinear per texel: a quarter of
        // the searches, and the field is smooth where it matters (at the edges).
        constexpr int kGrid = 33;
        std::array<float, kGrid * kGrid> gridD{}, gridW{};
        for (int gz = 0; gz < kGrid; ++gz)
            for (int gx = 0; gx < kGrid; ++gx)
            {
                const Nearest nb = NearestOn(segs, c.baseX + gx * kChunkSize / (kGrid - 1), c.baseZ + gz * kChunkSize / (kGrid - 1));
                gridD[size_t(gz * kGrid + gx)] = std::sqrt(nb.d2);
                gridW[size_t(gz * kGrid + gx)] = nb.width;
            }
        auto sample = [&](const std::array<float, kGrid * kGrid>& g, float u, float v) {
            const float gu = u * (kGrid - 1), gv = v * (kGrid - 1);
            const int iu = std::min(int(gu), kGrid - 2), iv = std::min(int(gv), kGrid - 2);
            const float fu = gu - iu, fv = gv - iv;
            const float a = g[size_t(iv * kGrid + iu)] + (g[size_t(iv * kGrid + iu + 1)] - g[size_t(iv * kGrid + iu)]) * fu;
            const float b = g[size_t((iv + 1) * kGrid + iu)] + (g[size_t((iv + 1) * kGrid + iu + 1)] - g[size_t((iv + 1) * kGrid + iu)]) * fu;
            return a + (b - a) * fv;
        };
        for (int ty = 0; ty < 64; ++ty)
            for (int tx = 0; tx < 64; ++tx)
            {
                const float u = (tx + 0.5f) / 64, v = (ty + 0.5f) / 64;
                const float x = c.baseX + u * kChunkSize, z = c.baseZ + v * kChunkSize;
                const float d = sample(gridD, u, v), half = sample(gridW, u, v) / 2;
                if (d > half + shoulder + 3.0f) continue;
                const float rough = std::clamp(road->noise, 0.0f, 1.0f);
                const float n1 = Noise(x, z, 3.0f) * 0.7f + Noise(x, z, 1.3f) * 0.3f;
                const float n2 = Noise(x + 91.7f, z - 37.1f, 3.5f) * 0.7f + Noise(x - 11.3f, z + 53.9f, 1.5f) * 0.3f;
                const size_t i = size_t(ty * 64 + tx);
                // The edge wanders (coarse noise) and frays (fine noise): stones thin out into the dirt, the dirt breaks
                // into the grass in patches, as Blizzard's Barrens roads do.
                const float fray1 = Noise(x + 13.1f, z + 7.7f, 0.8f), fray2 = Noise(x - 5.3f, z + 29.3f, 1.1f);
                if (!road->texture.empty())
                {
                    const float edge = half + n1 * rough * half * 0.8f;
                    centre[i] = Smooth((edge - d) / 2.0f + 0.5f + fray1 * rough * 0.7f);
                    anyCentre |= centre[i] > 0;
                }
                if (!road->shoulderTexture.empty() && shoulder > 0)
                {
                    const float edge = half + shoulder + n2 * rough * shoulder * 1.2f;
                    side[i] = Smooth((edge - d) / 3.0f + 0.5f + fray2 * rough * 0.8f);
                    anySide |= side[i] > 0;
                }
            }
        const std::vector<std::string> keep{ road->texture, road->shoulderTexture };
        if (anySide) RaiseShare(c, textures, road->shoulderTexture, side.data(), keep);
        if (anyCentre) RaiseShare(c, textures, road->texture, centre.data(), keep);
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------- store

const Road* RoadStore::Find(uint32_t id) const
{
    if (m_preview && m_preview->id == id) return &*m_preview;
    auto it = m_roads.find(id);
    return it == m_roads.end() ? nullptr : &it->second;
}

std::vector<const Road*> RoadStore::OnMap(const std::string& map) const
{
    std::vector<const Road*> out;
    for (const auto& [id, r] : m_roads)
        if (const Road* shown = Find(id); shown && shown->map == map) out.push_back(shown);
    if (m_preview && !m_roads.count(m_preview->id) && m_preview->map == map) out.push_back(&*m_preview);
    return out;
}

uint32_t RoadStore::NextId() const
{
    uint32_t next = 1;
    for (const auto& [id, r] : m_roads) next = std::max(next, id + 1);
    if (m_preview) next = std::max(next, m_preview->id + 1);
    return next;
}

std::map<std::pair<int, int>, uint64_t> RoadCellHashes(const Road& road)
{
    std::map<std::pair<int, int>, uint64_t> cells;
    nlohmann::json settings = road.ToJson();
    settings.erase("points");
    settings.erase("name");
    const uint64_t seed = std::hash<std::string>{}(settings.dump());
    // A sample shapes the ground within the road's reach, and (following the ground) the line's height over the
    // smoothing stretch around it.
    const float margin = RoadReach(road) + (road.followGround ? std::max(road.width * 1.5f, 8.0f) : 0.0f);
    for (const RoadSample& s : SampleRoad(road))
    {
        uint32_t bits[4];
        std::memcpy(bits, &s.pos, 12);
        std::memcpy(bits + 3, &s.width, 4);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t b : bits) h = (h ^ b) * 1099511628211ull;
        for (int gz = int(std::floor((s.pos.z - margin) / kChunkSize)); gz <= int(std::floor((s.pos.z + margin) / kChunkSize)); ++gz)
            for (int gx = int(std::floor((s.pos.x - margin) / kChunkSize)); gx <= int(std::floor((s.pos.x + margin) / kChunkSize)); ++gx)
            {
                auto [it, added] = cells.try_emplace({ gx, gz }, seed);
                it->second = (it->second ^ h) * 1099511628211ull;
            }
    }
    return cells;
}

void RoadStore::Changed(const Road* a, const Road* b)
{
    ++m_version;
    if (!onChanged) return;
    // Cells whose hash differs between the two versions (or that only one reaches), per map.
    const auto ha = a ? RoadCellHashes(*a) : std::map<std::pair<int, int>, uint64_t>{};
    const auto hb = b ? RoadCellHashes(*b) : std::map<std::pair<int, int>, uint64_t>{};
    if (a && b && a->map != b->map)
    {
        Cells all;
        for (const auto& [cell, h] : ha) all.insert(cell);
        onChanged(a->map, all);
        all.clear();
        for (const auto& [cell, h] : hb) all.insert(cell);
        onChanged(b->map, all);
        return;
    }
    Cells dirty;
    for (const auto& [cell, h] : ha)
        if (auto it = hb.find(cell); it == hb.end() || it->second != h) dirty.insert(cell);
    for (const auto& [cell, h] : hb)
        if (!ha.count(cell)) dirty.insert(cell);
    if (!dirty.empty()) onChanged(a ? a->map : b->map, dirty);
}

void RoadStore::Set(const Change& change, bool after)
{
    const uint32_t id = change.data.at("id");
    const nlohmann::json& j = change.data.at(after ? "after" : "before");
    std::optional<Road> old;
    if (const Road* shown = Find(id)) old = *shown;
    if (m_preview && m_preview->id == id) m_preview.reset();
    if (j.is_null()) m_roads.erase(id);
    else m_roads[id] = Road::FromJson(j);
    Changed(old ? &*old : nullptr, Find(id));
}

std::vector<const Road*> RoadStore::All() const
{
    std::vector<const Road*> out;
    for (const auto& [id, r] : m_roads) out.push_back(&r);
    return out;
}

Change RoadStore::MakeChange(const Road* before, const Road* after, const std::string& label) const
{
    const Road* any = after ? after : before;
    Change c;
    c.domain = Domain();
    c.label = label;
    c.target = any ? any->map + " road " + std::to_string(any->id) : std::string();
    c.data = { { "id", any ? any->id : 0u }, { "before", before ? before->ToJson() : nlohmann::json() }, { "after", after ? after->ToJson() : nlohmann::json() } };
    return c;
}

void RoadStore::Commit(const Road* before, const Road* after, const std::string& label)
{
    if (!before && !after) return;
    Change c = MakeChange(before, after, label);
    Apply(c);
    m_store.Commit(std::move(c));
}

void RoadStore::Preview(const Road* road)
{
    std::optional<Road> old;   // as shown until now: the last preview, or the saved road
    if (m_preview) old = *m_preview;
    else if (const Road* saved = road ? Saved(road->id) : nullptr) old = *saved;
    if (road) m_preview = *road;
    else m_preview.reset();
    const Road* now = road ? road : old ? Find(old->id) : nullptr;
    Changed(old ? &*old : nullptr, now);
}

// ---------------------------------------------------------------------------------------------- self test

bool RoadSelfTest()
{
    // A flat chunk at height 10 with one texture; a straight road along its middle (z = 16.7) at height 10.
    AdtChunk c;
    c.baseX = 0;
    c.baseZ = 0;
    c.baseY = 10;
    c.layerCount = 1;
    c.textureIds[0] = 0;
    c.alpha.assign(4096 * 4, 0);
    std::vector<std::string> textures{ "grass.blp" };
    Road r;
    r.width = 6;
    r.texture = "cobble.blp";
    r.shoulderTexture = "dirt.blp";
    r.shoulder = 3;
    r.grade = 1;
    r.sink = 0.5f;
    r.noise = 0;
    const float mid = kChunkSize / 2;
    r.points = { { { -20, 10, mid } }, { { 60, 10, mid } } };
    const auto samples = SampleRoad(r);
    if (samples.size() < 60 || std::fabs(samples.front().pos.x + 20) > 0.01f || std::fabs(samples.back().pos.x - 60) > 0.01f) return false;
    if (!ApplyRoads({ &r }, c, textures)) return false;
    // Row 4 of the outer grid runs along the middle: sunk by 0.5. Row 0 (16.7 yd away, past the reach) untouched.
    if (std::fabs(c.heights[4 * 17 + 4] + 0.5f) > 0.01f || c.heights[4] != 0.0f) return false;
    if (textures.size() != 3 || c.layerCount != 3) return false;
    // Texel in the middle: all cobbles; at the chunk's edge row: all grass.
    const size_t centre = size_t(32 * 64 + 32), edge = size_t(0 * 64 + 32);
    const auto w = LayerWeights(c.alpha[centre * 4] / 255.0f, c.alpha[centre * 4 + 1] / 255.0f, c.alpha[centre * 4 + 2] / 255.0f);
    const auto e = LayerWeights(c.alpha[edge * 4] / 255.0f, c.alpha[edge * 4 + 1] / 255.0f, c.alpha[edge * 4 + 2] / 255.0f);
    const int cobble = textures[c.textureIds[1]] == "cobble.blp" ? 1 : 2;
    return w[size_t(cobble)] > 0.97f && e[0] > 0.97f;
}
