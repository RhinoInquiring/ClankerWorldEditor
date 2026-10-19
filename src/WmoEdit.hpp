#pragma once

#include "Changes.hpp"
#include "Models.hpp"
#include "WmoCompose.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// A WMO's own doodads (the furniture inside a building: MODS sets, MODN names, MODD placements, each group's MODR
// list) as the editor edits them, written back into 3.3.5a v17 files. See docs/objects/objects.md.

/// The doodads of one WMO by stable id (the MODD index in the source file; new ones count on from there).
struct WmoDoodads
{
    struct Doodad
    {
        std::string model;                 // as MODN names it (.mdx or .m2)
        float pos[3] = {};                 // WoW axes, relative to the WMO
        float rot[4] = { 0, 0, 0, 1 };     // quaternion x, y, z, w
        float scale = 1;
        uint32_t color = 0xFF7F7F7F;       // BGRA
        uint8_t flags = 0;                 // MODD's top byte
        uint32_t set = 0;                  // index into sets
        std::vector<uint16_t> groups;      // slots whose MODR lists it; empty = the one whose box holds it (on write)
    };
    std::vector<std::string> sets;         // MODS names; set 0 always shows in the game
    std::map<uint32_t, Doodad> doodads;
    /// The groups, written in id order: the source's own are slots 0..n-1 (group i, no transform), pasted ones count on.
    std::map<uint32_t, WmoSlot> slots;
    uint32_t NextId() const { return doodads.empty() ? 0 : doodads.rbegin()->first + 1; }
    uint32_t NextSlotId() const { return slots.empty() ? 0 : slots.rbegin()->first + 1; }
    uint32_t NextBatch() const;
};

/// A doodad moved by `t` (position and turn, WoW axes).
void TransformDoodad(WmoDoodads::Doodad& d, const WmoTransform& t);
/// The output group (index into `boxes`) a doodad without groups belongs to: the smallest box holding its origin,
/// else the nearest; -1 when there are none.
int BoxGroupOf(const WmoDoodads::Doodad& d, const std::vector<WmoBox>& boxes);
/// MODS, MODN and MODD from `d`, and each output group's MODR (`indexOf` maps a doodad's slot ids to output groups,
/// -1 = gone). Counts for MOHD: names, definitions, sets.
void WmoDoodadChunks(const WmoDoodads& d, const std::vector<WmoBox>& boxes, const std::function<int(uint16_t)>& indexOf, std::vector<uint8_t>& mods,
                     std::vector<uint8_t>& modn, std::vector<uint8_t>& modd, std::vector<std::vector<uint16_t>>& modr, uint32_t counts[3]);

/// The doodads of a 3.3.5a root and its groups (groups by index; missing ones may be empty). None for a newer root
/// (FileDataID doodads or groups), which the editor converts on export instead.
std::optional<WmoDoodads> ReadWmoDoodads(const std::vector<uint8_t>& root, const std::vector<std::vector<uint8_t>>& groups, std::string& error);
/// Each group's bounding box from its MOGP header (WoW axes: min x y z, max x y z); zeros for a missing group.
std::vector<WmoBox> WmoGroupBoxes(const std::vector<std::vector<uint8_t>>& groups);
/// The root with MODS, MODN, MODD and the MOHD counts written from `d`; each group's MODR in `modr` (one list per
/// group, doodad indices in the written MODD order). A doodad without groups goes to the smallest box holding it,
/// else the nearest.
std::vector<uint8_t> WriteWmoRoot(const std::vector<uint8_t>& root, const WmoDoodads& d, const std::vector<WmoBox>& boxes,
                                  std::vector<std::vector<uint16_t>>& modr);
/// A group file with its MODR replaced (inserted after MOLR / MOBA, or removed when empty) and flag 0x800 to match.
std::vector<uint8_t> WriteWmoGroupModr(const std::vector<uint8_t>& group, const std::vector<uint16_t>& modr);
/// The doodads as ModelMesh lists in written order (sets laid out one after another), with each entry's id.
void MeshDoodads(const WmoDoodads& d, std::vector<ModelMesh::Doodad>& doodads, std::vector<ModelMesh::DoodadSet>& sets, std::vector<uint32_t>& ids);
/// A doodad's matrix inside its WMO (editor axes), and back: position, rotation and uniform scale from such a matrix.
DirectX::XMMATRIX DoodadMatrix(const WmoDoodads::Doodad& d);
void SetDoodadMatrix(WmoDoodads::Doodad& d, DirectX::FXMMATRIX m);

nlohmann::json ToJson(const WmoDoodads::Doodad& d);
WmoDoodads::Doodad DoodadFromJson(const nlohmann::json& j);

/// "World\\wmo\\Foo\\Inn.wmo" -> "World\\wmo\\Foo\\Inn_c1.wmo", the first number `taken` says is free.
std::string WmoCopyName(const std::string& root, const std::function<bool(const std::string&)>& taken);

/// Domain "wmo.doodads": a project copy of a WMO (or the WMO itself, edited in place) and edits to its doodads.
/// data: {"wmo": name, "from": source name, "create": true} registers it (copying the source's doodads);
///       {"wmo": name, "doodads": [[id, before|null, after|null]], "sets": [[index, before|null, after|null]],
///        "slots": [[id, before|null, after|null]]} edits it (slots: its groups, see WmoSlot).
/// Every applied WMO is written (ComposeWmo) as files under the project's wmos/ folder (game paths), served above the
/// game files. Sources are read as the game has them (the reader given to Reset skips the project's own files).
class WmoEditAdapter final : public Adapter
{
public:
    const char* Domain() const override { return "wmo.doodads"; }
    void Apply(const Change& change) override;
    void Revert(const Change& change) override;

    /// Where files come from (the project's base, read through the project's own files) and go. Clears every WMO and
    /// the folder: opening a project replays its changes into it.
    void Reset(const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read, const std::filesystem::path& dir);

    struct Edited
    {
        std::string name, from;
        WmoDoodads doodads;            // doodads, sets and the slots its groups are written from
        std::vector<WmoBox> boxes;     // output groups as last written: their boxes
        std::vector<uint32_t> order;   // and their slot ids
        std::vector<std::string> notes;
    };
    /// The WMO the project edits under this name (case-insensitive), or null.
    const Edited* Find(const std::string& name) const;
    const std::map<std::string, Edited>& All() const { return m_wmos; }
    /// Names whose files or doodads changed since the last call (the renderer refreshes them).
    std::set<std::string> TakeChanged() { return std::exchange(m_changed, {}); }
    /// Names whose groups changed shape (moved, added, cut) since the last call: the renderer reads them again.
    std::set<std::string> TakeRebuilt() { return std::exchange(m_rebuilt, {}); }
    /// Files written since the last call (the project's file layer is rebuilt).
    bool TakeFilesChanged() { return std::exchange(m_filesChanged, false); }
    /// A source's root and one of its groups as the compositor reads them (kept once read); null when missing.
    const std::vector<uint8_t>* SourceRoot(const std::string& source);
    const WmoGroupInfo* GroupInfo(const std::string& source, uint16_t group);
    /// Where a slot's group stands in the WMO (`base`: the file slots without `from` name).
    std::optional<WmoBox> SlotBox(const std::string& base, const WmoSlot& s);
    const std::filesystem::path& Dir() const { return m_dir; }

    /// The change that creates `name` from `from` (`name == from`: the original, edited in place). None, with `error`,
    /// when the source cannot be read or is a newer-client WMO.
    std::optional<Change> MakeCreate(const std::string& name, const std::string& from, std::string& error);
    /// An edit from `before` to `after` of one WMO (sets and doodads diffed); none when nothing differs.
    std::optional<Change> MakeEdit(const std::string& name, const WmoDoodads& before, const WmoDoodads& after, const std::string& label) const;

private:
    void Write(Edited& e);
    void Remove(const std::string& key);
    std::filesystem::path PathOf(const std::string& game) const;

    std::function<std::optional<std::vector<uint8_t>>(const std::string&)> m_read;
    std::filesystem::path m_dir;
    std::map<std::string, Edited> m_wmos;                  // by lower-case name
    std::map<std::string, std::vector<uint8_t>> m_roots;   // the source root each one was copied from
    struct Written { std::vector<std::string> keys, shapes; size_t count = 0; };   // per output group: plan keys last written
    std::map<std::string, Written> m_written;
    std::map<std::string, std::optional<std::vector<uint8_t>>> m_sources;   // source roots by lower-case name
    std::map<std::string, std::optional<WmoGroupInfo>> m_infos;              // "source|group"
    std::set<std::string> m_changed, m_rebuilt;
    bool m_filesChanged = false;
};

/// Read, edit, write and read back a synthetic WMO; the adapter's create, edit, undo and redo; false on a mismatch.
bool WmoEditSelfTest();
