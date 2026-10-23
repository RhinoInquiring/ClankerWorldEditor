#include "GroundEffects.hpp"

#include "Blend.hpp"
#include "Mpq.hpp"

#include <algorithm>
#include <cmath>

bool GroundEffects::Load(const MpqChain& mpq)
{
    m_effects.clear();
    m_doodads.clear();
    Dbc textures, doodads;
    const auto t = mpq.Read("DBFilesClient\\GroundEffectTexture.dbc");
    const auto d = mpq.Read("DBFilesClient\\GroundEffectDoodad.dbc");
    if (!t || !d || !textures.Load(*t) || !doodads.Load(*d) || textures.Fields() < 11 || doodads.Fields() < 2) return false;
    for (uint32_t r = 0; r < textures.Rows(); ++r)
    {
        GroundEffect e;
        e.id = textures.U32(r, 0);
        for (uint32_t k = 0; k < 4; ++k)
        {
            e.doodads[k] = textures.U32(r, 1 + k);
            e.weights[k] = textures.U32(r, 5 + k);
        }
        e.density = textures.U32(r, 9);
        e.sound = textures.U32(r, 10);
        m_effects.push_back(e);
    }
    std::sort(m_effects.begin(), m_effects.end(), [](const GroundEffect& a, const GroundEffect& b) { return a.id < b.id; });
    for (uint32_t r = 0; r < doodads.Rows(); ++r) m_doodads[doodads.U32(r, 0)] = doodads.Str(r, 1);
    return !m_effects.empty();
}

const GroundEffect* GroundEffects::Effect(uint32_t id) const
{
    const auto it = std::lower_bound(m_effects.begin(), m_effects.end(), id, [](const GroundEffect& e, uint32_t v) { return e.id < v; });
    return it != m_effects.end() && it->id == id ? &*it : nullptr;
}

std::string GroundEffects::Model(uint32_t doodad) const
{
    const auto it = m_doodads.find(doodad);
    if (it == m_doodads.end() || it->second.empty()) return {};
    std::string stem = it->second;
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
    return "World\\NoDXT\\Detail\\" + stem + ".m2";
}

std::string GroundEffects::Describe(const GroundEffect& effect) const
{
    std::string out;
    for (uint32_t d : effect.doodads)
    {
        const auto it = d ? m_doodads.find(d) : m_doodads.end();
        if (it == m_doodads.end()) continue;
        std::string stem = it->second;
        if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
        out += (out.empty() ? "" : ", ") + stem;
    }
    return out.empty() ? "none" : out;
}

std::array<uint8_t, 64> GroundEffectLayers(const AdtChunk& c)
{
    std::array<uint8_t, 64> out{};
    const uint32_t n = std::min<uint32_t>(c.layerCount, 4);
    if (n < 2 || c.alpha.size() != 4096 * 4) return out;
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx)
        {
            std::array<float, 4> total{};
            for (int y = cy * 8; y < cy * 8 + 8; ++y)
                for (int x = cx * 8; x < cx * 8 + 8; ++x)
                {
                    const size_t i = size_t(y * 64 + x);
                    const auto w = LayerWeights(n > 1 ? c.alpha[i * 4] / 255.0f : 0, n > 2 ? c.alpha[i * 4 + 1] / 255.0f : 0, n > 3 ? c.alpha[i * 4 + 2] / 255.0f : 0);
                    for (size_t l = 0; l < 4; ++l) total[l] += w[l];
                }
            out[size_t(cy * 8 + cx)] = uint8_t(std::max_element(total.begin(), total.begin() + n) - total.begin());
        }
    return out;
}

std::array<uint8_t, 16> PackEffectMap(const std::array<uint8_t, 64>& layers)
{
    std::array<uint8_t, 16> map{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            const int bit = x * 2;   // row y = 16 bits little-endian, cell x at bit 2x
            map[size_t(y * 2 + bit / 8)] |= uint8_t((layers[size_t(y * 8 + x)] & 3) << (bit % 8));
        }
    return map;
}

std::array<uint8_t, 64> UnpackEffectMap(const std::array<uint8_t, 16>& map)
{
    std::array<uint8_t, 64> layers{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            const int bit = x * 2;
            layers[size_t(y * 8 + x)] = uint8_t((map[size_t(y * 2 + bit / 8)] >> (bit % 8)) & 3);
        }
    return layers;
}

uint64_t RotateCellBits(uint64_t bits)
{
    uint64_t out = 0;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            if (bits >> (r * 8 + c) & 1) out |= 1ull << (c * 8 + (7 - r));
    return out;
}

namespace
{
    /// splitmix64: a fixed sequence per seed.
    struct Random
    {
        uint64_t s;
        uint64_t Next()
        {
            uint64_t z = (s += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        float Unit() { return float(Next() >> 40) / float(1ull << 24); }   // [0, 1)
    };
}

std::vector<DoodadPlacement> ScatterGroundEffects(const AdtChunk& c, const std::array<uint8_t, 64>& layers, const GroundEffects& effects,
                                                  int perCell, uint32_t seed, const std::function<std::optional<float>(float x, float z)>& height)
{
    std::vector<DoodadPlacement> out;
    Random rnd{ seed * 0x100000001B3ull + 1 };
    const float cell = kChunkSize / 8;
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx)
        {
            if (c.noEffect >> (cy * 8 + cx) & 1) continue;
            if (c.holes >> ((cy / 2) * 4 + cx / 2) & 1) continue;
            const uint8_t layer = layers[size_t(cy * 8 + cx)];
            if (layer >= c.layerCount) continue;
            const GroundEffect* e = c.effectIds[layer] ? effects.Effect(c.effectIds[layer]) : nullptr;
            if (!e) continue;
            uint32_t weightSum = 0, models = 0;
            std::array<std::string, 4> names;
            for (size_t k = 0; k < 4; ++k)
                if (e->doodads[k] && !(names[k] = effects.Model(e->doodads[k])).empty())
                {
                    ++models;
                    weightSum += e->weights[k];
                }
            if (!models) continue;
            const int count = int(std::lround(double(e->density) * perCell / 128.0));
            for (int i = 0; i < count; ++i)
            {
                // A model by weight (equal chances when the row gives none).
                size_t pick = 0;
                uint32_t roll = uint32_t(rnd.Next() % std::max<uint32_t>(weightSum ? weightSum : models, 1));
                for (size_t k = 0; k < 4; ++k)
                {
                    if (names[k].empty()) continue;
                    const uint32_t w = weightSum ? e->weights[k] : 1;
                    if (roll < w) { pick = k; break; }
                    roll -= w;
                }
                const float x = c.baseX + (cx + rnd.Unit()) * cell, z = c.baseZ + (cy + rnd.Unit()) * cell;
                const float turn = rnd.Unit() * 360.0f;
                if (names[pick].empty()) continue;
                const auto y = height(x, z);
                if (!y) continue;
                DoodadPlacement d{};
                d.model = names[pick];
                d.pos[0] = x;
                d.pos[1] = *y;
                d.pos[2] = z;
                d.rot[1] = turn;
                d.scale = 1.0f;
                d.uniqueId = uint32_t((cy * 8 + cx) * 256 + i);
                out.push_back(std::move(d));
            }
        }
    return out;
}

bool GroundEffectsSelfTest()
{
    std::array<uint8_t, 64> layers{};
    for (size_t i = 0; i < 64; ++i) layers[i] = uint8_t((i * 7 + i / 8) % 4);
    if (UnpackEffectMap(PackEffectMap(layers)) != layers) return false;
    // Cell (0, 1) is layer 1 -> row 0, bits 2..3 of the first byte.
    std::array<uint8_t, 64> one{};
    one[1] = 1;
    if (PackEffectMap(one)[0] != 0x04) return false;
    uint64_t bits = 0x0123456789ABCDEFull;
    uint64_t turned = bits;
    for (int i = 0; i < 4; ++i) turned = RotateCellBits(turned);
    if (turned != bits || RotateCellBits(1ull) != (1ull << 7)) return false;   // (0, 0) -> (0, 7)

    // One effect, one cell open: every doodad lands in that cell and on the ground given.
    GroundEffects none;
    AdtChunk c;
    c.layerCount = 1;
    c.effectIds[0] = 5;
    const auto empty = ScatterGroundEffects(c, {}, none, 64, 1, [](float, float) { return 0.0f; });
    return empty.empty();   // no tables loaded: nothing to scatter
}
