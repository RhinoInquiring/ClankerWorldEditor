#include "Project.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fs = std::filesystem;

bool Project::Save(std::string& error) const
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    nlohmann::json j = { { "name", name }, { "clientDir", clientDir }, { "author", author }, { "format", 1 } };
    if (!serverProfile.empty()) j["serverProfile"] = serverProfile;
    j["patchName"] = patchName;
    for (const auto& [kind, r] : idRanges) j["idRanges"][kind] = { r.first, r.last };
    auto source = [](const Source& s) {
        nlohmann::json layers = nlohmann::json::array();
        for (const MpqLayer& l : s.layers)
        {
            layers.push_back({ { "kind", MpqLayer::KindName(l.kind) }, { "path", l.path }, { "enabled", l.enabled }, { "installed", l.installed } });
            if (!l.from.empty()) layers.back()["from"] = l.from;
        }
        return nlohmann::json{ { "name", s.name }, { "layers", std::move(layers) } };
    };
    j["base"] = source(base);
    j["compare"] = nlohmann::json::array();
    for (const Source& s : compare) j["compare"].push_back(source(s));
    std::ofstream f(dir / "project.json");
    f << j.dump(2) << "\n";
    if (!f) { error = "Cannot write " + (dir / "project.json").string(); return false; }
    return true;
}

std::optional<Project> Project::Load(const fs::path& dir, std::string& error)
{
    try
    {
        std::ifstream f(dir / "project.json");
        if (!f) { error = "No project.json in " + dir.string(); return std::nullopt; }
        const nlohmann::json j = nlohmann::json::parse(f);
        Project p;
        p.dir = dir;
        p.name = j.value("name", dir.filename().string());
        p.clientDir = j.value("clientDir", "");
        p.author = j.value("author", "");
        p.serverProfile = j.value("serverProfile", "");
        p.patchName = j.value("patchName", p.patchName);
        if (j.contains("idRanges"))
            for (const auto& [kind, r] : j["idRanges"].items()) p.idRanges[kind] = { r.at(0).get<uint32_t>(), r.at(1).get<uint32_t>() };
        else   // format before ranges: an open start per table
            for (const char* kind : { "creature.guid", "gameobject.guid" })
            {
                const std::string key = std::string(kind) == "creature.guid" ? "creatureGuidStart" : "gameobjectGuidStart";
                if (j.contains(key)) p.idRanges[kind] = { j[key].get<uint32_t>(), j[key].get<uint32_t>() + 99999 };
            }
        auto source = [](const nlohmann::json& s) {
            Source out{ s.value("name", "") };
            for (const auto& l : s.value("layers", nlohmann::json::array()))
                out.layers.push_back({ MpqLayer::KindFrom(l.value("kind", "mpqfolder")), l.value("path", ""), l.value("enabled", true),
                                       l.value("installed", true), l.value("from", "") });
            return out;
        };
        if (j.contains("base")) p.base = source(j["base"]);
        if (p.base.layers.empty()) p.base = { "Client", { { MpqLayer::Kind::MpqFolder, p.clientDir } } };   // older projects: the client
        for (const auto& s : j.value("compare", nlohmann::json::array())) p.compare.push_back(source(s));
        // Older projects listed other clients as "sources" (a Data folder each).
        for (const auto& s : j.value("sources", nlohmann::json::array()))
            p.compare.push_back({ s.value("name", ""), { { MpqLayer::Kind::MpqFolder, s.value("dataDir", "") } } });
        return p;
    }
    catch (const std::exception& e)
    {
        error = "project.json: " + std::string(e.what());
        return std::nullopt;
    }
}

std::filesystem::path Project::PatchInstallPath() const
{
    // patch-enUS-Z.MPQ: the locale is the part between the first two dashes when it is four letters.
    std::string locale;
    if (const size_t a = patchName.find('-'), b = patchName.find('-', a + 1); a != std::string::npos && b != std::string::npos && b - a - 1 == 4)
        locale = patchName.substr(a + 1, 4);
    return locale.empty() ? DataDir() / patchName : DataDir() / locale / patchName;
}
