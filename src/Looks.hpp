#pragma once

#include "Formats.hpp"
#include "ModelRenderer.hpp"
#include "Spawns.hpp"

#include <DirectXMath.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

class MpqChain;

/// Display ids -> what the client draws, from the client's DBCs (read on first use).
///   creature:   CreatureDisplayInfo -> CreatureModelData model + TextureVariation skins (types 11-13);
///               humanoids (ExtendedDisplayInfoID) -> CreatureDisplayInfoExtra: baked skin, hair, facial hair,
///               armour geosets, cape, helmet and shoulder models, plus the spawn's weapons
///   gameobject: GameObjectDisplayInfo model
class DisplayLooks
{
public:
    explicit DisplayLooks(const MpqChain& mpq) : m_mpq(mpq) {}
    /// Forget the tables (another client attached).
    void Reset() { m_loaded = false; }

    /// A model carried at one of the body's attachment points (helmet, shoulders, weapons).
    struct Attached { ModelLook look; uint32_t attachment; };
    struct SpawnModel
    {
        ModelLook look;
        float scale = 1;   // final: template size x display scale
        std::vector<Attached> items;
    };
    /// What a spawn looks like (`displayId`, `size`, and for creatures `weapons`); null when no model.
    std::optional<SpawnModel> SpawnLook(const Spawn& spawn);
    /// The gameobject's model path (empty: none).
    std::string GameObjectModel(uint32_t displayId);

private:
    void Load();
    std::optional<SpawnModel> CreatureLook(const Spawn& spawn);
    /// Fills a humanoid's textures, geosets and armour models from CreatureDisplayInfoExtra.
    void Humanoid(uint32_t extraId, SpawnModel& out);
    /// A CharSections texture (`section` 0 skin, 3 hair) of race/sex/variation/colour; `which` picks TextureName[n].
    std::string Section(uint32_t race, uint32_t sex, uint32_t section, uint32_t variation, uint32_t color, uint32_t which) const;
    /// A weapon or shield (ItemDisplayInfo id) in a hand.
    void Weapon(uint32_t itemDisplay, uint32_t inventoryType, bool left, SpawnModel& out);

    const MpqChain& m_mpq;
    bool m_loaded = false;
    Dbc m_displayInfo, m_modelData, m_goDisplay, m_extra, m_sections, m_hair, m_facial, m_items, m_races, m_helmVis;
};

/// Placement of a spawn's model at editor position `p`: server orientation o turns the model's +x to the server
/// angle, which is placement yaw o + 180 deg (see PlacementMatrix).
DirectX::XMMATRIX SpawnMatrix(const DirectX::XMFLOAT3& p, float orientation, float scale);

/// Adds a spawn's model and the items it carries to `renderer` on tile `key`.
void AddSpawnModel(ModelRenderer& renderer, int key, const DisplayLooks::SpawnModel& model, const DirectX::XMFLOAT3& pos, float orientation,
                   uint32_t uid, const MpqChain& mpq);
