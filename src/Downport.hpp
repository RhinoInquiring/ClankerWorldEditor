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

/// Runs the converters on synthetic files; false on the first wrong result.
bool DownportSelfTest();
