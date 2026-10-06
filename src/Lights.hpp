#pragma once

#include "Areas.hpp"

#include <DirectXMath.h>

#include <array>
#include <functional>
#include <optional>

/// What the client's light tables say for one LightParams set at one time of day, or a blend of several.
/// Colours 0..1. Band meanings (wowdev, 3.3.5): see kLightColorNames / kLightFloatNames.
struct LightState
{
    std::array<DirectX::XMFLOAT3, 18> colors{};
    std::array<float, 6> floats{};   // [0] fog end in yards (stored x36), [1] fog start as a share of the end, ...
    uint32_t skybox = 0;             // LightSkybox id of the winning set
    float glow = 0;
};

constexpr const char* kLightColorNames[18] = {
    "Diffuse (sun on ground)", "Ambient", "Sky top", "Sky middle", "Sky middle to horizon", "Sky above horizon", "Sky at horizon",
    "Fog and far mountains", "Unused (shadow)", "Sun and halo", "Sun outer halo / cloud base", "Cloud edge", "Cloud second base",
    "Unused (cloud layer 2)", "Ocean shallow", "Ocean deep", "River shallow", "River deep" };
constexpr const char* kLightFloatNames[6] = { "Fog distance", "Fog start", "Sun glow through clouds", "Cloud density", "Unknown 4", "Unknown 5" };
/// Light.dbc LightParamsID[8]: which set the client uses when.
constexpr const char* kLightSlotNames[8] = { "Clear", "Clear, under water", "Storm", "Storm, under water", "Death", "Slot 5", "Slot 6", "Slot 7" };
constexpr int kDayHalfMinutes = 2880;   // band times: half-minutes from midnight

/// A Light.dbc row as the editor works with it: position and radii in the editor's axes and yards (the file stores the
/// ADT placement space in inches, x36). A row at 0,0,0 with no radius is the map's default light.
struct LightVolume
{
    uint32_t id = 0, map = 0;
    DirectX::XMFLOAT3 pos{};
    float inner = 0, outer = 0;
    std::array<uint32_t, 8> params{};
    bool Global() const { return pos.x == 0 && pos.y == 0 && pos.z == 0 && inner == 0 && outer == 0; }
};

/// The five light tables. LightIntBand rows of params set P are 18P-17 .. 18P; LightFloatBand rows 6P-5 .. 6P.
class Lights
{
public:
    Lights(MpqChain& mpq, ChangeStore& store);

    DbcTable light, params, intBands, floatBands, skyboxes;
    std::vector<DbcTable*> Tables() { return { &light, &params, &intBands, &floatBands, &skyboxes }; }

    /// A row as it should be read: `draft` (unsaved edits by table name and id) first, then the table.
    using Draft = std::map<std::pair<std::string, uint32_t>, nlohmann::json>;
    const nlohmann::json& Row(const DbcTable& table, uint32_t id, const Draft* draft = nullptr) const;

    static LightVolume FromRow(const nlohmann::json& row);
    static nlohmann::json ToRow(const LightVolume& v, nlohmann::json row);
    /// Every light of a map, the default one first when it has one.
    std::vector<LightVolume> OnMap(uint32_t map, const Draft* draft = nullptr) const;

    /// One params set at a time of day (half-minutes), bands interpolated between their keys.
    LightState Params(uint32_t paramsId, int time, const Draft* draft = nullptr) const;
    /// The light at an editor position: the map's default (or Light 1, as the client falls back) blended towards each
    /// local light by how far inside its falloff the point is. `slot` picks the LightParamsID (0 = clear weather).
    LightState At(uint32_t map, const DirectX::XMFLOAT3& pos, int time, int slot = 0, const Draft* draft = nullptr) const;
    /// The same from a map's lights already listed (OnMap), for callers that keep the list.
    LightState At(const std::vector<LightVolume>& all, const DirectX::XMFLOAT3& pos, int time, int slot = 0, const Draft* draft = nullptr) const;

    /// One band's value at a time, and its keys (time, raw value) for editing.
    static std::vector<std::pair<int, uint32_t>> Keys(const nlohmann::json& band);
    static nlohmann::json SetKeys(nlohmann::json band, const std::vector<std::pair<int, uint32_t>>& keys);
    static DirectX::XMFLOAT3 Color(uint32_t raw);
    static uint32_t Raw(const DirectX::XMFLOAT3& color);
    static uint32_t FloatBits(float v);
    static float FloatOf(uint32_t bits);

    /// The lowest params id in [first, last] whose own row and 18 + 6 band rows are all free; 0 when none.
    uint32_t FreeParamsId(uint32_t first, uint32_t last) const;
    /// Rows (table, id, row) for a new params set copied from `from` (its params row and every band row).
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> CopyParams(uint32_t from, uint32_t to, const Draft* draft = nullptr);
    /// How many Light rows use a params id (any slot).
    size_t Users(uint32_t paramsId, const Draft* draft = nullptr) const;
};
