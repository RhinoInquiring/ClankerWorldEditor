// Sources: the project's base files and the versions to compare against, each a stack of layers (folders of MPQs,
// single MPQs, unpacked folders).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };

/// One .mpq file, chosen in the standard Windows dialog.
std::optional<std::string> PickMpq(HWND owner)
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    const COMDLG_FILTERSPEC types[] = { { L"MPQ archives", L"*.mpq;*.MPQ" }, { L"All files", L"*.*" } };
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Pick an MPQ archive");
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dlg->Show(owner))) return std::nullopt;
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), n, nullptr, nullptr);
    CoTaskMemFree(path);
    return result;
}

const char* KindLabel(MpqLayer::Kind k)
{
    return k == MpqLayer::Kind::MpqFolder ? "MPQ folder" : k == MpqLayer::Kind::MpqFile ? "MPQ" : "Unpacked";
}
}

std::vector<std::pair<std::string, std::vector<MpqLayer>>> App::CompareSources() const
{
    std::vector<std::pair<std::string, std::vector<MpqLayer>>> out;
    if (m_project)
        for (const Project::Source& s : m_project->compare) out.push_back({ s.name, s.layers });
    return out;
}

void App::ApplySources()
{
    if (!m_project || m_sourcesEdit.empty()) return;
    const bool baseChanged = [&] {
        const auto& a = m_sourcesEdit.front().layers;
        const auto& b = m_project->base.layers;
        if (a.size() != b.size()) return true;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].kind != b[i].kind || a[i].path != b[i].path || a[i].enabled != b[i].enabled || a[i].installed != b[i].installed) return true;
        return false;
    }();
    m_project->base = m_sourcesEdit.front();
    m_project->compare.assign(m_sourcesEdit.begin() + 1, m_sourcesEdit.end());
    if (baseChanged)
    {
        // Everything reads the base files (map tiles, catalog, models): the project opens again on the new ones.
        if (!Save()) return;
        const std::string dir = m_project->dir.string();
        Log("Base files changed: reopening the project on them.");
        OpenProject(dir);
        return;
    }
    std::string error;
    if (!m_project->Save(error)) { Log("%s", error.c_str()); return; }
    // Compare sources only: the ghost sources start over, the rest stays open.
    StopCompare();
    m_diffs.Cancel();
    std::vector<int> layers;
    for (const auto& l : m_ghosts.Layers()) layers.push_back(l.id);
    for (int id : layers) RemoveGhostLayer(id);
    m_loader.Stop();   // the fallback chains are about to close
    m_mpq.SetFallbacks({});
    std::vector<std::string> errors;
    m_ghosts.Reset(&m_mpq, m_project->name, CompareSources(), errors);
    for (const auto& e : errors) Log("Source not attached: %s", e.c_str());
    UpdateFallbacks();
    m_versionsKey.clear();
    Log("Compare sources applied: %zu attached.", m_ghosts.Sources().size() - 1);
}

void App::DrawSources()
{
    if (!m_showSources) return;
    if (!ImGui::Begin("Sources", &m_showSources)) { ImGui::End(); return; }
    if (!m_project) { ImGui::TextColored(kQuiet, "Open a project first."); ImGui::End(); return; }
    if (m_sourcesEdit.empty() || ImGui::IsWindowAppearing())
    {
        m_sourcesEdit.clear();
        m_sourcesEdit.push_back(m_project->base);
        m_sourcesEdit.insert(m_sourcesEdit.end(), m_project->compare.begin(), m_project->compare.end());
    }
    ImGui::TextColored(kQuiet, "Each source is a stack of layers: later (lower in the list) layers win, as later patches do.\n"
                               "Changes take effect on Apply.");

    int removeSource = -1;
    for (size_t si = 0; si < m_sourcesEdit.size(); ++si)
    {
        Project::Source& s = m_sourcesEdit[si];
        // The report of the chain as last opened: the base is the editor's own, compare sources the ghosts'.
        const MpqChain* opened = si == 0 ? &m_mpq : si < m_ghosts.Sources().size() ? m_ghosts.Sources()[si].mpq : nullptr;
        ImGui::PushID(int(si));
        ImGui::SeparatorText(si == 0 ? "Project base (edited, exported, shown)" : ("Compare source " + std::to_string(si)).c_str());
        ImGui::SetNextItemWidth(240);
        ImGui::InputText("Name", &s.name);
        if (si > 0)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove source")) removeSource = int(si);
        }
        int move = 0, moveFrom = -1, removeLayer = -1;
        if (ImGui::BeginTable("##layers", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("On");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Players have it");
            ImGui::TableSetupColumn("Gives");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (size_t li = 0; li < s.layers.size(); ++li)
            {
                MpqLayer& l = s.layers[li];
                ImGui::PushID(int(li));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Checkbox("##on", &l.enabled);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(KindLabel(l.kind));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(l.path.c_str());
                ImGui::SetItemTooltip("%s", l.path.c_str());
                ImGui::TableNextColumn();
                ImGui::Checkbox("##installed", &l.installed);
                ImGui::SetItemTooltip("On: players' clients have these files (a client's Data, installed patches): the project's patch\n"
                                      "does not carry them. Off: files the edits use from here go into the patch.");
                ImGui::TableNextColumn();
                const bool current = opened && li < opened->Report().size();
                if (current)
                {
                    const auto& r = opened->Report()[li];
                    if (!r.note.empty()) ImGui::TextColored(kWarn, "%s", r.note.c_str());
                    else if (l.kind == MpqLayer::Kind::Folder) ImGui::Text("%zu files", r.files);
                    else ImGui::Text("%zu archive(s)", r.archives);
                }
                else
                    ImGui::TextColored(kQuiet, "apply to see");
                ImGui::TableNextColumn();
                if (ImGui::ArrowButton("##up", ImGuiDir_Up) && li > 0) { move = -1; moveFrom = int(li); }
                ImGui::SameLine();
                if (ImGui::ArrowButton("##down", ImGuiDir_Down) && li + 1 < s.layers.size()) { move = 1; moveFrom = int(li); }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) removeLayer = int(li);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (moveFrom >= 0) std::swap(s.layers[size_t(moveFrom)], s.layers[size_t(moveFrom + move)]);
        if (removeLayer >= 0) s.layers.erase(s.layers.begin() + removeLayer);
        // New layers go on top (the end): they override what is below, as loose files and later patches do.
        if (ImGui::Button("+ MPQ folder"))
            if (auto dir = PickFolder(m_hwnd, L"A folder of MPQs (a client's Data folder, a module bundle...)"))
                s.layers.push_back({ MpqLayer::Kind::MpqFolder, *dir, true, true });
        ImGui::SameLine();
        if (ImGui::Button("+ MPQ file"))
            if (auto file = PickMpq(m_hwnd)) s.layers.push_back({ MpqLayer::Kind::MpqFile, *file, true, true });
        ImGui::SameLine();
        if (ImGui::Button("+ Unpacked folder"))
            if (auto dir = PickFolder(m_hwnd, L"A folder of unpacked game files (the one holding World, DBFilesClient, Textures...)"))
                s.layers.push_back({ MpqLayer::Kind::Folder, *dir, true, si != 0 });   // in the base: new art the patch must carry
        ImGui::SameLine();
        // A scan: what a folder holds, MPQs and unpacked trees at any depth, each its own layer.
        auto note = [&](const std::string& root, const LayerScan& scan, size_t added) {
            std::string text = std::to_string(added) + " layer(s) from " + root;
            if (scan.strayCount)
            {
                text += "; " + std::to_string(scan.strayCount) + " file(s) not under any game folder, left out:";
                for (const auto& f : scan.strays) text += "\n    " + f;
                if (scan.strayCount > scan.strays.size()) text += "\n    ...";
            }
            m_scanNotes[si] = text;
        };
        if (ImGui::Button("+ Scan folder"))
            if (auto dir = PickFolder(m_hwnd, L"A folder holding MPQs and/or unpacked game files, at any depth"))
            {
                LayerScan scan = ScanForLayers(*dir);
                for (MpqLayer& l : scan.layers)
                {
                    if (l.kind == MpqLayer::Kind::Folder) l.installed = si != 0;   // unpacked in the base: the patch carries what is used
                    s.layers.push_back(l);
                }
                note(*dir, scan, scan.layers.size());
            }
        ImGui::SetItemTooltip("Every .mpq inside (any depth) becomes a layer, in the client's load order; every folder holding\n"
                              "World, DBFilesClient, Textures... becomes an unpacked layer above them. Rescan picks up changes.");
        std::set<std::string> roots;
        for (const MpqLayer& l : s.layers)
            if (!l.from.empty()) roots.insert(l.from);
        for (const std::string& root : roots)
        {
            ImGui::SameLine();
            ImGui::PushID(root.c_str());
            if (ImGui::Button("Rescan"))
            {
                LayerScan scan;
                std::set<std::string> had;
                for (const MpqLayer& l : s.layers) had.insert(l.path);
                s.layers = RescanLayers(s.layers, root, &scan);
                for (MpqLayer& l : s.layers)   // newly found unpacked trees in the base: the patch carries what is used
                    if (!had.count(l.path) && l.kind == MpqLayer::Kind::Folder) l.installed = si != 0;
                note(root, scan, scan.layers.size());
            }
            ImGui::SetItemTooltip("Scan %s again: layers still there keep their place and settings, gone ones go, new ones go on top", root.c_str());
            ImGui::PopID();
        }
        if (const auto it = m_scanNotes.find(si); it != m_scanNotes.end()) ImGui::TextColored(kQuiet, "%s", it->second.c_str());
        ImGui::PopID();
    }
    if (removeSource > 0) m_sourcesEdit.erase(m_sourcesEdit.begin() + removeSource);
    ImGui::Separator();
    if (ImGui::Button("+ Compare source"))
        if (auto dir = PickFolder(m_hwnd, L"Another version: a client folder, its Data folder, or any folder of MPQs"))
            m_sourcesEdit.push_back({ std::filesystem::path(*dir).filename().string(), { { MpqLayer::Kind::MpqFolder, *dir, true, true } } });
    ImGui::SameLine(0, 24);
    if (ImGui::Button("Apply")) ApplySources();
    ImGui::SetItemTooltip("Saves the sources in project.json. A base change reopens the project; compare sources reattach.");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) m_sourcesEdit.clear();
    ImGui::End();
}
