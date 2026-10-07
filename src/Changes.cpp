#include "Changes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{
    std::string FileName(uint64_t id)
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%06llu.json", static_cast<unsigned long long>(id));
        return buf;
    }
}

Adapter* ChangeStore::Find(const std::string& domain) const
{
    for (Adapter* a : m_adapters)
        if (domain == a->Domain()) return a;
    return nullptr;
}

void ChangeStore::Commit(Change change)
{
    change.id = m_nextId++;
    change.author = author;
    change.time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_done.push_back(std::move(change));
    m_undone.clear();
    m_dirty = true;
    ++m_revision;
}

void ChangeStore::ApplyChange(const Change& c, bool apply) const
{
    if (c.domain == kBatch)
    {
        // Parts apply in order and revert in reverse.
        const auto& parts = c.data.at("changes");
        auto one = [&](const nlohmann::json& p) {
            Change part;
            part.domain = p.at("domain");
            part.target = p.value("target", "");
            part.label = c.label;
            part.data = p.at("data");
            if (Adapter* a = Find(part.domain)) apply ? a->Apply(part) : a->Revert(part);
        };
        if (apply) for (const auto& p : parts) one(p);
        else for (auto it = parts.rbegin(); it != parts.rend(); ++it) one(*it);
        return;
    }
    if (Adapter* a = Find(c.domain)) apply ? a->Apply(c) : a->Revert(c);
}

void ChangeStore::Commit(std::vector<Change> parts, const std::string& label)
{
    if (parts.empty()) return;
    if (parts.size() == 1)
    {
        parts[0].label = label;
        Commit(std::move(parts[0]));
        return;
    }
    Change batch;
    batch.domain = kBatch;
    batch.label = label;
    batch.target = parts.empty() ? std::string() : parts[0].target;
    batch.data = { { "changes", nlohmann::json::array() } };
    for (Change& p : parts) batch.data["changes"].push_back({ { "domain", p.domain }, { "target", p.target }, { "data", std::move(p.data) } });
    Commit(std::move(batch));
}

void ChangeStore::ForEach(const std::vector<Change>& list, const std::function<void(const std::string&, const nlohmann::json&)>& fn)
{
    for (const Change& c : list)
        if (c.domain == kBatch)
            for (const auto& p : c.data.at("changes")) fn(p.at("domain").get_ref<const std::string&>(), p.at("data"));
        else
            fn(c.domain, c.data);
}

std::vector<ChangeStore::Part> ChangeStore::Parts(const std::vector<Change>& list)
{
    std::vector<Part> out;
    for (const Change& c : list)
        if (c.domain == kBatch)
            for (const auto& p : c.data.at("changes")) out.push_back({ p.at("domain").get_ref<const std::string&>(), p.at("data") });
        else
            out.push_back({ c.domain, c.data });
    return out;
}

void ChangeStore::Undo()
{
    if (m_done.empty()) return;
    Change c = std::move(m_done.back());
    m_done.pop_back();
    ApplyChange(c, false);
    m_undone.push_back(std::move(c));
    m_dirty = true;
    ++m_revision;
}

void ChangeStore::Redo()
{
    if (m_undone.empty()) return;
    Change c = std::move(m_undone.back());
    m_undone.pop_back();
    ApplyChange(c, true);
    m_done.push_back(std::move(c));
    m_dirty = true;
    ++m_revision;
}

void ChangeStore::Clear()
{
    m_done.clear();
    m_undone.clear();
    m_nextId = 1;
    ++m_revision;
    m_dirty = false;
}

bool ChangeStore::Save(const fs::path& dir, std::string& error)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = "Cannot create " + dir.string() + ": " + ec.message(); return false; }

    std::set<std::string> keep;
    for (const Change& c : m_done)
    {
        const std::string name = FileName(c.id);
        keep.insert(name);
        const fs::path path = dir / name;
        if (fs::exists(path, ec)) continue;   // changes never mutate once committed
        nlohmann::json j = { { "id", c.id }, { "domain", c.domain }, { "target", c.target }, { "label", c.label },
                             { "author", c.author }, { "time", c.time }, { "data", c.data } };
        std::ofstream f(path);
        f << j.dump(1) << "\n";
        if (!f) { error = "Cannot write " + path.string(); return false; }
    }
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.path().extension() == ".json" && !keep.count(entry.path().filename().string()))
            fs::remove(entry.path(), ec);
    m_dirty = false;
    return true;
}

bool ChangeStore::Load(const fs::path& dir, std::string& error)
{
    Clear();
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    for (const auto& path : files)
    {
        try
        {
            std::ifstream f(path);
            const nlohmann::json j = nlohmann::json::parse(f);
            Change c{ j.at("id").get<uint64_t>(), j.at("domain"), j.at("target"), j.at("label"),
                      j.value("author", ""), j.value("time", int64_t(0)), j.at("data") };
            ApplyChange(c, true);
            m_nextId = std::max(m_nextId, c.id + 1);
            m_done.push_back(std::move(c));
        }
        catch (const std::exception& e)
        {
            error = path.filename().string() + ": " + e.what();
            return false;
        }
    }
    m_dirty = false;
    ++m_revision;
    return true;
}

bool ChangesSelfTest()
{
    struct Counter final : Adapter
    {
        int value = 0;
        const char* Domain() const override { return "test.counter"; }
        void Apply(const Change& c) override { value = c.data.at("after"); }
        void Revert(const Change& c) override { value = c.data.at("before"); }
    } counter;

    ChangeStore store;
    store.Register(counter);
    auto edit = [&](int to) {
        Change c;
        c.domain = counter.Domain();
        c.target = "t";
        c.label = "set";
        c.data = { { "before", counter.value }, { "after", to } };
        counter.value = to;   // tools apply before committing
        store.Commit(std::move(c));
    };
    edit(1);
    edit(2);
    edit(3);
    store.Undo();
    if (counter.value != 2 || store.Done().size() != 2 || store.Undone().size() != 1) return false;
    store.Redo();
    store.Undo();
    edit(5);   // a new edit drops the redo stack
    if (counter.value != 5 || store.CanRedo()) return false;

    const fs::path dir = fs::temp_directory_path() / "wow-world-editor-selftest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string error;
    if (!store.Save(dir, error) || store.Dirty()) return false;

    Counter fresh;
    ChangeStore reloaded;
    reloaded.Register(fresh);
    bool ok = reloaded.Load(dir, error) && fresh.value == 5 && reloaded.Done().size() == 3 && !reloaded.Dirty();

    // A batch: two domains in one change; undo reverts both, a reload replays both.
    struct Other final : Adapter
    {
        int value = 0;
        const char* Domain() const override { return "test.other"; }
        void Apply(const Change& c) override { value = c.data.at("after"); }
        void Revert(const Change& c) override { value = c.data.at("before"); }
    } other;
    store.Register(other);
    Change a{ 0, counter.Domain(), "t", "", "", 0, { { "before", counter.value }, { "after", 7 } } };
    Change b{ 0, other.Domain(), "t", "", "", 0, { { "before", other.value }, { "after", 9 } } };
    counter.value = 7;
    other.value = 9;
    store.Commit({ a, b }, "both");
    int parts = 0;
    ChangeStore::ForEach(store.Done(), [&](const std::string& domain, const nlohmann::json&) { parts += domain == "test.other"; });
    store.Undo();
    ok = ok && counter.value == 5 && other.value == 0 && parts == 1;
    store.Redo();
    ok = ok && counter.value == 7 && other.value == 9 && store.Save(dir, error);
    Counter fresh2;
    Other other2;
    ChangeStore again;
    again.Register(fresh2);
    again.Register(other2);
    ok = ok && again.Load(dir, error) && fresh2.value == 7 && other2.value == 9;
    fs::remove_all(dir, ec);
    return ok;
}
