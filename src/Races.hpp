#pragma once

#include "Formats.hpp"

#include <cstdint>
#include <string>
#include <vector>

class MpqChain;

/// The races of one client and what their characters can look like: ChrRaces, CharSections, CharHairGeosets,
/// CharacterFacialHairStyles, CharBaseInfo, CharStartOutfit and the race's character displays, read in that client's
/// own layout (1.12 or 3.3.5; the columns of CharSections, for one, come in another order in 1.12). Lets the races of
/// other clients be listed, previewed and imported.
class RaceCatalog
{
public:
    enum class Layout { None, Classic, Wrath };   // Classic: 1.12 (build 5875), Wrath: 3.3.5a (12340)

    struct Race
    {
        uint32_t id = 0, flags = 0, faction = 0, display[2] = {}, baseLanguage = 0, creatureType = 0, cinematic = 0;
        int alliance = -1;                       // 0 Alliance, 1 Horde; -1: the layout has no such field (1.12)
        std::string name, names[2], prefix, fileString;   // names: female and male forms (3.3.5 only)
    };
    /// One CharSections row. type: 0 skin, 1 face, 2 facial hair, 3 hair (scalp), 4 underwear.
    struct Section { uint32_t id = 0, race = 0, sex = 0, type = 0, variation = 0, color = 0, flags = 0; std::string textures[3]; };
    struct HairGeoset { uint32_t id = 0, race = 0, sex = 0, variation = 0, geoset = 0, showScalp = 0; };
    /// CharacterFacialHairStyles: no id of its own; race, sex and variation are the key. Geosets: 5 in 3.3.5, 6 in 1.12.
    struct FacialHair { uint32_t race = 0, sex = 0, variation = 0; std::vector<uint32_t> geosets; };
    /// CharStartOutfit: what a race / class / sex starts wearing (12 slots in 1.12, 24 in 3.3.5).
    struct Outfit { uint32_t id = 0; uint8_t race = 0, cls = 0, sex = 0, outfit = 0; std::vector<uint32_t> items, displays, types; };
    /// How many of each choice a race and sex has, counted as the character creator offers them.
    struct Counts { size_t skins = 0, faces = 0, hairStyles = 0, hairColors = 0, facialHair = 0; };

    /// Reads the tables from `mpq`. False (with `error`) when ChrRaces is missing or in a layout this does not know;
    /// the other tables may be missing (a client with no customisations).
    bool Load(const MpqChain& mpq, std::string& error);
    Layout Format() const { return m_layout; }
    static const char* LayoutName(Layout l) { return l == Layout::Classic ? "1.12" : l == Layout::Wrath ? "3.3.5" : "unknown"; }

    const std::vector<Race>& Races() const { return m_races; }
    const Race* Find(uint32_t id) const;
    const std::vector<Section>& Sections() const { return m_sections; }
    const std::vector<HairGeoset>& HairGeosets() const { return m_hair; }
    const std::vector<FacialHair>& FacialHairStyles() const { return m_facial; }
    const std::vector<std::pair<uint8_t, uint8_t>>& BaseInfo() const { return m_baseInfo; }   // (race, class) a character can be
    const std::vector<Outfit>& Outfits() const { return m_outfits; }

    Counts Count(uint32_t race, uint32_t sex) const;
    /// The character model of a race and sex (ChrRaces display -> CreatureDisplayInfo -> CreatureModelData), as an
    /// .m2 path; empty when the chain breaks.
    std::string Model(uint32_t race, uint32_t sex) const;
    /// Classes the race can be (CharBaseInfo), ascending.
    std::vector<uint32_t> Classes(uint32_t race) const;
    /// Every game file the race's characters use: both models and every CharSections texture, once each.
    std::vector<std::string> Files(uint32_t race) const;

private:
    Layout m_layout = Layout::None;
    std::vector<Race> m_races;
    std::vector<Section> m_sections;
    std::vector<HairGeoset> m_hair;
    std::vector<FacialHair> m_facial;
    std::vector<std::pair<uint8_t, uint8_t>> m_baseInfo;
    std::vector<Outfit> m_outfits;
    Dbc m_displayInfo, m_modelData;
};

/// Reads synthetic 1.12 and 3.3.5 tables; false on the first wrong field.
bool RacesSelfTest();
