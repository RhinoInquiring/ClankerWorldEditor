#include "Looks.hpp"

#include "Models.hpp"
#include "Mpq.hpp"

#include <algorithm>
#include <cmath>

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
    namespace IDI { constexpr uint32_t ModelName = 1, ModelTexture = 3, InventoryIcon = 5, GeosetGroup = 7, HelmetGeosetVis = 13, Texture = 15; }
    namespace CR { constexpr uint32_t MaleDisplay = 4, FemaleDisplay = 5, ClientPrefix = 6, Name = 14; }

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

    // Where each part goes on a 3.3.5 character skin, in pixels of a 256 x 256 skin (scaled to the skin's size):
    // x, y, width, height. The client's fixed layout (no CharComponentTextureSections table before Cataclysm).
    enum Region { ArmUpper, ArmLower, Hand, FaceUpper, FaceLower, TorsoUpper, TorsoLower, LegUpper, LegLower, Foot };
    constexpr int kRegions[10][4] = { { 0, 0, 128, 64 },    { 0, 64, 128, 64 },    { 0, 128, 128, 32 },  { 0, 160, 128, 32 },  { 0, 192, 128, 64 },
                                      { 128, 0, 128, 64 },  { 128, 64, 128, 32 },  { 128, 96, 128, 64 }, { 128, 160, 128, 64 }, { 128, 224, 128, 32 } };
    // ItemDisplayInfo Texture[0..7]: the region each fills and the folder its file sits in (Item\TextureComponents\<folder>).
    constexpr Region kItemRegions[8] = { ArmUpper, ArmLower, Hand, TorsoUpper, TorsoLower, LegUpper, LegLower, Foot };
    const char* const kItemFolders[8] = { "ArmUpperTexture", "ArmLowerTexture", "HandTexture", "TorsoUpperTexture",
                                          "TorsoLowerTexture", "LegUpperTexture", "LegLowerTexture", "FootTexture" };

    struct Layer { Region region; std::vector<std::string> files; };   // the first file that exists is drawn

    /// RGBA pixels of a BLP from the chain; empty when missing.
    std::vector<uint8_t> LoadPixels(const MpqChain& mpq, const std::string& path, uint32_t& w, uint32_t& h)
    {
        const auto file = mpq.Read(path);
        const auto image = file ? ParseBlp(*file) : std::nullopt;
        if (!image) return {};
        w = image->width;
        h = image->height;
        return BlpPixels(*image);
    }

    /// The base skin with every layer alpha-blended into its region (bilinear resampled), plus a box-filtered mip chain.
    std::optional<BlpImage> ComposeSkin(const MpqChain& mpq, const std::string& base, const std::vector<Layer>& layers)
    {
        uint32_t W = 0, H = 0;
        std::vector<uint8_t> canvas = LoadPixels(mpq, base, W, H);
        if (canvas.empty()) return std::nullopt;
        for (const Layer& layer : layers)
        {
            uint32_t w = 0, h = 0;
            std::vector<uint8_t> src;
            for (const std::string& f : layer.files)
                if (src = LoadPixels(mpq, f, w, h); !src.empty()) break;
            if (src.empty()) continue;
            const int* r = kRegions[layer.region];
            const float sx = W / 256.0f, sy = H / 256.0f;
            const int x0 = int(r[0] * sx), y0 = int(r[1] * sy), rw = std::max(1, int(r[2] * sx)), rh = std::max(1, int(r[3] * sy));
            auto texel = [&](int x, int y, int c) {
                x = std::clamp(x, 0, int(w) - 1);
                y = std::clamp(y, 0, int(h) - 1);
                return float(src[(size_t(y) * w + size_t(x)) * 4 + size_t(c)]);
            };
            for (int y = 0; y < rh && y0 + y < int(H); ++y)
                for (int x = 0; x < rw && x0 + x < int(W); ++x)
                {
                    const float u = (x + 0.5f) * float(w) / float(rw) - 0.5f, v = (y + 0.5f) * float(h) / float(rh) - 0.5f;
                    const int iu = int(std::floor(u)), iv = int(std::floor(v));
                    const float fu = u - float(iu), fv = v - float(iv);
                    float px[4];
                    for (int c = 0; c < 4; ++c)
                        px[c] = (texel(iu, iv, c) * (1 - fu) + texel(iu + 1, iv, c) * fu) * (1 - fv) +
                                (texel(iu, iv + 1, c) * (1 - fu) + texel(iu + 1, iv + 1, c) * fu) * fv;
                    uint8_t* d = &canvas[(size_t(y0 + y) * W + size_t(x0 + x)) * 4];
                    const float a = px[3] / 255.0f;
                    for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(px[c] * a + d[c] * (1 - a), 0.0f, 255.0f));
                }
        }
        BlpImage out;
        out.width = W;
        out.height = H;
        out.format = BlpImage::Format::RGBA8;
        out.mips.push_back(std::move(canvas));
        for (uint32_t w = W, h = H; w > 1 || h > 1;)
        {
            const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            const std::vector<uint8_t>& up = out.mips.back();
            std::vector<uint8_t> down(size_t(nw) * nh * 4);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        const uint32_t x1 = std::min(x * 2 + 1, w - 1), y1 = std::min(y * 2 + 1, h - 1);
                        const int sum = up[(size_t(y * 2) * w + x * 2) * 4 + c] + up[(size_t(y * 2) * w + x1) * 4 + c] + up[(size_t(y1) * w + x * 2) * 4 + c] +
                                        up[(size_t(y1) * w + x1) * 4 + c];
                        down[(size_t(y) * nw + x) * 4 + size_t(c)] = uint8_t(sum / 4);
                    }
            out.mips.push_back(std::move(down));
            w = nw;
            h = nh;
        }
        return out;
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
    load(m_animations, "AnimationData.dbc");
    load(m_factionTemplates, "FactionTemplate.dbc");
    load(m_factions, "Faction.dbc");
}

std::string DisplayLooks::FactionName(uint32_t factionTemplate)
{
    Load();
    const auto t = m_factionTemplates.Find(factionTemplate);
    const auto f = t ? m_factions.Find(m_factionTemplates.U32(*t, 1)) : std::nullopt;   // FactionTemplate.Faction
    return f ? m_factions.Str(*f, 23) : std::string();                                   // Faction.Name_lang (enUS)
}

std::string DisplayLooks::ItemIcon(uint32_t itemDisplay)
{
    Load();
    const auto row = m_items.Find(itemDisplay);
    const std::string icon = row ? m_items.Str(*row, IDI::InventoryIcon) : std::string();
    return icon.empty() ? icon : "Interface\\Icons\\" + icon + ".blp";
}

std::string DisplayLooks::AnimationName(uint32_t id)
{
    Load();
    const auto row = m_animations.Find(id);
    const std::string name = row ? m_animations.Str(*row, 1) : std::string();
    return name.empty() ? "Anim " + std::to_string(id) : name;
}

std::vector<uint32_t> DisplayLooks::SameModel(uint32_t displayId)
{
    Load();
    std::vector<uint32_t> out;
    const auto d = Display(displayId);
    if (!d) return out;
    for (uint32_t r = 0; r < m_displayInfo.Rows(); ++r)
        if (m_displayInfo.U32(r, CDI::ModelID) == d->model) out.push_back(m_displayInfo.U32(r, 0));
    if (std::find(out.begin(), out.end(), displayId) == out.end()) out.push_back(displayId);   // one of the project's
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> DisplayLooks::SkinNames(uint32_t displayId, bool* humanoid)
{
    std::vector<std::string> out;
    const auto d = Display(displayId);
    if (humanoid) *humanoid = d && d->extra != 0;
    if (!d) return out;
    for (const std::string& skin : d->skins)
        if (!skin.empty()) out.push_back(skin);
    return out;
}

std::optional<DisplayLooks::DisplayRow> DisplayLooks::Display(uint32_t id)
{
    Load();
    DisplayRow d;
    if (const nlohmann::json* j = m_override ? m_override(0, id) : nullptr)
    {
        if (!j->is_object()) return std::nullopt;
        d.model = j->value("ModelID", 0u);
        d.extra = j->value("ExtendedDisplayInfoID", 0u);
        d.scale = j->value("CreatureModelScale", 1.0f);
        for (int i = 0; i < 3; ++i) d.skins[i] = j->value("TextureVariation[" + std::to_string(i) + "]", std::string());
        return d;
    }
    const auto row = m_displayInfo.Find(id);
    if (!row) return std::nullopt;
    d.model = m_displayInfo.U32(*row, CDI::ModelID);
    d.extra = m_displayInfo.U32(*row, CDI::ExtendedDisplayInfoID);
    d.scale = m_displayInfo.F32(*row, CDI::CreatureModelScale);
    for (uint32_t i = 0; i < 3; ++i) d.skins[i] = m_displayInfo.Str(*row, CDI::TextureVariation + i);
    return d;
}

std::optional<DisplayLooks::ExtraRow> DisplayLooks::Extra(uint32_t id)
{
    Load();
    ExtraRow e;
    if (const nlohmann::json* j = m_override ? m_override(1, id) : nullptr)
    {
        if (!j->is_object()) return std::nullopt;
        e.race = j->value("DisplayRaceID", 0u);
        e.sex = j->value("DisplaySexID", 0u);
        e.skin = j->value("SkinID", 0u);
        e.face = j->value("FaceID", 0u);
        e.hairStyle = j->value("HairStyleID", 0u);
        e.hairColor = j->value("HairColorID", 0u);
        e.facial = j->value("FacialHairID", 0u);
        for (int i = 0; i < 11; ++i) e.items[i] = j->value("NPCItemDisplay[" + std::to_string(i) + "]", 0u);
        e.bake = j->value("BakeName", std::string());
        return e;
    }
    const auto row = m_extra.Find(id);
    if (!row) return std::nullopt;
    e.race = m_extra.U32(*row, CDIE::Race);
    e.sex = m_extra.U32(*row, CDIE::Sex);
    e.skin = m_extra.U32(*row, CDIE::Skin);
    e.face = m_extra.U32(*row, CDIE::Face);
    e.hairStyle = m_extra.U32(*row, CDIE::HairStyle);
    e.hairColor = m_extra.U32(*row, CDIE::HairColor);
    e.facial = m_extra.U32(*row, CDIE::FacialHair);
    for (uint32_t i = 0; i < 11; ++i) e.items[i] = m_extra.U32(*row, CDIE::Items + i);
    e.bake = m_extra.Str(*row, CDIE::BakeName);
    return e;
}

std::vector<DisplayLooks::Race> DisplayLooks::Races()
{
    Load();
    std::vector<Race> out;
    for (uint32_t r = 0; r < m_races.Rows(); ++r)
        out.push_back({ m_races.U32(r, 0), m_races.Str(r, CR::Name) });
    return out;
}

uint32_t DisplayLooks::RaceDisplay(uint32_t race, uint32_t sex)
{
    Load();
    const auto row = m_races.Find(race);
    return row ? m_races.U32(*row, sex ? CR::FemaleDisplay : CR::MaleDisplay) : 0;
}

uint32_t DisplayLooks::CharacterModel(uint32_t race, uint32_t sex)
{
    const auto d = Display(RaceDisplay(race, sex));
    return d ? d->model : 0;
}

DisplayLooks::Choices DisplayLooks::CharacterChoices(uint32_t race, uint32_t sex, uint32_t skin, uint32_t hairStyle)
{
    Load();
    Choices c;
    auto add = [](std::vector<uint32_t>& list, uint32_t v) { if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v); };
    for (uint32_t r = 0; r < m_sections.Rows(); ++r)
    {
        if (m_sections.U32(r, CS::Race) != race || m_sections.U32(r, CS::Sex) != sex) continue;
        const uint32_t section = m_sections.U32(r, CS::Section), variation = m_sections.U32(r, CS::Variation), color = m_sections.U32(r, CS::Color);
        if (section == 0 && variation == 0) add(c.skins, color);
        else if (section == 1 && color == skin) add(c.faces, variation);
        else if (section == 3 && variation == hairStyle) add(c.hairColors, color);
    }
    for (uint32_t r = 0; r < m_hair.Rows(); ++r)
        if (m_hair.U32(r, CHG::Race) == race && m_hair.U32(r, CHG::Sex) == sex) add(c.hairStyles, m_hair.U32(r, CHG::Variation));
    for (uint32_t r = 0; r < m_facial.Rows(); ++r)
        if (m_facial.U32(r, CFHS::Race) == race && m_facial.U32(r, CFHS::Sex) == sex) add(c.facialHair, m_facial.U32(r, CFHS::Variation));
    for (auto* list : { &c.skins, &c.faces, &c.hairStyles, &c.hairColors, &c.facialHair }) std::sort(list->begin(), list->end());
    return c;
}

std::string DisplayLooks::Composite(const ExtraRow& e)
{
    const std::string base = Section(e.race, e.sex, 0, 0, e.skin, 0);
    if (base.empty()) return {};
    std::vector<Layer> layers;
    auto add = [&](Region region, std::string file) { if (!file.empty()) layers.push_back({ region, { std::move(file) } }); };
    add(FaceLower, Section(e.race, e.sex, 1, e.face, e.skin, 0));            // face
    add(FaceUpper, Section(e.race, e.sex, 1, e.face, e.skin, 1));
    add(FaceLower, Section(e.race, e.sex, 2, e.facial, e.hairColor, 0));     // facial hair
    add(FaceUpper, Section(e.race, e.sex, 2, e.facial, e.hairColor, 1));
    add(FaceLower, Section(e.race, e.sex, 3, e.hairStyle, e.hairColor, 1));  // scalp
    add(FaceUpper, Section(e.race, e.sex, 3, e.hairStyle, e.hairColor, 2));
    add(LegUpper, Section(e.race, e.sex, 4, 0, e.skin, 0));                  // underwear
    add(TorsoUpper, Section(e.race, e.sex, 4, 0, e.skin, 1));
    // Armour from under to over; each part is <name>_<M|F>.blp for one sex, else <name>_U.blp.
    static const Slot kOrder[] = { Shirt, Legs, Boots, Wrist, Chest, Gloves, Tabard, Belt };
    for (const Slot slot : kOrder)
        if (const auto row = e.items[slot] ? m_items.Find(e.items[slot]) : std::nullopt)
            for (uint32_t part = 0; part < 8; ++part)
                if (const std::string name = m_items.Str(*row, IDI::Texture + part); !name.empty())
                {
                    const std::string stem = std::string("Item\\TextureComponents\\") + kItemFolders[part] + "\\" + name;
                    layers.push_back({ kItemRegions[part], { stem + (e.sex ? "_F.blp" : "_M.blp"), stem + "_U.blp" } });
                }
    std::string key = "composite:" + base;
    for (const Layer& l : layers) key += "|" + std::to_string(int(l.region)) + "=" + l.files.front();
    if (!m_composed.count(key) && m_upload)
    {
        if (const auto image = ComposeSkin(m_mpq, base, layers)) m_upload(key, *image);
        else return base;
        m_composed.insert(key);
    }
    return m_upload ? key : base;
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
    const auto d = Display(s.displayId);
    if (!d) return std::nullopt;
    const auto model = m_modelData.Find(d->model);
    if (!model) return std::nullopt;
    SpawnModel m;
    m.look.model = m_modelData.Str(*model, CMD::ModelName);
    if (m.look.model.empty()) return std::nullopt;
    const float scale = d->scale > 0.01f && d->scale < 100.0f ? d->scale : 1;
    m.scale = scale * (s.size > 0.01f && s.size < 100.0f ? s.size : 1);
    // Skins are file names next to the model.
    for (uint32_t i = 0; i < 3; ++i)
        if (!d->skins[i].empty()) m.look.textures[11 + i] = Folder(m.look.model) + d->skins[i] + ".blp";
    if (d->extra) Humanoid(d->extra, m);
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
    const auto x = Extra(extraId);
    if (!x) return;
    const uint32_t race = x->race, sex = x->sex, skin = x->skin, hairStyle = x->hairStyle, hairColor = x->hairColor, facial = x->facial;
    uint32_t items[11];
    std::copy(std::begin(x->items), std::end(x->items), items);

    // Textures: an NPC's skin, face and armour usually come baked into one body texture; without one the client
    // composites them, and so does the editor.
    if (!x->bake.empty()) m.look.textures[1] = "Textures\\BakedNpcTextures\\" + x->bake;
    else if (const std::string body = Composite(*x); !body.empty()) m.look.textures[1] = body;
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
