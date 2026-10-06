#pragma once

#include "Formats.hpp"
#include "ModelRenderer.hpp"
#include "Spawns.hpp"

#include <DirectXMath.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
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

    /// NPC viewer: an animation's name from AnimationData.dbc ("Stand", "Walk", ...; "Anim <id>" when unknown).
    std::string AnimationName(uint32_t animationId);
    /// Every CreatureDisplayInfo id drawing the same model as `displayId` (itself included), ascending: its skins.
    std::vector<uint32_t> SameModel(uint32_t displayId);
    /// A creature display's skin names (TextureVariation, empty ones left out) and whether it is a humanoid
    /// (CreatureDisplayInfoExtra) display.
    std::vector<std::string> SkinNames(uint32_t displayId, bool* humanoid = nullptr);
    /// NPC editor: the faction a FactionTemplate id belongs to (Faction.dbc name; empty when unknown).
    std::string FactionName(uint32_t factionTemplate);
    /// An item display's inventory icon as a texture path ("Interface\Icons\....blp"; empty when none).
    std::string ItemIcon(uint32_t itemDisplay);

    /// Rows that win over the client's CreatureDisplayInfo (`table` 0) and CreatureDisplayInfoExtra (1): the project's,
    /// and the NPC editor's unapplied ones. Returns null when the id is not overridden (a null json: the row is gone).
    using RowOverride = std::function<const nlohmann::json*(int table, uint32_t id)>;
    void SetRowOverride(RowOverride fn) { m_override = std::move(fn); }
    /// Where composited character skins go (the renderer's texture cache), under their "composite:..." names.
    void SetUpload(std::function<void(const std::string& name, const BlpImage& image)> fn) { m_upload = std::move(fn); }

    // Appearance editor: what a character model can wear.
    struct Race { uint32_t id = 0; std::string name; };
    std::vector<Race> Races();
    /// The CreatureModelData id of a race's character model (ChrRaces Male/FemaleDisplayID -> CreatureDisplayInfo).
    uint32_t CharacterModel(uint32_t race, uint32_t sex);
    /// The display ChrRaces names for a race and sex (a template row for new appearances); 0 when none.
    uint32_t RaceDisplay(uint32_t race, uint32_t sex);
    /// The values each customisation can take for a race and sex (faces depend on the skin colour, hair colours on
    /// the hair style), from CharSections, CharHairGeosets and CharacterFacialHairStyles.
    struct Choices { std::vector<uint32_t> skins, faces, hairStyles, hairColors, facialHair; };
    Choices CharacterChoices(uint32_t race, uint32_t sex, uint32_t skin, uint32_t hairStyle);

private:
    void Load();
    std::optional<SpawnModel> CreatureLook(const Spawn& spawn);
    /// The fields of CreatureDisplayInfo / CreatureDisplayInfoExtra the looks use, overrides first.
    struct DisplayRow { uint32_t model = 0, extra = 0; float scale = 1; std::string skins[3]; };
    std::optional<DisplayRow> Display(uint32_t id);
    struct ExtraRow { uint32_t race = 0, sex = 0, skin = 0, face = 0, hairStyle = 0, hairColor = 0, facial = 0, items[11] = {}; std::string bake; };
    std::optional<ExtraRow> Extra(uint32_t id);
    /// Fills a humanoid's textures, geosets and armour models from CreatureDisplayInfoExtra.
    void Humanoid(uint32_t extraId, SpawnModel& out);
    /// The body texture of a humanoid with no baked texture: skin, face, facial hair, scalp, underwear and armour
    /// drawn into their regions, as the client composites it. Returns the cached texture's name (empty: no skin).
    std::string Composite(const ExtraRow& e);
    /// A CharSections texture (`section` 0 skin, 3 hair) of race/sex/variation/colour; `which` picks TextureName[n].
    std::string Section(uint32_t race, uint32_t sex, uint32_t section, uint32_t variation, uint32_t color, uint32_t which) const;
    /// A weapon or shield (ItemDisplayInfo id) in a hand.
    void Weapon(uint32_t itemDisplay, uint32_t inventoryType, bool left, SpawnModel& out);

    const MpqChain& m_mpq;
    RowOverride m_override;
    std::function<void(const std::string&, const BlpImage&)> m_upload;
    std::set<std::string> m_composed;   // composite names already uploaded
    bool m_loaded = false;
    Dbc m_displayInfo, m_modelData, m_goDisplay, m_extra, m_sections, m_hair, m_facial, m_items, m_races, m_helmVis, m_animations, m_factionTemplates, m_factions;
};

/// Placement of a spawn's model at editor position `p`: server orientation o turns the model's +x to the server
/// angle, which is placement yaw o + 180 deg (see PlacementMatrix).
DirectX::XMMATRIX SpawnMatrix(const DirectX::XMFLOAT3& p, float orientation, float scale);

/// Adds a spawn's model and the items it carries to `renderer` on tile `key`.
void AddSpawnModel(ModelRenderer& renderer, int key, const DisplayLooks::SpawnModel& model, const DirectX::XMFLOAT3& pos, float orientation,
                   uint32_t uid, const MpqChain& mpq);
