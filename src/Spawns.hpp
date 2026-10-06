#pragma once

#include "Changes.hpp"

#include <DirectXMath.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class Db;

/// Which spawn table: world.creature or world.gameobject.
enum class SpawnKind { Creature, GameObject };

/// The fields of AzerothCore's world.creature / world.gameobject the editor shows and sets. Server coordinates:
/// x north, y west, z up.
struct Spawn
{
    SpawnKind kind = SpawnKind::Creature;
    uint32_t guid = 0, entry = 0;
    uint32_t map = 0, zoneId = 0, areaId = 0;
    float x = 0, y = 0, z = 0, orientation = 0;
    uint32_t spawnTime = 300;     // seconds
    float wander = 0;             // creature: yards; MovementType 1 (random) when above 0
    uint32_t movementType = 0;    // creature: 0 idle, 1 random, 2 waypoint path
    std::string name;             // template name, display only
    uint32_t displayId = 0;       // display id (creature: creature_template_model Idx 0), display only
    float size = 1;               // model scale (gameobject_template size / creature DisplayScale), display only
    uint32_t weapons[2] = {}, weaponTypes[2] = {};   // creature: main/off hand item displayid + InventoryType, display only
    std::vector<int> events;      // game_event_creature / _gameobject eventEntry (negative: gone during it), display only

    /// Whether the spawn is in the world while only `event` runs (0: none): an event's spawns appear only during
    /// it, negative ones leave during it.
    bool InWorld(int event) const;

    /// From a row (column -> value); missing columns keep the defaults above.
    static Spawn FromRow(const nlohmann::json& row, SpawnKind kind = SpawnKind::Creature);
    /// `base` (a full row, or empty for a new spawn) with the editor's fields set. A gameobject whose facing
    /// changed gets a yaw-only rotation quaternion (any tilt is dropped).
    nlohmann::json ToRow(nlohmann::json base = nlohmann::json::object()) const;
};

/// Editor axes <-> server coordinates (the editor's x runs with the tile column, z with the tile row, y up).
DirectX::XMFLOAT3 ServerToEditor(float x, float y, float z);
void EditorToServer(const DirectX::XMFLOAT3& p, float& x, float& y, float& z);

/// gameobject_template.type as its AzerothCore name ("CHEST", "MAILBOX", ...).
const char* GameObjectTypeName(uint32_t type);
/// creature_template.type as a name ("Beast", "Humanoid", ...); null when unknown.
const char* CreatureTypeName(uint32_t type);

/// Creature or gameobject spawns as project changes, written straight to the server's world database.
///
/// A change holds whole rows before and after (column -> value; "before" null = added, "after" null =
/// deleted), one row or several ("rows": a group edit, one undo step), so columns the editor does not show
/// survive moves, undo and deletes. Writes are idempotent
/// (INSERT ... ON DUPLICATE KEY UPDATE / DELETE by guid), so reopening a project can replay them. The running
/// worldserver only picks spawn changes up on restart: AzerothCore has no reload for single spawns.
class SpawnAdapter final : public Adapter
{
public:
    SpawnAdapter(ChangeStore& store, SpawnKind kind) : m_store(store), m_kind(kind) {}

    const char* Domain() const override { return m_kind == SpawnKind::Creature ? "world.creature" : "world.gameobject"; }
    /// The table: "creature" or "gameobject".
    const char* Table() const { return m_kind == SpawnKind::Creature ? "creature" : "gameobject"; }
    SpawnKind Kind() const { return m_kind; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }

    /// The database writes go to (null: none; Sync catches up once connected).
    void SetDb(Db* db) { m_db = db; Refresh(); }
    bool Connected() const;
    /// Writes the final state of every spawn the project touches; false with the error.
    bool Sync(std::string& error);

    /// Spawns of `map` in the server-coordinate box: the database's, with the project's own over them. Cached
    /// until the box changes or Refresh.
    const std::vector<Spawn>& Around(uint32_t map, float minX, float minY, float maxX, float maxY);
    void Refresh() { m_aroundKey.clear(); }
    /// Bumped whenever Around recomputes its list.
    uint32_t Version() const { return m_version; }

    /// `detail`: creature "<subname>  (levels)", gameobject its type name.
    struct Template
    {
        uint32_t entry; std::string name, detail; uint32_t displayId = 0; float size = 1;
        uint32_t weapons[2] = {}, weaponTypes[2] = {};   // creature: equipment set 1
        uint32_t category = 0;                           // creature_template.type / gameobject_template.type
    };
    /// Template rows whose name contains `text` (or whose entry is `text`), up to 60.
    std::vector<Template> Search(const std::string& text, std::string& error) const;
    /// Every template row (the catalog), by name.
    std::vector<Template> All(std::string& error) const { return Templates("1 = 1", 0, error); }

    /// The next guid of the range [first, last]: above every guid used there by the project (redo-able ones too) or
    /// the database; null when the range is full or unset. Guids are never reused.
    std::optional<uint32_t> NextGuid(uint32_t first, uint32_t last) const;
    /// How a guid range is used: rows this project added there, other database rows inside it (a clash with
    /// another project or module), and the highest guid in the whole table (for suggesting a free range).
    struct RangeUse { size_t mine = 0, others = 0; uint32_t highest = 0; };
    RangeUse Use(uint32_t first, uint32_t last) const;
    /// The project's id range kind of this table ("creature.guid", "gameobject.guid").
    std::string IdKind() const { return std::string(Table()) + ".guid"; }
    /// The full current row of a spawn: the project's state if it touched the guid, else the database's.
    std::optional<nlohmann::json> Row(uint32_t guid) const;

    /// One change from rows (either may be empty).
    Change MakeChange(const std::optional<nlohmann::json>& before, const std::optional<nlohmann::json>& after, const std::string& label) const;
    /// One change for several spawns (a group move, turn or delete): one undo step.
    Change MakeChange(const std::vector<std::pair<std::optional<nlohmann::json>, std::optional<nlohmann::json>>>& rows, const std::string& label) const;

    /// The project's net spawn changes as SQL for the world database (<table>_spawns.sql), plus the statements
    /// undoing them (<table>_spawns_revert.sql).
    bool ExportSql(const std::filesystem::path& outDir, std::string& error) const;
    /// guids the project added, moved/edited and deleted (for the status line).
    void Counts(size_t& added, size_t& changed, size_t& deleted) const;

    /// The last write error (cleared by a successful write).
    const std::string& LastError() const { return m_lastError; }

private:
    /// Template rows matching an SQL condition on the template table (alias t), by name; limit 0 = all.
    std::vector<Template> Templates(const std::string& where, size_t limit, std::string& error) const;
    struct RowChange { uint32_t guid; const nlohmann::json* before; const nlohmann::json* after; };
    /// The rows a change touches, in order.
    static std::vector<RowChange> Rows(const nlohmann::json& data);
    void Set(const Change& change, bool after);
    bool Write(uint32_t guid, const nlohmann::json& row, std::string& error) const;
    /// guid -> final row (null = deleted) over the applied changes, and the row before the project touched it.
    void NetState(std::map<uint32_t, nlohmann::json>& now, std::map<uint32_t, nlohmann::json>& original) const;

    ChangeStore& m_store;
    SpawnKind m_kind;
    Db* m_db = nullptr;
    uint32_t m_version = 0;
    std::string m_lastError;
    std::string m_aroundKey;
    std::vector<Spawn> m_around;
};
