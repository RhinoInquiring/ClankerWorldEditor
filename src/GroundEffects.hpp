#pragma once

#include "Formats.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class MpqChain;

// Ground effects: the detail doodads (grass, flowers, pebbles) the client scatters over the terrain near the camera.
// A texture layer names one (MCLY effect id, a GroundEffectTexture row of up to four GroundEffectDoodad models with
// weights and a density); the chunk header says, per 8 x 8 cell, which layer's effect grows there and which cells
// grow none (AdtChunk::effectMap / noEffect). Server-side nothing reads them.

/// GroundEffectTexture.dbc row (3.3.5: ID, DoodadID[4], DoodadWeight[4], Density, Sound).
struct GroundEffect
{
    uint32_t id = 0;
    std::array<uint32_t, 4> doodads{};   // GroundEffectDoodad ids; 0 = none
    std::array<uint32_t, 4> weights{};
    uint32_t density = 0;
    uint32_t sound = 0;
};

/// The client's ground effect tables.
class GroundEffects
{
public:
    /// Reads GroundEffectTexture.dbc and GroundEffectDoodad.dbc; false when either is missing.
    bool Load(const MpqChain& mpq);
    bool Loaded() const { return !m_effects.empty(); }
    const GroundEffect* Effect(uint32_t id) const;
    /// Every row, by id.
    const std::vector<GroundEffect>& All() const { return m_effects; }
    /// A GroundEffectDoodad row's model: World\NoDXT\Detail\<name>.m2; empty when there is no such row.
    std::string Model(uint32_t doodad) const;
    /// The doodads' file names without folder or extension, comma separated ("ElwGra01, ElwFlo02"); "none" when empty.
    std::string Describe(const GroundEffect& effect) const;

private:
    std::vector<GroundEffect> m_effects;          // sorted by id
    std::map<uint32_t, std::string> m_doodads;   // GroundEffectDoodad id -> file name as stored (ElwGra01.mdl)
};

/// The layer whose ground effect grows on each 8 x 8 cell (index y * 8 + x): the one covering most of the cell, as
/// the alpha reads with LayerWeights (ties: the lower layer). A chunk of one layer is all 0.
std::array<uint8_t, 64> GroundEffectLayers(const AdtChunk& chunk);
/// Cell layers packed the way the MCNK header stores them (AdtChunk::effectMap), and back.
std::array<uint8_t, 16> PackEffectMap(const std::array<uint8_t, 64>& layers);
std::array<uint8_t, 64> UnpackEffectMap(const std::array<uint8_t, 16>& map);
/// The 8 x 8 noEffect bits turned a quarter clockwise seen from above, as chunk cells turn (row, col) -> (col, 7 - row).
uint64_t RotateCellBits(uint64_t bits);

/// Detail doodads of one chunk for the editor's preview: `perCell` scales the rows' densities the way the client's
/// groundEffectDensity does (16 low .. 128 ultra; doodads per cell = density x perCell / 128), cells in holes or with
/// their noEffect bit get none, positions and turns come from a fixed seed so they stay put between rebuilds.
/// `height` gives the ground under a point (none: skipped).
/// ponytail: the client's own scatter pattern and count are not known; this matches what grows where, not each blade.
std::vector<DoodadPlacement> ScatterGroundEffects(const AdtChunk& chunk, const std::array<uint8_t, 64>& layers, const GroundEffects& effects,
                                                  int perCell, uint32_t seed, const std::function<std::optional<float>(float x, float z)>& height);

/// Packing, rotation and scattering round trips; false on the first mismatch.
bool GroundEffectsSelfTest();
