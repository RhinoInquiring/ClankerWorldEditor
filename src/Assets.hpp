#pragma once

#include <filesystem>
#include <string>
#include <vector>

class MpqChain;

/// What exported tiles need that the base client does not have, copied next to them so the patch is complete.
struct AssetReport
{
    std::vector<std::string> copied;    // files written (found in a fallback source)
    std::vector<std::string> missing;   // referenced but found nowhere
};

/// Follows every reference of the given ADT files (textures and their _s variants, M2s with their skins and
/// textures, WMOs with their groups, textures and doodad models) and writes each file players lack (no installed
/// layer has it: MpqLayer::installed), read from any layer or fallback, under outDir at its game path. Files players
/// have are not copied, nor followed further.
AssetReport CopyMissingAssets(const MpqChain& mpq, const std::vector<std::filesystem::path>& adtFiles, const std::filesystem::path& outDir);

/// Files one game file refers to directly (by extension: .adt, .m2, .wmo; anything else refers to nothing).
std::vector<std::string> AssetReferences(const MpqChain& mpq, const std::string& path, const std::vector<uint8_t>& bytes);
