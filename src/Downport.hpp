#pragma once

// Newer clients' files (Cataclysm and later: chunked M2s, FileDataID WMOs) rewritten into the shapes the 3.3.5a
// client reads, for export. Split ADTs are merged when read (MergeSplitAdt), so they need nothing here.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/// FileDataID -> game path (empty when unknown).
using FileIdName = std::function<std::string(uint32_t)>;
/// Reads a game file (for a skin, the model it belongs to).
using ReadGameFile = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;

/// The 3.3.5a form of game file `name`: newer M2s (MD21 or MD20 past version 264), their skins and .anim files,
/// and WMO roots and groups are converted; anything else, or a file already in 3.3.5a shape, comes back unchanged.
/// Empty when it cannot be converted (the reason is in `notes`). `notes` gets one line per thing that was lost or
/// changed on the way.
std::vector<uint8_t> Downport(const std::string& name, std::vector<uint8_t> bytes, const FileIdName& nameOf, const ReadGameFile& read,
                              std::vector<std::string>& notes);

/// True when the file needs Downport (a newer format than 3.3.5a's).
bool IsNewerFormat(const std::string& name, const std::vector<uint8_t>& bytes);

/// For a WXL client: true when its extensions (wxl-modern-m2, wxl-modern-wmo) read this newer file as it is, losing
/// nothing — an MD21 model without an external skeleton, or a WMO root without MODI doodads, GFID groups or a MOSI
/// skybox whose groups have no MLIQ water or MOVX triangles (`read` fetches the groups). Skins, .anim files and WMO
/// groups follow their model or root.
bool WxlReadsAsIs(const std::string& name, const std::vector<uint8_t>& bytes, const ReadGameFile& read);

/// The FileDataIDs WXL resolves through wxl.fdid in a file kept as it is: an MD21 model's TXID textures, a modern WMO
/// root's MOMT textures (all nine for the four-layer shader 23).
std::vector<uint32_t> WxlFileIds(const std::string& name, const std::vector<uint8_t>& bytes);

/// Runs the converters on synthetic files; false on the first wrong result.
bool DownportSelfTest();
