#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class MpqChain;

/// What exported tiles need that the base client does not have, copied next to them so the patch is complete.
struct AssetReport
{
    std::vector<std::string> copied;    // files written (found in a fallback source)
    std::vector<std::string> missing;   // referenced but found nowhere
    std::vector<std::string> converted; // newer-client files rewritten for 3.3.5a (Downport)
    std::vector<std::string> notes;     // what those conversions lost or changed
    std::vector<std::string> kept;      // newer-client files shipped as they are (WXL target: WxlReadsAsIs)
    std::map<uint32_t, std::string> fileIds;   // FileDataIDs the kept files name -> game path (WriteFileIdTables)
};

/// The client the export is for: plain 3.3.5a converts every newer-client file; WXL ships the ones its extensions read
/// (wxl-modern-m2/-wmo) as they are and converts the rest.
enum class ExportTarget { Client335, Wxl };

/// Follows every reference of the given ADT files (textures and their _s variants, M2s with their skins and
/// textures, WMOs with their groups, textures and doodad models) and writes each file players lack (no installed
/// layer has it: MpqLayer::installed), read from any layer or fallback, under outDir at its game path. Files players
/// have are not copied, nor followed further. Files from newer clients are converted to 3.3.5a first (Downport) and
/// their references followed from the converted file.
/// With `resume`, a file already under outDir is taken as written (read back from there for its references).
/// For ExportTarget::Wxl, newer files WXL reads are written unchanged (report.kept) and the FileDataIDs they name are
/// collected in report.fileIds.
AssetReport CopyMissingAssets(const MpqChain& mpq, const std::vector<std::filesystem::path>& adtFiles, const std::filesystem::path& outDir,
                              bool resume = false, ExportTarget target = ExportTarget::Client335);

/// Files one game file refers to directly (by extension: .adt, .wdt, .m2, .wmo; anything else refers to nothing).
std::vector<std::string> AssetReferences(const MpqChain& mpq, const std::string& path, const std::vector<uint8_t>& bytes);

/// AssetReferences, also for a newer-client file kept as it is: its references as the 3.3.5a conversion names them,
/// plus the files its FileDataIDs (WxlFileIds) name through `nameOf`.
std::vector<std::string> FileReferences(const MpqChain& mpq, const std::string& path, const std::vector<uint8_t>& bytes,
                                        const std::function<std::string(uint32_t)>& nameOf);

/// wxl-db2's FileDataID tables, DBFilesClient\TextureFilePath.db2 (.blp) and ModelFilePath.db2 (everything else): WDC1,
/// one section, records {uint32 id; uint32 path offset} (the layout WarcraftXL's DB2Gen writes). Writes `fileIds` under
/// outDir, merged over the client's own tables when it has them (ours win); an empty table is not written. Returns the
/// game paths written; `notes` gets a client table that could not be read.
std::vector<std::string> WriteFileIdTables(const MpqChain& mpq, const std::map<uint32_t, std::string>& fileIds,
                                           const std::filesystem::path& outDir, std::vector<std::string>& notes);
std::vector<uint8_t> WriteFileIdTable(const std::map<uint32_t, std::string>& rows);
/// A table in that layout (DB2Gen's or ours); nothing for any other.
std::optional<std::map<uint32_t, std::string>> ReadFileIdTable(const std::vector<uint8_t>& bytes);
