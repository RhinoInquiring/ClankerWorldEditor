#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// The project folder: project.json, changes/, out/. See docs/change-model.md.
struct Project
{
    std::filesystem::path dir;
    std::string name;
    std::string clientDir;   // WXL client used as data source and for play-testing
    std::string author;
    /// IDs this project hands out, per kind ("creature.guid", "gameobject.guid", "area.id", "wmoarea.id"): new rows take the lowest free id in
    /// their range and never leave it. IDs are permanent once a change records them (docs/change-model.md).
    struct IdRange { uint32_t first = 0, last = 0; };
    std::map<std::string, IdRange> idRanges = { { "creature.guid", { 9000000, 9099999 } }, { "gameobject.guid", { 9000000, 9099999 } },
                                                { "area.id", { 20000, 20999 } },        // AreaTable; creature.zoneId is 16-bit
                                                { "wmoarea.id", { 700000, 709999 } },     // WMOAreaTable (client max 665483)
                                                { "worldmaparea.id", { 9000, 9099 } }, { "worldmapoverlay.id", { 90000, 90999 } } };
    IdRange Range(const std::string& kind) const { auto it = idRanges.find(kind); return it == idRanges.end() ? IdRange{} : it->second; }
    bool Owns(const std::string& kind, uint32_t id) const { const IdRange r = Range(kind); return id >= r.first && id <= r.last && r.first; }
    std::string serverProfile;   // name of the user's ServerProfile (Server.hpp); credentials never live here
    std::vector<std::pair<std::string, std::string>> sources;   // other clients to compare against: name, data dir

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
