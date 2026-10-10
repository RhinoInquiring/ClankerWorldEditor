#pragma once

#include "Areas.hpp"
#include "Changes.hpp"
#include "Formats.hpp"
#include "Tables.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class MpqChain;
struct Project;

/// One column of a client table: its name as mod-dbc-patch's schemas give it (`Name[i]` for arrays; a localized string
/// is `Name_lang` for enUS, `Name_lang[i]` for locale slot i and `Name_lang_flags`), its byte offset in the record and
/// its type: 'i' 32-bit integer, 'b' 8-bit integer, 'f' float, 's' string.
struct DbcColumn { std::string name; uint32_t offset; char type; };
/// The columns of a table in one client layout. `fields` is what a WDBC header of it says.
struct DbcLayout { std::vector<DbcColumn> columns; uint32_t recordSize = 0, fields = 0; };

/// A row as a JSON object by column name.
nlohmann::json DbcRowJson(const Dbc& dbc, uint32_t row, const DbcLayout& layout);
/// A WDBC file of `rows` (JSON objects by column name, in the order given; missing columns 0 or "") in `layout`.
std::vector<uint8_t> WriteDbcRows(const std::vector<nlohmann::json>& rows, const DbcLayout& layout);

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
        nlohmann::json row;                      // the whole ChrRaces row, by 3.3.5 column names
    };
    /// One CharSections row. type: 0 skin, 1 face, 2 facial hair, 3 hair (scalp), 4 underwear.
    struct Section { uint32_t id = 0, race = 0, sex = 0, type = 0, variation = 0, color = 0, flags = 0; std::string textures[3]; };
    struct HairGeoset { uint32_t id = 0, race = 0, sex = 0, variation = 0, geoset = 0, showScalp = 0; };
    /// CharacterFacialHairStyles: no id of its own; race, sex and variation are the key. Geosets: 5 in 3.3.5, 3 in 1.12.
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

    /// A race and every row of its own, by 3.3.5 column names whatever the client's layout: {"ChrRaces": row,
    /// "CharSections": [rows], "CharHairGeosets": [...], "CharacterFacialHairStyles": [...], "CharBaseInfo": [...],
    /// "CharStartOutfit": [...]}. Null when there is no such race.
    nlohmann::json Package(uint32_t race) const;
    /// Makes `race` what `package` says (rows of the race replaced), or removes it (package null): the project's races
    /// over the client's. `models` names the model of a display the client's tables lack (the project's displays).
    void Apply(uint32_t race, const nlohmann::json& package);
    void SetModelLookup(std::function<std::string(uint32_t display)> models) { m_modelOf = std::move(models); }
    /// A CreatureDisplayInfo / CreatureModelData row by 3.3.5 column names; null when there is none.
    nlohmann::json DisplayRow(uint32_t id) const;
    nlohmann::json ModelRow(uint32_t id) const;
    /// The id of a CreatureModelData row with this model (case-insensitive, .mdx and .m2 alike); 0 when none.
    uint32_t ModelId(const std::string& model) const;
    /// Ids the rows of a table use ("CharSections", "CharHairGeosets", "CharStartOutfit").
    std::set<uint32_t> Ids(const std::string& table) const;
    /// Every row of a race table (any of RaceCatalog::kTables) by 3.3.5 column names, every race's.
    std::vector<nlohmann::json> TableRows(const std::string& table) const;
    /// The race tables, in the order export writes them.
    static constexpr const char* kTables[] = { "ChrRaces", "CharSections", "CharHairGeosets", "CharacterFacialHairStyles", "CharBaseInfo", "CharStartOutfit" };
    /// The name of the faction a FactionTemplate id belongs to in this client ("PLAYER, Human"); empty when it has none.
    std::string FactionName(uint32_t factionTemplate) const;
    /// Languages.dbc: every language (id, name) of this client, and one's name (empty when it has none).
    std::vector<std::pair<uint32_t, std::string>> Languages() const;
    std::string LanguageName(uint32_t id) const;

private:
    Layout m_layout = Layout::None;
    std::vector<Race> m_races;
    std::vector<Section> m_sections;
    std::vector<HairGeoset> m_hair;
    std::vector<FacialHair> m_facial;
    std::vector<std::pair<uint8_t, uint8_t>> m_baseInfo;
    std::vector<Outfit> m_outfits;
    Dbc m_displayInfo, m_modelData, m_factionTemplates, m_factions, m_languages;
    std::function<std::string(uint32_t)> m_modelOf;
};

/// The races the project adds or changes, each as one package (RaceCatalog::Package's form, plus "source" {client, race}
/// it came from and "added" {table: [ids]}: the display and model rows its import added). Domain "race"; change data
/// {"race": id, "before": package | null, "after": package | null}; null = the client's race (or none).
class RaceAdapter final : public Adapter
{
public:
    const char* Domain() const override { return "race"; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }
    /// The project's package of a race; null when the project has not touched it.
    const nlohmann::json* Package(uint32_t race) const { auto it = m_packages.find(race); return it == m_packages.end() ? nullptr : &it->second; }
    const std::map<uint32_t, nlohmann::json>& Packages() const { return m_packages; }
    Change MakeChange(uint32_t race, const nlohmann::json& after, const std::string& label) const;
    /// Forget every package (the project closed).
    void Clear() { m_packages.clear(); ++m_version; }
    uint64_t Version() const { return m_version; }

private:
    void Set(const Change& change, bool after);
    std::map<uint32_t, nlohmann::json> m_packages;
    uint64_t m_version = 0;
};

/// The 3.3.5 columns of CreatureDisplayInfo and CreatureModelData, as their DbcTables take them.
const std::vector<DbcField>& CreatureDisplayInfoFields();
const std::vector<DbcField>& CreatureModelDataFields();

/// What an import changes about the race besides its ids. Unset values keep the source's.
struct RaceImportOptions
{
    int alliance = -1;                   // ChrRaces.Alliance: 0 Alliance, 1 Horde
    uint32_t faction = 0;                // ChrRaces.FactionID (a FactionTemplate id)
    std::optional<std::set<uint32_t>> classes;   // CharBaseInfo: the classes it can be; a class left out loses its starting outfits
    uint32_t outfitDonor = 0;            // a race of the project whose starting outfits a class takes that the source has none of
};

/// What importing race `sourceRace` of `source` (a client named `sourceName`) as race `target` of the project takes, as
/// changes not yet applied: the race package with every id of its own renumbered into the project's ranges
/// (CharSections, CharHairGeosets, CharStartOutfit), and new CreatureDisplayInfo rows for its two displays with their
/// CreatureModelData (a model the project's client already lists keeps its row). `project` is the project's races as
/// they are now (client and packages), `displays` / `models` the project's tables. Empty (and `error`) when a range is full
/// or the race has no row.
std::vector<Change> ImportRaceChanges(const RaceCatalog& source, const std::string& sourceName, uint32_t sourceRace, uint32_t target,
                                      const RaceCatalog& project, const RaceAdapter& races, const DbcTable& displays, const DbcTable& models,
                                      const Project& ranges, std::string& error, const RaceImportOptions& options = {},
                                      const MpqChain* sourceFiles = nullptr, const MpqChain* projectFiles = nullptr);
/// With `sourceFiles` and `projectFiles` (the source client's and the project's files) the import also lists what the
/// race's characters use: its models with their skins, .anim files and fixed textures, and every CharSections texture.
/// The package's "files" ([{"path", "from", "textures": {old: new}}]) are what export copies from the source: files the
/// project's client lacks, under their own names, and files it has with other bytes, renamed into Character\Race<id>\...
/// so the client's own stay as they are; every reference follows a rename (CharSections textures, a model's own row,
/// fixed texture names inside a copied model). A file the client has as it is, players have: nothing to copy.

/// Converts the CharSections flags of a package imported from a 1.12 client before imports converted them (1.12: 1 =
/// only NPCs wear it; 3.3.5: 0x1 = players may choose it) and marks it done ("sectionFlags"). False when already done.
bool FixClassicSectionFlags(nlohmann::json& package);

/// The changes removing a project race the project added: its package and the display and model rows its import added.
std::vector<Change> RemoveRaceChanges(uint32_t race, const RaceAdapter& races, const DbcTable& displays, const DbcTable& models);

/// A model with fixed texture names replaced (old -> new, case-insensitive; names appended to the file).
std::vector<uint8_t> RenameM2Textures(std::vector<uint8_t> m2, const std::map<std::string, std::string>& names);

/// Writes the race tables of `project` (every race's rows, the project's packages applied; CharSections sorted by race,
/// sex, section, variation and colour, as the client's cache needs) into each of `dbcDirs`, and copies every package's
/// files from its source client (`source` finds a client by name; null when it is not attached) under `clientOut`. With
/// no package, removes race tables an earlier export left. `notes` gets what export could not do. False on a write error.
/// With `client` (the project's client) it also writes the character creator (CreatorScript) and copies a race's own
/// creator background from its source client when players lack one; `assets` is the project's folder of files that
/// go into the patch as they are (a race's icon).
bool ExportRaces(const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages,
                 const std::function<const MpqChain*(const std::string& client)>& source, const std::vector<std::filesystem::path>& dbcDirs,
                 const std::filesystem::path& clientOut, std::vector<std::string>& notes, std::string& error,
                 const MpqChain* client = nullptr, const std::filesystem::path& assets = {});

/// The client's character creator script (Interface\GlueXML\CharacterCreate.lua, given as `clientLua`) with the
/// project's playable races added: a button for every playable race (the client's XML has ten), laid out per faction,
/// and each project race's icon, description, abilities and creator background — its own, or its donor race's. A
/// package's "creator" holds the choices: {"donor": race, "icon": game path without .blp (a -Round twin beside it),
/// "description", "abilities": [lines], "background": a UI_ scene name}. What an earlier export added (from the
/// "-- wow-world-editor" line on) is cut off `clientLua` first. `has` says whether players will have a game file.
/// Empty when no project race is playable.
std::string CreatorScript(const std::string& clientLua, const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages,
                          const std::function<bool(const std::string&)>& has);
/// The creator scene a background name loads: Interface\Glues\Models\UI_<name>\UI_<name>.m2.
std::string CreatorBackgroundPath(const std::string& name);
/// The race whose icon, texts and background a project race borrows when it has none of its own: its package's
/// "creator" donor, else the first playable client race of its team.
uint32_t CreatorDonor(const RaceCatalog& project, const std::map<uint32_t, nlohmann::json>& packages, uint32_t race);
/// The square creator icon (64 x 64) and its round twin from a picture of any size (RGBA), as two BLP files.
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> CreatorIcons(uint32_t width, uint32_t height, const uint8_t* rgba);

/// What a character chooses in the creator, each with its own values per sex.
enum class RaceChoice { Skin, Face, HairStyle, HairColor, FacialHair };
/// The values a race package offers of a choice for a sex (CharSections colours / variations, CharHairGeosets and
/// CharacterFacialHairStyles variations), ascending.
std::vector<uint32_t> RaceChoiceValues(const nlohmann::json& package, uint32_t sex, RaceChoice choice);
/// Takes one value of a choice out of a race package and closes the gap: the character creator steps through a choice
/// by index, so the values above it move down one. A skin colour takes its faces and underwear with it, a hair style
/// its geosets and scalp textures, a hair colour its scalp and facial hair textures of that colour, a facial hair style
/// its geosets and textures. False when the package has no such value.
bool RemoveRaceChoice(nlohmann::json& package, uint32_t sex, RaceChoice choice, uint32_t value);
/// Gives a race package these classes (CharBaseInfo). A class it no longer has loses its starting outfits; a class it has
/// no outfits of takes `donor`'s (a package; new ids from `outfitIds`, which returns 0 when none is left). False (and
/// `error`) when ids ran out.
bool SetRaceClasses(nlohmann::json& package, const std::set<uint32_t>& classes, const nlohmann::json& donor,
                    const std::function<uint32_t()>& outfitIds, std::string& error);

/// The world database rows a new character of a race is made from (AzerothCore's ObjectMgr::LoadPlayerInfo): per race
/// and class its start (playercreateinfo), action bar buttons and extra items, per race its base stats, and per race mask
/// its skills and spells. Rows of a mask that is the race's own bit belong to it; rows of shared masks (0 = every race,
/// or several races) are left alone.
struct RaceServerTables
{
    TableRowsAdapter& start;     // playercreateinfo by race: race, class, map, zone, position_x/y/z, orientation
    TableRowsAdapter& actions;   // playercreateinfo_action by race: race, class, button, action, type
    TableRowsAdapter& items;     // playercreateinfo_item by race: race, class, itemid, amount, Note
    TableRowsAdapter& stats;     // player_race_stats by Race: Strength, Agility, Stamina, Intellect, Spirit
    TableRowsAdapter& skills;    // playercreateinfo_skills by raceMask: classMask, skill, rank, comment
    TableRowsAdapter& spells;    // playercreateinfo_spell_custom by racemask: classmask, Spell, Note
};
/// A race's own bit of an AzerothCore race mask (race 1 = 1).
inline uint32_t RaceBit(uint32_t race) { return race >= 1 && race <= 32 ? 1u << (race - 1) : 0; }
/// The SkillLine of a language (Languages.dbc id): Common 98, Orcish 109, ... 0 when not a racial language.
uint32_t LanguageSkill(uint32_t language);

/// The changes giving `race` the server rows `donor` has, for `classes`: its starts, action bars and extra items per
/// class (a class the donor has no start of takes the first race's that has one, `fallbacks` in order), its base stats,
/// and the skills and spells of its own mask bit; a racial language skill becomes `baseLanguage`'s. Changes not yet
/// applied, one per table touched.
std::vector<Change> CopyRaceServerRows(uint32_t race, uint32_t donor, const std::vector<uint32_t>& classes, uint32_t baseLanguage,
                                       const std::vector<uint32_t>& fallbacks, const RaceServerTables& t);
/// The change setting the start of one class of a race (playercreateinfo), keeping the other classes' rows.
Change SetRaceStart(uint32_t race, uint32_t cls, uint32_t map, uint32_t zone, float x, float y, float z, float orientation, const RaceServerTables& t);

/// Reads synthetic 1.12 and 3.3.5 tables and round-trips a package; false on the first wrong field.
bool RacesSelfTest();
