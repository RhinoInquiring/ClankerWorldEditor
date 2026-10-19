#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// A WMO put together from groups of WMOs (its own, others): each output group is a slot naming a source file and
// group with a transform. The root and the groups are written again from the slots: materials, textures, group
// names, lights and fogs a moved-in group uses come with it; portals stay where both their groups came in together.
// Transforms are quarter turns about the vertical plus an offset, so each group's collision tree (MOBN, axis-aligned
// splits) stays valid without being built again. 3.3.5a (v17) files only.

using WmoBox = std::array<float, 6>;   // WoW axes: min x y z, max x y z

/// Quarter turns about +z (counterclockwise seen from above), then an offset; WoW axes, the WMO's own space.
struct WmoTransform
{
    int turns = 0;
    float offset[3] = {};
    bool Identity() const { return (turns & 3) == 0 && offset[0] == 0 && offset[1] == 0 && offset[2] == 0; }
    void Vector(float v[3]) const;   // turned
    void Point(float p[3]) const;    // turned and moved
    WmoBox Box(const WmoBox& b) const;
    /// This, then `next`.
    WmoTransform Then(const WmoTransform& next) const;
    bool operator==(const WmoTransform& o) const;
    std::string Key() const;
};

/// One output group: group `group` of `from` (empty: the edited WMO's own source), placed by `at`.
struct WmoSlot
{
    std::string from;
    uint16_t group = 0;
    uint32_t batch = 0;          // groups pasted together share a batch: portals between them are kept
    WmoTransform at;
    int8_t outdoor = -1;         // -1 as the source has it, 0 indoor (0x2000), 1 outdoor (0x8)
};
nlohmann::json ToJson(const WmoSlot& s);
WmoSlot SlotFromJson(const nlohmann::json& j);

/// What the compositor needs from one source group file (read once, kept).
struct WmoGroupInfo
{
    uint32_t nameOffset = 0, descOffset = 0, flags = 0, uniqueId = 0, liquid = 0;
    std::string name;                     // its MOGN name (filled by whoever has the root)
    WmoBox box{};
    uint16_t portalStart = 0, portalCount = 0;
    uint8_t fogs[4] = {};
    std::set<uint16_t> materials;         // MOPY, MOBA and MLIQ material ids
    std::vector<uint16_t> lights;         // MOLR
    bool hasLiquid = false;
};
std::optional<WmoGroupInfo> ReadWmoGroupInfo(const std::vector<uint8_t>& group);

/// How one output group is written from its source group file.
struct WmoGroupPlan
{
    std::string source;                   // the file it comes from
    uint16_t group = 0;
    WmoTransform at;
    int8_t outdoor = -1;
    int64_t name = -1, descName = -1;     // new MOGN offsets (-1: as the source has them)
    std::vector<int> materials;           // source material id -> output (empty: the same)
    std::vector<int> lights;              // source light index -> output (empty: the same)
    std::optional<std::array<uint8_t, 4>> fogs;
    uint16_t portalStart = 0, portalCount = 0;
    std::optional<uint32_t> uniqueId;     // WMOAreaTable group id for a group that came from elsewhere
    std::vector<uint16_t> modr;
    /// Everything that decides its bytes: a group whose key did not change need not be written again.
    std::string Key() const;
};

struct WmoComposed
{
    std::vector<uint8_t> root;
    std::vector<WmoGroupPlan> groups;     // in output order (slots by id)
    std::vector<uint32_t> slots;          // the slot id of each output group
    std::vector<WmoBox> boxes;            // each output group's box
    std::vector<std::string> notes;       // what may look different (liquid kinds, vertex colours across flag sets)
};

struct WmoDoodads;
/// The WMO `base` (root bytes of the edited WMO's source, named `baseName`) rebuilt from `slots` (id -> slot, written
/// in id order) and `doodads` (whose groups name slot ids). `rootOf` and `infoOf` read source roots and group infos
/// (null when missing: that slot is left out, with a note).
WmoComposed ComposeWmo(const std::vector<uint8_t>& base, const std::string& baseName, const std::map<uint32_t, WmoSlot>& slots, const WmoDoodads& doodads,
                       const std::function<const std::vector<uint8_t>*(const std::string&)>& rootOf,
                       const std::function<const WmoGroupInfo*(const std::string&, uint16_t)>& infoOf);
/// One output group from its source group file.
std::vector<uint8_t> EmitWmoGroup(const WmoGroupPlan& plan, const std::vector<uint8_t>& source);

/// A collision tree (MOBN nodes) moved by `t`; a liquid (MLIQ body) moved by `t`.
void TransformBsp(std::vector<uint8_t>& mobn, const WmoTransform& t);
void TransformLiquid(std::vector<uint8_t>& mliq, const WmoTransform& t);
/// The leaf faces (MOBR) a point lands in, walking the tree; empty when a branch ends.
std::vector<uint16_t> BspFacesAt(const std::vector<uint8_t>& mobn, const std::vector<uint8_t>& mobr, const float p[3]);

bool WmoComposeSelfTest();
