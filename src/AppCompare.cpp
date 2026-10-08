// Compare: the selected chunks shown as every other version of them in turn, before anything is pasted.
#include "App.hpp"

#include "Mpq.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kAccent{ 0.40f, 0.70f, 1.00f, 1.00f };

bool StartsWithNoCase(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}
}

void App::StartCompare(const CompareTarget* only)
{
    if (m_selection.empty()) { Log("Select the chunks to compare first (Select or Copy tool)."); return; }
    StopCompare();
    std::set<int> tiles;
    for (ChunkRef r : m_selection) tiles.insert(r.tile);

    m_compare.push_back({ 0, false, "This map (as edited)" });
    if (only)
    {
        // One version only (a reviewed difference): the map at these coordinates in that source.
        const std::string shown = only->map == m_terrain.Map() ? std::string() : only->map;
        Ghosts::Layer& l = m_ghosts.AddLayer(only->source, -1, only->label, shown);
        l.visible = false;
        l.only = tiles;
        m_compare.push_back({ l.id, true, l.label });
        m_comparing = true;
        m_compareAt = 1;
        m_compareKey.clear();
        m_compareStatsKey.clear();
        m_tool = Tool::Copy;
        m_compareCycledAt = -1e9;   // straight to the blended view
        return;
    }
    for (const auto& l : m_ghosts.Layers()) m_compare.push_back({ l.id, false, l.label });
    // Versions shipped as maps of their own at the same coordinates (Azeroth_Asc, Azeroth_Epoch, ... for Azeroth).
    const std::string& dir = m_terrain.Map();
    const std::string base = dir.substr(0, dir.find('_'));
    for (const MapEntry& m : m_maps)
    {
        if (m.directory == dir || !StartsWithNoCase(m.directory, base) || m_ghosts.Find(0, -1, m.directory)) continue;
        const auto wdt = m_mpq.Read("World\\Maps\\" + m.directory + "\\" + m.directory + ".wdt");
        if (!wdt) continue;
        const std::vector<bool> present = WdtTiles(*wdt);
        if (std::none_of(tiles.begin(), tiles.end(), [&](int k) { return size_t(k) < present.size() && present[size_t(k)]; })) continue;
        Ghosts::Layer& l = m_ghosts.AddLayer(0, -1, m.name + " (" + m.directory + ")", m.directory);
        l.visible = false;
        l.only = tiles;
        m_compare.push_back({ l.id, true, l.label });
    }
    if (m_compare.size() == 1)
    {
        m_compare.clear();
        Log("No other version of this area: no ghost layers and no %s_* maps cover it.", base.c_str());
        return;
    }
    m_comparing = true;
    m_compareAt = 1;
    m_compareKey.clear();
    m_compareStatsKey.clear();
    m_tool = Tool::Copy;
    Log("Comparing %zu chunk(s) across %zu version(s): [ and ] cycle, Enter pastes the shown one, Esc stops.", m_selection.size(), m_compare.size() - 1);
}

void App::CommitCompare()
{
    const size_t before = m_store.Done().size();
    CommitPlacement();
    if (m_store.Done().size() == before) return;   // nothing pasted (tiles still loading, say): the compare stays
    if (!m_diffActive.empty())
    {
        std::vector<std::pair<int, int>> cells;
        for (ChunkRef r : m_selection) cells.push_back(m_terrain.GridOf(r));
        for (const auto& pc : m_plan.chunks) cells.push_back(m_terrain.GridOf(pc.ref));   // the blend band differs now by design
        m_diffs.SetStatus(cells, Differences::Status::Pasted);
        Log("Difference approved and pasted (%zu chunks marked done).", cells.size());
    }
    StopCompare();
}

void App::StopCompare()
{
    if (m_compare.empty()) return;
    for (const CompareEntry& e : m_compare)
        if (e.own) RemoveGhostLayer(e.layer);
    m_compare.clear();
    m_comparing = false;
    m_diffActive.clear();
    m_compareClips.clear();
    m_planCache.clear();
    m_compareClipTag.clear();
    m_compareClipVersion = -1;
    CancelPin();
    m_placing = false;
    m_clipboard = {};
    ++m_clipVersion;
}

void App::CycleCompare(int step)
{
    if (!m_comparing) return;
    const int n = int(m_compare.size());
    for (int i = 1; i <= n; ++i)
    {
        const size_t at = size_t(((int(m_compareAt) + step * i) % n + n) % n);
        const CompareEntry& e = m_compare[at];
        // Versions known to be identical here, or not covering the area at all, are skipped (the map itself never is).
        if (at && e.ready && (e.diff.Same() || !e.diff.cells)) continue;
        m_compareAt = at;
        break;
    }
    m_tool = Tool::Copy;
    m_compareCycledAt = ImGui::GetTime();
}

void App::UpdateCompare()
{
    if (!m_comparing) return;
    std::set<int> tiles;
    std::set<std::pair<int, int>> cells;
    for (ChunkRef r : m_selection)
    {
        tiles.insert(r.tile);
        cells.insert(m_terrain.GridOf(r));
    }
    for (const CompareEntry& e : m_compare)
        if (e.own)
            if (Ghosts::Layer* l = m_ghosts.Find(e.layer)) l->only = tiles;

    // What the numbers depend on: the selection, the map's edits and which tiles each version has loaded so far.
    std::string sel;
    for (const auto& [x, z] : cells) sel += std::to_string(x) + "," + std::to_string(z) + ";";
    std::string key = std::to_string(std::hash<std::string>{}(sel)) + " " + std::to_string(m_store.Done().size()) + " " +
                      std::to_string(m_terrain.MissingTiles(tiles));
    std::vector<size_t> loaded(m_compare.size());
    for (size_t i = 0; i < m_compare.size(); ++i)
        if (const Ghosts::Layer* l = m_compare[i].layer ? m_ghosts.Find(m_compare[i].layer) : nullptr)
            for (int k : tiles) loaded[i] += l->tiles.count(k) + l->missing.count(k);
    for (size_t n : loaded) key += " " + std::to_string(n);

    if (key != m_compareStatsKey)
    {
        m_compareStatsKey = key;
        auto classify = [&](std::string name) {   // 0 project client, 1 another client (export copies it), 2 nowhere
            if (name.size() > 4)
            {
                std::string ext = name.substr(name.size() - 4);
                for (char& c : ext) c = char(std::tolower((unsigned char)c));
                if (ext == ".mdx" || ext == ".mdl") name.replace(name.size() - 4, 4, ".m2");
            }
            if (m_mpq.HasInstalled(name)) return 0;
            if (m_mpq.HasOwn(name)) return 1;   // in the project's files, not in players' clients: export copies it
            for (size_t s = 1; s < m_ghosts.Sources().size(); ++s)
                if (m_ghosts.Sources()[s].mpq->HasOwn(name)) return 1;
            return 2;
        };
        for (size_t i = 0; i < m_compare.size(); ++i)
        {
            CompareEntry& e = m_compare[i];
            const Ghosts::Layer* l = e.layer ? m_ghosts.Find(e.layer) : nullptr;
            e.ready = !l || loaded[i] == tiles.size();
            e.diff = l ? CompareArea(m_terrain.Tiles(), l->tiles, cells) : AreaDiff{};
            e.assetsOther = e.assetsMissing = 0;
            if (!l) continue;
            // Files the version's chunks and new objects name directly (what their models pull in is checked on export).
            std::set<std::string> files;
            for (const auto& [gx, gz] : cells)
            {
                const auto it = l->tiles.find(TileKey(gx / 16, gz / 16));
                if (it == l->tiles.end()) continue;
                const int16_t c = it->second.byGrid[size_t((gz % 16) * 16 + gx % 16)];
                if (c < 0) continue;
                const AdtChunk& chunk = it->second.adt.chunks[size_t(c)];
                for (uint32_t t = 0; t < chunk.layerCount && t < 4; ++t)
                    if (chunk.textureIds[t] < it->second.adt.textures.size()) files.insert(it->second.adt.textures[chunk.textureIds[t]]);
            }
            for (const auto& d : e.diff.newDoodads) files.insert(d.model);
            for (const auto& w : e.diff.newWmos) files.insert(w.model);
            for (const std::string& f : files)
                switch (classify(f))
                {
                case 1: ++e.assetsOther; break;
                case 2: ++e.assetsMissing; break;
                default: break;
                }
        }
        // The models the versions would add load in the background now, not when the version is first shown.
        std::vector<std::pair<std::string, bool>> models;
        for (const CompareEntry& e : m_compare)
        {
            for (const auto& d : e.diff.newDoodads) models.push_back({ M2Name(d.model), false });
            for (const auto& w : e.diff.newWmos) models.push_back({ w.model, true });
        }
        if (!models.empty()) m_loader.WantModels(std::move(models));
    }

    // The shown version, pinned where the selection is, built again when it or the selection changes.
    const std::string showKey = std::to_string(std::hash<std::string>{}(sel)) + " " + std::to_string(m_compareAt) + " " +
                                std::to_string(loaded[m_compareAt]) + " " + std::to_string(m_store.Done().size());
    if (showKey == m_compareKey) return;
    m_compareKey = showKey;
    const CompareEntry& e = m_compare[m_compareAt];
    const Ghosts::Layer* l = e.layer ? m_ghosts.Find(e.layer) : nullptr;
    if (auto hit = m_compareClips.find(showKey); hit != m_compareClips.end() && !hit->second.Empty())
    {
        ShowCompareClip(showKey, hit->second);
        return;
    }
    TerrainClipboard clip = l && !cells.empty() ? TerrainAdapter::CopyFrom(l->tiles, cells) : TerrainClipboard{};
    if (clip.Empty())
    {
        // The map itself, or nothing of this version loaded under the selection yet.
        CancelPin();
        m_placing = false;
        return;
    }
    // Landmarks: the paste skips the ones the map has already (AddPastedPois).
    clip.pois = VersionPois(m_ghosts.Chain(l->source), l->map.empty() ? m_terrain.Map() : l->map, cells, clip.originX, clip.originZ);
    // Objects: the paste leaves the area's objects as the version has them.
    SetAreaObjects(clip, e.diff);
    if (m_compareClips.size() >= 32) m_compareClips.clear();   // ponytail: whole-cache reset; an LRU if huge selections thrash it
    ShowCompareClip(showKey, m_compareClips[showKey] = std::move(clip));
}

void App::ShowCompareClip(const std::string& tag, const TerrainClipboard& clip)
{
    m_clipboard = clip;
    ++m_clipVersion;
    m_compareClipTag = tag;
    m_compareClipVersion = m_clipVersion;
    m_placing = true;
    if (!m_heightBeforeInPlace) m_heightBeforeInPlace = m_pasteHeightMode;
    m_pasteHeightMode = PasteHeight::Absolute;
    m_pasteOffset = 0;
    m_pin = std::pair{ m_clipboard.originX + (m_clipboard.Width() - 1) / 2, m_clipboard.originZ + (m_clipboard.Depth() - 1) / 2 };
}

void App::DrawCompare()
{
    ImGui::SeparatorText("Compare selection");
    if (!m_comparing)
    {
        ImGui::BeginDisabled(m_selection.empty());
        if (ImGui::Button("Compare versions   [ / ]")) StartCompare();
        ImGui::EndDisabled();
        ImGui::TextColored(kQuiet, "Shows the selected chunks as each other version in place\n(ghost layers and this map's _Asc, _Epoch, ... copies).");
        return;
    }
    if (const Differences::Region* r = m_diffActive.empty() ? nullptr : m_diffs.Find(m_diffActive))
    {
        ImGui::TextColored(kAccent, "Reviewing: %s, %zu chunk(s)", AreaLabel(r->area).c_str(), r->cells.size());
        if (ImGui::Button("Approve  Enter") && m_pin) { CommitCompare(); return; }
        ImGui::SetItemTooltip("Pastes this version here and marks the area done");
        ImGui::SameLine();
        if (ImGui::Button("Reject  Del")) { RejectDifference(); return; }
        ImGui::SameLine();
        if (ImGui::Button("Close  Esc")) { StopCompare(); return; }
    }
    else
    {
        if (ImGui::Button("Paste shown version  Enter") && m_pin) { CommitCompare(); return; }
        ImGui::SameLine();
        if (ImGui::Button("Stop  Esc")) { StopCompare(); return; }
    }
    if (m_compareAt && m_pin && m_plan.widthYards > 0) ImGui::TextColored(kQuiet, "Blend band for the shown version: %.0f yd", m_plan.widthYards);

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##compare", 7, flags, { 0, std::min(26.0f * float(m_compare.size() + 1), 320.0f) })) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Chunks");
    ImGui::TableSetupColumn("Height mean / max");
    ImGui::TableSetupColumn("Edge step");
    ImGui::TableSetupColumn("Water");
    ImGui::TableSetupColumn("Objects + / -");
    ImGui::TableSetupColumn("Assets");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < m_compare.size(); ++i)
    {
        const CompareEntry& e = m_compare[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(e.label.c_str(), m_compareAt == i, ImGuiSelectableFlags_SpanAllColumns)) { m_compareAt = i; m_tool = Tool::Copy; m_compareCycledAt = ImGui::GetTime(); }
        const AreaDiff& d = e.diff;
        ImGui::TableNextColumn();
        if (!i) { ImGui::TextColored(kQuiet, "-"); ImGui::PopID(); continue; }
        if (!e.ready) ImGui::TextColored(kQuiet, "loading");
        else if (!d.cells) ImGui::TextColored(kQuiet, "not here");
        else if (d.Same()) ImGui::TextColored(kQuiet, "identical");
        else ImGui::Text("%zu / %zu", d.changed, d.cells);
        ImGui::SetItemTooltip("Chunks that differ (heights > 0.5 yd, textures or holes) / chunks this version has");
        ImGui::TableNextColumn();
        if (d.cells) ImGui::Text("%.2f / %.1f yd", d.meanHeight, d.maxHeight);
        ImGui::TableNextColumn();
        if (d.cells) ImGui::TextColored(d.maxEdge > 2.0f ? ImVec4(1, 0.66f, 0.25f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%.1f yd", d.maxEdge);
        ImGui::SetItemTooltip("Largest height step on the selection's outer edge (mean %.2f yd): what the paste must blend into the map", d.meanEdge);
        ImGui::TableNextColumn();
        if (d.cells) d.water ? ImGui::Text("%zu", d.water) : ImGui::TextColored(kQuiet, "same");
        ImGui::SetItemTooltip("Chunks whose terrain water differs: the paste replaces their water with this version's (removes it where it has none)");
        ImGui::TableNextColumn();
        if (d.cells) ImGui::Text("+%zu / -%zu", d.newDoodads.size() + d.newWmos.size(), d.goneDoodads + d.goneWmos);
        ImGui::SetItemTooltip("+ objects only this version has (a paste adds them)\n- objects only the map has (a paste removes them)");
        ImGui::TableNextColumn();
        if (e.assetsMissing) ImGui::TextColored({ 1, 0.4f, 0.35f, 1 }, "%zu missing", e.assetsMissing);
        else if (e.assetsOther) ImGui::Text("%zu copied", e.assetsOther);
        else if (d.cells) ImGui::TextColored(kQuiet, "ok");
        ImGui::SetItemTooltip("Textures and models the version names directly:\ncopied = only another attached client has them (export copies them)\nmissing = no source has them");
        ImGui::PopID();
    }
    ImGui::EndTable();
}
