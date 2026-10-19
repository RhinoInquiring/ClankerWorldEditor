#pragma once

#include "Changes.hpp"
#include "Mpq.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class DbcTable;
class RaceCatalog;
struct Project;

/// The creature models the project imported from other clients (Creature models window): one package each, keyed by
/// the project's CreatureModelData id. A package is {"source": {"client", "model", "path"}, "path": the model in the
/// project, "files": [{"path", "from", "textures": {old: new}}] export copies from the source client, "missingFiles",
/// "displays": {source display: project display}, "added": {"CreatureModelData": [ids], "CreatureDisplayInfo": [ids]}}.
class CreatureModelAdapter final : public Adapter
{
public:
    const char* Domain() const override { return "creaturemodel"; }
    void Apply(const Change& change) override { Set(change, true); }
    void Revert(const Change& change) override { Set(change, false); }
    const nlohmann::json* Package(uint32_t model) const { auto it = m_packages.find(model); return it == m_packages.end() ? nullptr : &it->second; }
    const std::map<uint32_t, nlohmann::json>& Packages() const { return m_packages; }
    /// The project model a source model was imported as (0: not imported).
    uint32_t ImportedAs(const std::string& client, uint32_t sourceModel) const;
    Change MakeChange(uint32_t model, const nlohmann::json& after, const std::string& label) const;
    void Clear() { m_packages.clear(); ++m_version; }
    uint64_t Version() const { return m_version; }

private:
    void Set(const Change& change, bool after);
    std::map<uint32_t, nlohmann::json> m_packages;
    uint64_t m_version = 0;
};

/// What importing model `sourceModel` (CreatureModelData id) of the client named `sourceName` takes, with its skins
/// `sourceDisplays` (CreatureDisplayInfo ids drawing it), as changes not yet applied: a new CreatureModelData row in the
/// project's "creaturemodeldata.id" range, one CreatureDisplayInfo row per skin in "creaturedisplayinfo.id", and the
/// package listing the files export copies. `source` reads the source's tables (1.12 or 3.3.5, rows by 3.3.5 names),
/// `client` the project's; `sourceFiles` / `projectFiles` their files. A file the project's client has with the same
/// bytes is not copied; one it has with other bytes makes the whole model (its .skin and .anim files, and every skin
/// texture, which sit beside it) move to Creature\<source>\..., so the client's own stay as they are. A 1.12 row gets the
/// effect scales it lacks (from the client's row of that model, else 1). Empty (and `error`) when a range is full, the
/// source has no such row, or the model was imported already.
std::vector<Change> ImportCreatureModelChanges(const RaceCatalog& source, const std::string& sourceName, uint32_t sourceModel,
                                               const std::vector<uint32_t>& sourceDisplays, const MpqChain& sourceFiles,
                                               const MpqChain& projectFiles, const RaceCatalog& client, const CreatureModelAdapter& adapter,
                                               const DbcTable& displays, const DbcTable& models, const Project& ranges, std::string& error);

/// What adding a model from loose files takes: the .m2 at `file`, with its .skin and .anim files beside it
/// (<model>NN.skin, <model>NNNN-NN.anim) and every .blp there, copied now into `assets`\<gameFolder>\ (the project's
/// assets: export puts them in the patch). A newer model is converted to 3.3.5a (texture file ids named through `client`,
/// which knows them when a newer client is attached as a source); a 1.12 one (version 256) is refused:
/// convert it with wow-upport first. A fixed texture the model names that the client lacks but the folder has (same
/// file name) is named in <gameFolder> instead. Changes: a CreatureModelData row in "creaturemodeldata.id" (collision
/// size from the model's box, effect scales 1), one CreatureDisplayInfo row in "creaturedisplayinfo.id" with the
/// folder's other textures as its skins, and a package ({"source": {"folder"}, "path", "added"}) so it can be removed
/// like an import. `notes` gets what the model names that nobody has. Empty (and `error`) when it cannot be read,
/// converted or loaded, or a range is full.
std::vector<Change> AddLooseCreatureModel(const std::filesystem::path& file, const std::string& gameFolder, const MpqChain& client,
                                          const std::filesystem::path& assets, const CreatureModelAdapter& adapter, const DbcTable& displays,
                                          const DbcTable& models, const Project& ranges, std::vector<std::string>& notes, std::string& error);

/// The project's files that sit in no archive, as a layer for the project's MpqChain (read first, as the patch will be):
/// every file of `assets` (by its path under it), and every file an import copies at export, read from its source
/// client (`source` finds it by name; files of a source not attached are left out) or, marked "fromClient", from
/// `client`, with the fixed texture names export renames. A file of the assets wins over an import's. Files of
/// `generated` (edited WMOs: WmoEditAdapter::Dir) are listed the same way and win over both.
std::shared_ptr<const MpqChain::ProjectFiles> ProjectFilesLayer(const std::filesystem::path& assets, const std::map<uint32_t, nlohmann::json>& packages,
                                                                const std::function<const MpqChain*(const std::string&)>& source, const MpqChain& client,
                                                                const std::filesystem::path& generated = {});

/// The changes removing an imported model: its package and the rows it added.
std::vector<Change> RemoveCreatureModelChanges(uint32_t model, const CreatureModelAdapter& adapter, const DbcTable& displays, const DbcTable& models);

/// Copies every package's files from its source client (`source` finds a client by name; null when it is not attached)
/// under `clientOut` (a file marked "fromClient" from `projectClient`), fixed texture names renamed as listed
/// and newer formats converted to 3.3.5a. `notes` gets what could not be done. False on a write error.
bool ExportCreatureModels(const std::map<uint32_t, nlohmann::json>& packages, const std::function<const MpqChain*(const std::string&)>& source, const MpqChain& projectClient,
                          const std::filesystem::path& clientOut, std::vector<std::string>& notes, std::string& error);
