#include "Looks.hpp"

#include "Models.hpp"
#include "Mpq.hpp"

#include <algorithm>

using namespace DirectX;

namespace
{
    // 3.3.5 (12340) field indices, from WoWDBDefs.
    namespace CDI { constexpr uint32_t ModelID = 1, ExtendedDisplayInfoID = 3, CreatureModelScale = 4, TextureVariation = 6; }
    namespace CMD { constexpr uint32_t ModelName = 2; }
    namespace GODI { constexpr uint32_t ModelName = 1; }
    namespace CDIE { constexpr uint32_t Race = 1, Sex = 2, Skin = 3, Face = 4, HairStyle = 5, HairColor = 6, FacialHair = 7, Items = 8, BakeName = 20; }
    namespace CS { constexpr uint32_t Race = 1, Sex = 2, Section = 3, Texture = 4, Variation = 8, Color = 9; }
    namespace CHG { constexpr uint32_t Race = 1, Sex = 2, Variation = 3, Geoset = 4; }
    namespace CFHS { constexpr uint32_t Race = 0, Sex = 1, Variation = 2, Geoset = 3; }
    namespace IDI { constexpr uint32_t ModelName = 1, ModelTexture = 3, GeosetGroup = 7, HelmetGeosetVis = 13; }
    namespace CR { constexpr uint32_t ClientPrefix = 6; }

    // CreatureDisplayInfoExtra NPCItemDisplay slots.
    enum Slot { Helm, Shoulder, Shirt, Chest, Belt, Legs, Boots, Wrist, Gloves, Tabard, Cape };
    // M2 attachment ids.
    constexpr uint32_t kShield = 0, kHandRight = 1, kHandLeft = 2, kShoulderRight = 5, kShoulderLeft = 6, kHelm = 11;

    std::string Folder(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    }

    std::string Stem(const std::string& file)
    {
        const size_t dot = file.find_last_of('.');
        return dot == std::string::npos ? file : file.substr(0, dot);
    }
}

void DisplayLooks::Load()
{
    if (m_loaded) return;
    m_loaded = true;
    auto load = [&](Dbc& dbc, const char* name) { dbc.Load(m_mpq.Read(std::string("DBFilesClient\\") + name).value_or(std::vector<uint8_t>{})); };
    load(m_displayInfo, "CreatureDisplayInfo.dbc");
    load(m_modelData, "CreatureModelData.dbc");
    load(m_goDisplay, "GameObjectDisplayInfo.dbc");
    load(m_extra, "CreatureDisplayInfoExtra.dbc");
    load(m_sections, "CharSections.dbc");
    load(m_hair, "CharHairGeosets.dbc");
    load(m_facial, "CharacterFacialHairStyles.dbc");
    load(m_items, "ItemDisplayInfo.dbc");
    load(m_races, "ChrRaces.dbc");
    load(m_helmVis, "HelmetGeosetVisData.dbc");
}

std::optional<DisplayLooks::SpawnModel> DisplayLooks::SpawnLook(const Spawn& s)
{
    Load();
    if (s.kind == SpawnKind::Creature) return CreatureLook(s);
    SpawnModel m;
    m.look.model = GameObjectModel(s.displayId);
    m.scale = s.size > 0.01f && s.size < 100.0f ? s.size : 1;
    if (m.look.model.empty()) return std::nullopt;
    return m;
}

std::string DisplayLooks::GameObjectModel(uint32_t displayId)
{
    Load();
    const auto row = m_goDisplay.Find(displayId);
    return row ? m_goDisplay.Str(*row, GODI::ModelName) : std::string();
}

std::optional<DisplayLooks::SpawnModel> DisplayLooks::CreatureLook(const Spawn& s)
{
    const auto row = m_displayInfo.Find(s.displayId);
    if (!row) return std::nullopt;
    const auto model = m_modelData.Find(m_displayInfo.U32(*row, CDI::ModelID));
    if (!model) return std::nullopt;
    SpawnModel m;
    m.look.model = m_modelData.Str(*model, CMD::ModelName);
    if (m.look.model.empty()) return std::nullopt;
    float scale = m_displayInfo.F32(*row, CDI::CreatureModelScale);
    if (!(scale > 0.01f && scale < 100.0f)) scale = 1;
    m.scale = scale * (s.size > 0.01f && s.size < 100.0f ? s.size : 1);
    // Skins are file names next to the model.
    for (uint32_t i = 0; i < 3; ++i)
        if (const std::string skin = m_displayInfo.Str(*row, CDI::TextureVariation + i); !skin.empty())
            m.look.textures[11 + i] = Folder(m.look.model) + skin + ".blp";
    if (const uint32_t extra = m_displayInfo.U32(*row, CDI::ExtendedDisplayInfoID)) Humanoid(extra, m);
    for (int hand = 0; hand < 2; ++hand)
        if (s.weapons[hand]) Weapon(s.weapons[hand], s.weaponTypes[hand], hand == 1, m);
    return m;
}

std::string DisplayLooks::Section(uint32_t race, uint32_t sex, uint32_t section, uint32_t variation, uint32_t color, uint32_t which) const
{
    for (uint32_t r = 0; r < m_sections.Rows(); ++r)
        if (m_sections.U32(r, CS::Race) == race && m_sections.U32(r, CS::Sex) == sex && m_sections.U32(r, CS::Section) == section &&
            m_sections.U32(r, CS::Variation) == variation && m_sections.U32(r, CS::Color) == color)
            return m_sections.Str(r, CS::Texture + which);
    return {};
}

void DisplayLooks::Humanoid(uint32_t extraId, SpawnModel& m)
{
    const auto e = m_extra.Find(extraId);
    if (!e) return;
    const uint32_t race = m_extra.U32(*e, CDIE::Race), sex = m_extra.U32(*e, CDIE::Sex);
    const uint32_t skin = m_extra.U32(*e, CDIE::Skin), hairStyle = m_extra.U32(*e, CDIE::HairStyle), hairColor = m_extra.U32(*e, CDIE::HairColor);
    const uint32_t facial = m_extra.U32(*e, CDIE::FacialHair);
    uint32_t items[11];
    for (uint32_t i = 0; i < 11; ++i) items[i] = m_extra.U32(*e, CDIE::Items + i);

    // Textures: the NPC's skin, face and armour come pre-baked into one body texture.
    // ponytail: no BakeName -> plain skin from CharSections (no face or armour compositing).
    if (const std::string bake = m_extra.Str(*e, CDIE::BakeName); !bake.empty()) m.look.textures[1] = "Textures\\BakedNpcTextures\\" + bake;
    else if (const std::string base = Section(race, sex, 0, 0, skin, 0); !base.empty()) m.look.textures[1] = base;
    if (const std::string hair = Section(race, sex, 3, hairStyle, hairColor, 0); !hair.empty()) m.look.textures[6] = hair;
    if (const std::string fur = Section(race, sex, 0, 0, skin, 1); !fur.empty()) m.look.textures[8] = fur;

    // Geosets: group * 100 + variant, one variant per group. Defaults first, then hair, facial hair, armour.
    std::map<uint32_t, uint32_t> group = { { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 }, { 7, 2 }, { 8, 1 }, { 9, 1 },
                                           { 10, 1 }, { 11, 1 }, { 12, 1 }, { 13, 1 }, { 15, 1 }, { 18, 1 } };
    uint32_t hairGeoset = 0;
    for (uint32_t r = 0; r < m_hair.Rows(); ++r)
        if (m_hair.U32(r, CHG::Race) == race && m_hair.U32(r, CHG::Sex) == sex && m_hair.U32(r, CHG::Variation) == hairStyle)
            hairGeoset = m_hair.U32(r, CHG::Geoset);
    for (uint32_t r = 0; r < m_facial.Rows(); ++r)
        if (m_facial.U32(r, CFHS::Race) == race && m_facial.U32(r, CFHS::Sex) == sex && m_facial.U32(r, CFHS::Variation) == facial)
        {
            // Geoset[0..2] are groups 1, 3 and 2.
            group[1] = m_facial.U32(r, CFHS::Geoset);
            group[3] = m_facial.U32(r, CFHS::Geoset + 1);
            group[2] = m_facial.U32(r, CFHS::Geoset + 2);
        }
    auto geosetGroup = [&](Slot slot, uint32_t n) -> uint32_t {
        const auto row = items[slot] ? m_items.Find(items[slot]) : std::nullopt;
        return row ? m_items.U32(*row, IDI::GeosetGroup + n) : 0;
    };
    auto apply = [&](Slot slot, uint32_t n, uint32_t g) {
        if (const uint32_t v = geosetGroup(slot, n)) group[g] = 1 + v;
    };
    apply(Gloves, 0, 4);
    apply(Boots, 0, 5);
    apply(Shirt, 0, 8);
    apply(Shirt, 1, 10);
    apply(Chest, 0, 8);
    apply(Chest, 1, 10);
    apply(Legs, 0, 11);
    apply(Legs, 1, 9);
    apply(Legs, 2, 13);
    apply(Chest, 2, 13);   // a robe wins over trousers
    apply(Belt, 0, 18);
    apply(Cape, 0, 15);
    if (items[Tabard]) group[12] = 2;

    // Armour models: helmet (named per race and sex), shoulders; the cape is a texture.
    const auto raceRow = m_races.Find(race);
    const std::string prefix = raceRow ? m_races.Str(*raceRow, CR::ClientPrefix) : std::string();
    if (const auto helm = items[Helm] ? m_items.Find(items[Helm]) : std::nullopt)
    {
        if (const std::string model = m_items.Str(*helm, IDI::ModelName); !model.empty() && !prefix.empty())
        {
            Attached a{ {}, kHelm };
            a.look.model = "Item\\ObjectComponents\\Head\\" + Stem(model) + "_" + prefix + (sex ? "F" : "M") + ".m2";
            if (const std::string tex = m_items.Str(*helm, IDI::ModelTexture); !tex.empty()) a.look.textures[2] = "Item\\ObjectComponents\\Head\\" + tex + ".blp";
            m.items.push_back(std::move(a));
        }
        // HelmetGeosetVisData: per group (hair, facial 1-3, ears, ...) a non-zero value hides it under the helmet.
        if (const auto vis = m_helmVis.Find(m_items.U32(*helm, IDI::HelmetGeosetVis + (sex ? 1 : 0))))
        {
            if (m_helmVis.U32(*vis, 1)) hairGeoset = 0;
            const uint32_t groups[4] = { 1, 2, 3, 7 };
            for (uint32_t i = 0; i < 4; ++i)
                if (m_helmVis.U32(*vis, 2 + i)) group[groups[i]] = 0;
        }
    }
    if (const auto shoulder = items[Shoulder] ? m_items.Find(items[Shoulder]) : std::nullopt)
        for (uint32_t side = 0; side < 2; ++side)
            if (const std::string model = m_items.Str(*shoulder, IDI::ModelName + side); !model.empty())
            {
                Attached a{ {}, side == 0 ? kShoulderLeft : kShoulderRight };
                a.look.model = "Item\\ObjectComponents\\Shoulder\\" + Stem(model) + ".m2";
                if (const std::string tex = m_items.Str(*shoulder, IDI::ModelTexture + side); !tex.empty())
                    a.look.textures[2] = "Item\\ObjectComponents\\Shoulder\\" + tex + ".blp";
                m.items.push_back(std::move(a));
            }
    if (const auto cape = items[Cape] ? m_items.Find(items[Cape]) : std::nullopt)
        if (const std::string tex = m_items.Str(*cape, IDI::ModelTexture); !tex.empty()) m.look.textures[2] = "Item\\ObjectComponents\\Cape\\" + tex + ".blp";

    m.look.geosets = { 0 };
    if (hairGeoset) m.look.geosets.push_back(uint16_t(hairGeoset));
    for (const auto& [g, v] : group)
        if (v) m.look.geosets.push_back(uint16_t(g * 100 + v));
}

void DisplayLooks::Weapon(uint32_t itemDisplay, uint32_t inventoryType, bool left, SpawnModel& m)
{
    const auto row = m_items.Find(itemDisplay);
    if (!row) return;
    const std::string model = m_items.Str(*row, IDI::ModelName);
    if (model.empty()) return;
    const bool shield = inventoryType == 14;   // INVTYPE_SHIELD
    const std::string folder = shield ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
    Attached a{ {}, shield ? kShield : left ? kHandLeft : kHandRight };
    a.look.model = folder + Stem(model) + ".m2";
    if (const std::string tex = m_items.Str(*row, IDI::ModelTexture); !tex.empty()) a.look.textures[2] = folder + tex + ".blp";
    m.items.push_back(std::move(a));
}

XMMATRIX SpawnMatrix(const XMFLOAT3& p, float orientation, float scale)
{
    const float pos[3] = { p.x, p.y, p.z }, rot[3] = { 0, XMConvertToDegrees(orientation) + 180.0f, 0 };
    return PlacementMatrix(pos, rot, scale);
}

void AddSpawnModel(ModelRenderer& renderer, int key, const DisplayLooks::SpawnModel& model, const XMFLOAT3& pos, float orientation, uint32_t uid,
                   const MpqChain& mpq)
{
    const XMMATRIX world = SpawnMatrix(pos, orientation, model.scale);
    renderer.AddModel(key, model.look, world, model.scale, uid, mpq);
    // ponytail: items follow the body's first animation frame, not its idle sway; per-frame attachments if visible.
    for (const auto& item : model.items)
        if (const auto at = renderer.AttachmentMatrix(model.look.model, item.attachment, mpq))
            renderer.AddModel(key, item.look, XMLoadFloat4x4(&*at) * world, model.scale, uid, mpq);
}
