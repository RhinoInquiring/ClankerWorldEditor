// Creature models imported from other clients: the package each import leaves (files to copy, rows it added), making
// the import's rows and file list, and copying the files at export.
#include "CreatureModels.hpp"

#include "Areas.hpp"
#include "Assets.hpp"
#include "Downport.hpp"
#include "Models.hpp"
#include "Mpq.hpp"
#include "Project.hpp"
#include "Races.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <unordered_map>

namespace
{
std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string S(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string Folder(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

/// A client's name as a folder: letters and digits only ("Turtle Client" -> "TurtleClient").
std::string FolderName(const std::string& client)
{
    std::string out;
    for (char c : client)
        if (std::isalnum((unsigned char)c)) out += c;
    return out.empty() ? std::string("Imported") : out;
}
}

uint32_t CreatureModelAdapter::ImportedAs(const std::string& client, uint32_t sourceModel) const
{
    for (const auto& [id, p] : m_packages)
    {
        const nlohmann::json& s = p.value("source", nlohmann::json::object());
        if (s.value("client", std::string()) == client && s.value("model", 0u) == sourceModel) return id;
    }
    return 0;
}

Change CreatureModelAdapter::MakeChange(uint32_t model, const nlohmann::json& after, const std::string& label) const
{
    Change c;
    c.domain = Domain();
    c.label = label;
    const nlohmann::json* before = Package(model);
    const nlohmann::json& shown = after.is_object() ? after : before ? *before : after;
    c.target = "creature model " + std::to_string(model) + (shown.is_object() ? " " + S(shown, "path") : "");
    c.data = { { "model", model }, { "before", before ? *before : nlohmann::json() }, { "after", after } };
    return c;
}

void CreatureModelAdapter::Set(const Change& change, bool after)
{
    const uint32_t model = change.data.at("model");
    const nlohmann::json& p = change.data.at(after ? "after" : "before");
    if (p.is_null()) m_packages.erase(model);
    else m_packages[model] = p;
    ++m_version;
}

std::vector<Change> ImportCreatureModelChanges(const RaceCatalog& source, const std::string& sourceName, uint32_t sourceModel,
                                               const std::vector<uint32_t>& sourceDisplays, const MpqChain& sourceFiles,
                                               const MpqChain& projectFiles, const RaceCatalog& client, const CreatureModelAdapter& adapter,
                                               const DbcTable& displays, const DbcTable& models, const Project& ranges, std::string& error)
{
    if (const uint32_t done = adapter.ImportedAs(sourceName, sourceModel))
    {
        error = "already imported as model " + std::to_string(done);
        return {};
    }
    nlohmann::json model = source.ModelRow(sourceModel);
    if (!model.is_object()) { error = "the source has no CreatureModelData row " + std::to_string(sourceModel); return {}; }
    const std::string modelName = S(model, "ModelName");
    const std::string m2 = M2Name(modelName);
    // A source of patches lacks what it did not change: those files are the project's client's (players have them).
    const auto bytes = sourceFiles.HasOwn(m2) ? sourceFiles.Read(m2) : projectFiles.Read(m2);
    if (!bytes) { error = "neither the source nor the project's client has " + m2; return {}; }
    const MpqChain& refsFrom = sourceFiles.HasOwn(m2) ? sourceFiles : projectFiles;

    // The model's files: its .skin and .anim files (they move with it), its fixed textures, and every skin texture of its
    // displays (named relative to the model's folder, so they move with it too).
    std::vector<std::string> group{ m2 }, fixed, skins;
    for (const std::string& ref : AssetReferences(refsFrom, m2, *bytes))
        (Lower(ref).ends_with(".blp") ? fixed : group).push_back(ref);
    std::vector<nlohmann::json> rows;
    for (uint32_t d : sourceDisplays)
    {
        nlohmann::json row = source.DisplayRow(d);
        if (!row.is_object() || row.value("ExtendedDisplayInfoID", 0u)) continue;   // humanoid looks are not models
        for (int i = 0; i < 3; ++i)
            if (const std::string t = S(row, ("TextureVariation[" + std::to_string(i) + "]").c_str()); !t.empty())
                if (std::find(skins.begin(), skins.end(), t) == skins.end()) skins.push_back(t);
        rows.push_back(std::move(row));
    }

    enum class State { Same, Copy, Clash, Missing };
    auto state = [&](const std::string& f) {
        if (!sourceFiles.HasOwn(f)) return projectFiles.HasOwn(f) ? State::Same : State::Missing;
        if (!projectFiles.HasOwn(f)) return State::Copy;
        const auto a = sourceFiles.Read(f), b = projectFiles.Read(f);
        if (!a || !b || *a != *b) return State::Clash;
        return projectFiles.HasInstalled(f) ? State::Same : State::Copy;
    };
    const std::string folder = Folder(m2);
    auto skinPath = [&](const std::string& name) { return folder + name + ".blp"; };
    bool clash = false;
    for (const std::string& f : group) clash = clash || state(f) == State::Clash;
    for (const std::string& t : skins) clash = clash || state(skinPath(t)) == State::Clash;
    for (const std::string& t : fixed) clash = clash || state(t) == State::Clash;   // its renamed texture names change the model's bytes
    // A clash moves the model under Creature\<source>\ (the rest of its path kept): the client's own model stays.
    const std::string tag = "Creature\\" + FolderName(sourceName) + "\\";
    auto moved = [&](const std::string& f) { return clash ? tag + (Lower(f).rfind("creature\\", 0) == 0 ? f.substr(9) : f) : f; };

    nlohmann::json files = nlohmann::json::array();
    size_t missing = 0;
    std::set<std::string> listed;
    auto add = [&](const std::string& path, const std::string& from, const nlohmann::json& textures = nlohmann::json()) {
        if (!listed.insert(Lower(path)).second) return;
        nlohmann::json e = { { "path", path }, { "from", from } };
        if (!sourceFiles.HasOwn(from)) e["fromClient"] = true;   // moved with the model, but only the project's client has it
        if (textures.is_object() && !textures.empty()) e["textures"] = textures;
        files.push_back(std::move(e));
    };
    // Fixed textures keep their names unless the client has other bytes under them.
    nlohmann::json textureRenames = nlohmann::json::object();
    for (const std::string& t : fixed)
    {
        const State s = state(t);
        if (s == State::Clash)
        {
            const std::string to = tag + (Lower(t).rfind("creature\\", 0) == 0 ? t.substr(9) : t);
            textureRenames[t] = to;
            add(to, t);
        }
        else if (s == State::Copy) add(t, t);
        else if (s == State::Missing) ++missing;
    }
    for (size_t i = 0; i < group.size(); ++i)
    {
        const State s = state(group[i]);
        if (s == State::Missing) { missing += i == 0; continue; }   // a model has up to 4 .skin files: only the model counts
        if (clash || s == State::Copy) add(moved(group[i]), group[i], i == 0 ? textureRenames : nlohmann::json());
    }
    for (const std::string& t : skins)
    {
        const State s = state(skinPath(t));
        if (s == State::Missing) ++missing;
        else if (clash || s == State::Copy) add(moved(skinPath(t)), skinPath(t));
    }

    // Ids: the lowest free in the project's ranges, never one the client or the project uses already.
    std::set<uint32_t> taken;
    auto next = [&](const char* range, const DbcTable& table) -> uint32_t {
        const Project::IdRange r = ranges.Range(range);
        for (uint32_t id = std::max(r.first, 1u); id && id <= r.last; ++id)
            if (!taken.count(id) && table.Row(id).is_null()) { taken.insert(id); return id; }
        error = std::string("the ") + range + " range is full";
        return 0;
    };
    const uint32_t modelId = next("creaturemodeldata.id", models);
    if (!modelId) return {};
    nlohmann::json modelRow = nlohmann::json::object();
    for (const DbcField& f : CreatureModelDataFields()) modelRow[f.name] = model.contains(f.name) ? model[f.name] : f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
    modelRow["ID"] = modelId;
    modelRow["ModelName"] = moved(modelName);
    // A 1.12 row has no effect scales (0: buffs, heals and spell hits drawn at no size): the client's row of the same model
    // has them, else 1.
    if (modelRow.value("WorldEffectScale", 0.0) == 0 && modelRow.value("AttachedEffectScale", 0.0) == 0)
    {
        const nlohmann::json same = client.ModelRow(client.ModelId(modelName));
        static const char* const kColumns[] = { "MountHeight", "GeoBoxMinX", "GeoBoxMinY", "GeoBoxMinZ", "GeoBoxMaxX", "GeoBoxMaxY", "GeoBoxMaxZ",
                                                "WorldEffectScale", "AttachedEffectScale", "MissileCollisionRadius", "MissileCollisionPush", "MissileCollisionRaise" };
        if (same.is_object() && same.value("AttachedEffectScale", 0.0) != 0)
            for (const char* c : kColumns) modelRow[c] = same.value(c, 0.0);
        else
        {
            modelRow["WorldEffectScale"] = 1.0;
            modelRow["AttachedEffectScale"] = 1.0;
        }
    }

    std::vector<Change> out;
    nlohmann::json added = { { "CreatureModelData", nlohmann::json::array({ modelId }) }, { "CreatureDisplayInfo", nlohmann::json::array() } };
    nlohmann::json displayFor = nlohmann::json::object();
    out.push_back(models.MakeChange(modelId, nullptr, modelRow, "import creature model"));
    for (const nlohmann::json& d : rows)
    {
        const uint32_t displayId = next("creaturedisplayinfo.id", displays);
        if (!displayId) return {};
        nlohmann::json row = nlohmann::json::object();
        for (const DbcField& f : CreatureDisplayInfoFields()) row[f.name] = d.contains(f.name) ? d[f.name] : f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
        row["ID"] = displayId;
        row["ModelID"] = modelId;
        out.push_back(displays.MakeChange(displayId, nullptr, row, "import creature model"));
        added["CreatureDisplayInfo"].push_back(displayId);
        displayFor[std::to_string(d.value("ID", 0u))] = displayId;
    }
    nlohmann::json p = { { "source", { { "client", sourceName }, { "model", sourceModel }, { "path", modelName } } },
                         { "path", moved(modelName) }, { "files", files }, { "missingFiles", missing }, { "displays", displayFor }, { "added", added } };
    out.insert(out.begin(), adapter.MakeChange(modelId, p, "import creature model"));
    return out;
}

namespace
{
/// The fixed texture names of a 3.3.5a model (textures of type 0 with a name).
std::vector<std::string> FixedTextures(const std::vector<uint8_t>& m2)
{
    std::vector<std::string> out;
    uint32_t count = 0, offset = 0;
    if (m2.size() < 0x58) return out;
    std::memcpy(&count, m2.data() + 0x50, 4);
    std::memcpy(&offset, m2.data() + 0x54, 4);
    for (uint32_t i = 0; i < count && size_t(offset) + (i + 1) * 16 <= m2.size(); ++i)
    {
        uint32_t type = 0, len = 0, at = 0;
        std::memcpy(&type, m2.data() + offset + i * 16, 4);
        std::memcpy(&len, m2.data() + offset + i * 16 + 8, 4);
        std::memcpy(&at, m2.data() + offset + i * 16 + 12, 4);
        if (type || len < 2 || size_t(at) + len > m2.size()) continue;
        out.emplace_back(reinterpret_cast<const char*>(m2.data() + at), strnlen(reinterpret_cast<const char*>(m2.data() + at), len));
    }
    return out;
}
}

std::vector<Change> AddLooseCreatureModel(const std::filesystem::path& file, const std::string& gameFolderIn, const MpqChain& client,
                                          const std::filesystem::path& assets, const CreatureModelAdapter& adapter, const DbcTable& displays,
                                          const DbcTable& models, const Project& ranges, std::vector<std::string>& notes, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string gameFolder = gameFolderIn;
    std::replace(gameFolder.begin(), gameFolder.end(), '/', '\\');
    if (!gameFolder.empty() && gameFolder.back() != '\\') gameFolder += '\\';
    if (gameFolder.empty()) { error = "no game folder"; return {}; }
    const auto m2 = ReadFileBytes(file);
    if (!m2 || m2->size() < 8) { error = "cannot read " + file.string(); return {}; }
    const std::string stem = file.stem().string();
    const fs::path dir = file.parent_path();
    const std::string gameName = gameFolder + stem + ".m2";
    if (std::memcmp(m2->data(), "MD20", 4) == 0)
    {
        uint32_t version = 0;
        std::memcpy(&version, m2->data() + 4, 4);
        if (version == 256) { error = "a 1.12 model (version 256): convert it to 3.3.5 with wow-upport first"; return {}; }
        if (version < 264) { error = "M2 version " + std::to_string(version) + " (before 3.3.5's 264) is not converted"; return {}; }
    }
    else if (std::memcmp(m2->data(), "MD21", 4) != 0) { error = file.filename().string() + " is not an M2 model"; return {}; }

    // The files beside it, by game path in the new folder: its .skin and .anim files and every texture.
    std::map<std::string, fs::path> beside;   // lower-case file name -> file
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file()) beside[Lower(e.path().filename().string())] = e.path();
    const std::string lowerStem = Lower(stem);
    auto read = [&](const std::string& game) -> std::optional<std::vector<uint8_t>> {   // a game path of the new folder, from disk
        if (Lower(game).rfind(Lower(gameFolder), 0) == 0)
            if (const auto it = beside.find(Lower(game.substr(gameFolder.size()))); it != beside.end()) return ReadFileBytes(it->second);
        return client.Read(game);
    };
    std::vector<std::string> lossNotes;
    auto convert = [&](const std::string& game, std::vector<uint8_t> bytes) {
        return IsNewerFormat(game, bytes) ? Downport(game, std::move(bytes), [&client](uint32_t id) { return client.NameOf(id); }, read, lossNotes) : bytes;
    };
    std::vector<uint8_t> model = convert(gameName, *m2);
    if (model.empty()) { error = "cannot convert " + file.filename().string() + (lossNotes.empty() ? "" : ": " + lossNotes.back()); return {}; }

    // Fixed textures: the client's are used as they are; one only the folder has is named in the new folder.
    std::map<std::string, std::string> renames;
    std::set<std::string> fixedFiles;   // lower-case file names of the folder used as fixed textures
    for (const std::string& t : FixedTextures(model))
    {
        if (client.HasOwn(t)) continue;
        const std::string name = t.substr(t.find_last_of("\\/") + 1);
        if (beside.count(Lower(name))) { renames[t] = gameFolder + name; fixedFiles.insert(Lower(name)); }
        else notes.push_back(stem + ": texture " + t + " is in neither the client nor the folder");
    }
    if (!renames.empty()) model = RenameM2Textures(std::move(model), renames);

    // Copy: the model, its skins and animations (converted with it), every texture of the folder.
    std::vector<std::string> skins;   // the folder's textures that are not fixed ones: the display's skins
    auto write = [&](const std::string& name, const std::vector<uint8_t>& bytes) {
        std::string rel = gameFolder + name;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const fs::path target = assets / rel;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return bool(out);
    };
    if (!write(stem + ".m2", model)) { error = "cannot write into " + assets.string(); return {}; }
    size_t copied = 1;
    for (const auto& [lower, path] : beside)
    {
        const std::string name = path.filename().string();
        const bool skinFile = lower.rfind(lowerStem, 0) == 0 && lower.ends_with(".skin");
        const bool animFile = lower.rfind(lowerStem, 0) == 0 && lower.ends_with(".anim");
        const bool texture = lower.ends_with(".blp");
        if (!skinFile && !animFile && !texture) continue;
        auto bytes = ReadFileBytes(path);
        if (!bytes) continue;
        if (skinFile || animFile) *bytes = convert(gameFolder + name, std::move(*bytes));
        if (texture && !fixedFiles.count(lower)) skins.push_back(name.substr(0, name.size() - 4));
        if (!bytes->empty() && write(name, *bytes)) ++copied;
    }
    // It must load as the client will find it: the new folder over the client.
    auto fromAssets = [&](const std::string& game) -> std::optional<std::vector<uint8_t>> {
        std::string rel = game;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (auto bytes = ReadFileBytes(assets / rel)) return bytes;
        return client.Read(game);
    };
    const auto mesh = LoadM2(gameName, fromAssets);
    if (!mesh) { error = stem + " does not load as a 3.3.5a model (its .skin missing or unreadable?); its files are in assets\\" + gameFolder; return {}; }
    for (const std::string& n : lossNotes) notes.push_back(stem + ": " + n);
    notes.push_back(stem + ": " + std::to_string(copied) + " file(s) copied into assets\\" + gameFolder);

    // Rows: the model and one skin.
    std::set<uint32_t> taken;
    auto next = [&](const char* range, const DbcTable& table) -> uint32_t {
        const Project::IdRange r = ranges.Range(range);
        for (uint32_t id = std::max(r.first, 1u); id && id <= r.last; ++id)
            if (!taken.count(id) && table.Row(id).is_null()) { taken.insert(id); return id; }
        error = std::string("the ") + range + " range is full";
        return 0;
    };
    const uint32_t modelId = next("creaturemodeldata.id", models), displayId = modelId ? next("creaturedisplayinfo.id", displays) : 0;
    if (!displayId) return {};
    nlohmann::json modelRow = nlohmann::json::object();
    for (const DbcField& f : CreatureModelDataFields()) modelRow[f.name] = f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
    const float dx = mesh->boundsMax.x - mesh->boundsMin.x, dy = mesh->boundsMax.y - mesh->boundsMin.y, dz = mesh->boundsMax.z - mesh->boundsMin.z;
    modelRow["ID"] = modelId;
    modelRow["ModelName"] = gameFolder + stem + ".mdx";   // as the client's rows name models
    modelRow["ModelScale"] = 1.0;
    modelRow["CollisionWidth"] = std::max(dx, dz) * 0.5;
    modelRow["CollisionHeight"] = dy;
    modelRow["WorldEffectScale"] = 1.0;
    modelRow["AttachedEffectScale"] = 1.0;
    nlohmann::json displayRow = nlohmann::json::object();
    for (const DbcField& f : CreatureDisplayInfoFields()) displayRow[f.name] = f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
    displayRow["ID"] = displayId;
    displayRow["ModelID"] = modelId;
    displayRow["CreatureModelScale"] = 1.0;
    uint32_t slots = 0;
    for (const auto& b : mesh->batches)
        if (b.textureType >= 11 && b.textureType <= 13) slots |= 1u << (b.textureType - 11);
    for (uint32_t i = 0; i < 3 && !skins.empty(); ++i)
        if (slots >> i & 1) displayRow["TextureVariation[" + std::to_string(i) + "]"] = skins[std::min<size_t>(i, skins.size() - 1)];
    if (slots && skins.empty()) notes.push_back(stem + ": the model has skin slots but the folder has no other texture for them");

    nlohmann::json p = { { "source", { { "folder", dir.string() } } }, { "path", S(modelRow, "ModelName") }, { "files", nlohmann::json::array() },
                         { "added", { { "CreatureModelData", nlohmann::json::array({ modelId }) }, { "CreatureDisplayInfo", nlohmann::json::array({ displayId }) } } } };
    return { adapter.MakeChange(modelId, p, "add creature model"), models.MakeChange(modelId, nullptr, modelRow, "add creature model"),
             displays.MakeChange(displayId, nullptr, displayRow, "add creature model") };
}

std::shared_ptr<const MpqChain::ProjectFiles> ProjectFilesLayer(const std::filesystem::path& assets, const std::map<uint32_t, nlohmann::json>& packages,
                                                                const std::function<const MpqChain*(const std::string&)>& source, const MpqChain& client,
                                                                const std::filesystem::path& generated)
{
    namespace fs = std::filesystem;
    struct File { fs::path disk; const MpqChain* chain = nullptr; std::string from; std::map<std::string, std::string> renames; };
    auto files = std::make_shared<std::unordered_map<std::string, File>>();
    auto key = [](std::string s) {
        std::replace(s.begin(), s.end(), '/', '\\');
        return Lower(s);
    };
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(assets, ec))
        if (e.is_regular_file()) (*files)[key(fs::relative(e.path(), assets, ec).string())].disk = e.path();
    if (!generated.empty())
        for (const auto& e : fs::recursive_directory_iterator(generated, ec))
            if (e.is_regular_file()) (*files)[key(fs::relative(e.path(), generated, ec).string())] = File{ e.path() };
    for (const auto& [id, p] : packages)
    {
        const MpqChain* from = source(p.value("source", nlohmann::json::object()).value("client", std::string()));
        for (const nlohmann::json& f : ChangeStore::List(p, "files"))
        {
            File file;
            file.from = S(f, "from");
            file.chain = f.value("fromClient", false) ? &client : from;
            const std::string path = key(S(f, "path"));
            if (!file.chain || (file.chain == &client && key(file.from) == path)) continue;   // its source is not attached; or it would read itself
            if (const auto it = f.find("textures"); it != f.end() && it->is_object())
                for (const auto& [a, b] : it->items()) file.renames[a] = b.get<std::string>();
            files->try_emplace(path, std::move(file));   // the assets win: export copies them last
        }
    }
    auto layer = std::make_shared<MpqChain::ProjectFiles>();
    layer->has = [files](const std::string& path) { return files->count(path) != 0; };
    layer->read = [files](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        const auto it = files->find(path);
        if (it == files->end()) return std::nullopt;
        const File& f = it->second;
        if (!f.disk.empty()) return ReadFileBytes(f.disk);
        auto bytes = f.chain->Read(f.from);
        if (bytes && !f.renames.empty()) *bytes = RenameM2Textures(std::move(*bytes), f.renames);
        return bytes;
    };
    return layer;
}

std::vector<Change> RemoveCreatureModelChanges(uint32_t model, const CreatureModelAdapter& adapter, const DbcTable& displays, const DbcTable& models)
{
    const nlohmann::json* p = adapter.Package(model);
    if (!p) return {};
    std::vector<Change> out{ adapter.MakeChange(model, nullptr, "remove creature model") };
    const nlohmann::json added = p->value("added", nlohmann::json::object());   // a local: a temporary dies before the loop
    for (const nlohmann::json& id : ChangeStore::List(added, "CreatureDisplayInfo"))
        if (const nlohmann::json& row = displays.Row(id.get<uint32_t>()); !row.is_null())
            out.push_back(displays.MakeChange(id.get<uint32_t>(), row, nullptr, "remove creature model"));
    for (const nlohmann::json& id : ChangeStore::List(added, "CreatureModelData"))
        if (const nlohmann::json& row = models.Row(id.get<uint32_t>()); !row.is_null())
            out.push_back(models.MakeChange(id.get<uint32_t>(), row, nullptr, "remove creature model"));
    return out;
}

bool ExportCreatureModels(const std::map<uint32_t, nlohmann::json>& packages, const std::function<const MpqChain*(const std::string&)>& source, const MpqChain& projectClient,
                          const std::filesystem::path& clientOut, std::vector<std::string>& notes, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& [id, p] : packages)
    {
        const std::string client = p.value("source", nlohmann::json::object()).value("client", std::string());
        const nlohmann::json& files = ChangeStore::List(p, "files");
        if (files.empty()) continue;
        const MpqChain* from = source(client);
        const std::string what = "creature model " + std::to_string(id) + " (" + S(p, "path") + ")";
        if (!from) { notes.push_back(what + ": its client \"" + client + "\" is not a source now: its " + std::to_string(files.size()) + " file(s) were not copied"); continue; }
        size_t lost = 0, converted = 0;
        std::vector<std::string> lossNotes;
        for (const nlohmann::json& f : files)
        {
            const std::string name = S(f, "from");
            const MpqChain* reader = f.value("fromClient", false) ? &projectClient : from;
            auto bytes = reader->Read(name);
            if (!bytes) { ++lost; continue; }
            if (IsNewerFormat(name, *bytes))
            {
                *bytes = Downport(name, std::move(*bytes), [from](uint32_t fdid) { return from->NameOf(fdid); },
                                  [from](const std::string& path) { return from->Read(path); }, lossNotes);
                if (bytes->empty()) { ++lost; continue; }
                ++converted;
            }
            if (f.contains("textures"))
            {
                std::map<std::string, std::string> names;
                for (const auto& [a, b] : f["textures"].items()) names[a] = b.get<std::string>();
                *bytes = RenameM2Textures(std::move(*bytes), names);
            }
            std::string rel = S(f, "path");
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const fs::path target = clientOut / fs::path(rel);
            fs::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
            if (!out) { error = "Cannot write " + target.string(); return false; }
        }
        if (converted) notes.push_back(what + ": " + std::to_string(converted) + " file(s) converted to 3.3.5a" + (lossNotes.empty() ? "" : " (" + lossNotes.front() + ")"));
        if (lost) notes.push_back(what + ": " + std::to_string(lost) + " file(s) could not be read from " + client);
    }
    return true;
}
