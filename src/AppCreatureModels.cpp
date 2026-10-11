// Creature models window: the creature models of every source (the project's client and the compare sources), each
// with its skins (CreatureDisplayInfo rows) and animations, in a preview of its own. Sources whose tables this cannot
// read (a newer client's DB2s) list their creature model files instead.
#include "App.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
const ImVec4 kNew{ 0.45f, 0.85f, 0.55f, 1.00f };
const ImVec4 kProject{ 0.45f, 0.70f, 1.00f, 1.00f };

std::string S(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
constexpr float kFrameMs = 1000.0f / 30;   // one step of the frame buttons

std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

/// A model path as the client finds it: backslashes, lower case, .mdx read as .m2.
std::string ModelKey(const std::string& path)
{
    std::string s = Lower(M2Name(path));
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}
}

void App::SetCreatureModelSequence(int sequence)
{
    CreatureModelsView& v = m_creatureModels;
    v.sequence = sequence;
    v.timeMs = 0;
    const MpqChain* mpq = v.previewMpq;
    v.pose = v.look && mpq ? LoadSkeleton(M2Name(v.look->look.model), [mpq](const std::string& path) { return mpq->Read(path); }, sequence) : nullptr;
}

void App::RefreshCreatureModelPreview()
{
    CreatureModelsView& v = m_creatureModels;
    v.stale = false;
    v.look.reset();
    v.info.reset();
    v.pose.reset();
    v.sequence = -1;
    v.hasFile = v.fromClient = false;
    if (v.source >= v.sources.size()) return;
    CreatureModelsView::Source& src = v.sources[v.source];
    if (!src.mpq || v.model < 0 || size_t(v.model) >= v.listed.size()) return;
    const DisplayLooks::CreatureModel& m = v.listed[size_t(v.model)];
    // A source holding only a client's patches lacks the files it did not change: each file the source lacks comes
    // from the project's client, under the same path (a chain of its own reading the source first, then the client).
    // The project's client reads the project's assets first: pictures made into skins are there until export.
    const MpqChain* files = src.mpq;
    const MpqChain* client = v.sources[0].mpq;
    if (v.source == 0 && m_project)
    {
        if (!v.assets || v.assetsStale)
        {
            v.assets = std::make_unique<MpqChain>();
            MpqLayer layer;
            layer.kind = MpqLayer::Kind::Folder;
            layer.path = m_project->AssetsDir().string();
            layer.installed = false;
            std::error_code ec;
            if (std::filesystem::is_directory(m_project->AssetsDir(), ec)) v.assets->Open(std::vector<MpqLayer>{ layer });
            v.assetsStale = false;
        }
        if (!v.chain) v.chain = std::make_unique<MpqChain>();
        v.chain->SetFallbacks({ v.assets.get(), src.mpq });
        files = v.chain.get();
    }
    else if (v.source && client)
    {
        if (!v.chain) v.chain = std::make_unique<MpqChain>();
        v.chain->SetFallbacks({ src.mpq, client });
        files = v.chain.get();
    }
    const std::string file = M2Name(m.path);
    v.fromClient = v.source && client && !src.mpq->HasOwn(file) && client->HasOwn(file);
    v.hasFile = src.mpq->HasOwn(file) || v.fromClient;
    if (v.previewSource != v.source || !v.models)   // a renderer caches files by name: another source needs a new one
    {
        v.previewSource = v.source;
        v.previewMpq = files;
        v.models.reset();
        v.renderer = std::make_unique<Renderer>();
        std::string error;
        if (!v.renderer->Init(m_device, m_context, error)) { Log("Creature models preview: %s", error.c_str()); v.renderer.reset(); return; }
        v.models = std::make_unique<ModelRenderer>();
        if (!v.models->Init(m_device, m_context, *v.renderer, error)) { Log("Creature models preview: %s", error.c_str()); v.models.reset(); return; }
    }
    if (v.display && src.looks) v.look = src.looks->DisplayLook(v.display);
    if (!v.look)
    {
        v.look.emplace();
        v.look->look.model = m.path;   // no skin: the model with only its fixed textures
    }
    v.info = v.models->Info(v.look->look.model, *files);
    if (!v.info) { v.look.reset(); return; }
    if (const auto& skel = v.info->skeleton; skel && !skel->sequences.empty())
    {
        int stand = 0;
        for (size_t i = 0; i < skel->sequences.size(); ++i)
            if (skel->sequences[i].id == 0) { stand = int(i); break; }
        SetCreatureModelSequence(stand);
    }
    v.scene.Frame(v.info->boundsMin, v.info->boundsMax, v.look->scale);
}

void App::DrawCreatureModels()
{
    if (!m_showCreatureModels) return;
    ImGui::SetNextWindowSize({ 1400, 800 }, ImGuiCond_FirstUseEver);
    const unsigned int space = PanelHost("Creature models", &m_showCreatureModels, [](unsigned int node) {
        ImGuiID rest = node;
        const ImGuiID left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.28f, nullptr, &rest);
        const ImGuiID right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.30f, nullptr, &rest);
        ImGui::DockBuilderDockWindow("Model list", left);
        ImGui::DockBuilderDockWindow("Model details", right);
        ImGui::DockBuilderDockWindow("Model preview", rest);
    });
    if (!m_showCreatureModels) return;
    auto only = [&](const char* text) {
        BeginPanel("Model list", space);
        ImGui::TextColored(kQuiet, "%s", text);
        ImGui::End();
    };
    if (!m_project) { only("Open a project first."); return; }
    CreatureModelsView& v = m_creatureModels;

    // The sources as attached now; a changed list starts over.
    const auto& ghosts = m_ghosts.Sources();
    bool same = ghosts.size() == v.sources.size();
    for (size_t i = 0; same && i < ghosts.size(); ++i) same = ghosts[i].mpq == v.sources[i].mpq && ghosts[i].name == v.sources[i].name;
    if (!same)
    {
        v.sources.clear();
        for (const Ghosts::Source& g : ghosts) v.sources.push_back({ g.name, g.mpq });
        v.source = std::min(v.source, v.sources.empty() ? size_t(0) : v.sources.size() - 1);
        v.model = -1;
        v.display = 0;
        v.previewMpq = nullptr;
        v.previewSource = ~size_t(0);
        v.chain.reset();   // its fallbacks were the old sources
        v.models.reset();
        v.renderer.reset();
        v.basePaths.clear();
        v.stale = true;
    }
    if (v.sources.empty()) { only("No sources."); return; }
    auto read = [](CreatureModelsView::Source& s) {
        if (s.read || !s.mpq) return;
        s.read = true;
        s.looks = std::make_unique<DisplayLooks>(*s.mpq);
        s.models = s.looks->CreatureModels();
        if (!s.models.empty()) return;
        // No CreatureModelData this can read: every creature model file, without skins.
        s.filesOnly = true;
        for (const MpqChain::Entry& e : s.mpq->List())
        {
            const std::string lower = Lower(e.name);
            if (lower.rfind("creature\\", 0) == 0 && (lower.ends_with(".m2") || lower.ends_with(".mdx"))) s.models.push_back({ 0, e.name, {} });
        }
        std::sort(s.models.begin(), s.models.end(), [](const auto& a, const auto& b) { return _stricmp(a.path.c_str(), b.path.c_str()) < 0; });
    };
    read(v.sources[0]);
    // The project's client shows the project's own skins over its rows (and the one whose scale is being dragged).
    if (v.sources[0].looks)
        v.sources[0].looks->SetRowOverride([this](int table, uint32_t id) -> const nlohmann::json* {
            if (table == 2) return m_modelRows.Edited(id);   // the project's own models
            if (table != 0) return nullptr;
            if (id && id == m_creatureModels.edit && m_creatureModels.editRow.is_object()) return &m_creatureModels.editRow;
            return m_displayRows.Edited(id);
        });
    if (v.basePaths.empty())
        for (const auto& m : v.sources[0].models) v.basePaths.insert(ModelKey(m.path));
    CreatureModelsView::Source& src = v.sources[v.source];
    read(src);

    // What the list shows: the source's models; in the project's client the project's own come first (rows it added,
    // imported or from files), each with the skins it gave them. The selection stays on its model across rebuilds.
    const uint64_t key[3] = { v.source, m_modelRows.Version(), m_displayRows.Version() };
    if (!std::equal(key, key + 3, v.listedKey))
    {
        const std::optional<DisplayLooks::CreatureModel> kept = v.model >= 0 && size_t(v.model) < v.listed.size() ? std::optional(v.listed[size_t(v.model)]) : std::nullopt;
        const bool sameSource = v.listedKey[0] == key[0];
        std::copy(key, key + 3, v.listedKey);
        v.listed.clear();
        if (v.source == 0)
            for (const auto& [id, row] : m_modelRows.Edits())
            {
                if (!row.is_object() || !m_project->Owns("creaturemodeldata.id", id)) continue;
                const std::string path = S(row, "ModelName");
                if (path.empty() || _strnicmp(path.c_str(), "Character\\", 10) == 0) continue;
                DisplayLooks::CreatureModel m{ id, path, {}, true };
                for (const auto& [d, display] : m_displayRows.Edits())
                    if (display.is_object() && display.value("ModelID", 0u) == id && !display.value("ExtendedDisplayInfoID", 0u)) m.displays.push_back(d);
                v.listed.push_back(std::move(m));
            }
        v.listed.insert(v.listed.end(), src.models.begin(), src.models.end());
        int found = -1;
        if (kept && sameSource)
            for (int i = 0; i < int(v.listed.size()) && found < 0; ++i)
                if (v.listed[size_t(i)].id == kept->id && v.listed[size_t(i)].project == kept->project) found = i;
        if (v.pendingSelect)
            for (int i = 0; i < int(v.listed.size()); ++i)
                if (v.listed[size_t(i)].project && v.listed[size_t(i)].id == v.pendingSelect)
                {
                    found = i;
                    v.display = v.listed[size_t(i)].displays.empty() ? 0 : v.listed[size_t(i)].displays.front();
                    v.pendingSelect = 0;
                    break;
                }
        if (found != v.model) v.stale = true;
        v.model = found;
    }

    // The list: the source and its models.
    BeginPanel("Model list", space);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##source", (src.name + (v.source == 0 ? "  (project)" : "")).c_str()))
    {
        for (size_t i = 0; i < v.sources.size(); ++i)
            if (ImGui::Selectable((v.sources[i].name + (i == 0 ? "  (project)" : "")).c_str(), i == v.source) && i != v.source)
            {
                v.source = i;
                v.model = -1;
                v.display = 0;
                v.stale = true;
            }
        ImGui::EndCombo();
    }
    if (!src.mpq) ImGui::TextColored(kWarn, "Not attached.");
    else if (src.filesOnly) ImGui::TextColored(kQuiet, "%zu model files (no tables this can read: no skins)", v.listed.size());
    else ImGui::TextColored(kQuiet, "%zu models", v.listed.size());
    ImGui::SetNextItemWidth(v.source ? -110.0f : -1.0f);
    ImGui::InputTextWithHint("##modelfilter", "Filter by path", &v.filter);
    if (v.source)
    {
        ImGui::SameLine();
        ImGui::Checkbox("Only new", &v.onlyNew);
        ImGui::SetItemTooltip("Models the project's client does not have");
    }
    // A model from files on disk (an export of a modelling tool, another client's unpacked files): into the project.
    if (ImGui::Button("Add a model file..."))
        if (const auto file = PickFile(m_hwnd, L"A creature model (.m2) with its .skin files and textures beside it", L"M2 models", L"*.m2"))
        {
            v.addFile = *file;
            v.addFolder = "Creature\\" + std::filesystem::path(*file).stem().string() + "\\";
            v.addOpen = true;
            ImGui::OpenPopup("Add a model file");
        }
    ImGui::SetItemTooltip("An .m2 (3.3.5, or newer: converted) with its .skin and .anim files and textures in its folder; "
                          "they are copied into the project's assets.");
    if (ImGui::BeginPopupModal("Add a model file", &v.addOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(v.addFile.c_str());
        ImGui::TextColored(kQuiet, "Its folder in the game (the client finds it there; skins sit beside it):");
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("##addfolder", &v.addFolder);
        if (ImGui::Button("Add", { 120, 0 }))
        {
            std::vector<std::string> notes;
            std::string error;
            auto parts = AddLooseCreatureModel(v.addFile, v.addFolder, m_mpq, m_project->AssetsDir(), m_creatureModelRows, m_displayRows, m_modelRows,
                                               *m_project, notes, error);
            for (const std::string& n : notes) Log("Add model: %s", n.c_str());
            if (parts.empty()) Log("Add model: %s", error.c_str());
            else
            {
                const uint32_t id = parts.front().data.at("model");
                for (const Change& c : parts)
                {
                    if (c.domain == m_creatureModelRows.Domain()) m_creatureModelRows.Apply(c);
                    else if (c.domain == m_displayRows.Domain()) m_displayRows.Apply(c);
                    else if (c.domain == m_modelRows.Domain()) m_modelRows.Apply(c);
                }
                m_store.Commit(std::move(parts), "add creature model");
                // Show it: the project's client, the new model and its skin.
                v.source = 0;
                v.listedKey[0] = ~0ull;
                v.assetsStale = true;
                ++m_assetsVersion;   // the editor's files read the new ones too
                v.folderFor.clear();
                v.previewSource = ~size_t(0);
                v.addOpen = false;
                v.pendingSelect = id;
                Log("Added %s as model %u.", v.addFile.c_str(), id);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 120, 0 })) { v.addOpen = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    const std::string filter = Lower(v.filter);
    if (ImGui::BeginTable("##models", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, { 0, 0 }))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Skins");
        ImGui::TableSetupColumn("ID");
        ImGui::TableHeadersRow();
        std::vector<int> shown;
        for (int i = 0; i < int(v.listed.size()); ++i)
        {
            const auto& m = v.listed[size_t(i)];
            if (!filter.empty() && Lower(m.path).find(filter) == std::string::npos) continue;
            if (v.source && v.onlyNew && v.basePaths.count(ModelKey(m.path))) continue;
            shown.push_back(i);
        }
        ImGuiListClipper clip;
        clip.Begin(int(shown.size()));
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
            {
                const int i = shown[size_t(row)];
                const auto& m = v.listed[size_t(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const size_t slash = m.path.find_last_of("\\/");
                const std::string file = slash == std::string::npos ? m.path : m.path.substr(slash + 1);
                if (ImGui::Selectable((file + "##" + std::to_string(i)).c_str(), v.model == i, ImGuiSelectableFlags_SpanAllColumns) && v.model != i)
                {
                    v.model = i;
                    v.display = m.displays.empty() ? 0 : m.displays.front();
                    v.stale = true;
                }
                ImGui::SetItemTooltip("%s", m.path.c_str());
                if (m.project)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(kProject, "project");
                }
                else if (v.source && m.id && m_creatureModelRows.ImportedAs(src.name, m.id))
                {
                    ImGui::SameLine();
                    ImGui::TextColored(kProject, "in project");
                }
                else if (v.source && !v.basePaths.count(ModelKey(m.path)))
                {
                    ImGui::SameLine();
                    ImGui::TextColored(kNew, "new");
                }
                ImGui::TableNextColumn();
                if (src.filesOnly) ImGui::TextColored(kQuiet, "-");
                else ImGui::Text("%zu", m.displays.size());
                ImGui::TableNextColumn();
                if (m.id) ImGui::Text("%u", m.id);
            }
        ImGui::EndTable();
    }
    ImGui::End();

    const DisplayLooks::CreatureModel* m = v.model >= 0 && size_t(v.model) < v.listed.size() ? &v.listed[size_t(v.model)] : nullptr;
    if (v.stale) RefreshCreatureModelPreview();

    // The preview and its animation bar.
    const ImGuiStyle& style = ImGui::GetStyle();
    if (BeginPanel("Model preview", space))
    {
        if (!m) ImGui::TextColored(kQuiet, "Pick a model.");
        else
        {
            const ModelSkeleton* skel = v.info && v.info->skeleton ? v.info->skeleton.get() : nullptr;
            const float barHeight = skel && !skel->sequences.empty() ? ImGui::GetFrameHeightWithSpacing() * 2 + style.ItemSpacing.y : 0;
            if (!v.paused && v.pose && v.pose->duration) v.timeMs = std::fmod(v.timeMs + ImGui::GetIO().DeltaTime * 1000.0f * v.speed, float(v.pose->duration));
            std::vector<ModelRenderer::Part> parts;
            if (v.look && v.models)
            {
                ModelRenderer::Part body{ v.look->look, {}, v.pose.get(), uint32_t(v.timeMs) };
                XMStoreFloat4x4(&body.world, XMMatrixScaling(v.look->scale, v.look->scale, v.look->scale));
                parts.push_back(body);
            }
            ImVec2 size = ImGui::GetContentRegionAvail();
            size.y = std::max(size.y - barHeight, 120.0f);
            if (v.models && v.previewMpq) DrawScene(v.scene, size, *v.models, *v.previewMpq, parts, v.hasFile ? "Nothing to draw: this model has no mesh (particles or an invisible marker)." : "Model file missing: see the details.");
            if (skel && !skel->sequences.empty() && src.looks)
            {
                auto label = [&](int i) {
                    const ModelSequence& q = skel->sequences[size_t(i)];
                    std::string s = src.looks->AnimationName(q.id);
                    if (q.variation) s += " " + std::to_string(q.variation + 1);
                    if (q.flags & 0x40) s += "  (alias)";
                    return s + "##" + std::to_string(i);
                };
                ImGui::SetNextItemWidth(260);
                if (ImGui::BeginCombo("##anim", v.sequence >= 0 ? label(v.sequence).c_str() : "Animation", ImGuiComboFlags_HeightLarge))
                {
                    for (int i = 0; i < int(skel->sequences.size()); ++i)
                    {
                        if (ImGui::Selectable(label(i).c_str(), i == v.sequence)) SetCreatureModelSequence(i);
                        if (i == v.sequence) ImGui::SetItemDefaultFocus();
                        ImGui::SameLine(220);
                        ImGui::TextColored(kQuiet, "%u ms", skel->sequences[size_t(i)].duration);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button(v.paused ? "Play" : "Pause", { 60, 0 })) v.paused = !v.paused;
                ImGui::SameLine();
                ImGui::BeginDisabled(!v.paused);
                const float duration = v.pose ? float(v.pose->duration) : 0.0f;
                if (ImGui::ArrowButton("##prev", ImGuiDir_Left) && duration > 0) v.timeMs = std::fmod(v.timeMs - kFrameMs + duration, duration);
                ImGui::SetItemTooltip("Previous frame");
                ImGui::SameLine();
                if (ImGui::ArrowButton("##next", ImGuiDir_Right) && duration > 0) v.timeMs = std::fmod(v.timeMs + kFrameMs, duration);
                ImGui::SetItemTooltip("Next frame");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160);
                ImGui::SliderFloat("##speed", &v.speed, 0.1f, 2.0f, "speed %.1fx");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##time", &v.timeMs, 0, std::max(duration - 1, 0.0f), "%.0f ms")) v.paused = true;
            }
        }
    }
    ImGui::End();

    // What the model is: its file, its skins, its textures.
    if (BeginPanel("Model details", space) && m)
    {
        ImGui::TextWrapped("%s", m->path.c_str());
        if (m->id) ImGui::TextColored(kQuiet, "CreatureModelData %u", m->id);
        if (v.source)
        {
            if (v.basePaths.count(ModelKey(m->path))) ImGui::TextColored(kQuiet, "The project's client has this model.");
            else ImGui::TextColored(kNew, "New: the project's client does not have this model.");
        }
        if (v.fromClient) ImGui::TextColored(kQuiet, "This source does not hold the file: shown with the project client's copy.");
        if (!v.hasFile)
            ImGui::TextWrapped("%s is in neither this source nor the project's client. Either the source's client never shipped it (its "
                               "tables name it anyway), or the source is a converted copy that left out the archive holding it: "
                               "convert that archive into the source too.", M2Name(m->path).c_str());
        else if (!v.info) ImGui::TextColored(kQuiet, "No mesh to draw (particles or an invisible marker).");

        // Into the project: the model, its skins and its files, as one undo step.
        if (v.source && !src.filesOnly && m->id)
        {
            ImGui::SeparatorText("Project");
            auto commit = [&](std::vector<Change> parts, const std::string& label) {
                for (const Change& c : parts)
                {
                    if (c.domain == m_creatureModelRows.Domain()) m_creatureModelRows.Apply(c);
                    else if (c.domain == m_displayRows.Domain()) m_displayRows.Apply(c);
                    else if (c.domain == m_modelRows.Domain()) m_modelRows.Apply(c);
                }
                m_store.Commit(std::move(parts), label);
            };
            if (const uint32_t mine = m_creatureModelRows.ImportedAs(src.name, m->id))
            {
                const nlohmann::json& p = *m_creatureModelRows.Package(mine);
                const nlohmann::json added = p.value("added", nlohmann::json::object());
                const nlohmann::json& shown = ChangeStore::List(added, "CreatureDisplayInfo");
                ImGui::TextColored(kProject, "In the project as model %u, %zu skin(s): displays %s", mine, shown.size(),
                                   shown.empty() ? "-" : (std::to_string(shown.front().get<uint32_t>()) +
                                                          (shown.size() > 1 ? "-" + std::to_string(shown.back().get<uint32_t>()) : "")).c_str());
                const size_t files = ChangeStore::List(p, "files").size();
                ImGui::TextColored(kQuiet, "%zu file(s) go into the patch from %s%s", files, src.name.c_str(),
                                   S(p, "path") != S(p.value("source", nlohmann::json::object()), "path") ? ", under Creature\\<source>\\ (the client has other files of its names)" : "");
                if (p.value("missingFiles", 0u)) ImGui::TextColored(kWarn, "%u file(s) it names are missing in the source.", p.value("missingFiles", 0u));
                if (ImGui::Button("Remove from the project")) commit(RemoveCreatureModelChanges(mine, m_creatureModelRows, m_displayRows, m_modelRows), "remove creature model");
            }
            else
            {
                if (ImGui::Button("Import into the project"))
                {
                    auto tables = [&](CreatureModelsView::Source& s) -> const RaceCatalog* {
                        if (!s.tables)
                        {
                            s.tables = std::make_unique<RaceCatalog>();
                            s.tables->Load(*s.mpq, s.tablesError);
                        }
                        return s.tablesError.empty() ? s.tables.get() : nullptr;
                    };
                    const RaceCatalog* from = tables(src);
                    const RaceCatalog* client = tables(v.sources[0]);
                    std::string error;
                    std::vector<Change> parts;
                    // Files the source lacks and the project's client has are players' already: nothing to copy.
                    if (from && client)
                        parts = ImportCreatureModelChanges(*from, src.name, m->id, m->displays, *src.mpq, m_mpq, *client,
                                                           m_creatureModelRows, m_displayRows, m_modelRows, *m_project, error);
                    else error = !src.tablesError.empty() ? src.tablesError : v.sources[0].tablesError;
                    if (parts.empty()) Log("Import %s: %s", m->path.c_str(), error.c_str());
                    else
                    {
                        commit(std::move(parts), "import creature model");
                        const uint32_t mine = m_creatureModelRows.ImportedAs(src.name, m->id);
                        Log("Imported %s as model %u with %zu skin(s); export puts its files in the patch.", m->path.c_str(), mine, m->displays.size());
                    }
                }
                ImGui::SetItemTooltip("A new CreatureModelData row and one CreatureDisplayInfo row per skin, in the project's id ranges "
                                      "(File > Project settings); export copies its files from this source.");
            }
        }

        // A model of the project's own: where it came from, and taking it out again.
        if (v.source == 0 && m->project)
        {
            ImGui::SeparatorText("Project");
            const nlohmann::json* p = m_creatureModelRows.Package(m->id);
            const nlohmann::json from = p ? p->value("source", nlohmann::json::object()) : nlohmann::json::object();
            if (!p) ImGui::TextColored(kQuiet, "A model row of the project's own.");
            else if (from.contains("folder"))
                ImGui::TextWrapped("Added from files in %s; its copies are in the project's assets (export puts them in the patch).",
                                   S(from, "folder").c_str());
            else
                ImGui::TextWrapped("Imported from %s (its model %u); export copies its %zu file(s) from there, so this view cannot show "
                                   "files it moved under Creature\\<source>\\ yet: pick it in that source.", S(from, "client").c_str(),
                                   from.value("model", 0u), ChangeStore::List(*p, "files").size());
            if (p && ImGui::Button("Remove from the project"))
            {
                auto parts = RemoveCreatureModelChanges(m->id, m_creatureModelRows, m_displayRows, m_modelRows);
                for (const Change& c : parts)
                {
                    if (c.domain == m_creatureModelRows.Domain()) m_creatureModelRows.Apply(c);
                    else if (c.domain == m_displayRows.Domain()) m_displayRows.Apply(c);
                    else if (c.domain == m_modelRows.Domain()) m_modelRows.Apply(c);
                }
                m_store.Commit(std::move(parts), "remove creature model");
            }
            if (p && from.contains("folder")) ImGui::SetItemTooltip("Takes its rows out; its files stay in assets (delete them there to leave them out of the patch).");
        }

        ImGui::SeparatorText("Skins");
        // The project's client: its skins with the project's own (rows it added on this model) among them.
        std::vector<uint32_t> skins = m->displays;
        auto ownSkin = [&](uint32_t id) { return v.source == 0 && m_project->Owns("creaturedisplayinfo.id", id) && m_displayRows.Edited(id); };
        if (v.source == 0)
        {
            std::erase_if(skins, [&](uint32_t id) { const nlohmann::json* e = m_displayRows.Edited(id); return e && !e->is_object(); });
            for (const auto& [id, row] : m_displayRows.Edits())
                if (row.is_object() && row.value("ModelID", 0u) == m->id && !row.value("ExtendedDisplayInfoID", 0u) &&
                    std::find(skins.begin(), skins.end(), id) == skins.end())
                    skins.push_back(id);
        }
        if (src.filesOnly) ImGui::TextColored(kQuiet, "This source's tables cannot be read: no skins listed.");
        else if (skins.empty()) ImGui::TextColored(kQuiet, "No creature display uses this model.");
        else if (ImGui::BeginTable("##skins", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                                   { 0, std::min(ImGui::GetTextLineHeightWithSpacing() * (float(skins.size()) + 1.5f), 260.0f) }))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Display");
            ImGui::TableSetupColumn("Skin textures", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Scale");
            ImGui::TableHeadersRow();
            for (uint32_t id : skins)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable((std::to_string(id) + "##skin").c_str(), v.display == id, ImGuiSelectableFlags_SpanAllColumns) && v.display != id)
                {
                    v.display = id;
                    v.stale = true;
                }
                if (ownSkin(id))
                {
                    ImGui::SameLine();
                    ImGui::TextColored(kProject, "project");
                }
                ImGui::TableNextColumn();
                std::string names;
                for (const std::string& n : src.looks->SkinNames(id)) names += (names.empty() ? "" : ", ") + n;
                ImGui::TextUnformatted(names.empty() ? "-" : names.c_str());
                ImGui::TableNextColumn();
                const auto look = src.looks->DisplayLook(id);
                ImGui::Text("%.2f", look ? look->scale : 1.0f);
            }
            ImGui::EndTable();
        }
        if (v.look && ImGui::Selectable("Bare model (no skin)", v.display == 0) && v.display != 0)
        {
            v.display = 0;
            v.stale = true;
        }

        // New skins on a client model: rows of the project's own, edited here; pictures become textures beside the model.
        if (v.source == 0 && m->id && !src.filesOnly)
        {
            const std::string folder = m->path.substr(0, m->path.find_last_of("\\/") + 1);
            if (v.folderFor != m->path)   // the skin textures there are: BLPs in the model's folder (the client's and the assets')
            {
                v.folderFor = m->path;
                v.folderTextures.clear();
                const std::string lowerFolder = Lower(folder);
                std::set<std::string> seen;
                auto from = [&](const MpqChain* chain) {
                    if (!chain) return;
                    for (const MpqChain::Entry& e : chain->List())
                    {
                        const std::string lower = Lower(e.name);
                        if (lower.rfind(lowerFolder, 0) != 0 || !lower.ends_with(".blp") || lower.find_first_of("\\/", lowerFolder.size()) != std::string::npos) continue;
                        const std::string name = e.name.substr(folder.size(), e.name.size() - folder.size() - 4);
                        if (seen.insert(Lower(name)).second) v.folderTextures.push_back(name);
                    }
                };
                from(src.mpq);
                from(v.assets.get());
                std::sort(v.folderTextures.begin(), v.folderTextures.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
            }
            auto commitRow = [&](uint32_t id, const nlohmann::json& row, const char* label) {
                m_displayRows.Commit(id, row, label);
                v.stale = true;
            };
            if (ImGui::Button("New skin"))
            {
                const Project::IdRange r = m_project->Range("creaturedisplayinfo.id");
                const uint32_t id = m_displayRows.FreeId(std::max(r.first, 1u), r.last);
                if (!id) Log("New skin: the creaturedisplayinfo.id range is full (File > Project settings).");
                else
                {
                    nlohmann::json row = v.display ? m_displayRows.Row(v.display) : nlohmann::json();
                    if (!row.is_object())
                    {
                        row = nlohmann::json::object();
                        for (const DbcField& f : CreatureDisplayInfoFields()) row[f.name] = f.type == 's' ? nlohmann::json("") : nlohmann::json(0);
                        row["CreatureModelScale"] = 1.0;
                    }
                    row["ID"] = id;
                    row["ModelID"] = m->id;
                    row["ExtendedDisplayInfoID"] = 0;
                    commitRow(id, row, "new creature skin");
                    v.display = id;
                }
            }
            ImGui::SetItemTooltip("A display of the project's own on this model (creaturedisplayinfo.id range), copied from the skin picked above.");
            if (v.display && ownSkin(v.display))
            {
                const uint32_t id = v.display;
                nlohmann::json row = m_displayRows.Row(id);
                ImGui::SameLine();
                if (ImGui::Button("Delete skin"))
                {
                    m_displayRows.Commit(id, nullptr, "delete creature skin");
                    v.display = 0;
                    v.stale = true;
                }
                ImGui::Text("Skin %u", id);
                for (int slot = 1; slot <= 3; ++slot)
                {
                    const std::string key = "TextureVariation[" + std::to_string(slot - 1) + "]";
                    const std::string now = row.value(key, std::string());
                    if (!(v.info && (v.info->skinSlots >> slot & 1)) && now.empty()) continue;   // a slot the model does not draw
                    ImGui::PushID(slot);
                    ImGui::SetNextItemWidth(-90);
                    if (ImGui::BeginCombo(("Skin " + std::to_string(slot)).c_str(), now.empty() ? "(none)" : now.c_str(), ImGuiComboFlags_HeightLarge))
                    {
                        if (ImGui::Selectable("(none)", now.empty())) { row[key] = ""; commitRow(id, row, "creature skin texture"); }
                        for (const std::string& t : v.folderTextures)
                            if (ImGui::Selectable(t.c_str(), _stricmp(t.c_str(), now.c_str()) == 0)) { row[key] = t; commitRow(id, row, "creature skin texture"); }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Picture..."))
                        if (const auto file = PickPicture(m_hwnd, L"Skin texture: a picture whose width and height are powers of two"))
                        {
                            uint32_t w = 0, h = 0;
                            std::vector<uint8_t> rgba;
                            if (!ReadPicture(*file, w, h, rgba)) Log("Skin texture: cannot read %s", file->c_str());
                            else if (!w || !h || (w & (w - 1)) || (h & (h - 1)) || w > 2048 || h > 2048)
                                Log("Skin texture: %s is %u x %u; the client needs powers of two (64, 128, 256 ... 2048) each way.", file->c_str(), w, h);
                            else
                            {
                                // Beside the model under a name of its own: <model>Skin<display>[_<slot>].blp, in assets until export.
                                std::string stem = m->path.substr(folder.size());
                                stem = stem.substr(0, stem.find_last_of('.')) + "Skin" + std::to_string(id) + (slot > 1 ? "_" + std::to_string(slot) : "");
                                std::string rel = folder + stem + ".blp";
                                std::replace(rel.begin(), rel.end(), '\\', '/');
                                const std::filesystem::path target = m_project->AssetsDir() / rel;
                                std::error_code ec;
                                std::filesystem::create_directories(target.parent_path(), ec);
                                const std::vector<uint8_t> blp = WriteIndexedBlp(w, h, rgba.data());
                                std::ofstream(target, std::ios::binary).write(reinterpret_cast<const char*>(blp.data()), std::streamsize(blp.size()));
                                row[key] = stem;
                                commitRow(id, row, "creature skin picture");
                                v.assetsStale = true;
                                ++m_assetsVersion;   // the editor's files read the new ones too
                                v.folderFor.clear();
                                v.previewSource = ~size_t(0);   // a new renderer: the old one may hold a texture of that name
                                Log("Skin texture: %s (%u x %u) as assets\\%s.", file->c_str(), w, h, rel.c_str());
                            }
                        }
                    ImGui::SetItemTooltip("PNG, JPEG, BMP or BLP, sides powers of two. Saved beside the model in the project's assets; export puts it in the patch.");
                    ImGui::PopID();
                }
                float scale = float((v.edit == id ? v.editRow : row).value("CreatureModelScale", 1.0));
                ImGui::SetNextItemWidth(-90);
                if (ImGui::SliderFloat("Scale", &scale, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
                {
                    if (v.edit != id) { v.edit = id; v.editRow = row; }
                    v.editRow["CreatureModelScale"] = scale;
                    if (v.look) v.look->scale = scale;
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && v.edit == id)
                {
                    commitRow(id, v.editRow, "creature skin scale");
                    v.edit = 0;
                    v.editRow = nullptr;
                }
            }
            else if (v.display) ImGui::TextColored(kQuiet, "A skin of the client's: New skin makes an editable copy of it.");
        }

        ImGui::SeparatorText("Textures");
        if (v.look)
            for (const auto& [type, path] : v.look->look.textures)
            {
                const bool has = src.mpq->HasOwn(path) || (v.source && v.sources[0].mpq && v.sources[0].mpq->HasOwn(path));
                ImGui::TextColored(has ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kWarn, "skin %u: %s%s", type - 10, path.c_str(), has ? "" : "  (missing)");
            }
        if (v.info)
            for (const std::string& t : v.info->textures) ImGui::TextUnformatted(t.c_str());
        if (v.info && v.info->skeleton) ImGui::TextColored(kQuiet, "%zu animations", v.info->skeleton->sequences.size());
    }
    ImGui::End();
}
