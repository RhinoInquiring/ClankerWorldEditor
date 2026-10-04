#pragma once

#include "Terrain.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// A saved area: terrain (heights, textures, holes) and the objects on it, kept for later pasting.
/// One JSON file per blueprint in the project's blueprints/ folder, thumbnail included.
struct Blueprint
{
    std::string name, notes;
    std::string map;           // where it was taken from (map folder, plus the ghost layer if any)
    std::string created;       // local time, "2026-10-03 14:05"
    TerrainClipboard clip;
    uint32_t thumbSize = 0;    // square RGBA thumbnail, top-down
    std::vector<uint8_t> thumb;
    std::filesystem::path file;

    bool Save(const std::filesystem::path& dir, std::string& error);   // picks a file name from `name` when new
    static std::optional<Blueprint> Load(const std::filesystem::path& file, std::string& error);
    /// Every blueprint in a folder, sorted by name.
    static std::vector<Blueprint> LoadAll(const std::filesystem::path& dir, std::vector<std::string>& errors);
};

/// Save/load round trip on a synthetic blueprint; false on the first mismatch.
bool BlueprintSelfTest();
