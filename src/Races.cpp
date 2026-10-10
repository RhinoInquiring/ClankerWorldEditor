#include "Races.hpp"

#include "Models.hpp"
#include "Mpq.hpp"
#include "Project.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }

    // ---------------------------------------------------------------------------------------------- layouts
    // From WoWDBDefs (builds 5875 and 12340), named as mod-dbc-patch's 3.3.5 schemas name them, so a 1.12 row reads as a
    // 3.3.5 one: columns 1.12 has and 3.3.5 does not are left out, columns 3.3.5 has and 1.12 does not read as 0.

    /// Builds a layout from (name, column, type, count) entries; columns are 4 bytes unless a byte offset is given.
    struct LayoutBuilder
    {
        DbcLayout l;
        LayoutBuilder& Col(const std::string& name, uint32_t column, char type = 'i', uint32_t count = 1)
        {
            for (uint32_t i = 0; i < count; ++i)
                l.columns.push_back({ count == 1 ? name : name + "[" + std::to_string(i) + "]", (column + i) * 4, type });
            return *this;
        }
        LayoutBuilder& Byte(const std::string& name, uint32_t offset) { l.columns.push_back({ name, offset, 'b' }); return *this; }
        /// A localized string of `locales` slots and its flags: Name_lang (slot 0, enUS), Name_lang[i], Name_lang_flags.
        LayoutBuilder& Loc(const std::string& name, uint32_t column, uint32_t locales)
        {
            l.columns.push_back({ name, column * 4, 's' });
            for (uint32_t i = 1; i < locales; ++i) l.columns.push_back({ name + "[" + std::to_string(i) + "]", (column + i) * 4, 's' });
            l.columns.push_back({ name + "_flags", (column + locales) * 4, 'i' });
            return *this;
        }
        DbcLayout Done(uint32_t recordSize, uint32_t fields) { l.recordSize = recordSize; l.fields = fields; return l; }
    };

    struct Layouts { DbcLayout races, sections, hair, facial, baseInfo, outfits, displays, models; };

    const Layouts& Wrath()
    {
        static const Layouts k = [] {
            Layouts w;
            w.races = LayoutBuilder{}.Col("ID", 0).Col("Flags", 1).Col("FactionID", 2).Col("ExplorationSoundID", 3).Col("MaleDisplayID", 4)
                          .Col("FemaleDisplayID", 5).Col("ClientPrefix", 6, 's').Col("BaseLanguage", 7).Col("CreatureType", 8)
                          .Col("ResSicknessSpellID", 9).Col("SplashSoundID", 10).Col("ClientFileString", 11, 's').Col("CinematicSequenceID", 12)
                          .Col("Alliance", 13).Loc("Name_lang", 14, 16).Loc("Name_female_lang", 31, 16).Loc("Name_male_lang", 48, 16)
                          .Col("FacialHairCustomization", 65, 's', 2).Col("HairCustomization", 67, 's').Col("Required_expansion", 68)
                          .Done(69 * 4, 69);
            w.sections = LayoutBuilder{}.Col("ID", 0).Col("RaceID", 1).Col("SexID", 2).Col("BaseSection", 3).Col("TextureName", 4, 's', 3)
                             .Col("Flags", 7).Col("VariationIndex", 8).Col("ColorIndex", 9).Done(40, 10);
            w.hair = LayoutBuilder{}.Col("ID", 0).Col("RaceID", 1).Col("SexID", 2).Col("VariationID", 3).Col("GeosetID", 4).Col("Showscalp", 5)
                         .Done(24, 6);
            w.facial = LayoutBuilder{}.Col("RaceID", 0).Col("SexID", 1).Col("VariationID", 2).Col("Geoset", 3, 'i', 5).Done(32, 8);
            w.baseInfo = LayoutBuilder{}.Byte("RaceID", 0).Byte("ClassID", 1).Done(2, 2);
            w.outfits = LayoutBuilder{}.Col("ID", 0).Byte("RaceID", 4).Byte("ClassID", 5).Byte("SexID", 6).Byte("OutfitID", 7)
                            .Col("ItemID", 2, 'i', 24).Col("DisplayItemID", 26, 'i', 24).Col("InventoryType", 50, 'i', 24).Done(296, 77);
            w.displays = LayoutBuilder{}.Col("ID", 0).Col("ModelID", 1).Col("SoundID", 2).Col("ExtendedDisplayInfoID", 3).Col("CreatureModelScale", 4, 'f')
                             .Col("CreatureModelAlpha", 5).Col("TextureVariation", 6, 's', 3).Col("PortraitTextureName", 9, 's').Col("SizeClass", 10)
                             .Col("BloodID", 11).Col("NPCSoundID", 12).Col("ParticleColorID", 13).Col("CreatureGeosetData", 14)
                             .Col("ObjectEffectPackageID", 15).Done(64, 16);
            w.models = LayoutBuilder{}.Col("ID", 0).Col("Flags", 1).Col("ModelName", 2, 's').Col("SizeClass", 3).Col("ModelScale", 4, 'f')
                           .Col("BloodID", 5).Col("FootprintTextureID", 6).Col("FootprintTextureLength", 7, 'f').Col("FootprintTextureWidth", 8, 'f')
                           .Col("FootprintParticleScale", 9, 'f').Col("FoleyMaterialID", 10).Col("FootstepShakeSize", 11).Col("DeathThudShakeSize", 12)
                           .Col("SoundID", 13).Col("CollisionWidth", 14, 'f').Col("CollisionHeight", 15, 'f').Col("MountHeight", 16, 'f')
                           .Col("GeoBoxMinX", 17, 'f').Col("GeoBoxMinY", 18, 'f').Col("GeoBoxMinZ", 19, 'f').Col("GeoBoxMaxX", 20, 'f')
                           .Col("GeoBoxMaxY", 21, 'f').Col("GeoBoxMaxZ", 22, 'f').Col("WorldEffectScale", 23, 'f').Col("AttachedEffectScale", 24, 'f')
                           .Col("MissileCollisionRadius", 25, 'f').Col("MissileCollisionPush", 26, 'f').Col("MissileCollisionRaise", 27, 'f')
                           .Done(112, 28);
            return w;
        }();
        return k;
    }

    const Layouts& Classic()
    {
        static const Layouts k = [] {
            Layouts c;
            // ChrRaces 1.12: MountScale (7), LoginEffectSpellID (10), CombatStunSpellID (11) and StartingTaxiNodes (14) have no
            // 3.3.5 column; 1.12 has no Alliance, female / male names or Required_expansion. Locales: 8 slots, the first 8 of 3.3.5's.
            c.races = LayoutBuilder{}.Col("ID", 0).Col("Flags", 1).Col("FactionID", 2).Col("ExplorationSoundID", 3).Col("MaleDisplayID", 4)
                          .Col("FemaleDisplayID", 5).Col("ClientPrefix", 6, 's').Col("BaseLanguage", 8).Col("CreatureType", 9)
                          .Col("ResSicknessSpellID", 12).Col("SplashSoundID", 13).Col("ClientFileString", 15, 's').Col("CinematicSequenceID", 16)
                          .Loc("Name_lang", 17, 8).Col("FacialHairCustomization", 26, 's', 2).Col("HairCustomization", 28, 's')
                          .Done(29 * 4, 29);
            c.sections = LayoutBuilder{}.Col("ID", 0).Col("RaceID", 1).Col("SexID", 2).Col("BaseSection", 3).Col("VariationIndex", 4)
                             .Col("ColorIndex", 5).Col("TextureName", 6, 's', 3).Col("Flags", 9).Done(40, 10);
            c.hair = Wrath().hair;
            // WoWDBDefs lists Geoset[6]; the first three are unused (0xCCCCCCCC in Turtle's table), the client's are the last
            // three, in the order 3.3.5 keeps first.
            c.facial = LayoutBuilder{}.Col("RaceID", 0).Col("SexID", 1).Col("VariationID", 2).Col("Geoset", 6, 'i', 3).Done(36, 9);
            c.baseInfo = Wrath().baseInfo;
            c.outfits = LayoutBuilder{}.Col("ID", 0).Byte("RaceID", 4).Byte("ClassID", 5).Byte("SexID", 6).Byte("OutfitID", 7)
                            .Col("ItemID", 2, 'i', 12).Col("DisplayItemID", 14, 'i', 12).Col("InventoryType", 26, 'i', 12).Done(152, 41);
            c.displays = LayoutBuilder{}.Col("ID", 0).Col("ModelID", 1).Col("SoundID", 2).Col("ExtendedDisplayInfoID", 3).Col("CreatureModelScale", 4, 'f')
                             .Col("CreatureModelAlpha", 5).Col("TextureVariation", 6, 's', 3).Col("SizeClass", 9).Col("BloodID", 10).Col("NPCSoundID", 11)
                             .Done(48, 12);
            DbcLayout models = Wrath().models;
            models.columns.resize(16);   // 1.12 ends at CollisionHeight
            models.recordSize = 64;
            models.fields = 16;
            c.models = models;
            return c;
        }();
        return k;
    }

    Dbc ReadDbc(const MpqChain& mpq, const char* name)
    {
        Dbc d;
        if (auto bytes = mpq.Read(std::string("DBFilesClient\\") + name + ".dbc")) d.Load(std::move(*bytes));
        return d;
    }

    const DbcColumn* Column(const DbcLayout& l, const std::string& name)
    {
        for (const DbcColumn& c : l.columns)
            if (c.name == name) return &c;
        return nullptr;
    }

    // ---------------------------------------------------------------------------------------------- rows <-> structs
    uint32_t U(const nlohmann::json& row, const char* key) { const auto it = row.find(key); return it != row.end() && it->is_number() ? it->get<uint32_t>() : 0; }
    std::string S(const nlohmann::json& row, const std::string& key) { const auto it = row.find(key); return it != row.end() && it->is_string() ? it->get<std::string>() : std::string(); }

    RaceCatalog::Race RaceFrom(const nlohmann::json& row, bool hasAlliance)
    {
        RaceCatalog::Race r;
        r.id = U(row, "ID");
        r.flags = U(row, "Flags");
        r.faction = U(row, "FactionID");
        r.display[0] = U(row, "MaleDisplayID");
        r.display[1] = U(row, "FemaleDisplayID");
        r.prefix = S(row, "ClientPrefix");
        r.baseLanguage = U(row, "BaseLanguage");
        r.creatureType = U(row, "CreatureType");
        r.fileString = S(row, "ClientFileString");
        r.cinematic = U(row, "CinematicSequenceID");
        r.alliance = hasAlliance ? int(U(row, "Alliance")) : -1;
        r.name = S(row, "Name_lang");
        r.names[0] = S(row, "Name_female_lang");
        r.names[1] = S(row, "Name_male_lang");
        r.row = row;
        return r;
    }
    RaceCatalog::Section SectionFrom(const nlohmann::json& j)
    {
        RaceCatalog::Section s{ U(j, "ID"), U(j, "RaceID"), U(j, "SexID"), U(j, "BaseSection"), U(j, "VariationIndex"), U(j, "ColorIndex"), U(j, "Flags") };
        for (int i = 0; i < 3; ++i) s.textures[i] = S(j, "TextureName[" + std::to_string(i) + "]");
        return s;
    }
    nlohmann::json SectionJson(const RaceCatalog::Section& s)
    {
        return { { "ID", s.id }, { "RaceID", s.race }, { "SexID", s.sex }, { "BaseSection", s.type }, { "TextureName[0]", s.textures[0] },
                 { "TextureName[1]", s.textures[1] }, { "TextureName[2]", s.textures[2] }, { "Flags", s.flags }, { "VariationIndex", s.variation },
                 { "ColorIndex", s.color } };
    }
    RaceCatalog::HairGeoset HairFrom(const nlohmann::json& j)
    {
        return { U(j, "ID"), U(j, "RaceID"), U(j, "SexID"), U(j, "VariationID"), U(j, "GeosetID"), U(j, "Showscalp") };
    }
    nlohmann::json HairJson(const RaceCatalog::HairGeoset& h)
    {
        return { { "ID", h.id }, { "RaceID", h.race }, { "SexID", h.sex }, { "VariationID", h.variation }, { "GeosetID", h.geoset }, { "Showscalp", h.showScalp } };
    }
    RaceCatalog::FacialHair FacialFrom(const nlohmann::json& j)
    {
        RaceCatalog::FacialHair f{ U(j, "RaceID"), U(j, "SexID"), U(j, "VariationID") };
        for (int i = 0; i < 5 && j.contains("Geoset[" + std::to_string(i) + "]"); ++i) f.geosets.push_back(U(j, ("Geoset[" + std::to_string(i) + "]").c_str()));
        return f;
    }
    nlohmann::json FacialJson(const RaceCatalog::FacialHair& f)
    {
        nlohmann::json j = { { "RaceID", f.race }, { "SexID", f.sex }, { "VariationID", f.variation } };
        for (int i = 0; i < 5; ++i) j["Geoset[" + std::to_string(i) + "]"] = size_t(i) < f.geosets.size() ? f.geosets[size_t(i)] : 0u;
        return j;
    }
    RaceCatalog::Outfit OutfitFrom(const nlohmann::json& j)
    {
        RaceCatalog::Outfit o{ U(j, "ID"), uint8_t(U(j, "RaceID")), uint8_t(U(j, "ClassID")), uint8_t(U(j, "SexID")), uint8_t(U(j, "OutfitID")) };
        for (int i = 0; j.contains("ItemID[" + std::to_string(i) + "]"); ++i)
        {
            const std::string n = "[" + std::to_string(i) + "]";
            o.items.push_back(U(j, ("ItemID" + n).c_str()));
            o.displays.push_back(U(j, ("DisplayItemID" + n).c_str()));
            o.types.push_back(U(j, ("InventoryType" + n).c_str()));
        }
        return o;
    }
    nlohmann::json OutfitJson(const RaceCatalog::Outfit& o)
    {
        nlohmann::json j = { { "ID", o.id }, { "RaceID", o.race }, { "ClassID", o.cls }, { "SexID", o.sex }, { "OutfitID", o.outfit } };
        for (size_t i = 0; i < 24; ++i)   // 3.3.5 has 24 slots; a 1.12 outfit fills the first 12
        {
            const std::string n = "[" + std::to_string(i) + "]";
            j["ItemID" + n] = i < o.items.size() ? o.items[i] : 0u;
            j["DisplayItemID" + n] = i < o.displays.size() ? o.displays[i] : 0u;
            j["InventoryType" + n] = i < o.types.size() ? o.types[i] : 0u;
        }
        return j;
    }

    /// The rows of a loaded table as JSON, or none when the file is not in `layout` (a table another tool reshaped).
    std::vector<nlohmann::json> Rows(const Dbc& d, const DbcLayout& layout)
    {
        std::vector<nlohmann::json> out;
        if (d.RecordSize() != layout.recordSize) return out;
        out.reserve(d.Rows());
        for (uint32_t r = 0; r < d.Rows(); ++r) out.push_back(DbcRowJson(d, r, layout));
        return out;
    }
}

// ---------------------------------------------------------------------------------------------- rows

nlohmann::json DbcRowJson(const Dbc& dbc, uint32_t row, const DbcLayout& layout)
{
    nlohmann::json j = nlohmann::json::object();
    for (const DbcColumn& c : layout.columns)
    {
        if (c.type == 'b') j[c.name] = uint32_t(dbc.U8At(row, c.offset));
        else if (c.type == 's') j[c.name] = dbc.Text(dbc.U32At(row, c.offset));
        else if (c.type == 'f')
        {
            const uint32_t bits = dbc.U32At(row, c.offset);
            float f;
            std::memcpy(&f, &bits, 4);
            j[c.name] = f;
        }
        else j[c.name] = dbc.U32At(row, c.offset);
    }
    return j;
}

std::vector<uint8_t> WriteDbcRows(const std::vector<nlohmann::json>& rows, const DbcLayout& layout)
{
    std::string strings(1, '\0');
    std::map<std::string, uint32_t> offsets;
    auto text = [&](const std::string& s) -> uint32_t {
        if (s.empty()) return 0;
        auto [it, added] = offsets.try_emplace(s, uint32_t(strings.size()));
        if (added) strings.append(s).push_back('\0');
        return it->second;
    };
    std::vector<uint8_t> body(rows.size() * layout.recordSize, 0);
    for (size_t r = 0; r < rows.size(); ++r)
    {
        uint8_t* rec = body.data() + r * layout.recordSize;
        for (const DbcColumn& c : layout.columns)
        {
            const auto it = rows[r].find(c.name);
            if (it == rows[r].end() || it->is_null()) continue;
            uint32_t bits = 0;
            if (c.type == 's') bits = text(it->is_string() ? it->get<std::string>() : std::string());
            else if (c.type == 'f') { const float f = it->get<float>(); std::memcpy(&bits, &f, 4); }
            else bits = it->get<uint32_t>();
            if (c.type == 'b') rec[c.offset] = uint8_t(bits);
            else std::memcpy(rec + c.offset, &bits, 4);
        }
    }
    const uint32_t header[5] = { 0x43424457 /* WDBC */, uint32_t(rows.size()), layout.fields, layout.recordSize, uint32_t(strings.size()) };
    std::vector<uint8_t> out(sizeof header);
    std::memcpy(out.data(), header, sizeof header);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

const std::vector<DbcField>& CreatureDisplayInfoFields()
{
    static const std::vector<DbcField> k = [] {
        std::vector<DbcField> f;
        static std::vector<std::string> names;   // the DbcField names point into these
        for (const DbcColumn& c : Wrath().displays.columns) names.push_back(c.name);
        for (size_t i = 0; i < names.size(); ++i)
            f.push_back({ names[i].c_str(), Wrath().displays.columns[i].offset / 4, Wrath().displays.columns[i].type });
        return f;
    }();
    return k;
}

const std::vector<DbcField>& CreatureModelDataFields()
{
    static const std::vector<DbcField> k = [] {
        std::vector<DbcField> f;
        static std::vector<std::string> names;
        for (const DbcColumn& c : Wrath().models.columns) names.push_back(c.name);
        for (size_t i = 0; i < names.size(); ++i)
            f.push_back({ names[i].c_str(), Wrath().models.columns[i].offset / 4, Wrath().models.columns[i].type });
        return f;
    }();
    return k;
}

// ---------------------------------------------------------------------------------------------- catalog

bool RaceCatalog::Load(const MpqChain& mpq, std::string& error)
{
    *this = {};
    const Dbc races = ReadDbc(mpq, "ChrRaces");
    if (!races.Rows()) { error = "no ChrRaces.dbc"; return false; }
    const Layouts* l = races.RecordSize() == Wrath().races.recordSize ? &Wrath() : races.RecordSize() == Classic().races.recordSize ? &Classic() : nullptr;
    if (!l) { error = "ChrRaces.dbc has " + std::to_string(races.Fields()) + " fields: not a 1.12 or 3.3.5 layout"; return false; }
    m_layout = l == &Wrath() ? Layout::Wrath : Layout::Classic;
    // Rows by 3.3.5 names whatever the layout; then the structs the window and the counts use.
    for (const nlohmann::json& j : Rows(races, l->races)) m_races.push_back(RaceFrom(j, m_layout == Layout::Wrath));
    for (const nlohmann::json& j : Rows(ReadDbc(mpq, "CharSections"), l->sections)) m_sections.push_back(SectionFrom(j));
    for (const nlohmann::json& j : Rows(ReadDbc(mpq, "CharHairGeosets"), l->hair)) m_hair.push_back(HairFrom(j));
    for (const nlohmann::json& j : Rows(ReadDbc(mpq, "CharacterFacialHairStyles"), l->facial)) m_facial.push_back(FacialFrom(j));
    for (const nlohmann::json& j : Rows(ReadDbc(mpq, "CharBaseInfo"), l->baseInfo)) m_baseInfo.push_back({ uint8_t(U(j, "RaceID")), uint8_t(U(j, "ClassID")) });
    for (const nlohmann::json& j : Rows(ReadDbc(mpq, "CharStartOutfit"), l->outfits)) m_outfits.push_back(OutfitFrom(j));
    m_displayInfo = ReadDbc(mpq, "CreatureDisplayInfo");
    m_modelData = ReadDbc(mpq, "CreatureModelData");
    m_factionTemplates = ReadDbc(mpq, "FactionTemplate");
    m_factions = ReadDbc(mpq, "Faction");
    m_languages = ReadDbc(mpq, "Languages");
    return true;
}

std::vector<std::pair<uint32_t, std::string>> RaceCatalog::Languages() const
{
    std::vector<std::pair<uint32_t, std::string>> out;
    for (uint32_t r = 0; r < m_languages.Rows(); ++r) out.push_back({ m_languages.U32(r, 0), m_languages.Str(r, 1) });   // ID, Name_lang
    return out;
}

std::string RaceCatalog::LanguageName(uint32_t id) const
{
    const auto r = m_languages.Find(id);
    return r ? m_languages.Str(*r, 1) : std::string();
}

std::string RaceCatalog::FactionName(uint32_t factionTemplate) const
{
    const auto t = m_factionTemplates.Find(factionTemplate);
    const auto f = t ? m_factions.Find(m_factionTemplates.U32(*t, 1)) : std::nullopt;   // FactionTemplate.Faction
    // Faction.Name_lang: column 23 in 3.3.5, 19 in 1.12 (no ParentFactionMod / Cap before it).
    return f ? m_factions.Str(*f, m_factions.Fields() >= 57 ? 23 : 19) : std::string();
}

const RaceCatalog::Race* RaceCatalog::Find(uint32_t id) const
{
    for (const Race& r : m_races)
        if (r.id == id) return &r;
    return nullptr;
}

RaceCatalog::Counts RaceCatalog::Count(uint32_t race, uint32_t sex) const
{
    std::set<uint32_t> skins, faces, hairColors, hairStyles, facial;
    for (const Section& s : m_sections)
    {
        if (s.race != race || s.sex != sex) continue;
        if (s.type == 0 && s.variation == 0) skins.insert(s.color);
        else if (s.type == 1) faces.insert(s.variation);
        else if (s.type == 3) hairColors.insert(s.color);
    }
    for (const HairGeoset& h : m_hair)
        if (h.race == race && h.sex == sex) hairStyles.insert(h.variation);
    for (const FacialHair& f : m_facial)
        if (f.race == race && f.sex == sex) facial.insert(f.variation);
    return { skins.size(), faces.size(), hairStyles.size(), hairColors.size(), facial.size() };
}

nlohmann::json RaceCatalog::DisplayRow(uint32_t id) const
{
    const auto row = m_displayInfo.Find(id);
    if (!row) return nullptr;
    const DbcLayout& l = m_displayInfo.RecordSize() == Wrath().displays.recordSize ? Wrath().displays : Classic().displays;
    return m_displayInfo.RecordSize() == l.recordSize ? DbcRowJson(m_displayInfo, *row, l) : nlohmann::json();
}

nlohmann::json RaceCatalog::ModelRow(uint32_t id) const
{
    const auto row = m_modelData.Find(id);
    if (!row) return nullptr;
    const DbcLayout& l = m_modelData.RecordSize() == Wrath().models.recordSize ? Wrath().models : Classic().models;
    return m_modelData.RecordSize() == l.recordSize ? DbcRowJson(m_modelData, *row, l) : nlohmann::json();
}

uint32_t RaceCatalog::ModelId(const std::string& model) const
{
    const std::string want = Lower(M2Name(model));
    for (uint32_t r = 0; r < m_modelData.Rows(); ++r)
        if (Lower(M2Name(m_modelData.Str(r, 2))) == want) return m_modelData.U32(r, 0);   // ModelName
    return 0;
}

std::string RaceCatalog::Model(uint32_t race, uint32_t sex) const
{
    const Race* r = Find(race);
    if (!r) return {};
    const uint32_t display = r->display[sex ? 1 : 0];
    if (m_modelOf)
        if (std::string name = m_modelOf(display); !name.empty()) return M2Name(name);
    const auto d = m_displayInfo.Find(display);
    const auto model = d ? m_modelData.Find(m_displayInfo.U32(*d, 1)) : std::nullopt;   // ModelID
    const std::string name = model ? m_modelData.Str(*model, 2) : std::string();         // ModelName
    return name.empty() ? name : M2Name(name);
}

std::vector<uint32_t> RaceCatalog::Classes(uint32_t race) const
{
    std::set<uint32_t> out;
    for (const auto& [r, c] : m_baseInfo)
        if (r == race) out.insert(c);
    return { out.begin(), out.end() };
}

std::vector<std::string> RaceCatalog::Files(uint32_t race) const
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& name) {
        if (!name.empty() && seen.insert(Lower(name)).second) out.push_back(name);
    };
    for (uint32_t sex = 0; sex < 2; ++sex) add(Model(race, sex));
    for (const Section& s : m_sections)
        if (s.race == race)
            for (const std::string& t : s.textures) add(t);
    return out;
}

nlohmann::json RaceCatalog::Package(uint32_t race) const
{
    const Race* r = Find(race);
    if (!r) return nullptr;
    nlohmann::json p = { { "ChrRaces", r->row } };
    nlohmann::json& sections = p["CharSections"] = nlohmann::json::array();
    for (const Section& s : m_sections)
        if (s.race == race) sections.push_back(SectionJson(s));
    nlohmann::json& hair = p["CharHairGeosets"] = nlohmann::json::array();
    for (const HairGeoset& h : m_hair)
        if (h.race == race) hair.push_back(HairJson(h));
    nlohmann::json& facial = p["CharacterFacialHairStyles"] = nlohmann::json::array();
    for (const FacialHair& f : m_facial)
        if (f.race == race) facial.push_back(FacialJson(f));
    nlohmann::json& base = p["CharBaseInfo"] = nlohmann::json::array();
    for (const auto& [rr, c] : m_baseInfo)
        if (rr == race) base.push_back({ { "RaceID", rr }, { "ClassID", c } });
    nlohmann::json& outfits = p["CharStartOutfit"] = nlohmann::json::array();
    for (const Outfit& o : m_outfits)
        if (o.race == race) outfits.push_back(OutfitJson(o));
    return p;
}

void RaceCatalog::Apply(uint32_t race, const nlohmann::json& package)
{
    std::erase_if(m_races, [&](const Race& r) { return r.id == race; });
    std::erase_if(m_sections, [&](const Section& s) { return s.race == race; });
    std::erase_if(m_hair, [&](const HairGeoset& h) { return h.race == race; });
    std::erase_if(m_facial, [&](const FacialHair& f) { return f.race == race; });
    std::erase_if(m_baseInfo, [&](const std::pair<uint8_t, uint8_t>& b) { return b.first == race; });
    std::erase_if(m_outfits, [&](const Outfit& o) { return o.race == race; });
    if (!package.is_object()) return;
    m_races.push_back(RaceFrom(package.at("ChrRaces"), true));
    std::sort(m_races.begin(), m_races.end(), [](const Race& a, const Race& b) { return a.id < b.id; });
    for (const nlohmann::json& j : ChangeStore::List(package, "CharSections")) m_sections.push_back(SectionFrom(j));
    for (const nlohmann::json& j : ChangeStore::List(package, "CharHairGeosets")) m_hair.push_back(HairFrom(j));
    for (const nlohmann::json& j : ChangeStore::List(package, "CharacterFacialHairStyles")) m_facial.push_back(FacialFrom(j));
    for (const nlohmann::json& j : ChangeStore::List(package, "CharBaseInfo")) m_baseInfo.push_back({ uint8_t(U(j, "RaceID")), uint8_t(U(j, "ClassID")) });
    for (const nlohmann::json& j : ChangeStore::List(package, "CharStartOutfit")) m_outfits.push_back(OutfitFrom(j));
}

std::set<uint32_t> RaceCatalog::Ids(const std::string& table) const
{
    std::set<uint32_t> out;
    if (table == "CharSections") for (const Section& s : m_sections) out.insert(s.id);
    if (table == "CharHairGeosets") for (const HairGeoset& h : m_hair) out.insert(h.id);
    if (table == "CharStartOutfit") for (const Outfit& o : m_outfits) out.insert(o.id);
    return out;
}

// ---------------------------------------------------------------------------------------------- the project's races

Change RaceAdapter::MakeChange(uint32_t race, const nlohmann::json& after, const std::string& label) const
{
    Change c;
    c.domain = Domain();
    c.label = label;
    const nlohmann::json* before = Package(race);
    const nlohmann::json& shown = after.is_object() ? after : before ? *before : after;
    c.target = "race " + std::to_string(race) + (shown.is_object() ? " " + S(shown.at("ChrRaces"), "Name_lang") : "");
    c.data = { { "race", race }, { "before", before ? *before : nlohmann::json() }, { "after", after } };
    return c;
}

void RaceAdapter::Set(const Change& change, bool after)
{
    const uint32_t race = change.data.at("race");
    const nlohmann::json& p = change.data.at(after ? "after" : "before");
    if (p.is_null()) m_packages.erase(race);
    else m_packages[race] = p;
    ++m_version;
}

std::vector<Change> ImportRaceChanges(const RaceCatalog& source, const std::string& sourceName, uint32_t sourceRace, uint32_t target,
                                      const RaceCatalog& project, const RaceAdapter& races, const DbcTable& displays, const DbcTable& models,
                                      const Project& ranges, std::string& error, const RaceImportOptions& options)
{
    nlohmann::json p = source.Package(sourceRace);
    if (!p.is_object()) { error = "the source has no race " + std::to_string(sourceRace); return {}; }

    // The import's choices: team, faction, and the classes it can be (outfits of an added class from the donor race).
    if (options.alliance >= 0) p["ChrRaces"]["Alliance"] = options.alliance;
    if (options.faction) p["ChrRaces"]["FactionID"] = options.faction;
    if (options.classes)   // outfit ids are placeholders here: every outfit gets a new one below
        if (!SetRaceClasses(p, *options.classes, options.outfitDonor ? project.Package(options.outfitDonor) : nlohmann::json(), [] { return 1u; }, error))
            return {};

    // Ids: the lowest free in the project's range for each table, never one the client or the project uses already.
    std::map<std::string, std::set<uint32_t>> taken;
    auto next = [&](const std::string& table, const char* range, const std::function<bool(uint32_t)>& used) -> uint32_t {
        const Project::IdRange r = ranges.Range(range);
        std::set<uint32_t>& mine = taken[table];
        for (uint32_t id = std::max(r.first, 1u); id && id <= r.last; ++id)
            if (!mine.count(id) && !used(id)) { mine.insert(id); return id; }
        error = std::string("the ") + range + " range is full";
        return 0;
    };
    const std::set<uint32_t> sectionIds = project.Ids("CharSections"), hairIds = project.Ids("CharHairGeosets"), outfitIds = project.Ids("CharStartOutfit");

    p["ChrRaces"]["ID"] = target;
    p["ChrRaces"]["Flags"] = U(p["ChrRaces"], "Flags") & ~1u;   // 0x1 = an NPC race: an import is meant to be played
    for (const char* table : { "CharSections", "CharHairGeosets", "CharacterFacialHairStyles", "CharBaseInfo", "CharStartOutfit" })
        for (nlohmann::json& row : p[table]) row["RaceID"] = target;
    for (nlohmann::json& row : p["CharSections"])
        if (!(row["ID"] = next("CharSections", "charsections.id", [&](uint32_t id) { return sectionIds.count(id) != 0; })).get<uint32_t>()) return {};
    for (nlohmann::json& row : p["CharHairGeosets"])
        if (!(row["ID"] = next("CharHairGeosets", "charhairgeosets.id", [&](uint32_t id) { return hairIds.count(id) != 0; })).get<uint32_t>()) return {};
    for (nlohmann::json& row : p["CharStartOutfit"])
        if (!(row["ID"] = next("CharStartOutfit", "charstartoutfit.id", [&](uint32_t id) { return outfitIds.count(id) != 0; })).get<uint32_t>()) return {};

    // The two character displays, each with its model: a model the project's client lists keeps its row.
    std::vector<Change> rows;
    nlohmann::json added = { { "CreatureDisplayInfo", nlohmann::json::array() }, { "CreatureModelData", nlohmann::json::array() } };
    std::map<uint32_t, uint32_t> displayFor;   // source display -> new display
    for (const char* key : { "MaleDisplayID", "FemaleDisplayID" })
    {
        const uint32_t from = p["ChrRaces"].value(key, 0u);
        if (!from) continue;
        if (auto done = displayFor.find(from); done != displayFor.end()) { p["ChrRaces"][key] = done->second; continue; }
        nlohmann::json display = source.DisplayRow(from);
        if (!display.is_object()) { error = "the source has no CreatureDisplayInfo row " + std::to_string(from) + " for its race"; return {}; }
        const nlohmann::json model = source.ModelRow(display.value("ModelID", 0u));
        if (!model.is_object()) { error = "the source has no CreatureModelData row for display " + std::to_string(from); return {}; }
        uint32_t modelId = project.ModelId(model.value("ModelName", ""));
        if (!modelId)
            for (const auto& [id, row] : models.Rows())   // one the project added already (another race of the same source)
                if (Lower(M2Name(S(row, "ModelName"))) == Lower(M2Name(S(model, "ModelName")))) { modelId = id; break; }
        if (!modelId)
        {
            modelId = next("CreatureModelData", "creaturemodeldata.id", [&](uint32_t id) { return !models.Row(id).is_null(); });
            if (!modelId) return {};
            nlohmann::json row = nlohmann::json::object();
            for (const DbcField& f : CreatureModelDataFields()) row[f.name] = model.contains(f.name) ? model[f.name] : f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
            row["ID"] = modelId;
            rows.push_back(models.MakeChange(modelId, nullptr, row, "import race"));
            added["CreatureModelData"].push_back(modelId);
        }
        const uint32_t displayId = next("CreatureDisplayInfo", "creaturedisplayinfo.id", [&](uint32_t id) { return !displays.Row(id).is_null(); });
        if (!displayId) return {};
        nlohmann::json row = nlohmann::json::object();
        for (const DbcField& f : CreatureDisplayInfoFields()) row[f.name] = display.contains(f.name) ? display[f.name] : f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
        row["ID"] = displayId;
        row["ModelID"] = modelId;
        rows.push_back(displays.MakeChange(displayId, nullptr, row, "import race"));
        added["CreatureDisplayInfo"].push_back(displayId);
        displayFor[from] = displayId;
        p["ChrRaces"][key] = displayId;
    }
    // 1.12 rows have no Alliance: the faction of the client's race with the same faction template, else Horde.
    if (!p["ChrRaces"].contains("Alliance"))
    {
        int alliance = 1;
        for (const RaceCatalog::Race& r : project.Races())
            if (r.faction == U(p["ChrRaces"], "FactionID") && r.alliance >= 0) { alliance = r.alliance; break; }
        p["ChrRaces"]["Alliance"] = alliance;
    }
    p["source"] = { { "client", sourceName }, { "race", sourceRace } };
    p["added"] = added;
    std::vector<Change> out{ races.MakeChange(target, p, "import race") };
    out.insert(out.end(), rows.begin(), rows.end());
    return out;
}

std::vector<Change> RemoveRaceChanges(uint32_t race, const RaceAdapter& races, const DbcTable& displays, const DbcTable& models)
{
    const nlohmann::json* p = races.Package(race);
    if (!p) return {};
    std::vector<Change> out{ races.MakeChange(race, nullptr, "remove race") };
    const nlohmann::json added = p->value("added", nlohmann::json::object());
    for (const nlohmann::json& id : ChangeStore::List(added, "CreatureDisplayInfo"))
        if (const nlohmann::json* row = displays.Edited(id)) out.push_back(displays.MakeChange(id, *row, nullptr, "remove race"));
    for (const nlohmann::json& id : ChangeStore::List(added, "CreatureModelData"))
        if (const nlohmann::json* row = models.Edited(id)) out.push_back(models.MakeChange(id, *row, nullptr, "remove race"));
    return out;
}

namespace
{
    // Where a choice lives: its CharSections base sections and the column holding its value there, and its geoset table.
    struct ChoiceRows { std::vector<uint32_t> sections; const char* column; const char* geosets; };
    ChoiceRows RowsOf(RaceChoice c)
    {
        switch (c)
        {
        case RaceChoice::Skin: return { { 0, 1, 4 }, "ColorIndex", nullptr };          // skin, its faces, its underwear
        case RaceChoice::Face: return { { 1 }, "VariationIndex", nullptr };
        case RaceChoice::HairStyle: return { { 3 }, "VariationIndex", "CharHairGeosets" };
        case RaceChoice::HairColor: return { { 3, 2 }, "ColorIndex", nullptr };      // scalp (where its values are) and facial hair of that colour
        case RaceChoice::FacialHair: return { { 2 }, "VariationIndex", "CharacterFacialHairStyles" };
        }
        return {};
    }
}

std::vector<uint32_t> RaceChoiceValues(const nlohmann::json& package, uint32_t sex, RaceChoice choice)
{
    std::set<uint32_t> out;
    const ChoiceRows c = RowsOf(choice);
    if (c.geosets)
        for (const nlohmann::json& row : ChangeStore::List(package, c.geosets))
        {
            if (U(row, "SexID") == sex) out.insert(U(row, "VariationID"));
        }
    else
        for (const nlohmann::json& row : ChangeStore::List(package, "CharSections"))
            if (U(row, "SexID") == sex && U(row, "BaseSection") == c.sections.front()) out.insert(U(row, c.column));
    return { out.begin(), out.end() };
}

bool RemoveRaceChoice(nlohmann::json& package, uint32_t sex, RaceChoice choice, uint32_t value)
{
    const std::vector<uint32_t> values = RaceChoiceValues(package, sex, choice);
    if (std::find(values.begin(), values.end(), value) == values.end()) return false;
    const ChoiceRows c = RowsOf(choice);
    auto shift = [&](nlohmann::json& rows, const char* column, const std::function<bool(const nlohmann::json&)>& mine) {
        nlohmann::json kept = nlohmann::json::array();
        for (nlohmann::json& row : rows)
        {
            if (mine(row))
            {
                const uint32_t v = U(row, column);
                if (v == value) continue;
                if (v > value) row[column] = v - 1;
            }
            kept.push_back(std::move(row));
        }
        rows = std::move(kept);
    };
    if (package.contains("CharSections"))
        shift(package["CharSections"], c.column, [&](const nlohmann::json& row) {
            return U(row, "SexID") == sex && std::count(c.sections.begin(), c.sections.end(), U(row, "BaseSection"));
        });
    if (c.geosets && package.contains(c.geosets))
        shift(package[c.geosets], "VariationID", [&](const nlohmann::json& row) { return U(row, "SexID") == sex; });
    return true;
}

bool SetRaceClasses(nlohmann::json& package, const std::set<uint32_t>& classes, const nlohmann::json& donor,
                    const std::function<uint32_t()>& outfitIds, std::string& error)
{
    const uint32_t race = U(package.at("ChrRaces"), "ID");
    std::set<uint32_t> had;   // classes it has outfits of
    nlohmann::json outfits = nlohmann::json::array();
    for (const nlohmann::json& row : ChangeStore::List(package, "CharStartOutfit"))
        if (classes.count(U(row, "ClassID"))) { outfits.push_back(row); had.insert(U(row, "ClassID")); }
    if (donor.is_object())
        for (nlohmann::json row : ChangeStore::List(donor, "CharStartOutfit"))
            if (classes.count(U(row, "ClassID")) && !had.count(U(row, "ClassID")))
            {
                const uint32_t id = outfitIds();
                if (!id) { error = "the charstartoutfit.id range is full"; return false; }
                row["ID"] = id;
                row["RaceID"] = race;
                outfits.push_back(std::move(row));
            }
    nlohmann::json base = nlohmann::json::array();
    for (uint32_t c : classes) base.push_back({ { "RaceID", race }, { "ClassID", c } });
    package["CharBaseInfo"] = std::move(base);
    package["CharStartOutfit"] = std::move(outfits);
    return true;
}

uint32_t LanguageSkill(uint32_t language)
{
    switch (language)   // Languages.dbc id -> SkillLine (the racial languages players learn)
    {
    case 1: return 109;    // Orcish
    case 2: return 113;    // Darnassian
    case 3: return 115;    // Taurahe
    case 6: return 111;    // Dwarvish
    case 7: return 98;     // Common
    case 10: return 137;   // Thalassian
    case 13: return 313;   // Gnomish
    case 14: return 315;   // Troll
    case 33: return 673;   // Gutterspeak
    case 35: return 759;   // Draenei
    default: return 0;
    }
}

std::vector<Change> CopyRaceServerRows(uint32_t race, uint32_t donor, const std::vector<uint32_t>& classes, uint32_t baseLanguage,
                                       const std::vector<uint32_t>& fallbacks, const RaceServerTables& t)
{
    std::vector<Change> out;
    const std::string r = std::to_string(race);
    auto num = [](const nlohmann::json& row, const char* col) { const auto it = row.find(col); return it != row.end() && it->is_string() ? uint32_t(std::stoul("0" + it->get<std::string>())) : 0u; };
    // Per class: the donor's rows, else the first fallback race's that has a start for it.
    auto perClass = [&](TableRowsAdapter& table, const char* raceColumn) {
        std::vector<nlohmann::json> rows;
        for (uint32_t cls : classes)
        {
            for (uint32_t from : [&] { std::vector<uint32_t> v{ donor }; v.insert(v.end(), fallbacks.begin(), fallbacks.end()); return v; }())
            {
                std::vector<nlohmann::json> found;
                for (nlohmann::json row : table.Rows(from))
                    if (num(row, "class") == cls) { row[raceColumn] = r; found.push_back(std::move(row)); }
                bool hasStart = false;
                for (const nlohmann::json& s : t.start.Rows(from)) hasStart = hasStart || num(s, "class") == cls;
                if (!hasStart) continue;   // this race cannot be the class: try the next
                rows.insert(rows.end(), found.begin(), found.end());
                break;
            }
        }
        out.push_back(table.MakeChange(race, table.Rows(race), rows, "race server rows"));
    };
    perClass(t.start, "race");
    perClass(t.actions, "race");
    perClass(t.items, "race");
    std::vector<nlohmann::json> stats;
    for (nlohmann::json row : t.stats.Rows(donor)) { row["Race"] = r; stats.push_back(std::move(row)); }
    out.push_back(t.stats.MakeChange(race, t.stats.Rows(race), stats, "race server rows"));
    // Skills and spells of the donor's own mask bit; a racial language becomes the race's own.
    const uint32_t bit = RaceBit(race), donorBit = RaceBit(donor), language = LanguageSkill(baseLanguage);
    std::vector<nlohmann::json> skills;
    for (nlohmann::json row : t.skills.Rows(donorBit))
    {
        row["raceMask"] = std::to_string(bit);
        const uint32_t skill = num(row, "skill");
        bool isLanguage = false;
        for (uint32_t l : { 1u, 2u, 3u, 6u, 7u, 10u, 13u, 14u, 33u, 35u }) isLanguage = isLanguage || LanguageSkill(l) == skill;
        if (isLanguage && language) row["skill"] = std::to_string(language);
        skills.push_back(std::move(row));
    }
    if (language && std::none_of(skills.begin(), skills.end(), [&](const nlohmann::json& s) { return num(s, "skill") == language; }))
        skills.push_back({ { "raceMask", std::to_string(bit) }, { "classMask", "0" }, { "skill", std::to_string(language) }, { "rank", "300" },
                           { "comment", "language" } });
    out.push_back(t.skills.MakeChange(bit, t.skills.Rows(bit), skills, "race server rows"));
    std::vector<nlohmann::json> spells;
    for (nlohmann::json row : t.spells.Rows(donorBit)) { row["racemask"] = std::to_string(bit); spells.push_back(std::move(row)); }
    out.push_back(t.spells.MakeChange(bit, t.spells.Rows(bit), spells, "race server rows"));
    return out;
}

Change SetRaceStart(uint32_t race, uint32_t cls, uint32_t map, uint32_t zone, float x, float y, float z, float orientation, const RaceServerTables& t)
{
    std::vector<nlohmann::json> rows;
    for (const nlohmann::json& row : t.start.Rows(race))
        if (row.value("class", std::string()) != std::to_string(cls)) rows.push_back(row);
    auto f = [](float v) { char b[32]; std::snprintf(b, sizeof b, "%.4f", v); return std::string(b); };
    rows.push_back({ { "race", std::to_string(race) }, { "class", std::to_string(cls) }, { "map", std::to_string(map) }, { "zone", std::to_string(zone) },
                     { "position_x", f(x) }, { "position_y", f(y) }, { "position_z", f(z) }, { "orientation", f(orientation) } });
    return t.start.MakeChange(race, t.start.Rows(race), rows, "race start");
}

bool RacesSelfTest()
{
    // Synthetic tables of both layouts, written from 3.3.5-named rows and read back as the catalog reads them.
    for (const Layouts* l : { &Classic(), &Wrath() })
    {
        const bool classic = l == &Classic();
        nlohmann::json race = { { "ID", 7 }, { "MaleDisplayID", 1563 }, { "FemaleDisplayID", 1564 }, { "ClientPrefix", "Gn" },
                                { "ClientFileString", "Gnome" }, { "Name_lang", "Gnomish" }, { "Alliance", 0 } };
        const nlohmann::json section = { { "ID", 500 }, { "RaceID", 7 }, { "SexID", 1 }, { "BaseSection", 1 }, { "TextureName[0]", "face.blp" },
                                         { "Flags", 1 }, { "VariationIndex", 3 }, { "ColorIndex", 2 } };
        const nlohmann::json hair = { { "ID", 9 }, { "RaceID", 7 }, { "SexID", 1 }, { "VariationID", 4 }, { "GeosetID", 2 }, { "Showscalp", 1 } };
        const nlohmann::json facial = { { "RaceID", 7 }, { "SexID", 0 }, { "VariationID", 5 }, { "Geoset[0]", 10 }, { "Geoset[1]", 11 }, { "Geoset[2]", 12 } };
        nlohmann::json outfit = { { "ID", 12 }, { "RaceID", 7 }, { "ClassID", 4 }, { "SexID", 1 }, { "OutfitID", 0 } };
        for (int i = 0; i < 12; ++i) outfit["ItemID[" + std::to_string(i) + "]"] = 100 + i;
        Dbc races, sections, hairs, facials, base, outfits;
        races.Load(WriteDbcRows({ race }, l->races));
        sections.Load(WriteDbcRows({ section }, l->sections));
        hairs.Load(WriteDbcRows({ hair }, l->hair));
        facials.Load(WriteDbcRows({ facial }, l->facial));
        base.Load(WriteDbcRows({ { { "RaceID", 7 }, { "ClassID", 1 } }, { { "RaceID", 7 }, { "ClassID", 8 } } }, l->baseInfo));
        outfits.Load(WriteDbcRows({ outfit }, l->outfits));
        const RaceCatalog::Race r = RaceFrom(Rows(races, l->races).at(0), !classic);
        const RaceCatalog::Section s = SectionFrom(Rows(sections, l->sections).at(0));
        const RaceCatalog::HairGeoset h = HairFrom(Rows(hairs, l->hair).at(0));
        const RaceCatalog::FacialHair f = FacialFrom(Rows(facials, l->facial).at(0));
        const auto b = Rows(base, l->baseInfo);
        const RaceCatalog::Outfit o = OutfitFrom(Rows(outfits, l->outfits).at(0));
        if (r.id != 7 || r.prefix != "Gn" || r.fileString != "Gnome" || r.name != "Gnomish" || r.display[0] != 1563 || r.display[1] != 1564 ||
            r.alliance != (classic ? -1 : 0))
            return false;
        if (s.variation != 3 || s.color != 2 || s.textures[0] != "face.blp" || s.flags != 1 || s.type != 1 || h.geoset != 2 || h.variation != 4)
            return false;
        if (f.geosets.size() != (classic ? 3u : 5u) || f.geosets[2] != 12 || b.size() != 2 || U(b[1], "ClassID") != 8)
            return false;
        if (o.cls != 4 || o.sex != 1 || o.items.size() != (classic ? 12u : 24u) || o.items[11] != 111 || o.displays[0] != 0) return false;
        // A 1.12 header must match what the client reads: a CharSections file of the 1.12 order puts variation first.
        if (classic && sections.U32At(0, 16) != 3) return false;
    }
    // A package round-trips through a catalog: applied, read back, removed.
    RaceCatalog c;
    nlohmann::json p = { { "ChrRaces", { { "ID", 30 }, { "Name_lang", "Test" }, { "MaleDisplayID", 1 }, { "Alliance", 1 } } },
                         { "CharSections", { SectionJson({ 600000, 30, 0, 0, 0, 2, 1, { "a.blp", "", "" } }) } },
                         { "CharHairGeosets", { HairJson({ 9000, 30, 0, 1, 3, 1 }) } },
                         { "CharacterFacialHairStyles", { FacialJson({ 30, 0, 1, { 101, 301, 201 } }) } },
                         { "CharBaseInfo", { { { "RaceID", 30 }, { "ClassID", 1 } } } }, { "CharStartOutfit", nlohmann::json::array() } };
    c.Apply(30, p);
    const nlohmann::json back = c.Package(30);
    if (!back.is_object() || back["CharSections"].size() != 1 || back["CharSections"][0]["ColorIndex"] != 2 || back["CharHairGeosets"][0]["GeosetID"] != 3 ||
        back["CharacterFacialHairStyles"][0]["Geoset[2]"] != 201 || back["CharBaseInfo"].size() != 1 || c.Count(30, 0).skins != 1 || c.Classes(30) != std::vector<uint32_t>{ 1 })
        return false;
    c.Apply(30, nullptr);
    return !c.Find(30) && c.Sections().empty();
}
