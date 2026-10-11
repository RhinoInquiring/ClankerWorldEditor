#include "Races.hpp"

#include "Assets.hpp"
#include "Models.hpp"
#include "Mpq.hpp"
#include "Project.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <tuple>

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
    /// CharSections.Flags of 1.12 (1: only NPCs wear it) as 3.3.5 means it (0x1: players may choose it).
    /// CharSections.Flags of 1.12 (1: only NPCs wear it) as 3.3.5 means it: 0x1 players may choose it, and every stock
    /// player row but a face's also has 0x10 (all but Death Knight-only rows), which the client looks for when it
    /// rebuilds a character after a transform (without it: another race's skin, no Features).
    uint32_t ClassicSectionFlags(uint32_t flags, uint32_t section) { return (flags & 1) ? 0u : section == 1 ? 1u : 0x11u; }

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
    for (nlohmann::json& j : Rows(ReadDbc(mpq, "CharSections"), l->sections))
    {
        if (m_layout == Layout::Classic) j["Flags"] = ClassicSectionFlags(U(j, "Flags"), U(j, "BaseSection"));
        m_sections.push_back(SectionFrom(j));
    }
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

std::vector<nlohmann::json> RaceCatalog::TableRows(const std::string& table) const
{
    std::vector<nlohmann::json> out;
    if (table == "ChrRaces") for (const Race& r : m_races) out.push_back(r.row);
    if (table == "CharSections") for (const Section& s : m_sections) out.push_back(SectionJson(s));
    if (table == "CharHairGeosets") for (const HairGeoset& h : m_hair) out.push_back(HairJson(h));
    if (table == "CharacterFacialHairStyles") for (const FacialHair& f : m_facial) out.push_back(FacialJson(f));
    if (table == "CharBaseInfo") for (const auto& [r, c] : m_baseInfo) out.push_back({ { "RaceID", r }, { "ClassID", c } });
    if (table == "CharStartOutfit") for (const Outfit& o : m_outfits) out.push_back(OutfitJson(o));
    return out;
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
                                      const Project& ranges, std::string& error, const RaceImportOptions& options,
                                      const MpqChain* sourceFiles, const MpqChain* projectFiles)
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
    p["sectionFlags"] = "3.3.5";   // its CharSections flags as 3.3.5 means them (RaceCatalog::Load converts 1.12 ones)
    for (const char* table : { "CharSections", "CharHairGeosets", "CharacterFacialHairStyles", "CharBaseInfo", "CharStartOutfit" })
        for (nlohmann::json& row : p[table]) row["RaceID"] = target;
    for (nlohmann::json& row : p["CharSections"])
        if (!(row["ID"] = next("CharSections", "charsections.id", [&](uint32_t id) { return sectionIds.count(id) != 0; })).get<uint32_t>()) return {};
    for (nlohmann::json& row : p["CharHairGeosets"])
        if (!(row["ID"] = next("CharHairGeosets", "charhairgeosets.id", [&](uint32_t id) { return hairIds.count(id) != 0; })).get<uint32_t>()) return {};
    for (nlohmann::json& row : p["CharStartOutfit"])
        if (!(row["ID"] = next("CharStartOutfit", "charstartoutfit.id", [&](uint32_t id) { return outfitIds.count(id) != 0; })).get<uint32_t>()) return {};

    // The race's files: what export copies from the source, and what is renamed because the project's client has a file
    // of that name with other bytes (the client's own must stay what they are: its NPCs use them).
    nlohmann::json files = nlohmann::json::array();
    std::map<std::string, std::string> modelRename;   // lower-case model name (as its row gives it) -> its name in the project
    size_t missing = 0;
    if (sourceFiles && projectFiles)
    {
        const std::string tag = "Character\\Race" + std::to_string(target) + "\\";
        auto renameOf = [&](const std::string& f) { return tag + (Lower(f).rfind("character\\", 0) == 0 ? f.substr(10) : f); };
        enum class State { Same, Copy, Clash, Missing };
        auto state = [&](const std::string& f) {
            if (!sourceFiles->HasOwn(f)) return State::Missing;
            if (!projectFiles->HasOwn(f)) return State::Copy;
            const auto a = sourceFiles->Read(f), b = projectFiles->Read(f);
            if (!a || !b || *a != *b) return State::Clash;
            return projectFiles->HasInstalled(f) ? State::Same : State::Copy;
        };
        std::set<std::string> listed;   // lower-case targets
        auto add = [&](const std::string& path, const std::string& from, const nlohmann::json& textures = nlohmann::json()) {
            if (!listed.insert(Lower(path)).second) return;
            nlohmann::json e = { { "path", path }, { "from", from } };
            if (textures.is_object() && !textures.empty()) e["textures"] = textures;
            files.push_back(std::move(e));
        };
        // Each model with its skins, .anim files and fixed textures: renamed as one when any of them clashes.
        for (const char* key : { "MaleDisplayID", "FemaleDisplayID" })
        {
            const nlohmann::json display = source.DisplayRow(p["ChrRaces"].value(key, 0u));
            const nlohmann::json model = display.is_object() ? source.ModelRow(display.value("ModelID", 0u)) : nlohmann::json();
            const std::string modelName = model.is_object() ? S(model, "ModelName") : std::string();
            const std::string m2 = M2Name(modelName);
            if (m2.empty() || modelRename.count(Lower(modelName))) continue;
            const auto bytes = sourceFiles->Read(m2);
            if (!bytes) { ++missing; continue; }
            std::vector<std::string> group{ m2 }, textures;
            for (const std::string& ref : AssetReferences(*sourceFiles, m2, *bytes))
                (Lower(ref).ends_with(".blp") ? textures : group).push_back(ref);
            std::vector<State> groupStates;
            bool clash = false;
            for (const std::string& f : group) { groupStates.push_back(state(f)); clash = clash || groupStates.back() == State::Clash; }
            nlohmann::json textureRenames = nlohmann::json::object();
            for (const std::string& t : textures)
            {
                const State s = state(t);
                if (s == State::Clash) { textureRenames[t] = renameOf(t); add(renameOf(t), t); }
                else if (s == State::Copy) add(t, t);
                else if (s == State::Missing) ++missing;
            }
            clash = clash || !textureRenames.empty();
            for (size_t i = 0; i < group.size(); ++i)
            {
                if (groupStates[i] == State::Missing) { missing += i == 0; continue; }   // a model has up to 4 skins: only the model counts
                if (clash) add(renameOf(group[i]), group[i], i == 0 ? textureRenames : nlohmann::json());
                else if (groupStates[i] == State::Copy) add(group[i], group[i]);
            }
            modelRename[Lower(modelName)] = clash ? renameOf(modelName) : modelName;
        }
        // CharSections textures: a clashing one is renamed in every row naming it.
        std::map<std::string, std::string> textureName;   // lower-case -> name in the project
        for (nlohmann::json& row : p["CharSections"])
            for (int i = 0; i < 3; ++i)
            {
                const std::string column = "TextureName[" + std::to_string(i) + "]", t = S(row, column);
                if (t.empty()) continue;
                auto it = textureName.find(Lower(t));
                if (it == textureName.end())
                {
                    const State s = state(t);
                    const std::string name = s == State::Clash ? renameOf(t) : t;
                    if (s == State::Clash || s == State::Copy) add(name, t);
                    else if (s == State::Missing) ++missing;
                    it = textureName.emplace(Lower(t), name).first;
                }
                row[column] = it->second;
            }
    }

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
        nlohmann::json model = source.ModelRow(display.value("ModelID", 0u));
        if (!model.is_object()) { error = "the source has no CreatureModelData row for display " + std::to_string(from); return {}; }
        if (auto r = modelRename.find(Lower(S(model, "ModelName"))); r != modelRename.end()) model["ModelName"] = r->second;   // renamed: a row of its own
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
            FillPlayerModelRow(row, project);   // 1.12 rows lack the effect scales (0: buffs drawn at no size)
            rows.push_back(models.MakeChange(modelId, nullptr, row, "import race"));
            added["CreatureModelData"].push_back(modelId);
        }
        // A player's display: under 65536 (AzerothCore keeps it in 16 bits; 90000 shows as display 24464).
        const uint32_t displayId = next("CreatureDisplayInfo", "race.display.id", [&](uint32_t id) { return !displays.Row(id).is_null(); });
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
    if (sourceFiles && projectFiles)
    {
        p["files"] = files;
        p["missingFiles"] = missing;
    }
    std::vector<Change> out{ races.MakeChange(target, p, "import race") };
    out.insert(out.end(), rows.begin(), rows.end());
    return out;
}

std::vector<uint8_t> RenameM2Textures(std::vector<uint8_t> m2, const std::map<std::string, std::string>& names)
{
    std::map<std::string, std::string> lower;
    for (const auto& [from, to] : names) lower[Lower(from)] = to;
    uint32_t count = 0, offset = 0;   // MD20 header: textures M2Array at 0x50, 16-byte entries {type, flags, name length, name offset}
    if (m2.size() < 0x58) return m2;
    std::memcpy(&count, m2.data() + 0x50, 4);
    std::memcpy(&offset, m2.data() + 0x54, 4);
    for (uint32_t i = 0; i < count && size_t(offset) + (i + 1) * 16 <= m2.size(); ++i)
    {
        uint32_t len = 0, at = 0;
        std::memcpy(&len, m2.data() + offset + i * 16 + 8, 4);
        std::memcpy(&at, m2.data() + offset + i * 16 + 12, 4);
        if (!len || size_t(at) + len > m2.size()) continue;
        const std::string name(reinterpret_cast<const char*>(m2.data() + at), strnlen(reinterpret_cast<const char*>(m2.data() + at), len));
        const auto it = lower.find(Lower(name));
        if (it == lower.end()) continue;
        const uint32_t newAt = uint32_t(m2.size()), newLen = uint32_t(it->second.size() + 1);
        m2.insert(m2.end(), it->second.begin(), it->second.end());
        m2.push_back(0);
        std::memcpy(m2.data() + offset + i * 16 + 8, &newLen, 4);
        std::memcpy(m2.data() + offset + i * 16 + 12, &newAt, 4);
    }
    return m2;
}

namespace
{
    std::string Upper(std::string s)
    {
        for (char& c : s) c = char(std::toupper((unsigned char)c));
        return s;
    }

    std::string LuaString(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '\\' || c == '"') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if (c != '\r') out += c;
        }
        return out + "\"";
    }

    // Appended to the client's CharacterCreate.lua (Lua 5.1, glue), after EDITOR_CREATOR_RACES. Races are found by the
    // name the client shows (GetAvailableRaces, GetNameForRace): file strings can be shared (a High Elf with Blood Elf's)
    // and are not always what the client reports for a new race. Only what a file string lacks is filled in globally.
    constexpr const char* kCreatorLua = R"lua(do
	local races, byName, byFile = EDITOR_CREATOR_RACES, {}, {};
	local round = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-RacesRound";
	if ( MAX_RACES < EDITOR_CREATOR_PLAYABLE ) then
		MAX_RACES = EDITOR_CREATOR_PLAYABLE;
	end
	for _, r in ipairs(races) do
		for _, name in ipairs(r.names) do
			byName[name] = r;
		end
		if ( r.own ) then
			byFile[r.file] = r;
		end
		-- The client's own code looks these up by file string: they must be there.
		for _, sex in ipairs({ "_MALE", "_FEMALE" }) do
			if ( not RACE_ICON_TCOORDS[r.file..sex] ) then
				RACE_ICON_TCOORDS[r.file..sex] = RACE_ICON_TCOORDS[r.donor..sex] or RACE_ICON_TCOORDS["HUMAN"..sex];
			end
		end
		if ( not _G["RACE_INFO_"..r.file] ) then
			_G["RACE_INFO_"..r.file] = _G["RACE_INFO_"..r.donor] or "";
			_G["RACE_INFO_"..r.file.."_FEMALE"] = _G["RACE_INFO_"..r.donor.."_FEMALE"];
			local i = 1;
			while ( _G["ABILITY_INFO_"..r.donor..i] and not _G["ABILITY_INFO_"..r.file..i] ) do
				_G["ABILITY_INFO_"..r.file..i] = _G["ABILITY_INFO_"..r.donor..i];
				i = i + 1;
			end
		end
		if ( not _G[r.file.."_DISABLED"] ) then
			_G[r.file.."_DISABLED"] = _G[r.donor.."_DISABLED"] or "";
		end
	end

	-- Scenes: the race's own or the one it borrows, with a sound track (SetBackgroundModel needs one for every scene).
	local current;   -- the project race the creator shows, nil for others
	local function Scene(name, r)
		if ( r and strupper(name or "") ~= "DEATHKNIGHT" ) then
			name = r.scene;
		end
		if ( name and GlueAmbienceTracks and not GlueAmbienceTracks[strupper(name)] ) then
			GlueAmbienceTracks[strupper(name)] = (r and GlueAmbienceTracks[r.donor]) or GlueAmbienceTracks["CHARACTERSELECT"];
		end
		return name;
	end
	-- GlueParent's SetBackgroundModel, not the client's Get*BackgroundModel: the client registers its own functions
	-- again when a screen opens, which would drop a replacement of them.
	local setBackground = SetBackgroundModel;
	function SetBackgroundModel(model, name)
		local r;
		if ( model == CharacterCreate ) then
			r = current;
		else
			r = name and byFile[strupper(name)];
		end
		return setBackground(model, Scene(name, r));
	end

	-- The XML has ten race buttons: make the rest once the creator's frames exist.
	local onLoad = CharacterCreate_OnLoad;
	function CharacterCreate_OnLoad(self)
		local parent = CharacterCreateRaceButton1:GetParent();
		for i = 1, MAX_RACES do
			if ( not _G["CharacterCreateRaceButton"..i] ) then
				local button = CreateFrame("CheckButton", "CharacterCreateRaceButton"..i, parent, "CharacterCreateRaceButtonTemplate");
				button:SetID(i);
				button:Hide();
			end
		end
		onLoad(self);
	end

	-- A project race's own icon on its button, and every button in its faction's column. Each button hangs from a slot
	-- frame of scale 1 at its place, so shrinking a button never moves it.
	local enumerate = CharacterCreateEnumerateRaces;
	function CharacterCreateEnumerateRaces(...)
		enumerate(...);
		if ( CharacterCreate.numRaces > MAX_RACES ) then
			return;
		end
		local columns = { Alliance = {}, Horde = {} };
		for i = 1, CharacterCreate.numRaces do
			local button = _G["CharacterCreateRaceButton"..i];
			local r = byName[select(i * 3 - 2, ...)];
			if ( r and r.icon ) then
				for _, part in ipairs({ "NormalTexture", "PushedTexture" }) do
					local texture = _G[button:GetName()..part];
					texture:SetTexture(r.icon);
					texture:SetTexCoord(0, 1, 0, 1);
				end
			end
			local _, faction = GetFactionForRace(i);
			table.insert(columns[faction] or columns.Horde, button);
		end
		for faction, buttons in pairs(columns) do
			-- ponytail: the banners fit five rows at the client's spacing; more close up to the same height and shrink
			-- once the gap is gone (about ten a side stay usable).
			local step = #buttons > 5 and 236 / (#buttons - 1) or 59;
			local scale = math.min(1, (step - 4) / 38);
			for k, button in ipairs(buttons) do
				local slot = button.editorSlot;
				if ( not slot ) then
					slot = CreateFrame("Frame", nil, button:GetParent());
					slot:SetWidth(1);
					slot:SetHeight(1);
					button.editorSlot = slot;
				end
				slot:ClearAllPoints();
				slot:SetPoint("TOP", slot:GetParent(), "TOP", faction == "Alliance" and -50 or 50, -61 - (k - 1) * step);
				button:SetScale(scale);
				button:ClearAllPoints();
				button:SetPoint("TOP", slot, "TOP", 0, 0);
			end
		end
	end

	-- The panel: a project race's own icon and texts over what the client's code put there by file string.
	local function Panel()
		local r = current;
		if ( r and r.round ) then
			CharacterCreateRaceIcon:SetTexture(r.round);
			CharacterCreateRaceIcon:SetTexCoord(0, 1, 0, 1);
			CharacterCreateRaceIcon.editorIcon = true;
		elseif ( CharacterCreateRaceIcon.editorIcon ) then
			local _, file = GetNameForRace();
			local coords = RACE_ICON_TCOORDS[strupper(file)..(GetSelectedSex() == SEX_MALE and "_MALE" or "_FEMALE")];
			CharacterCreateRaceIcon:SetTexture(round);
			if ( coords ) then
				CharacterCreateRaceIcon:SetTexCoord(coords[1], coords[2], coords[3], coords[4]);
			end
			CharacterCreateRaceIcon.editorIcon = nil;
		end
		if ( r and r.texts ) then
			CharacterCreateRaceText:SetText(r.info.."|n|n");
			CharacterCreateRaceAbilityText:SetText(#r.abilities > 0 and table.concat(r.abilities, "\n\n").."\n\n" or "");
		end
	end
	local setRace, setGender = SetCharacterRace, SetCharacterGender;
	function SetCharacterRace(...)
		current = byName[GetNameForRace()];
		setRace(...);
		Panel();
	end
	function SetCharacterGender(...)
		setGender(...);
		Panel();
	end
end
)lua";
}

std::string CreatorBackgroundPath(const std::string& name)
{
    return "Interface\\Glues\\Models\\UI_" + name + "\\UI_" + name + ".m2";
}

uint32_t CreatorDonor(const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages, uint32_t race)
{
    const auto it = packages.find(race);
    if (it != packages.end())
        if (const uint32_t donor = it->second.value("creator", nlohmann::json::object()).value("donor", 0u); donor && donor != race && project.Find(donor))
            return donor;
    const RaceCatalog::Race* self = project.Find(race);
    for (const RaceCatalog::Race& r : project.Races())
        if (r.id != race && !(r.flags & 1) && !packages.count(r.id) && (!self || r.alliance == self->alliance)) return r.id;
    return 0;
}

std::string CreatorScript(const std::string& clientLua, const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages,
                          const std::function<bool(const std::string&)>& has)
{
    size_t playable = 0;
    for (const RaceCatalog::Race& r : project.Races()) playable += !(r.flags & 1);
    std::string entries;
    for (const auto& [id, p] : packages)
    {
        const RaceCatalog::Race* race = project.Find(id);
        if (!race || (race->flags & 1) || race->fileString.empty()) continue;
        const nlohmann::json c = p.value("creator", nlohmann::json::object());
        const RaceCatalog::Race* donor = project.Find(CreatorDonor(project, packages, id));
        // Every name the client may show it by (GetNameForRace gives the one of the chosen sex).
        std::vector<std::string> names;
        for (const std::string& n : { race->name, race->names[0], race->names[1] })
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        std::string entry = "\t{ names = {";
        for (const std::string& n : names) entry += " " + LuaString(n) + ",";
        entry += " }, file = " + LuaString(Upper(race->fileString)) + ", donor = " + LuaString(donor ? Upper(donor->fileString) : "HUMAN");
        // A file string no other playable race has also finds it on character select (whose scene is by file string).
        bool own = true;
        for (const RaceCatalog::Race& other : project.Races())
            own = own && (other.id == id || (other.flags & 1) || Lower(other.fileString) != Lower(race->fileString));
        if (own) entry += ", own = true";
        const std::string icon = c.value("icon", std::string());
        if (!icon.empty() && has(icon + ".blp") && has(icon + "-Round.blp"))
            entry += ", icon = " + LuaString(icon) + ", round = " + LuaString(icon + "-Round");
        // The scene chosen, else its own, else the donor's.
        std::string scene = c.value("background", std::string());
        if (scene.empty() || !has(CreatorBackgroundPath(scene)))
            scene = has(CreatorBackgroundPath(race->fileString)) ? race->fileString : donor ? donor->fileString : "Human";
        entry += ", scene = " + LuaString(scene);
        const std::string description = c.value("description", std::string());
        std::vector<std::string> abilities;
        for (const nlohmann::json& a : ChangeStore::List(c, "abilities"))
            if (a.is_string() && !a.get<std::string>().empty()) abilities.push_back(a.get<std::string>());
        if (!description.empty() || !abilities.empty())
        {
            entry += ",\n\t\ttexts = true, info = " + LuaString(description) + ",\n\t\tabilities = {";
            for (const std::string& a : abilities) entry += "\n\t\t\t" + LuaString(a) + ",";
            entry += " }";
        }
        entries += entry + " },\n";
    }
    if (entries.empty()) return {};
    std::string lua = clientLua;
    if (const size_t cut = lua.find("\n-- wow-world-editor"); cut != std::string::npos) lua.resize(cut);
    while (!lua.empty() && std::isspace((unsigned char)lua.back())) lua.pop_back();
    lua += '\n';
    return lua + "\n-- wow-world-editor: the project's races in the character creator. Written by export: change a race in the editor, not here.\n"
               "EDITOR_CREATOR_PLAYABLE = " + std::to_string(playable) + ";\nEDITOR_CREATOR_RACES = {\n" + entries + "};\n" + kCreatorLua;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> CreatorIcons(uint32_t width, uint32_t height, const uint8_t* rgba)
{
    // The middle square of the picture, sampled down (box filter) to 64 x 64; the round twin fades out past the circle.
    constexpr uint32_t kSize = 64;
    const uint32_t side = std::min(width, height), x0 = (width - side) / 2, y0 = (height - side) / 2;
    std::vector<uint8_t> square(kSize * kSize * 4), circle;
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const uint32_t ax = x0 + x * side / kSize, bx = std::max(ax + 1, x0 + (x + 1) * side / kSize);
            const uint32_t ay = y0 + y * side / kSize, by = std::max(ay + 1, y0 + (y + 1) * side / kSize);
            uint32_t sum[4] = {}, n = 0;
            for (uint32_t sy = ay; sy < by; ++sy)
                for (uint32_t sx = ax; sx < bx; ++sx, ++n)
                    for (int k = 0; k < 4; ++k) sum[k] += rgba[(size_t(sy) * width + sx) * 4 + k];
            for (int k = 0; k < 4; ++k) square[(y * kSize + x) * 4 + k] = uint8_t(sum[k] / n);
        }
    circle = square;
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const float dx = x + 0.5f - kSize / 2.0f, dy = y + 0.5f - kSize / 2.0f;
            const float inside = std::clamp(kSize / 2.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            uint8_t& a = circle[(y * kSize + x) * 4 + 3];
            a = uint8_t(a * inside);
        }
    return { WriteBlp(kSize, kSize, square.data()), WriteBlp(kSize, kSize, circle.data()) };
}

std::vector<uint8_t> FixM2AttachmentLookup(std::vector<uint8_t> m2)
{
    // MD20 v264: attachments (40 bytes each, id first) at 0xF0, the id -> attachment lookup (int16) at 0xF8. A model
    // converted from 1.12 can keep a lookup shorter than its ids (Turtle's goblin stops at 35 though it has 36-49), and the
    // client finds an attachment only through it: spell effects at those points never show. Rebuilt to every id (at
    // least the 50 stock characters have), appended to the file.
    if (m2.size() < 0x100 || std::memcmp(m2.data(), "MD20", 4) != 0) return m2;
    auto u32 = [&](size_t at) { uint32_t v = 0; std::memcpy(&v, m2.data() + at, 4); return v; };
    const uint32_t count = u32(0xF0), at = u32(0xF4), lookupCount = u32(0xF8), lookupAt = u32(0xFC);
    if (!count || size_t(at) + size_t(count) * 40 > m2.size()) return m2;
    uint32_t top = 49;
    for (uint32_t i = 0; i < count; ++i) top = std::max(top, u32(at + i * 40));
    if (top > 255) return m2;   // not attachment ids as characters have them: leave it
    std::vector<int16_t> lookup(top + 1, -1);
    for (uint32_t i = 0; i < count; ++i)
        if (int16_t& slot = lookup[u32(at + i * 40)]; slot < 0) slot = int16_t(i);
    if (lookupCount == lookup.size() && size_t(lookupAt) + lookup.size() * 2 <= m2.size() &&
        std::memcmp(m2.data() + lookupAt, lookup.data(), lookup.size() * 2) == 0)
        return m2;
    while (m2.size() % 16) m2.push_back(0);
    const uint32_t newAt = uint32_t(m2.size()), newCount = uint32_t(lookup.size());
    m2.insert(m2.end(), reinterpret_cast<const uint8_t*>(lookup.data()), reinterpret_cast<const uint8_t*>(lookup.data()) + lookup.size() * 2);
    std::memcpy(m2.data() + 0xF8, &newCount, 4);
    std::memcpy(m2.data() + 0xFC, &newAt, 4);
    return m2;
}

bool ExportRaces(const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages,
                 const std::function<const MpqChain*(const std::string& client)>& source, const std::vector<std::filesystem::path>& dbcDirs,
                 const std::filesystem::path& clientOut, std::vector<std::string>& notes, std::string& error,
                 const MpqChain* client, const std::filesystem::path& assets, const std::map<uint32_t, std::set<uint32_t>>& raceSkills)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (packages.empty())   // nothing of ours: no race tables left over from an earlier export either
    {
        for (const fs::path& dir : dbcDirs)
            for (const char* table : { "ChrRaces", "CharSections", "CharHairGeosets", "CharacterFacialHairStyles", "CharBaseInfo", "CharStartOutfit",
                                       "SkillRaceClassInfo", "SkillLineAbility", "Faction" })
                fs::remove(dir / (std::string(table) + ".dbc"), ec);
        return true;
    }
    const Layouts& l = Wrath();
    const std::map<std::string, const DbcLayout*> layouts = { { "ChrRaces", &l.races }, { "CharSections", &l.sections }, { "CharHairGeosets", &l.hair },
                                                              { "CharacterFacialHairStyles", &l.facial }, { "CharBaseInfo", &l.baseInfo },
                                                              { "CharStartOutfit", &l.outfits } };
    // An underwear row players may choose with no texture (1.12 races without underwear art, Turtle's female goblin)
    // gets a transparent one: the 3.3.5 creator cannot build the body texture around a missing file and shows the
    // model's own skin instead. Stock 3.3.5 never ships such a row.
    const std::string blank = "Character\\WowWorldEditor\\BlankUnderwear.blp";
    bool blankUsed = false;
    for (const char* table : RaceCatalog::kTables)
    {
        std::vector<nlohmann::json> rows = project.TableRows(table);
        if (std::string(table) == "CharSections")
            for (nlohmann::json& r : rows)
            {
                if ((U(r, "Flags") & 1) && U(r, "BaseSection") == 4 && S(r, "TextureName[0]").empty() && S(r, "TextureName[1]").empty())
                {
                    r["TextureName[0]"] = blank;
                    blankUsed = true;
                }
                // A player row of a look other than a face with only 0x1: stock gives every such row 0x10 too (all but
                // Death Knight-only ones), and the client wants it when it rebuilds a character after a transform.
                if (U(r, "Flags") == 1 && U(r, "BaseSection") != 1 && packages.count(U(r, "RaceID"))) r["Flags"] = 0x11u;
            }
        if (std::string(table) == "CharSections")   // the client's cache takes each race / sex / section / variation as one run
            std::stable_sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                auto key = [](const nlohmann::json& r) { return std::tuple(U(r, "RaceID"), U(r, "SexID"), U(r, "BaseSection"), U(r, "VariationIndex"), U(r, "ColorIndex")); };
                return key(a) < key(b);
            });
        const std::vector<uint8_t> bytes = WriteDbcRows(rows, *layouts.at(table));
        for (const fs::path& dir : dbcDirs)
        {
            fs::create_directories(dir, ec);
            std::ofstream f(dir / (std::string(table) + ".dbc"), std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            if (!f) { error = "Cannot write " + (dir / (std::string(table) + ".dbc")).string(); return false; }
        }
    }
    // The textures the client composites into a character's body (every CharSections texture but a hair row's first,
    // which is drawn on the hair itself): it takes only palettized BLPs, so others are converted as they are copied.
    std::set<std::string> composited;
    for (const auto& [race, p] : packages)
        for (const nlohmann::json& s : ChangeStore::List(p, "CharSections"))
            for (int i = U(s, "BaseSection") == 3 ? 1 : 0; i < 3; ++i)
                if (const std::string t = S(s, "TextureName[" + std::to_string(i) + "]"); !t.empty()) composited.insert(Lower(t));
    if (blankUsed)
    {
        const std::vector<uint8_t> pixels(128 * 64 * 4, 0);   // pelvis size (128 x 64), fully transparent
        const std::vector<uint8_t> bytes = WriteIndexedBlp(128, 64, pixels.data());
        const fs::path target = clientOut / "Character" / "WowWorldEditor" / "BlankUnderwear.blp";
        fs::create_directories(target.parent_path(), ec);
        std::ofstream(target, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    // Each race's files, from the client it came from.
    for (const auto& [race, p] : packages)
    {
        const std::string client = p.value("source", nlohmann::json::object()).value("client", std::string());
        const nlohmann::json& files = ChangeStore::List(p, "files");
        if (!client.empty() && !p.contains("files"))
            notes.push_back("race " + std::to_string(race) + ": imported before its files were listed: remove it and import it again so its models and textures come along");
        if (files.empty()) continue;
        const MpqChain* from = source(client);
        if (!from) { notes.push_back("race " + std::to_string(race) + ": its client \"" + client + "\" is not a source now: its " + std::to_string(files.size()) + " file(s) were not copied"); continue; }
        size_t lost = 0, indexed = 0;
        for (const nlohmann::json& f : files)
        {
            auto bytes = from->Read(S(f, "from"));
            if (!bytes) { ++lost; continue; }
            if (f.contains("textures"))
            {
                std::map<std::string, std::string> names;
                for (const auto& [a, b] : f["textures"].items()) names[a] = b.get<std::string>();
                *bytes = RenameM2Textures(std::move(*bytes), names);
            }
            if (const std::string path = Lower(S(f, "path")); path.size() > 3 && path.compare(path.size() - 3, 3, ".m2") == 0)
                *bytes = FixM2AttachmentLookup(std::move(*bytes));
            if (composited.count(Lower(S(f, "path"))) && !IsIndexedBlp(*bytes))
                if (const auto image = ParseBlp(*bytes))
                    if (const std::vector<uint8_t> rgba = BlpPixels(*image); !rgba.empty())
                    {
                        *bytes = WriteIndexedBlp(image->width, image->height, rgba.data());
                        ++indexed;
                    }
            std::string rel = S(f, "path");
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const fs::path target = clientOut / fs::path(rel);
            fs::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
            if (!out) { error = "Cannot write " + target.string(); return false; }
        }
        if (indexed) notes.push_back("race " + std::to_string(race) + ": " + std::to_string(indexed) + " body texture(s) made palettized (the client composites characters only from those)");
        if (lost) notes.push_back("race " + std::to_string(race) + ": " + std::to_string(lost) + " file(s) could not be read from " + client);
    }
    if (!client) return true;

    // Race masks: what a character may have is gated by its race's bit in these tables, on the server and the client
    // (AzerothCore drops every playercreateinfo_skills row SkillRaceClassInfo does not allow, learns skill spells by
    // SkillLineAbility, and sets starting reputations by Faction). A race the project imported takes its donor's place:
    // wherever a mask has the donor's bit it gets its own too. Masks of 0 or all bits already include it.
    // (table, skill column or -1, mask columns)
    static const std::tuple<const char*, int, std::vector<uint32_t>> kMasks[] = { { "SkillRaceClassInfo", 1, { 2 } }, { "SkillLineAbility", 1, { 3 } },
                                                                                  { "Faction", -1, { 2, 3, 4, 5 } } };
    const std::vector<std::pair<uint32_t, uint32_t>> bits = ImportedRaceMasks(project, packages);
    for (const auto& [donor, own] : bits)
        notes.push_back("race mask " + std::to_string(own) + ": skills, skill spells and starting reputations as mask " + std::to_string(donor) +
                        " has them (SkillRaceClassInfo, SkillLineAbility, Faction; quests, items and zone spells in out/server/race_masks.sql)");
    // A skill a race is given (the Server tab) is allowed it too, with the skill's spells; a mask of 0 takes every race already.
    std::map<uint32_t, uint32_t> skillBits;   // skill -> race bits
    for (const auto& [race, skills] : raceSkills)
        if (packages.count(race))
            for (uint32_t skill : skills) skillBits[skill] |= RaceBit(race);
    for (const auto& [table, skillField, fields] : kMasks)
    {
        const std::string file = std::string(table) + ".dbc";
        auto bytes = bits.empty() && (skillField < 0 || skillBits.empty()) ? std::nullopt : client->Read("DBFilesClient\\" + file);
        if (!bytes || bytes->size() < 20)
        {
            for (const fs::path& dir : dbcDirs) fs::remove(dir / file, ec);   // none to change: the client's own table stands
            continue;
        }
        uint32_t rows = 0, recordSize = 0;
        std::memcpy(&rows, bytes->data() + 4, 4);
        std::memcpy(&recordSize, bytes->data() + 12, 4);
        for (uint32_t i = 0; i < rows && 20 + size_t(i + 1) * recordSize <= bytes->size(); ++i)
        {
            uint8_t* record = bytes->data() + 20 + size_t(i) * recordSize;
            uint32_t skill = 0;
            if (skillField >= 0) std::memcpy(&skill, record + skillField * 4, 4);
            const auto picked = skillBits.find(skill);
            for (uint32_t f : fields)
            {
                uint32_t mask = 0;
                std::memcpy(&mask, record + f * 4, 4);
                for (const auto& [donor, own] : bits)
                    if (mask & donor) mask |= own;
                if (mask && picked != skillBits.end()) mask |= picked->second;
                std::memcpy(record + f * 4, &mask, 4);
            }
        }
        for (const fs::path& dir : dbcDirs)
        {
            fs::create_directories(dir, ec);
            std::ofstream out(dir / file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
            if (!out) { error = "Cannot write " + (dir / file).string(); return false; }
        }
    }

    // Random names (NameGen.dbc; the client makes them, the server is not asked): a race with none takes the names its
    // source client has for it, else its donor's. The layout is the same from 2.0 on (Turtle's too): ID, Name, RaceID, Sex.
    {
        const DbcLayout layout = LayoutBuilder{}.Col("ID", 0).Col("Name", 1, 's').Col("RaceID", 2).Col("Sex", 3).Done(16, 4);
        const Dbc names = ReadDbc(*client, "NameGen");
        if (names.RecordSize() == layout.recordSize)
        {
            std::vector<nlohmann::json> rows = Rows(names, layout);
            std::set<uint32_t> named;
            uint32_t last = 0;
            for (const nlohmann::json& r : rows) { named.insert(U(r, "RaceID")); last = std::max(last, U(r, "ID")); }
            bool added = false;
            for (const auto& [race, p] : packages)
            {
                const RaceCatalog::Race* r = project.Find(race);
                if (!r || (r->flags & 1) || named.count(race)) continue;
                std::vector<nlohmann::json> take;
                const nlohmann::json from = p.value("source", nlohmann::json::object());
                const std::string clientName = from.value("client", std::string());
                if (const MpqChain* chain = clientName.empty() ? nullptr : source(clientName))
                    if (const Dbc theirs = ReadDbc(*chain, "NameGen"); theirs.RecordSize() == layout.recordSize)
                        for (const nlohmann::json& n : Rows(theirs, layout))
                            if (U(n, "RaceID") == from.value("race", 0u)) take.push_back(n);
                const bool own = !take.empty();
                const uint32_t donor = CreatorDonor(project, packages, race);
                if (take.empty())
                    for (const nlohmann::json& n : rows)
                        if (U(n, "RaceID") == donor) take.push_back(n);
                for (nlohmann::json n : take)
                {
                    n["ID"] = ++last;
                    n["RaceID"] = race;
                    rows.push_back(std::move(n));
                    added = true;
                }
                notes.push_back("race " + std::to_string(race) + ": " + std::to_string(take.size()) + " random names, " +
                                (own ? "its own from " + clientName : "race " + std::to_string(donor) + "'s") + " (NameGen)");
            }
            if (added)
            {
                const std::vector<uint8_t> bytes = WriteDbcRows(rows, layout);
                const fs::path target = clientOut / "DBFilesClient" / "NameGen.dbc";
                fs::create_directories(target.parent_path(), ec);
                std::ofstream out(target, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                if (!out) { error = "Cannot write " + target.string(); return false; }
            }
        }
    }

    // The character creator: a race's own creator scene from its client when players lack it, then the script.
    auto write = [&](const std::string& path, const std::vector<uint8_t>& bytes) {
        std::string rel = path;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const fs::path target = clientOut / fs::path(rel);
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) error = "Cannot write " + target.string();
        return bool(out);
    };
    auto has = [&](const std::string& path) {
        std::string rel = path;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        return client->HasInstalled(path) || fs::exists(clientOut / fs::path(rel), ec) || (!assets.empty() && fs::exists(assets / fs::path(rel), ec));
    };
    for (const auto& [race, p] : packages)
    {
        const RaceCatalog::Race* r = project.Find(race);
        const std::string from = p.value("source", nlohmann::json::object()).value("client", std::string());
        const MpqChain* chain = from.empty() ? nullptr : source(from);
        if (!r || (r->flags & 1) || r->fileString.empty() || !chain || has(CreatorBackgroundPath(r->fileString))) continue;
        if (!chain->Read(CreatorBackgroundPath(r->fileString))) continue;   // its client has none either: a donor's then
        std::vector<std::string> todo{ CreatorBackgroundPath(r->fileString) };
        std::set<std::string> seen;
        size_t copied = 0;
        while (!todo.empty())
        {
            const std::string name = todo.back();
            todo.pop_back();
            if (!seen.insert(Lower(name)).second || client->HasInstalled(name)) continue;
            const auto bytes = chain->Read(name);
            if (!bytes) continue;
            if (!write(name, *bytes)) return false;
            ++copied;
            for (std::string& ref : AssetReferences(*chain, name, *bytes)) todo.push_back(std::move(ref));
        }
        notes.push_back("race " + std::to_string(race) + ": its creator scene UI_" + r->fileString + " (" + std::to_string(copied) + " file(s)) came from " + from);
    }
    const auto lua = client->Read("Interface\\GlueXML\\CharacterCreate.lua");
    if (!lua) { notes.push_back("the client has no Interface\\GlueXML\\CharacterCreate.lua: the creator was left as it is"); return true; }
    const std::string script = CreatorScript(std::string(lua->begin(), lua->end()), project, packages, has);
    return script.empty() || write("Interface\\GlueXML\\CharacterCreate.lua", std::vector<uint8_t>(script.begin(), script.end()));
}

std::vector<SkillInfo> ReadSkills(const MpqChain& mpq)
{
    // SkillLine 3.3.5: ID, CategoryID, SkillCostsID, DisplayName_lang (enUS at 3); SkillRaceClassInfo: ID, SkillID, ...
    const Dbc lines = ReadDbc(mpq, "SkillLine"), gates = ReadDbc(mpq, "SkillRaceClassInfo");
    std::set<uint32_t> gated;
    for (uint32_t i = 0; i < gates.Rows(); ++i) gated.insert(gates.U32(i, 1));
    std::vector<SkillInfo> out;
    for (uint32_t i = 0; i < lines.Rows(); ++i)
        if (std::string name = lines.Str(i, 3); !name.empty())
            out.push_back({ lines.U32(i, 0), lines.U32(i, 1), std::move(name), gated.count(lines.U32(i, 0)) != 0 });
    std::sort(out.begin(), out.end(), [](const SkillInfo& a, const SkillInfo& b) { return std::tie(a.category, a.name) < std::tie(b.category, b.name); });
    return out;
}

const char* SkillCategoryName(uint32_t category)
{
    switch (category)
    {
    case 5: return "Attributes";
    case 6: return "Weapons";
    case 7: return "Class";
    case 8: return "Armour";
    case 9: return "Secondary";
    case 10: return "Languages";
    case 11: return "Professions";
    case 12: return "Hidden";
    default: return "Other";
    }
}

std::vector<std::pair<uint32_t, uint32_t>> ImportedRaceMasks(const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages)
{
    std::vector<std::pair<uint32_t, uint32_t>> bits;
    for (const auto& [race, p] : packages)
    {
        const RaceCatalog::Race* r = project.Find(race);
        const uint32_t donor = CreatorDonor(project, packages, race);
        if (p.contains("source") && r && !(r->flags & 1) && RaceBit(race) && RaceBit(donor)) bits.push_back({ RaceBit(donor), RaceBit(race) });
    }
    return bits;
}

std::vector<std::string> RaceMaskSql(const std::vector<std::pair<uint32_t, uint32_t>>& bits, bool revert)
{
    // (table, mask column, extra condition). UPDATE IGNORE: the mask is part of spell_area's and conditions' keys.
    // ponytail: revert takes the race from every row with the donor's bit, also one that had both before (stock never
    // gives an unplayable race's bit alone); list the rows changed if that ever matters.
    static const std::tuple<const char*, const char*, const char*> kColumns[] = {
        { "quest_template", "AllowableRaces", "" }, { "item_template", "AllowableRace", "" }, { "spell_area", "racemask", "" },
        { "conditions", "ConditionValue1", " AND ConditionTypeOrReference = 16" } };
    std::vector<std::string> out;
    for (const auto& [donor, own] : bits)
        for (const auto& [table, column, also] : kColumns)
        {
            const std::string c = std::string("`") + column + "`", d = std::to_string(donor), o = std::to_string(own);
            out.push_back(revert ? "UPDATE IGNORE " + std::string(table) + " SET " + c + " = " + c + " & ~" + o + " WHERE (" + c + " & " + d + ") != 0" + also
                                 : "UPDATE IGNORE " + std::string(table) + " SET " + c + " = " + c + " | " + o + " WHERE (" + c + " & " + d + ") != 0 AND (" +
                                       c + " & " + o + ") = 0" + also);
        }
    return out;
}

std::vector<Change> UseSourceModelPaths(uint32_t race, const RaceAdapter& races, const DbcTable& models)
{
    const nlohmann::json* package = races.Package(race);
    if (!package || !package->contains("files")) return {};
    nlohmann::json p = *package;
    const std::string prefix = "character\\race" + std::to_string(race) + "\\";
    auto isModel = [](const std::string& lower) {
        for (const char* ext : { ".m2", ".skin", ".anim", ".mdx" })
            if (lower.size() > std::strlen(ext) && lower.compare(lower.size() - std::strlen(ext), std::strlen(ext), ext) == 0) return true;
        return false;
    };
    bool moved = false;
    for (nlohmann::json& f : p["files"])
        if (const std::string path = Lower(S(f, "path")); path.rfind(prefix, 0) == 0 && isModel(path)) { f["path"] = S(f, "from"); moved = true; }
    if (!moved) return {};
    std::vector<Change> out;
    const nlohmann::json added = p.value("added", nlohmann::json::object());
    for (const nlohmann::json& id : ChangeStore::List(added, "CreatureModelData"))
        if (const nlohmann::json* row = models.Edited(id.get<uint32_t>()); row && row->is_object())
            if (const std::string name = S(*row, "ModelName"); Lower(name).rfind(prefix, 0) == 0)
            {
                nlohmann::json now = *row;
                now["ModelName"] = "Character\\" + name.substr(prefix.size());
                out.push_back(models.MakeChange(id.get<uint32_t>(), *row, now, "source model paths"));
            }
    out.push_back(races.MakeChange(race, p, "source model paths"));
    return out;
}

bool FillPlayerModelRow(nlohmann::json& row, const RaceCatalog& client)
{
    bool changed = false;
    if (!(U(row, "Flags") & 0x800)) { row["Flags"] = U(row, "Flags") | 0x800u; changed = true; }
    auto f = [&](const char* key) { const auto it = row.find(key); return it != row.end() && it->is_number() ? it->get<double>() : 0.0; };
    if (f("WorldEffectScale") != 0 || f("AttachedEffectScale") != 0) return changed;
    // The model's own name: Character\Race9\Goblin\Male\... was Character\Goblin\Male\...
    std::string name = S(row, "ModelName");
    if (Lower(name).rfind("character\\race", 0) == 0)
        if (const size_t slash = name.find('\\', 10); slash != std::string::npos) name = "Character" + name.substr(slash);
    static const char* const kColumns[] = { "MountHeight", "GeoBoxMinX", "GeoBoxMinY", "GeoBoxMinZ", "GeoBoxMaxX", "GeoBoxMaxY", "GeoBoxMaxZ",
                                            "WorldEffectScale", "AttachedEffectScale", "MissileCollisionRadius", "MissileCollisionPush", "MissileCollisionRaise" };
    const nlohmann::json same = client.ModelRow(client.ModelId(name));
    if (same.is_object() && same.value("AttachedEffectScale", 0.0) != 0)
        for (const char* c : kColumns) row[c] = same.value(c, 0.0);
    else
    {
        row["WorldEffectScale"] = 1.0;
        row["AttachedEffectScale"] = 1.0;
    }
    return true;
}

std::vector<Change> FixRaceModelRows(uint32_t race, const RaceAdapter& races, const DbcTable& models, const RaceCatalog& client)
{
    const nlohmann::json* p = races.Package(race);
    if (!p) return {};
    std::vector<Change> out;
    const nlohmann::json added = p->value("added", nlohmann::json::object());   // a local: a temporary dies before the loop
    for (const nlohmann::json& id : ChangeStore::List(added, "CreatureModelData"))
        if (const nlohmann::json* row = models.Edited(id.get<uint32_t>()); row && row->is_object())
            if (nlohmann::json now = *row; FillPlayerModelRow(now, client)) out.push_back(models.MakeChange(id.get<uint32_t>(), *row, now, "fill model rows"));
    return out;
}

std::vector<Change> RenumberRaceDisplays(uint32_t race, const RaceAdapter& races, const DbcTable& displays, const Project& ranges, std::string& error)
{
    const nlohmann::json* package = races.Package(race);
    if (!package) return {};
    nlohmann::json p = *package;
    std::vector<Change> out;
    std::map<uint32_t, uint32_t> moved;   // old display -> new
    std::set<uint32_t> taken;
    const Project::IdRange range = ranges.Range("race.display.id");
    for (const char* key : { "MaleDisplayID", "FemaleDisplayID" })
    {
        const uint32_t id = U(p["ChrRaces"], key);
        if (id < 65536) continue;
        if (!moved.count(id))
        {
            const nlohmann::json* row = displays.Edited(id);
            if (!row || row->is_null()) { error = "display " + std::to_string(id) + " is not one the project added"; return {}; }
            uint32_t to = 0;
            for (uint32_t i = range.first; i && i <= range.last && !to; ++i)
                if (displays.Row(i).is_null() && !taken.count(i)) to = i;
            if (!to) { error = "the race.display.id range is full"; return {}; }
            taken.insert(to);
            nlohmann::json now = *row;
            now["ID"] = to;
            out.push_back(displays.MakeChange(to, nullptr, now, "renumber race displays"));
            out.push_back(displays.MakeChange(id, *row, nullptr, "renumber race displays"));
            moved[id] = to;
        }
        p["ChrRaces"][key] = moved[id];
    }
    if (moved.empty()) return {};
    for (nlohmann::json& id : p["added"]["CreatureDisplayInfo"])
        if (const auto it = moved.find(id.get<uint32_t>()); it != moved.end()) id = it->second;
    out.push_back(races.MakeChange(race, p, "renumber race displays"));
    return out;
}

bool FixClassicSectionFlags(nlohmann::json& package)
{
    if (package.contains("sectionFlags")) return false;
    for (nlohmann::json& s : package["CharSections"]) s["Flags"] = ClassicSectionFlags(U(s, "Flags"), U(s, "BaseSection"));
    package["sectionFlags"] = "3.3.5";
    return true;
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
        skills.push_back({ { "raceMask", std::to_string(bit) }, { "classMask", "0" }, { "skill", std::to_string(language) }, { "rank", "0" },
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
