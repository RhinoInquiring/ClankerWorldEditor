#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

// 3.3.5 (12340) file formats, parsed into plain data with no GPU dependency.
//
// Editor world coordinates are the ADT placement space: x and z horizontal (tile x/y from the
// file name times 533.33 yd), y up. Placement positions in MDDF/MODF use the same space directly.

constexpr float kTileSize  = 1600.0f / 3.0f;     // 533.33 yd
constexpr float kChunkSize = kTileSize / 16.0f;  // 33.33 yd
constexpr float kUnitSize  = kChunkSize / 8.0f;  // 4.1667 yd, vertex spacing
constexpr float kZeroPoint = 32.0f * kTileSize;

struct BlpImage
{
    enum class Format { BC1, BC2, BC3, RGBA8 };
    uint32_t width = 0, height = 0;
    Format format = Format::RGBA8;
    std::vector<std::vector<uint8_t>> mips;   // mip 0 first; RGBA8 rows are width * 4 bytes
};
std::optional<BlpImage> ParseBlp(const std::vector<uint8_t>& data);
/// RGBA pixels of a BLP's first mip (BC1/2/3 decoded with their alpha; RGBA8 as is); empty when there are none.
std::vector<uint8_t> BlpPixels(const BlpImage& image);

struct AdtChunk
{
    float baseX = 0, baseY = 0, baseZ = 0;   // corner of the chunk; baseY = base height
    uint32_t indexX = 0, indexY = 0;          // chunk position within the tile (0..15)
    std::array<float, 145> heights{};        // MCVT, relative to baseY: 9,8,9,...,9 rows
    size_t mcvtOffset = 0;                    // file offset of the MCVT floats (0 = none), for export
    size_t mcnrOffset = 0;                    // file offset of the MCNR normals (145 x 3 int8), for export
    size_t mccvOffset = 0;                    // file offset of the MCCV colours (145 x BGRA), for export
    size_t mcnkOffset = 0;                    // file offset of the MCNK chunk (its magic), for header patches
    uint32_t flags = 0;                       // MCNK flags (0x2 = impassable)
    uint16_t holes = 0;                       // 4x4 hole mask, bit (row * 4 + col); one bit = 2x2 cells (8.33 yd)
    uint32_t areaId = 0;
    uint32_t layerCount = 0;
    std::array<uint32_t, 4> textureIds{};     // indices into Adt::textures
    std::array<uint32_t, 4> layerFlags{};     // MCLY flags as read
    std::array<uint32_t, 4> effectIds{};      // MCLY ground effect (detail doodads) per layer
    std::vector<uint8_t> alpha;               // 64x64 RGBA: r,g,b = layers 1..3
    std::vector<uint8_t> colors;              // MCCV: 145 x BGRA, 0x7F = 1.0 (neutral); empty = the chunk has none
    std::vector<uint32_t> doodadRefs, wmoRefs;   // MCRF: indices into Adt::doodads / Adt::wmos
};

struct DoodadPlacement { std::string model; float pos[3]; float rot[3]; float scale; uint32_t uniqueId; uint16_t flags = 0; };
struct WmoPlacement { std::string model; float pos[3]; float rot[3]; float extMin[3]; float extMax[3]; uint32_t uniqueId; uint16_t doodadSet;
                      uint16_t flags = 0, nameSet = 0; };

/// One MH2O liquid instance of a chunk: a (w+1) x (h+1) height grid starting at cell (x, y).
struct AdtLiquid
{
    uint16_t type = 0;               // LiquidType.dbc id
    uint16_t format = 0;             // vertex format: 2 = depth only (flat ocean)
    float cornerX = 0, cornerZ = 0;  // the chunk's corner
    uint8_t x = 0, y = 0, w = 0, h = 0;
    std::vector<float> heights;      // (w + 1) * (h + 1), row-major
    std::vector<bool> exists;        // w * h cells
    bool fromMclq = false;           // read from an old MCLQ chunk (type then 1 water, 2 ocean, 3 magma, 4 slime)
    /// Per-vertex data after the heights, as MH2O stores it: depth bytes (formats 0 and 2), uv pairs of uint16 (1),
    /// uv then depth (3). MCLQ water is read as format 0, MCLQ magma and slime as format 1.
    std::vector<uint8_t> extra;
    uint64_t fishable = ~0ull, deep = 0;   // the chunk's MH2O attribute masks (8 x 8 cells), kept with each instance
};

/// Bytes of AdtLiquid::extra per vertex for an MH2O vertex format (0 for unknown formats).
size_t LiquidExtraPerVertex(uint16_t format);

struct Adt
{
    std::vector<std::string> textures;
    std::vector<AdtChunk> chunks;
    std::vector<DoodadPlacement> doodads;
    std::vector<WmoPlacement> wmos;
    std::vector<AdtLiquid> liquids;
};
std::optional<Adt> ParseAdt(const std::vector<uint8_t>& data, bool bigAlpha);

/// A new flat tile (x, y): 256 chunks at `height`, one ground texture, no objects, no water, all of area `areaId`.
std::vector<uint8_t> BlankAdt(int x, int y, float height, const std::string& texture, uint32_t areaId);
/// A new map's WDT with no tiles (MPHD `flags`: 0x4 = 8-bit alpha, 0x2 = vertex colours) and its empty WDL.
/// One 3.3.5 ADT from a newer client's split tile (Cataclysm and later): `root` (heights, normals, colours, holes,
/// MH2O), `tex0` (textures, layers, alpha, shadows), `obj0` (doodads, WMOs, chunk references). Names given by
/// FileDataID (MDID textures, MDDF/MODF entries flagged as such) come from `nameOf` (FILE%08X.dat when it has none);
/// fine 8 x 8 holes become 4 x 4; an MH2O LiquidObject id becomes the vertex format its data has. Empty when `root`
/// is not a split root. `notes` gets what could not be carried over as is.
std::vector<uint8_t> MergeSplitAdt(const std::vector<uint8_t>& root, const std::vector<uint8_t>& tex0, const std::vector<uint8_t>& obj0,
                                   const std::function<std::string(uint32_t)>& nameOf, std::vector<std::string>* notes = nullptr);
std::vector<uint8_t> BlankWdt(uint32_t flags);
/// A newer client's WDT in 3.3.5a shape: MPHD flags it knows (0x80 big alpha as 0x4), MAIN without MAID (tiles kept only
/// where `hasTile(x, y)`), a global WMO named in MWMO (a FileDataID through `nameOf`). Empty when it has no MAIN.
std::vector<uint8_t> DowngradeWdt(const std::vector<uint8_t>& wdt, const std::function<std::string(uint32_t)>& nameOf,
                                  const std::function<bool(int, int)>& hasTile, std::vector<std::string>* notes = nullptr);
std::vector<uint8_t> BlankWdl();

/// A whole MH2O chunk body (header table, attributes, instances, existence bits, vertex data) for adt's liquids,
/// each assigned to the chunk whose corner it carries. MCLQ-read liquids are written as MH2O too.
std::vector<uint8_t> WriteMh2o(const Adt& adt);

/// MPHD flags say whether the map's alpha maps are 8-bit (big alpha) or 4-bit.
bool WdtBigAlpha(const std::vector<uint8_t>& wdt);
/// MPHD flag 0x2: the client reads MCCV vertex colours on this map (and expects one in every chunk).
bool WdtVertexColors(const std::vector<uint8_t>& wdt);

/// True when the WDT MAIN table marks tile (x, y) as present.
bool WdtHasTile(const std::vector<uint8_t>& wdt, int x, int y);

/// All 64x64 present flags of a WDT, indexed y * 64 + x.
std::vector<bool> WdtTiles(const std::vector<uint8_t>& wdt);
/// The one WMO a WMO-only map (instances, most dungeons: MPHD flag 0x1) is made of, from its MWMO and MODF; null
/// for maps of terrain tiles. Position and extents are moved into the ADT placement space (the WDT counts from the
/// map's middle: + kZeroPoint on x and z); subtract it again when writing a WDT.
std::optional<WmoPlacement> WdtGlobalWmo(const std::vector<uint8_t>& wdt);
/// The WDT with its global WMO set to `p` (ADT placement space, as WdtGlobalWmo returns it): MWMO and MODF rewritten,
/// every other chunk kept. Empty when the WDT has no MODF to replace.
std::vector<uint8_t> WdtSetGlobalWmo(const std::vector<uint8_t>& wdt, const WmoPlacement& p);
/// The model tile key the editor shows a WMO-only map's WMO under (past the 4096 terrain tile keys, so it is picked like them).
constexpr int kGlobalWmoKey = 5000;

/// Sets the unique ids of an ADT's placements in place, MDDF and MODF entries in file order (ParseAdt's order).
/// False when the lists do not match the file's.
bool SetUniqueIds(std::vector<uint8_t>& adt, const std::vector<uint32_t>& doodads, const std::vector<uint32_t>& wmos);

/// The WDT with tile (x, y) marked present or absent in MAIN (empty when it has no MAIN).
std::vector<uint8_t> WdtSetTile(std::vector<uint8_t> wdt, int x, int y, bool present);

struct MapEntry { uint32_t id = 0; std::string directory; std::string name; std::string source; };   // source: the label of the layer it comes from
/// Map.dbc rows (3.3.5 layout: ID, Directory, ..., MapName_lang enUS at field 5).
std::vector<MapEntry> ParseMapDbc(const std::vector<uint8_t>& dbc);

/// Rewrites an ADT with new texture layers for `chunks` (indices into adt.chunks): MTEX becomes
/// adt.textures, each listed chunk gets fresh MCLY/MCAL sub-chunks appended (its old ones stay as unused
/// bytes), and MHDR/MCIN offsets are recomputed. Everything else is copied byte for byte.
/// Objects: when adt's doodad/WMO lists differ from the original file's (added, moved, deleted; matched by
/// unique id), MMDX/MMID/MDDF and MWMO/MWID/MODF are rebuilt and every chunk whose MCRF changes gets a new one:
/// untouched objects keep their original chunk references, moved and new ones are referenced where they stand.
/// Liquids: when adt.liquids differ from the original's, MH2O is written afresh (WriteMh2o; inserted before the
/// first MCNK when the file had none). The client then ignores the old MCLQ liquid of every chunk.
/// Returns an empty vector when the input has no MHDR or MCIN (or lacks object chunks it needs).
std::vector<uint8_t> RewriteAdt(const std::vector<uint8_t>& original, const Adt& adt, const std::set<size_t>& chunks, bool bigAlpha);

/// Smooth vertex normals for one chunk of a tile, editor axes (y up), from the heights across the whole tile
/// (central differences; one-sided at the tile's border). Same vertex order as MCVT.
/// `beyond` (optional) gives absolute heights of outer vertices just outside the tile, in the tile's 129 x 129
/// vertex grid (-1 or 129 on an axis), so normals on the tile border see the neighbouring tile.
std::array<std::array<float, 3>, 145> ChunkNormals(const Adt& adt, size_t chunk,
                                                   const std::function<std::optional<float>(int gx, int gz)>& beyond = {});

/// Structural check of an ADT as the client reads it: MHDR/MCIN pointers, every MCNK sub-chunk offset, layer
/// texture ids, alpha extents, MCRF and placement name indices. Returns problems (empty = sound); `summary` gets counts.
std::vector<std::string> ValidateAdt(const std::vector<uint8_t>& data, bool bigAlpha, std::string* summary = nullptr);

/// Record ids (first field) of a WDBC file.
std::set<uint32_t> DbcIds(const std::vector<uint8_t>& dbc);

/// A whole WDBC table: fields by record index, rows found by their first field (the id). Bad indices read 0 / "".
class Dbc
{
public:
    bool Load(std::vector<uint8_t> data);
    uint32_t Rows() const { return m_records; }
    uint32_t U32(uint32_t row, uint32_t field) const;
    float F32(uint32_t row, uint32_t field) const;
    std::string Str(uint32_t row, uint32_t field) const;
    /// Packed tables (CharBaseInfo, CharStartOutfit): a byte, or 32 bits, at a byte offset into the record.
    uint8_t U8At(uint32_t row, uint32_t offset) const;
    uint32_t U32At(uint32_t row, uint32_t offset) const;
    uint32_t RecordSize() const { return m_recordSize; }
    /// The string at an offset into the string block ("" for 0 or past it).
    std::string Text(uint32_t offset) const;
    /// Row index of an id, if present.
    std::optional<uint32_t> Find(uint32_t id) const;
    uint32_t Fields() const { return m_fields; }

private:
    std::vector<uint8_t> m_data;
    uint32_t m_records = 0, m_fields = 0, m_recordSize = 0, m_stringSize = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_index;   // (id, row), sorted
};

/// Frame `frame` (from 1) of a liquid type's animated texture: LiquidType.dbc Texture[0] with its %d filled in, e.g.
/// XTextures\river\lake_a.1.blp; empty when the type has none.
std::string LiquidFrameName(const Dbc& liquidTypes, uint16_t type, int frame);

/// Low-detail heights of a whole map from its .wdl: per tile (y * 64 + x) the 17 x 17 outer grid in yards
/// (row along z, column along x, the tile's corner vertices included), empty where the map has no tile.
std::vector<std::vector<int16_t>> ParseWdl(const std::vector<uint8_t>& wdl);

/// The WDL with tile (x, y)'s low-detail heights taken from `adt` (17 x 17 chunk corners, 16 x 16 chunk centres, in
/// yards; a new MARE + MAHO pair is appended and MAOF points at it), or cleared (MAOF 0) when `adt` is null. Empty
/// when the WDL has no MAOF.
std::vector<uint8_t> WdlSetTile(std::vector<uint8_t> wdl, int x, int y, const Adt* adt);

/// The minimap file md5translate.trs names for tile (x, y) of `map` (relative to textures\Minimap\), if any.
std::optional<std::string> TrsLookup(const std::vector<uint8_t>& trs, const std::string& map, int x, int y);
/// The index with tile (x, y) of `map` pointing at `file` (its line replaced, else added under the map's "dir:"
/// section, which is created at the end when missing).
std::vector<uint8_t> TrsSet(const std::vector<uint8_t>& trs, const std::string& map, int x, int y, const std::string& file);

/// A top-down picture of a map for the Maps panel: `pixels` per tile, 64 x 64 tiles, RGBA rows. Coloured by WDL
/// height: land green (sea level) -> brown (350 yd) -> pale rock (1400 yd), water (below 0) bright blue at the shore fading to dark in the deep;
/// tiles without terrain are transparent.
std::vector<uint8_t> MapPreview(const std::vector<std::vector<int16_t>>& wdl, int pixels);

/// An uncompressed BLP2 (ARGB8888, 8-bit alpha, no mipmaps; the client ships such files) from RGBA rows.
std::vector<uint8_t> WriteBlp(uint32_t width, uint32_t height, const uint8_t* rgba);

std::string Base64Encode(const uint8_t* data, size_t size);
std::vector<uint8_t> Base64Decode(const std::string& text);

/// Builds synthetic files and checks the parsers; false on the first mismatch.
bool FormatsSelfTest();
