#include "Races.hpp"

#include "Models.hpp"
#include "Mpq.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace
{
    /// Column positions of the tables that differ between the two layouts (WoWDBDefs, builds 5875 and 12340).
    struct Columns
    {
        uint32_t chrRacesFields;
        // ChrRaces
        uint32_t flags, faction, male, female, prefix, baseLanguage, creatureType, fileString, cinematic, alliance, name, nameFemale, nameMale;
        // CharSections
        uint32_t sectionVariation, sectionColor, sectionTexture, sectionFlags;
        uint32_t facialStart, facialGeosets;   // CharacterFacialHairStyles: first geoset column, how many
        uint32_t outfitSlots;      // CharStartOutfit item slots
    };
    constexpr uint32_t kNone = ~0u;
    // 1.12 CharacterFacialHairStyles: WoWDBDefs lists Geoset[6], but the first three are unused (0xCCCCCCCC in Turtle's
    // table); the client's geosets are the last three, in the order 3.3.5 keeps first.
    constexpr Columns kClassic{ 29, 1, 2, 4, 5, 6, 8, 9, 15, 16, kNone, 17, kNone, kNone, 4, 5, 6, 9, 6, 3, 12 };
    constexpr Columns kWrath{ 69, 1, 2, 4, 5, 6, 7, 8, 11, 12, 13, 14, 31, 48, 8, 9, 4, 7, 3, 5, 24 };

    Dbc ReadDbc(const MpqChain& mpq, const char* name)
    {
        Dbc d;
        if (auto bytes = mpq.Read(std::string("DBFilesClient\\") + name + ".dbc")) d.Load(std::move(*bytes));
        return d;
    }

    /// The tables of one layout, read from loaded files (shared by Load and the self-test).
    struct Tables { Dbc races, sections, hair, facial, baseInfo, outfits; };

    void Fill(const Tables& t, const Columns& c, std::vector<RaceCatalog::Race>& races, std::vector<RaceCatalog::Section>& sections,
              std::vector<RaceCatalog::HairGeoset>& hair, std::vector<RaceCatalog::FacialHair>& facial,
              std::vector<std::pair<uint8_t, uint8_t>>& baseInfo, std::vector<RaceCatalog::Outfit>& outfits)
    {
        auto str = [](const Dbc& d, uint32_t r, uint32_t f) { return f == kNone ? std::string() : d.Str(r, f); };
        for (uint32_t r = 0; r < t.races.Rows(); ++r)
        {
            RaceCatalog::Race x;
            x.id = t.races.U32(r, 0);
            x.flags = t.races.U32(r, c.flags);
            x.faction = t.races.U32(r, c.faction);
            x.display[0] = t.races.U32(r, c.male);
            x.display[1] = t.races.U32(r, c.female);
            x.prefix = t.races.Str(r, c.prefix);
            x.baseLanguage = t.races.U32(r, c.baseLanguage);
            x.creatureType = t.races.U32(r, c.creatureType);
            x.fileString = t.races.Str(r, c.fileString);
            x.cinematic = t.races.U32(r, c.cinematic);
            x.alliance = c.alliance == kNone ? -1 : int(t.races.U32(r, c.alliance));
            x.name = t.races.Str(r, c.name);
            x.names[0] = str(t.races, r, c.nameFemale);
            x.names[1] = str(t.races, r, c.nameMale);
            races.push_back(std::move(x));
        }
        for (uint32_t r = 0; r < t.sections.Rows(); ++r)
        {
            RaceCatalog::Section s{ t.sections.U32(r, 0), t.sections.U32(r, 1), t.sections.U32(r, 2), t.sections.U32(r, 3),
                                    t.sections.U32(r, c.sectionVariation), t.sections.U32(r, c.sectionColor), t.sections.U32(r, c.sectionFlags) };
            for (uint32_t i = 0; i < 3; ++i) s.textures[i] = t.sections.Str(r, c.sectionTexture + i);
            sections.push_back(std::move(s));
        }
        for (uint32_t r = 0; r < t.hair.Rows(); ++r)
            hair.push_back({ t.hair.U32(r, 0), t.hair.U32(r, 1), t.hair.U32(r, 2), t.hair.U32(r, 3), t.hair.U32(r, 4), t.hair.U32(r, 5) });
        for (uint32_t r = 0; r < t.facial.Rows(); ++r)
        {
            RaceCatalog::FacialHair f{ t.facial.U32(r, 0), t.facial.U32(r, 1), t.facial.U32(r, 2) };
            for (uint32_t i = 0; i < c.facialGeosets; ++i) f.geosets.push_back(t.facial.U32(r, c.facialStart + i));
            facial.push_back(std::move(f));
        }
        for (uint32_t r = 0; r < t.baseInfo.Rows(); ++r) baseInfo.push_back({ t.baseInfo.U8At(r, 0), t.baseInfo.U8At(r, 1) });
        for (uint32_t r = 0; r < t.outfits.Rows(); ++r)
        {
            RaceCatalog::Outfit o{ t.outfits.U32At(r, 0), t.outfits.U8At(r, 4), t.outfits.U8At(r, 5), t.outfits.U8At(r, 6), t.outfits.U8At(r, 7) };
            for (uint32_t i = 0; i < c.outfitSlots; ++i)
            {
                o.items.push_back(t.outfits.U32At(r, 8 + 4 * i));
                o.displays.push_back(t.outfits.U32At(r, 8 + 4 * (c.outfitSlots + i)));
                o.types.push_back(t.outfits.U32At(r, 8 + 4 * (2 * c.outfitSlots + i)));
            }
            outfits.push_back(std::move(o));
        }
    }
}

bool RaceCatalog::Load(const MpqChain& mpq, std::string& error)
{
    *this = {};
    Tables t;
    t.races = ReadDbc(mpq, "ChrRaces");
    const Columns* c = t.races.Fields() == kWrath.chrRacesFields ? &kWrath : t.races.Fields() == kClassic.chrRacesFields ? &kClassic : nullptr;
    if (!t.races.Rows()) { error = "no ChrRaces.dbc"; return false; }
    if (!c) { error = "ChrRaces.dbc has " + std::to_string(t.races.Fields()) + " fields: not a 1.12 or 3.3.5 layout"; return false; }
    m_layout = c == &kWrath ? Layout::Wrath : Layout::Classic;
    t.sections = ReadDbc(mpq, "CharSections");
    t.hair = ReadDbc(mpq, "CharHairGeosets");
    t.facial = ReadDbc(mpq, "CharacterFacialHairStyles");
    t.baseInfo = ReadDbc(mpq, "CharBaseInfo");
    t.outfits = ReadDbc(mpq, "CharStartOutfit");
    Fill(t, *c, m_races, m_sections, m_hair, m_facial, m_baseInfo, m_outfits);
    m_displayInfo = ReadDbc(mpq, "CreatureDisplayInfo");
    m_modelData = ReadDbc(mpq, "CreatureModelData");
    return true;
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

std::string RaceCatalog::Model(uint32_t race, uint32_t sex) const
{
    const Race* r = Find(race);
    const auto display = r ? m_displayInfo.Find(r->display[sex ? 1 : 0]) : std::nullopt;
    const auto model = display ? m_modelData.Find(m_displayInfo.U32(*display, 1)) : std::nullopt;   // ModelID
    const std::string name = model ? m_modelData.Str(*model, 2) : std::string();                    // ModelName
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
        std::string lower = name;
        for (char& ch : lower) ch = char(std::tolower((unsigned char)ch));
        if (!name.empty() && seen.insert(lower).second) out.push_back(name);
    };
    for (uint32_t sex = 0; sex < 2; ++sex) add(Model(race, sex));
    for (const Section& s : m_sections)
        if (s.race == race)
            for (const std::string& t : s.textures) add(t);
    return out;
}

bool RacesSelfTest()
{
    // A WDBC file of `fields` 32-bit fields per record (or `recordSize` bytes), the strings after the records.
    auto dbc = [](uint32_t fields, uint32_t recordSize, const std::vector<std::vector<uint8_t>>& records, const std::string& strings) {
        std::vector<uint8_t> b(20);
        const uint32_t header[5] = { 0x43424457, uint32_t(records.size()), fields, recordSize, uint32_t(strings.size()) };
        std::memcpy(b.data(), header, 20);
        for (const auto& r : records) b.insert(b.end(), r.begin(), r.end());
        b.insert(b.end(), strings.begin(), strings.end());
        Dbc d;
        d.Load(b);
        return d;
    };
    auto u32s = [](std::initializer_list<uint32_t> v) {
        std::vector<uint8_t> out(v.size() * 4);
        std::memcpy(out.data(), std::data(v), out.size());
        return out;
    };
    const std::string strings = std::string("\0Gn\0Gnome\0Gnomish\0skin.blp\0face.blp\0", 37);   // offsets 1, 4, 10, 18, 27
    auto check = [&](const Columns& c, bool classic) {
        std::vector<uint32_t> race(c.chrRacesFields, 0);
        race[0] = 7;
        race[c.male] = 1563;
        race[c.female] = 1564;
        race[c.prefix] = 1;
        race[c.fileString] = 4;
        race[c.name] = 10;
        if (c.alliance != kNone) race[c.alliance] = 0;
        std::vector<uint8_t> raceBytes(race.size() * 4);
        std::memcpy(raceBytes.data(), race.data(), raceBytes.size());
        std::vector<uint32_t> section(10, 0);
        section[0] = 500; section[1] = 7; section[2] = 1; section[3] = 1;
        section[c.sectionVariation] = 3;
        section[c.sectionColor] = 2;
        section[c.sectionTexture] = 27;
        section[c.sectionFlags] = 1;
        std::vector<uint8_t> sectionBytes(40);
        std::memcpy(sectionBytes.data(), section.data(), 40);
        Tables t;
        t.races = dbc(c.chrRacesFields, c.chrRacesFields * 4, { raceBytes }, strings);
        t.sections = dbc(10, 40, { sectionBytes }, strings);
        t.hair = dbc(6, 24, { u32s({ 9, 7, 1, 4, 2, 1 }) }, strings);
        std::vector<uint32_t> facial = { 7, 0, 5 };
        while (facial.size() < c.facialStart) facial.push_back(0xCCCCCCCC);
        for (uint32_t i = 0; i < c.facialGeosets; ++i) facial.push_back(10 + i);
        std::vector<uint8_t> facialBytes(facial.size() * 4);
        std::memcpy(facialBytes.data(), facial.data(), facialBytes.size());
        t.facial = dbc(uint32_t(facial.size()), uint32_t(facialBytes.size()), { facialBytes }, strings);
        t.baseInfo = dbc(2, 2, { { 7, 1 }, { 7, 8 } }, strings);
        std::vector<uint8_t> outfit = u32s({ 12 });
        outfit.insert(outfit.end(), { 7, 4, 1, 0 });
        for (uint32_t i = 0; i < 3 * c.outfitSlots; ++i) { const auto w = u32s({ i < c.outfitSlots ? 100 + i : 0 }); outfit.insert(outfit.end(), w.begin(), w.end()); }
        t.outfits = dbc(1 + 4 + 3 * c.outfitSlots, uint32_t(outfit.size()), { outfit }, strings);

        std::vector<RaceCatalog::Race> races;
        std::vector<RaceCatalog::Section> sections;
        std::vector<RaceCatalog::HairGeoset> hair;
        std::vector<RaceCatalog::FacialHair> facials;
        std::vector<std::pair<uint8_t, uint8_t>> baseInfo;
        std::vector<RaceCatalog::Outfit> outfits;
        Fill(t, c, races, sections, hair, facials, baseInfo, outfits);
        return races.size() == 1 && races[0].id == 7 && races[0].prefix == "Gn" && races[0].fileString == "Gnome" && races[0].name == "Gnomish" &&
               races[0].display[0] == 1563 && races[0].display[1] == 1564 && races[0].alliance == (classic ? -1 : 0) &&
               sections.size() == 1 && sections[0].variation == 3 && sections[0].color == 2 && sections[0].textures[0] == "face.blp" &&
               sections[0].flags == 1 && sections[0].type == 1 && hair.size() == 1 && hair[0].geoset == 2 && hair[0].variation == 4 && facials.size() == 1 &&
               facials[0].geosets.size() == c.facialGeosets && facials[0].geosets.back() == 10 + c.facialGeosets - 1 && baseInfo.size() == 2 &&
               baseInfo[1] == std::pair<uint8_t, uint8_t>(7, 8) && outfits.size() == 1 && outfits[0].cls == 4 && outfits[0].sex == 1 &&
               outfits[0].items.size() == c.outfitSlots && outfits[0].items.back() == 100 + c.outfitSlots - 1 && outfits[0].displays[0] == 0;
    };
    return check(kClassic, true) && check(kWrath, false);
}
