#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/// One undoable edit. `data` is owned by the domain's adapter and holds both before and after.
struct Change
{
    uint64_t id = 0;
    std::string domain;   // e.g. "terrain.heights"
    std::string target;   // e.g. "Azeroth_32_48"
    std::string label;    // shown in the Changes panel
    std::string author;
    int64_t time = 0;     // unix seconds
    nlohmann::json data;
};

/// Something the Problems panel lists. A located problem (map + tile) jumps the camera there when clicked.
struct Problem
{
    enum class Severity { Error, Warning } severity = Severity::Error;
    std::string area;      // Terrain, Assets, Server ...
    std::string message;
    std::string map;       // empty = no location
    int tx = -1, ty = -1;
};

/// One per domain. Applies and reverts its own changes; see docs/concepts/change-model.md.
class Adapter
{
public:
    virtual ~Adapter() = default;
    virtual const char* Domain() const = 0;
    virtual void Apply(const Change& change) = 0;
    virtual void Revert(const Change& change) = 0;
};

class ChangeStore
{
public:
    void Register(Adapter& adapter) { m_adapters.push_back(&adapter); }

    /// The domain of a change made of parts (several tables in one undo step): data {"changes": [{domain, target, data}]}.
    static constexpr const char* kBatch = "batch";

    /// Records a change the tool has already applied. Clears redo.
    void Commit(Change change);
    /// Records parts the tool has already applied as one change (one undo step); one part is committed as itself.
    void Commit(std::vector<Change> parts, const std::string& label);
    /// Every change in `list` with batches opened up: fn(domain, data) per part, in order.
    static void ForEach(const std::vector<Change>& list, const std::function<void(const std::string& domain, const nlohmann::json& data)>& fn);
    /// The same as a list to loop over (references into `list`).
    struct Part { const std::string& domain; const nlohmann::json& data; };
    static std::vector<Part> Parts(const std::vector<Change>& list);
    /// One change's parts (references into it).
    static std::vector<Part> Parts(const Change& change);
    /// data[key] by reference, or an empty array when it has none. Use this, not data.value(key, array()): that copies
    /// the whole array (pasted texture layers are tens of kilobytes a chunk) on every read.
    static const nlohmann::json& List(const nlohmann::json& data, const char* key)
    {
        static const nlohmann::json empty = nlohmann::json::array();
        const auto it = data.find(key);
        return it == data.end() ? empty : *it;
    }

    bool CanUndo() const { return !m_done.empty(); }
    bool CanRedo() const { return !m_undone.empty(); }
    void Undo();
    void Redo();

    /// Applied changes, oldest first.
    const std::vector<Change>& Done() const { return m_done; }
    /// Undone changes, most recently undone last.
    const std::vector<Change>& Undone() const { return m_undone; }

    bool Dirty() const { return m_dirty; }
    /// Bumped by every commit, undo, redo, clear and load: caches keyed on it see any change.
    uint64_t Revision() const { return m_revision; }
    void Clear();

    /// Writes applied changes as changes/NNNNNN.json and removes files that are no longer applied.
    bool Save(const std::filesystem::path& dir, std::string& error);
    /// Reads changes/ in order and applies each through its adapter.
    bool Load(const std::filesystem::path& dir, std::string& error);

    std::string author;

private:
    Adapter* Find(const std::string& domain) const;
    void ApplyChange(const Change& change, bool apply) const;

    std::vector<Adapter*> m_adapters;
    std::vector<Change> m_done, m_undone;
    uint64_t m_nextId = 1;
    bool m_dirty = false;
    uint64_t m_revision = 0;
};

/// Commit, undo, redo, save and reload through a test adapter in a temp folder; false on a mismatch.
bool ChangesSelfTest();
