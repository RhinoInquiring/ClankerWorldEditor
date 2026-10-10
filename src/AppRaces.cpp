// Races window: the races of every source (the project's client and the compare sources), read in each client's own
// layout, with what they are and what their characters can look like, a preview of the choices, and importing a race
// into the project (renumbered into the project's id ranges, one undo step).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
const ImVec4 kNew{ 0.45f, 0.85f, 0.55f, 1.00f };
const ImVec4 kProject{ 0.45f, 0.70f, 1.00f, 1.00f };

/// A race name without the game's colour codes (|cAARRGGBB ... |r), as some clients write them.
std::string Plain(const std::string& name)
{
    std::string out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '|' && i + 1 < name.size() && (name[i + 1] == 'c' || name[i + 1] == 'C') && i + 10 <= name.size()) { i += 9; continue; }
        if (name[i] == '|' && i + 1 < name.size() && (name[i + 1] == 'r' || name[i + 1] == 'R')) { ++i; continue; }
        out += name[i];
    }
    return out;
}

const char* ClassName(uint32_t c)
{
    static const char* const kNames[] = { "", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight", "Shaman", "Mage", "Warlock", "", "Druid" };
    return c < std::size(kNames) && *kNames[c] ? kNames[c] : "?";
}

/// Steps `value` to the next (+1) or previous (-1) entry of `list`, wrapping; the first entry when it is not in it.
uint32_t Step(const std::vector<uint32_t>& list, uint32_t value, int dir)
{
    if (list.empty()) return 0;
    const auto it = std::find(list.begin(), list.end(), value);
    if (it == list.end()) return list.front();
    const long i = long(it - list.begin()) + dir, n = long(list.size());
    return list[size_t((i % n + n) % n)];
}

/// The races of a catalog that characters can be (they have classes).
std::vector<const RaceCatalog::Race*> Playable(const RaceCatalog& races)
{
    std::vector<const RaceCatalog::Race*> out;
    for (const RaceCatalog::Race& r : races.Races())
        if (!races.Classes(r.id).empty()) out.push_back(&r);
    return out;
}

/// A faction template as "id: faction name  (races using it)", by the project's client.
std::string FactionLabel(const RaceCatalog& races, uint32_t id)
{
    std::string users;
    for (const RaceCatalog::Race* r : Playable(races))
        if (r->faction == id) users += (users.empty() ? "" : ", ") + Plain(r->name);
    const std::string name = races.FactionName(id);
    return std::to_string(id) + ": " + (name.empty() ? "not in the project's client" : name) + (users.empty() ? "" : "  (" + users + ")");
}

/// Picks a faction template among the playable races' (and `extra`, e.g. a source race's own); true when it changed.
bool FactionCombo(const RaceCatalog& races, uint32_t& faction, uint32_t extra = 0)
{
    bool changed = false;
    ImGui::SetNextItemWidth(360);
    if (ImGui::BeginCombo("Faction template", FactionLabel(races, faction).c_str()))
    {
        std::set<uint32_t> shown;
        for (const RaceCatalog::Race* r : Playable(races))
            if (shown.insert(r->faction).second && ImGui::Selectable(FactionLabel(races, r->faction).c_str(), faction == r->faction))
                faction = r->faction, changed = true;
        if (extra && !shown.count(extra) && ImGui::Selectable((FactionLabel(races, extra) + "  (the source's)").c_str(), faction == extra))
            faction = extra, changed = true;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(kQuiet, "(?)");
    ImGui::SetItemTooltip("The faction a new character belongs to: reputations, and which NPCs are friendly.");
    if (races.FactionName(faction).empty()) ImGui::TextColored(kWarn, "Faction template %u is not in the project's client: pick one that is.", faction);
    return changed;
}
}

void App::RefreshRacePreview(bool frame)
{
    RacesView& v = m_races;
    v.stale = false;
    v.look.reset();
    v.pose.reset();
    if (v.previewSource >= v.sources.size() || !v.previewRace) return;
    RacesView::Source& src = v.sources[v.previewSource];
    if (!src.mpq || src.races.Format() == RaceCatalog::Layout::None) return;
    if (v.previewMpq != src.mpq || !v.models)
    {
        v.previewMpq = src.mpq;
        v.models.reset();
        v.renderer = std::make_unique<Renderer>();
        std::string error;
        if (!v.renderer->Init(m_device, m_context, error)) { Log("Races preview: %s", error.c_str()); v.renderer.reset(); return; }
        v.models = std::make_unique<ModelRenderer>();
        if (!v.models->Init(m_device, m_context, *v.renderer, error)) { Log("Races preview: %s", error.c_str()); v.models.reset(); return; }
        v.looks = std::make_unique<DisplayLooks>(*src.mpq);
        v.looks->SetUpload([this](const std::string& name, const BlpImage& image) { m_races.renderer->CacheTexture(name, image); });
    }
    // The choices kept valid for this race and sex (faces depend on the skin, hair colours on the style).
    const uint32_t race = v.previewRace;
    DisplayLooks::Choices c = v.looks->CharacterChoices(race, v.sex, v.skin, v.hairStyle);
    if (std::find(c.skins.begin(), c.skins.end(), v.skin) == c.skins.end()) v.skin = c.skins.empty() ? 0 : c.skins.front();
    if (std::find(c.hairStyles.begin(), c.hairStyles.end(), v.hairStyle) == c.hairStyles.end()) v.hairStyle = c.hairStyles.empty() ? 0 : c.hairStyles.front();
    c = v.looks->CharacterChoices(race, v.sex, v.skin, v.hairStyle);
    if (std::find(c.faces.begin(), c.faces.end(), v.face) == c.faces.end()) v.face = c.faces.empty() ? 0 : c.faces.front();
    if (std::find(c.hairColors.begin(), c.hairColors.end(), v.hairColor) == c.hairColors.end()) v.hairColor = c.hairColors.empty() ? 0 : c.hairColors.front();
    if (std::find(c.facialHair.begin(), c.facialHair.end(), v.facial) == c.facialHair.end()) v.facial = c.facialHair.empty() ? 0 : c.facialHair.front();

    v.look = v.looks->CharacterLook(race, v.sex, v.skin, v.face, v.hairStyle, v.hairColor, v.facial);
    if (!v.look) return;
    const auto info = v.models->Info(v.look->look.model, *src.mpq);
    if (!info) { v.look.reset(); return; }
    if (const auto& skel = info->skeleton; skel && !skel->sequences.empty())
    {
        int stand = 0;
        for (size_t i = 0; i < skel->sequences.size(); ++i)
            if (skel->sequences[i].id == 0) { stand = int(i); break; }
        const MpqChain* mpq = src.mpq;
        v.pose = LoadSkeleton(M2Name(v.look->look.model), [mpq](const std::string& path) { return mpq->Read(path); }, stand);
    }
    if (frame) v.scene.Frame(info->boundsMin, info->boundsMax, v.look->scale);
}

void App::DrawRaces()
{
    if (!m_showRaces) return;
    ImGui::SetNextWindowSize({ 1500, 820 }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Races", &m_showRaces)) { ImGui::End(); return; }
    if (!m_project) { ImGui::TextColored(kQuiet, "Open a project first."); ImGui::End(); return; }
    RacesView& v = m_races;

    // The sources as attached now; a changed list starts over.
    const auto& ghosts = m_ghosts.Sources();
    bool same = ghosts.size() == v.sources.size();
    for (size_t i = 0; same && i < ghosts.size(); ++i) same = ghosts[i].mpq == v.sources[i].mpq && ghosts[i].name == v.sources[i].name;
    if (!same)
    {
        v.sources.clear();
        for (const Ghosts::Source& g : ghosts) v.sources.push_back({ g.name, g.mpq });
        v.source = std::min(v.source, v.sources.empty() ? size_t(0) : v.sources.size() - 1);
        v.race = 0;
        v.previewMpq = nullptr;
        v.models.reset();
        v.looks.reset();
        v.renderer.reset();
        v.stale = true;
        v.projectKey[0] = ~0ull;
    }
    if (v.sources.empty()) { ImGui::TextColored(kQuiet, "No sources."); ImGui::End(); return; }
    auto read = [](RacesView::Source& s) {
        if (s.read) return;
        s.read = true;
        if (s.mpq) s.races.Load(*s.mpq, s.error);
        else s.error = "not attached";
    };
    read(v.sources[0]);   // the project's client: what "new" is measured against
    RacesView::Source& src = v.sources[v.source];
    read(src);

    // The project's races: its client's, with the races the project imported or changed over them.
    if (v.projectKey[0] != m_raceRows.Version() || v.projectKey[1] != m_displayRows.Version() || v.projectKey[2] != m_modelRows.Version())
    {
        v.projectKey[0] = m_raceRows.Version();
        v.projectKey[1] = m_displayRows.Version();
        v.projectKey[2] = m_modelRows.Version();
        v.project = v.sources[0].races;
        v.project.SetModelLookup([this](uint32_t display) -> std::string {
            const nlohmann::json* d = m_displayRows.Edited(display);
            if (!d || !d->is_object()) return {};
            const nlohmann::json& m = m_modelRows.Row(d->value("ModelID", 0u));
            return m.is_object() ? m.value("ModelName", "") : std::string();
        });
        for (const auto& [id, package] : m_raceRows.Packages()) v.project.Apply(id, package);
        v.stale = true;
    }
    const RaceCatalog& base = v.project;
    const RaceCatalog& list = v.source == 0 ? v.project : src.races;

    // Left: the source and its races.
    ImGui::BeginChild("##racelist", { 340, 0 }, ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##source", (src.name + (v.source == 0 ? "  (project)" : "")).c_str()))
    {
        for (size_t i = 0; i < v.sources.size(); ++i)
            if (ImGui::Selectable((v.sources[i].name + (i == 0 ? "  (project)" : "")).c_str(), i == v.source) && i != v.source)
            {
                v.source = i;
                v.race = 0;
                v.stale = true;
            }
        ImGui::EndCombo();
    }
    if (!src.error.empty()) ImGui::TextColored(kWarn, "%s", src.error.c_str());
    else ImGui::TextColored(kQuiet, "%s layout, %zu races", RaceCatalog::LayoutName(src.races.Format()), list.Races().size());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##racefilter", "Filter by name or file string", &v.filter);
    std::string filter = v.filter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    auto select = [&](uint32_t race) {
        v.race = race;
        v.sex = 0;
        v.stale = true;
        v.missingTextures = 0;
        if (v.source != 0 && src.mpq)
            for (const std::string& file : src.races.Files(race))
                if (!src.mpq->HasOwn(file)) ++v.missingTextures;
    };
    if (ImGui::BeginTable("##races", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, { 0, 0 }))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Race", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Team");
        ImGui::TableSetupColumn("Skins M/F");
        ImGui::TableHeadersRow();
        for (const RaceCatalog::Race& r : list.Races())
        {
            const std::string name = Plain(r.name);
            std::string hay = name + " " + r.fileString;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (!filter.empty() && hay.find(filter) == std::string::npos) continue;
            const bool isNew = v.source != 0 && std::none_of(base.Races().begin(), base.Races().end(), [&](const RaceCatalog::Race& b) { return Plain(b.name) == name; });
            const bool isProject = v.source == 0 && m_raceRows.Package(r.id);
            const RaceCatalog::Counts m = list.Count(r.id, 0), f = list.Count(r.id, 1);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", r.id);
            ImGui::TableNextColumn();
            if (ImGui::Selectable((name + "##" + std::to_string(r.id)).c_str(), v.race == r.id, ImGuiSelectableFlags_SpanAllColumns)) select(r.id);
            if (isNew || isProject)
            {
                ImGui::SameLine();
                ImGui::TextColored(isProject ? kProject : kNew, isProject ? "project" : "new");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.alliance < 0 ? "?" : r.alliance ? "Horde" : "Alliance");
            ImGui::TableNextColumn();
            ImGui::Text("%zu / %zu", m.skins, f.skins);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    const RaceCatalog::Race* r = v.race ? list.Find(v.race) : nullptr;
    const nlohmann::json* package = r && v.source == 0 ? m_raceRows.Package(r->id) : nullptr;

    // Middle: the preview and its choices; a project race imported from a source shows with that source's files.
    constexpr float kDetailWidth = 480;
    ImGui::BeginChild("##racepreview", { -kDetailWidth, 0 }, ImGuiChildFlags_Borders);
    auto preview = [&] {
        // The preview and its choices: a project race imported from a source shows with that source's files.
        size_t previewSource = v.source;
        uint32_t previewRace = r->id;
        if (package)
        {
            const nlohmann::json from = package->value("source", nlohmann::json::object());
            previewRace = from.value("race", 0u);
            previewSource = v.sources.size();
            for (size_t i = 0; i < v.sources.size(); ++i)
                if (v.sources[i].name == from.value("client", std::string()))
                {
                    previewSource = i;
                    read(v.sources[i]);
                }
        }
        if (previewSource != v.previewSource || previewRace != v.previewRace)
        {
            v.previewSource = previewSource;
            v.previewRace = previewRace;
            v.stale = true;
        }
        if (previewSource >= v.sources.size())
        {
            ImGui::TextColored(kQuiet, "The client it came from is not a source now: no preview.");
            return;
        }
        if (v.sources[previewSource].races.Format() == RaceCatalog::Layout::Classic)
            ImGui::TextColored(kQuiet, "1.12 tables: the preview needs the client's models and textures converted to 3.3.5 (wow-upport) in this source.");
        bool frame = false;
        if (ImGui::RadioButton("Male", v.sex == 0) && v.sex != 0) { v.sex = 0; v.stale = frame = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Female", v.sex == 1) && v.sex != 1) { v.sex = 1; v.stale = frame = true; }
        if (v.stale || !v.models) RefreshRacePreview(true);
        if (v.looks)
        {
            const DisplayLooks::Choices c = v.looks->CharacterChoices(v.previewRace, v.sex, v.skin, v.hairStyle);
            auto chooser = [&](const char* label, const std::vector<uint32_t>& values, uint32_t& value) {
                ImGui::PushID(label);
                ImGui::BeginDisabled(values.size() < 2);
                if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) { value = Step(values, value, -1); v.stale = true; }
                ImGui::SameLine();
                if (ImGui::ArrowButton("##next", ImGuiDir_Right)) { value = Step(values, value, 1); v.stale = true; }
                ImGui::EndDisabled();
                ImGui::SameLine();
                const auto it = std::find(values.begin(), values.end(), value);
                ImGui::Text("%s %s", label, values.empty() ? "-" : it == values.end() ? "?" : (std::to_string(it - values.begin() + 1) + " / " + std::to_string(values.size())).c_str());
                ImGui::PopID();
            };
            if (ImGui::BeginTable("##choices", 2, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextColumn(); chooser("Skin", c.skins, v.skin);
                ImGui::TableNextColumn(); chooser("Face", c.faces, v.face);
                ImGui::TableNextColumn(); chooser("Hair style", c.hairStyles, v.hairStyle);
                ImGui::TableNextColumn(); chooser("Hair colour", c.hairColors, v.hairColor);
                ImGui::TableNextColumn(); chooser("Facial hair", c.facialHair, v.facial);
                ImGui::EndTable();
            }
            if (v.stale) RefreshRacePreview(frame);
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (v.pose && v.pose->duration) v.timeMs = std::fmod(v.timeMs + io.DeltaTime * 1000.0f, float(v.pose->duration));
        std::vector<ModelRenderer::Part> parts;
        if (v.look && v.models)
        {
            ModelRenderer::Part body{ v.look->look, {}, v.pose.get(), uint32_t(v.timeMs) };
            XMStoreFloat4x4(&body.world, XMMatrixScaling(v.look->scale, v.look->scale, v.look->scale));
            parts.push_back(body);
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const MpqChain* previewMpq = v.sources[previewSource].mpq;
        if (v.models && previewMpq)
            DrawScene(v.scene, { avail.x, std::max(avail.y, 120.0f) }, *v.models, *previewMpq, parts, "This race has no character model this client can show.");
    };
    if (r) preview();
    else ImGui::TextColored(kQuiet, "Pick a race on the left to see it here.");
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the selected race, and importing or editing it.
    ImGui::BeginChild("##racedetail");
    if (!r)
    {
        ImGui::TextColored(kQuiet, "Races come from the project's client and its compare sources (View > Sources).");
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    ImGui::Text("%s", Plain(r->name).c_str());
    ImGui::SameLine();
    ImGui::TextColored(kQuiet, "race %u in %s", r->id, v.source == 0 ? "the project" : src.name.c_str());
    if (ImGui::BeginTable("##raceinfo", 4, ImGuiTableFlags_SizingFixedFit))
    {
        auto row = [](const char* a, const std::string& b, const char* c, const std::string& d) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(kQuiet, "%s", a);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(kQuiet, "%s", c);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.c_str());
        };
        row("File string", r->fileString, "Prefix", r->prefix);
        row("Female / male name", r->names[0].empty() && r->names[1].empty() ? "-" : r->names[0] + " / " + r->names[1], "Team",
            r->alliance < 0 ? "not in a 1.12 table" : r->alliance ? "Horde" : "Alliance");
        const std::string factionName = list.FactionName(r->faction);
        row("Faction template", std::to_string(r->faction) + (factionName.empty() ? "" : ": " + factionName), "Base language", std::to_string(r->baseLanguage));
        row("Displays M / F", std::to_string(r->display[0]) + " / " + std::to_string(r->display[1]), "Cinematic", std::to_string(r->cinematic));
        ImGui::EndTable();
    }
    for (uint32_t sex = 0; sex < 2; ++sex)
    {
        const std::string model = list.Model(r->id, sex);
        ImGui::TextColored(kQuiet, sex ? "Female model" : "Male model");
        ImGui::SameLine(130);
        if (model.empty()) ImGui::TextColored(kWarn, "none");
        else if (v.source != 0 && !src.mpq->HasOwn(model)) ImGui::TextColored(kWarn, "%s (not in this client)", model.c_str());
        else ImGui::TextUnformatted(model.c_str());
    }
    std::string classes;
    for (uint32_t c : list.Classes(r->id)) classes += (classes.empty() ? "" : ", ") + std::string(ClassName(c));
    ImGui::TextColored(kQuiet, "Classes");
    ImGui::SameLine(130);
    ImGui::TextWrapped("%s", classes.empty() ? "none (not playable in this client)" : classes.c_str());
    if (v.source != 0)
    {
        // Hints for importing: a file string another race of the project uses.
        for (const RaceCatalog::Race& b : base.Races())
            if (b.fileString == r->fileString && Plain(b.name) != Plain(r->name))
                ImGui::TextColored(kWarn, "Its file string is %s's (race %u) in the project: it will need its own.", Plain(b.name).c_str(), b.id);
        if (v.missingTextures) ImGui::TextColored(kQuiet, "%zu texture(s) its CharSections name are not in this client (the client skips them).", v.missingTextures);
    }

    if (ImGui::BeginTable("##racecounts", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV))
    {
        for (const char* h : { "", "Skins", "Faces", "Hair styles", "Hair colours", "Facial hair" }) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (uint32_t sex = 0; sex < 2; ++sex)
        {
            const RaceCatalog::Counts c = list.Count(r->id, sex);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(sex ? "Female" : "Male");
            for (size_t n : { c.skins, c.faces, c.hairStyles, c.hairColors, c.facialHair }) { ImGui::TableNextColumn(); ImGui::Text("%zu", n); }
        }
        ImGui::EndTable();
    }

    // Applies changes made of parts (the race package and the display and model rows) and records them as one step.
    auto commit = [&](std::vector<Change> parts, const std::string& label) {
        for (const Change& c : parts)
        {
            if (c.domain == m_raceRows.Domain()) m_raceRows.Apply(c);
            else if (c.domain == m_displayRows.Domain()) m_displayRows.Apply(c);
            else if (c.domain == m_modelRows.Domain()) m_modelRows.Apply(c);
        }
        m_store.Commit(std::move(parts), label);
    };
    if (v.source != 0)
    {
        // Import: the race and every row of its own, renumbered into the project's ranges, as one undo step.
        ImGui::SeparatorText("Import into the project");
        const Project::IdRange range = m_project->Range("race.id");
        uint32_t firstFree = 0;
        for (uint32_t id = range.first; id && id <= range.last && !firstFree; ++id)
            if (!base.Find(id)) firstFree = id;
        if (!v.importTarget) v.importTarget = firstFree;
        int target = int(v.importTarget);
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputInt("Race id", &target)) v.importTarget = uint32_t(std::clamp(target, 1, 63));
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, "project range %u-%u%s", range.first, range.last, firstFree ? "" : " (full)");
        const RaceCatalog::Race* taken = base.Find(v.importTarget);
        if (taken) ImGui::TextColored(kWarn, "Race %u is %s in the project: importing replaces it.", v.importTarget, Plain(taken->name).c_str());
        if (v.importTarget > 32) ImGui::TextColored(kWarn, "Races past 32 need the client's 64-race extension and a server that takes them.");

        // Its choices start as the source race has them, each time another race is picked.
        RaceImportOptions& o = v.import;
        if (v.importFor != std::pair{ v.source, r->id })
        {
            v.importFor = { v.source, r->id };
            o = {};
            o.faction = r->faction;
            o.alliance = r->alliance;
            if (o.alliance < 0)   // a 1.12 race: the team of a project race with the same faction template, else Horde
            {
                o.alliance = 1;
                for (const RaceCatalog::Race& b : base.Races())
                    if (b.faction == r->faction && b.alliance >= 0) { o.alliance = b.alliance; break; }
            }
            const std::vector<uint32_t> had = src.races.Classes(r->id);
            o.classes = std::set<uint32_t>(had.begin(), had.end());
        }
        const std::vector<const RaceCatalog::Race*> playable = Playable(base);
        if (ImGui::RadioButton("Alliance", o.alliance == 0)) o.alliance = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Horde", o.alliance == 1)) o.alliance = 1;
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, "team: the side whose character creator lists it");
        FactionCombo(base, o.faction, r->faction);
        ImGui::TextColored(kQuiet, "Classes");
        std::set<uint32_t>& classes = *o.classes;
        const std::vector<uint32_t> had = src.races.Classes(r->id);
        bool added = false;
        int column = 0;
        const float x0 = ImGui::GetCursorPosX();
        for (uint32_t c : { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u })
        {
            if (column++ % 5) ImGui::SameLine();
            else ImGui::SetCursorPosX(x0 + 20);
            bool on = classes.count(c) != 0;
            if (ImGui::Checkbox(ClassName(c), &on)) on ? (void)classes.insert(c) : (void)classes.erase(c);
            added = added || (on && std::find(had.begin(), had.end(), c) == had.end());
        }
        if (added)
        {
            // Starting outfits for the classes the source race does not have: a project race's (default: the first of the team).
            if (!o.outfitDonor || !base.Find(o.outfitDonor))
                for (const RaceCatalog::Race* b : playable)
                    if (b->alliance == o.alliance) { o.outfitDonor = b->id; break; }
            const RaceCatalog::Race* donor = base.Find(o.outfitDonor);
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Starting outfits of added classes from", donor ? Plain(donor->name).c_str() : "none"))
            {
                if (ImGui::Selectable("none (they start naked)", !o.outfitDonor)) o.outfitDonor = 0;
                for (const RaceCatalog::Race* b : playable)
                    if (ImGui::Selectable(Plain(b->name).c_str(), o.outfitDonor == b->id)) o.outfitDonor = b->id;
                ImGui::EndCombo();
            }
        }
        if (classes.empty()) ImGui::TextColored(kWarn, "No class: nobody can make a character of it.");
        if (classes.count(6)) ImGui::TextColored(kQuiet, "Death Knights also need the server's Acherus start (server export).");
        ImGui::BeginDisabled(!v.importTarget);
        if (ImGui::Button(("Import as race " + std::to_string(v.importTarget)).c_str()))
        {
            std::string error;
            auto parts = ImportRaceChanges(src.races, src.name, r->id, v.importTarget, v.project, m_raceRows, m_displayRows, m_modelRows, *m_project, error,
                                           v.import);
            if (parts.empty()) Log("Import %s: %s", Plain(r->name).c_str(), error.c_str());
            else
            {
                const size_t rows = parts.front().data["after"]["CharSections"].size();
                commit(std::move(parts), "import race " + Plain(r->name));
                Log("Imported %s from %s as race %u (%zu CharSections rows). Its models and textures go into the patch on export.",
                    Plain(r->name).c_str(), src.name.c_str(), v.importTarget, rows);
                const uint32_t imported = v.importTarget;
                v.source = 0;
                v.importTarget = 0;
                v.importFor = { ~size_t(0), 0 };
                select(imported);
            }
        }
        ImGui::EndDisabled();
    }
    else DrawRaceEditor(*r);

    ImGui::EndChild();
    ImGui::End();
}

void App::DrawRaceEditor(const RaceCatalog::Race& race)
{
    RacesView& v = m_races;
    const RaceCatalog& base = v.project;
    const nlohmann::json* package = m_raceRows.Package(race.id);
    // The package as edited: read again when another race is picked, or when an undo changed it while unedited.
    if (v.editRace != race.id || (!v.editDirty && v.editVersion != m_raceRows.Version()))
    {
        v.editRace = race.id;
        v.editVersion = m_raceRows.Version();
        v.edit = package ? *package : base.Package(race.id);
        v.editDirty = false;
        v.editClass = 0;
        v.pickSlot = -1;
    }
    if (!v.edit.is_object()) return;
    nlohmann::json& p = v.edit;
    nlohmann::json& row = p["ChrRaces"];

    // Applies changes made of parts as one undo step.
    auto commit = [&](std::vector<Change> parts, const std::string& label) {
        for (const Change& c : parts)
        {
            if (c.domain == m_raceRows.Domain()) m_raceRows.Apply(c);
            else if (c.domain == m_displayRows.Domain()) m_displayRows.Apply(c);
            else if (c.domain == m_modelRows.Domain()) m_modelRows.Apply(c);
        }
        m_store.Commit(std::move(parts), label);
    };
    ImGui::SeparatorText("Edit");
    const nlohmann::json from = p.value("source", nlohmann::json::object());
    if (!from.empty()) ImGui::TextColored(kQuiet, "Imported from %s, race %u there.", from.value("client", std::string("?")).c_str(), from.value("race", 0u));
    else if (!package) ImGui::TextColored(kQuiet, "The client's race: applying an edit makes it the project's.");
    else ImGui::TextColored(kQuiet, "The client's race, changed by the project.");
    ImGui::BeginDisabled(!v.editDirty);
    if (ImGui::Button("Apply"))
    {
        commit({ m_raceRows.MakeChange(race.id, p, "edit race " + Plain(race.name)) }, "edit race " + Plain(race.name));
        v.editDirty = false;
        v.editVersion = m_raceRows.Version();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) v.editRace = 0;
    ImGui::EndDisabled();
    if (package)
    {
        ImGui::SameLine();
        if (ImGui::Button(from.empty() ? "Back to the client's" : "Remove from the project"))
            commit(RemoveRaceChanges(race.id, m_raceRows, m_displayRows, m_modelRows), (from.empty() ? "revert race " : "remove race ") + Plain(race.name));
    }
    if (v.editDirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "not applied");
    }
    auto dirty = [&] { v.editDirty = true; };

    if (!ImGui::BeginTabBar("##raceedit")) return;
    if (ImGui::BeginTabItem("Identity"))
    {
        auto text = [&](const char* label, const char* key, const char* tip = nullptr) {
            std::string value = row.value(key, std::string());
            ImGui::SetNextItemWidth(260);
            if (ImGui::InputText(label, &value)) { row[key] = value; dirty(); }
            if (tip) ImGui::SetItemTooltip("%s", tip);
        };
        text("Name", "Name_lang");
        text("Female name", "Name_female_lang", "What female characters of it are called (3.3.5); empty: the name.");
        text("Male name", "Name_male_lang", "What male characters of it are called (3.3.5); empty: the name.");
        text("File string", "ClientFileString",
             "Picks the login screen's model (Interface\\Glues\\Models\\UI_<file string>) and the race's sounds. Its characters' "
             "models and textures are named by its displays and CharSections, not by it.");
        text("Prefix", "ClientPrefix", "Helmets are found as <helmet>_<prefix><M|F>.m2: a prefix no helmet has shows none (a bare head).");
        int team = int(row.value("Alliance", 1u));
        if (ImGui::RadioButton("Alliance", team == 0)) { row["Alliance"] = 0; dirty(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Horde", team == 1)) { row["Alliance"] = 1; dirty(); }
        uint32_t faction = row.value("FactionID", 0u);
        if (FactionCombo(base, faction)) { row["FactionID"] = faction; dirty(); }
        const uint32_t language = row.value("BaseLanguage", 0u);
        const std::string languageName = base.LanguageName(language);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Base language", (std::to_string(language) + ": " + (languageName.empty() ? "none" : languageName)).c_str()))
        {
            for (const auto& [id, name] : base.Languages())
                if (ImGui::Selectable((std::to_string(id) + ": " + name).c_str(), id == language)) { row["BaseLanguage"] = id; dirty(); }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The language its characters speak (Languages.dbc). The server also needs its language skill (server rows, a later phase).");
        int cinematic = int(row.value("CinematicSequenceID", 0u));
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Intro cinematic", &cinematic)) { row["CinematicSequenceID"] = uint32_t(std::max(cinematic, 0)); dirty(); }
        ImGui::SetItemTooltip("CinematicSequences id played on a new character's first login; 0 none.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Classes"))
    {
        std::set<uint32_t> classes, outfits;
        for (const nlohmann::json& b : ChangeStore::List(p, "CharBaseInfo")) classes.insert(b.value("ClassID", 0u));
        for (const nlohmann::json& o : ChangeStore::List(p, "CharStartOutfit")) outfits.insert(o.value("ClassID", 0u));
        std::set<uint32_t> wanted = classes;
        bool changed = false;
        int column = 0;
        for (uint32_t c : { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u })
        {
            if (column++ % 5) ImGui::SameLine();
            bool on = wanted.count(c) != 0;
            if (ImGui::Checkbox(ClassName(c), &on)) { on ? (void)wanted.insert(c) : (void)wanted.erase(c); changed = true; }
        }
        std::vector<const RaceCatalog::Race*> playable = Playable(base);
        if (!v.editDonor || !base.Find(v.editDonor))
            for (const RaceCatalog::Race* b : playable)
                if (int(b->alliance) == int(row.value("Alliance", 1u)) && b->id != race.id) { v.editDonor = b->id; break; }
        const RaceCatalog::Race* donor = base.Find(v.editDonor);
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("Outfits of a class it has none of, from", donor ? Plain(donor->name).c_str() : "none"))
        {
            if (ImGui::Selectable("none (they start naked)", !v.editDonor)) v.editDonor = 0;
            for (const RaceCatalog::Race* b : playable)
                if (ImGui::Selectable(Plain(b->name).c_str(), v.editDonor == b->id)) v.editDonor = b->id;
            ImGui::EndCombo();
        }
        if (changed)
        {
            // New outfit ids: free in the project's range, past the ones the edit holds already.
            std::set<uint32_t> used = base.Ids("CharStartOutfit");
            for (const nlohmann::json& o : ChangeStore::List(p, "CharStartOutfit")) used.insert(o.value("ID", 0u));
            const Project::IdRange range = m_project->Range("charstartoutfit.id");
            auto next = [&]() -> uint32_t {
                for (uint32_t id = range.first; id && id <= range.last; ++id)
                    if (used.insert(id).second) return id;
                return 0;
            };
            std::string error;
            if (SetRaceClasses(p, wanted, v.editDonor ? base.Package(v.editDonor) : nlohmann::json(), next, error)) dirty();
            else Log("Classes: %s", error.c_str());
        }
        for (uint32_t c : wanted)
            if (!outfits.count(c) && classes.count(c)) ImGui::TextColored(kQuiet, "%s starts naked: no outfit (Starting items).", ClassName(c));
        if (wanted.empty()) ImGui::TextColored(kWarn, "No class: nobody can make a character of it.");
        if (wanted.count(6)) ImGui::TextColored(kQuiet, "Death Knights also need the server's Acherus start (server rows, a later phase).");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Looks"))
    {
        if (ImGui::RadioButton("Male##looks", v.editSex == 0)) v.editSex = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Female##looks", v.editSex == 1)) v.editSex = 1;
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, "Removing a choice moves the ones after it down; the preview shows the source's tables.");
        static const std::pair<RaceChoice, const char*> kChoices[] = { { RaceChoice::Skin, "Skin colours" }, { RaceChoice::Face, "Faces" },
                                                                       { RaceChoice::HairStyle, "Hair styles" }, { RaceChoice::HairColor, "Hair colours" },
                                                                       { RaceChoice::FacialHair, "Facial hair" } };
        for (const auto& [choice, label] : kChoices)
        {
            const std::vector<uint32_t> values = RaceChoiceValues(p, v.editSex, choice);
            if (!ImGui::TreeNode(label, "%s (%zu)", label, values.size())) continue;
            for (uint32_t value : values)
            {
                ImGui::PushID(int(value));
                // A texture of it, to tell the values apart.
                std::string texture;
                const int section = choice == RaceChoice::Skin ? 0 : choice == RaceChoice::Face ? 1 : choice == RaceChoice::FacialHair ? 2 : 3;
                const char* column = choice == RaceChoice::Skin || choice == RaceChoice::HairColor ? "ColorIndex" : "VariationIndex";
                for (const nlohmann::json& s : ChangeStore::List(p, "CharSections"))
                    if (s.value("SexID", 0u) == v.editSex && s.value("BaseSection", 0u) == uint32_t(section) && s.value(column, 0u) == value)
                    {
                        texture = s.value("TextureName[0]", std::string());
                        break;
                    }
                const bool remove = ImGui::SmallButton("Remove");
                ImGui::SameLine();
                ImGui::Text("%u", value + 1);
                ImGui::SameLine(110);
                ImGui::TextColored(kQuiet, "%s", texture.empty() ? (choice == RaceChoice::HairStyle || choice == RaceChoice::FacialHair ? "(geosets)" : "-") : texture.c_str());
                ImGui::PopID();
                if (remove && RemoveRaceChoice(p, v.editSex, choice, value)) { dirty(); break; }
            }
            ImGui::TreePop();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Starting items"))
    {
        std::vector<uint32_t> classes;
        for (const nlohmann::json& b : ChangeStore::List(p, "CharBaseInfo")) classes.push_back(b.value("ClassID", 0u));
        std::sort(classes.begin(), classes.end());
        if (classes.empty()) ImGui::TextColored(kQuiet, "No classes (Classes tab).");
        else
        {
            if (std::find(classes.begin(), classes.end(), v.editClass) == classes.end()) v.editClass = classes.front();
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("Class", ClassName(v.editClass)))
            {
                for (uint32_t c : classes)
                    if (ImGui::Selectable(ClassName(c), c == v.editClass)) v.editClass = c;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Male##items", v.editSex == 0)) v.editSex = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Female##items", v.editSex == 1)) v.editSex = 1;
            nlohmann::json* outfit = nullptr;
            for (nlohmann::json& o : p["CharStartOutfit"])
                if (o.value("ClassID", 0u) == v.editClass && o.value("SexID", 0u) == v.editSex) { outfit = &o; break; }
            if (!outfit)
            {
                ImGui::TextColored(kQuiet, "No starting outfit: characters of this class and sex start naked.");
                if (ImGui::Button("Add a starting outfit"))
                {
                    std::set<uint32_t> used = base.Ids("CharStartOutfit");
                    for (const nlohmann::json& o : p["CharStartOutfit"]) used.insert(o.value("ID", 0u));
                    const Project::IdRange range = m_project->Range("charstartoutfit.id");
                    uint32_t id = 0;
                    for (uint32_t i = range.first; i && i <= range.last && !id; ++i)
                        if (!used.count(i)) id = i;
                    if (!id) Log("Starting outfit: the charstartoutfit.id range is full.");
                    else
                    {
                        nlohmann::json o = { { "ID", id }, { "RaceID", race.id }, { "ClassID", v.editClass }, { "SexID", v.editSex }, { "OutfitID", 0 } };
                        for (int i = 0; i < 24; ++i)
                            for (const char* k : { "ItemID", "DisplayItemID", "InventoryType" }) o[std::string(k) + "[" + std::to_string(i) + "]"] = 0;
                        p["CharStartOutfit"].push_back(std::move(o));
                        dirty();
                    }
                }
            }
            else
            {
                if (!m_db.Connected()) ImGui::TextColored(kQuiet, "Connect the server (Server panel) to search items and see which exist there.");
                auto slot = [](const char* key, int i) { return std::string(key) + "[" + std::to_string(i) + "]"; };
                int empty = -1;
                if (ImGui::BeginTable("##outfit", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
                {
                    ImGui::TableSetupColumn("Item");
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Type");
                    ImGui::TableSetupColumn("");
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < 24; ++i)
                    {
                        const uint32_t entry = (*outfit).value(slot("ItemID", i), 0u);
                        if (!entry) { if (empty < 0) empty = i; continue; }
                        ImGui::PushID(i);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", entry);
                        ImGui::TableNextColumn();
                        if (m_db.Connected())
                        {
                            const NpcView::Item& item = NpcItem(entry);
                            if (item.found) ImGui::TextUnformatted(item.name.c_str());
                            else ImGui::TextColored(kWarn, "not on your server: nobody gets it");
                        }
                        else ImGui::TextColored(kQuiet, "-");
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", (*outfit).value(slot("InventoryType", i), 0u));
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("Change")) { v.pickSlot = i; ImGui::OpenPopup("##raceitem"); }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear"))
                        {
                            for (const char* k : { "ItemID", "DisplayItemID", "InventoryType" }) (*outfit)[slot(k, i)] = 0;
                            dirty();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::BeginDisabled(empty < 0 || !m_db.Connected());
                if (ImGui::Button("Add an item")) { v.pickSlot = empty; ImGui::OpenPopup("##raceitem"); }
                ImGui::EndDisabled();
                if (empty < 0) { ImGui::SameLine(); ImGui::TextColored(kQuiet, "all 24 slots used"); }
                ImGui::SetNextWindowSize({ 440, 420 });
                if (ImGui::BeginPopup("##raceitem"))
                {
                    if (const auto chosen = ItemSearch("Any item: name or entry", "1 = 1"); chosen && v.pickSlot >= 0)
                    {
                        const NpcView::Item& item = NpcItem(*chosen);
                        (*outfit)[slot("ItemID", v.pickSlot)] = *chosen;
                        (*outfit)[slot("DisplayItemID", v.pickSlot)] = *chosen ? item.display : 0;
                        (*outfit)[slot("InventoryType", v.pickSlot)] = *chosen ? item.type : 0;
                        dirty();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::TextColored(kQuiet, "The creator shows these, and the server gives them (AzerothCore reads the same table).");
            }
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}
