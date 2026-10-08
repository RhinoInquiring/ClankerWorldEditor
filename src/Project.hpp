#pragma once

#include "Mpq.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// The project folder: project.json, changes/, out/. See docs/concepts/change-model.md.
struct Project
{
    std::filesystem::path dir;
    std::string name;
    std::string clientDir;   // WXL client used as data source and for play-testing
    std::string author;
    /// IDs this project hands out, per kind ("creature.guid", "gameobject.guid", "area.id", "wmoarea.id"): new rows take the lowest free id in
    /// their range and never leave it. IDs are permanent once a change records them (docs/concepts/change-model.md).
    struct IdRange { uint32_t first = 0, last = 0; };
    std::map<std::string, IdRange> idRanges = { { "creature.guid", { 9000000, 9099999 } }, { "gameobject.guid", { 9000000, 9099999 } },
                                                { "area.id", { 20000, 20999 } },        // AreaTable; creature.zoneId is 16-bit
                                                { "wmoarea.id", { 700000, 709999 } },     // WMOAreaTable (client max 665483)
                                                { "worldmaparea.id", { 9000, 9099 } }, { "worldmapoverlay.id", { 90000, 90999 } },
                                                { "areatrigger.id", { 60000, 60999 } },     // AreaTrigger.dbc + areatrigger (client max ~5900)
                                                { "areapoi.id", { 60000, 60999 } },         // AreaPOI.dbc (client max 2392)
                                                { "points_of_interest.id", { 60000, 60999 } }, { "game_tele.id", { 60000, 60999 } },
                                                { "taxinode.id", { 1, 448 } },              // TaxiNodes: AzerothCore's taxi mask ends at 448 (client max 440, gaps used)
                                                { "taxipath.id", { 5000, 5999 } },          // TaxiPath (client max 1978)
                                                { "taxipathnode.id", { 60000, 69999 } },    // TaxiPathNode (client max 46874)
                                                { "creature_template.entry", { 9000000, 9099999 } },     // new NPCs (stock max ~200000)
                                                { "creaturedisplayinfo.id", { 90000, 90999 } },          // CreatureDisplayInfo (client max ~32754)
                                                { "creaturedisplayinfoextra.id", { 90000, 90999 } },     // CreatureDisplayInfoExtra (client max ~21381)
                                                { "gossip_menu.id", { 9000000, 9099999 } },              // gossip menus (stock max ~90002)
                                                { "npc_text.id", { 9000000, 9099999 } },                 // gossip texts (stock max ~921061)
                                                { "light.id", { 20000, 20999 } },                        // Light.dbc (client max 14497)
                                                { "lightparams.id", { 11000, 11999 } },                  // LightParams (client max 10025); bands follow at 18P-17, 6P-5
                                                { "soundemitter.id", { 20000, 20999 } },                 // SoundEmitters.dbc (client max 2549)
                                                { "map.id", { 800, 999 } },                              // Map.dbc (client max 724)
                                                { "mapdifficulty.id", { 1000, 1099 } } };                // MapDifficulty.dbc
    IdRange Range(const std::string& kind) const { auto it = idRanges.find(kind); return it == idRanges.end() ? IdRange{} : it->second; }
    bool Owns(const std::string& kind, uint32_t id) const { const IdRange r = Range(kind); return id >= r.first && id <= r.last && r.first; }
    std::string serverProfile;   // name of the user's ServerProfile (Server.hpp); credentials never live here
    /// A version of the game files: layers of MPQ folders, single MPQs and unpacked folders, later layers above.
    struct Source { std::string name; std::vector<MpqLayer> layers; };
    /// What the project edits and exports against, and what the editor shows. Default: the client's Data folder.
    Source base;
    /// Other versions to compare against (ghost layers, compare, Differences) and to take assets from.
    std::vector<Source> compare;
    /// The patch MPQ the project builds (out/<name>) and installs (client Data, in the locale folder for a
    /// patch-<locale>-X name). patch-enUS-Z loads after every patch-X, so the project's files win over the modules'.
    std::string patchName = "patch-enUS-Z.MPQ";
    std::filesystem::path PatchOutPath() const { return dir / "out" / patchName; }
    /// Where the client looks for it: Data\<locale>\ for patch-<locale>-X.MPQ, else Data\.
    std::filesystem::path PatchInstallPath() const;

    std::filesystem::path DataDir() const { return std::filesystem::path(clientDir) / "Data"; }
    std::filesystem::path ChangesDir() const { return dir / "changes"; }
    std::filesystem::path BlueprintsDir() const { return dir / "blueprints"; }
    /// Files the editor generated for the client (world map pictures, ...), laid out as in the MPQs; export copies them.
    std::filesystem::path AssetsDir() const { return dir / "assets"; }
    std::filesystem::path ClientOutDir() const { return dir / "out" / "client"; }
    /// Where the play-test link extension serves files from.
    std::filesystem::path OverlayDir() const { return std::filesystem::path(clientDir) / "Extensions" / "wxl-editor-poc" / "overlay"; }

    bool Save(std::string& error) const;
    static std::optional<Project> Load(const std::filesystem::path& dir, std::string& error);
};
