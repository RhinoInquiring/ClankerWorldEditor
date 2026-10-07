// Differences: another version of the open map scanned against it; each edited area becomes a card to review,
// fly to, and paste (approve) or reject.
#include "App.hpp"

#include "Mpq.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kAccent{ 0.40f, 0.70f, 1.00f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };

bool StartsWithNoCase(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}

/// `text` cut with an ellipsis to fit `width` pixels.
std::string Fit(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string s = text;
    while (s.size() > 1 && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
    return s + "...";
}

std::string Clock(double seconds)
{
    const int s = int(seconds + 0.5);
    char text[16];
    snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
    return text;
}

/// "HTW+O": what kind of edit an area holds, one letter each.
std::string Kinds(uint8_t k)
{
    std::string s;
    if (k & CellDiff::NewTerrain) s += "new terrain ";
    if (k & CellDiff::Heights) s += "heights ";
    if (k & CellDiff::Textures) s += "textures ";
    if (k & CellDiff::Holes) s += "holes ";
    if (k & CellDiff::Water) s += "water ";
    if (k & CellDiff::Objects) s += "objects ";
    if (!s.empty()) s.pop_back();
    return s;
}
}

std::vector<App::DiffCandidate> App::DiffCandidates() const
{
    // This map's copies in the project's client (Azeroth_Asc, ... for Azeroth), then the same map in every attached client.
    std::vector<DiffCandidate> out;
    const std::string& dir = m_terrain.Map();
    if (dir.empty()) return out;
    const std::string base = dir.substr(0, dir.find('_'));
    for (const MapEntry& m : m_maps)
        if (m.directory != dir && StartsWithNoCase(m.directory, base)) out.push_back({ 0, m.directory, m.name + " (" + m.directory + ")" });
    for (size_t s = 1; s < m_ghosts.Sources().size(); ++s)
        if (m_ghosts.Chain(s).Read("World\\Maps\\" + dir + "\\" + dir + ".wdt"))
            out.push_back({ s, dir, m_ghosts.Sources()[s].name + ": " + dir });
    return out;
}

void App::StartDifferences(const DiffCandidate& c)
{
    if (!m_project || m_terrain.Map().empty()) return;
    std::string file = m_terrain.Map() + "__" + (c.source ? m_ghosts.Sources()[c.source].name + "_" : std::string()) + c.map + ".json";
    for (char& ch : file)
        if (!std::isalnum((unsigned char)ch) && ch != '_' && ch != '.' && ch != '-') ch = '_';
    StopCompare();
    m_diffThumbs.clear();
    m_diffNoThumb.clear();
    m_diffTarget = c;
    m_diffs.Start(m_mpq, m_terrain.Map(), c.source ? m_ghosts.Chain(c.source) : m_mpq, c.map, c.label, m_store.Done(),
                  m_project->dir / "differences" / file);
    Log("Scanning %s against %s: %zu tile(s).", c.label.c_str(), m_terrain.Map().c_str(), m_diffs.GetProgress().total);
}

void App::UpdateDifferences()
{
    m_diffs.Update();
    if (m_diffPending.empty() || !m_diffNewTiles.empty()) return;   // nothing to review, or new tiles waiting for Enter
    // Tiles the map does not have: shown alone (solo) from the other version; Enter adds them whole.
    std::set<int> missing;
    for (const auto& [gx, gz] : m_diffPending)
        if (const int key = TileKey(gx / 16, gz / 16); size_t(key) >= m_terrain.Present().size() || !m_terrain.Present()[size_t(key)])
            missing.insert(key);
    if (!missing.empty())
    {
        const std::string shown = m_diffTarget.map == m_terrain.Map() ? std::string() : m_diffTarget.map;
        Ghosts::Layer& l = m_ghosts.AddLayer(m_diffTarget.source, -1, m_diffTarget.label + " (new tiles)", shown);
        l.only = missing;
        m_diffNewLayer = l.id;
        m_diffNewTiles = std::move(missing);
        SetSolo(l.id);
        Log("%zu tile(s) here are not on this map: shown alone from %s. Enter adds them, Del rejects, Esc closes.", m_diffNewTiles.size(),
            m_diffTarget.label.c_str());
        return;
    }
    // A reviewed area is selected once its tiles are loaded (they are pinned meanwhile), then compared in place.
    std::set<ChunkRef> picked;
    size_t waiting = 0;
    for (const auto& [gx, gz] : m_diffPending)
    {
        const int key = TileKey(gx / 16, gz / 16);
        if (size_t(key) >= m_terrain.Present().size() || !m_terrain.Present()[size_t(key)]) continue;   // the map has no tile there
        if (const auto ref = m_terrain.ChunkAtGrid(gx, gz)) picked.insert(*ref);
        else ++waiting;
    }
    if (waiting) return;
    m_diffPending.clear();
    if (picked.empty())
    {
        Log("This area is terrain your map does not have (whole tiles missing); adding tiles is not supported yet.");
        m_diffActive.clear();
        return;
    }
    m_selection = std::move(picked);
    const CompareTarget target{ m_diffTarget.source, m_diffTarget.map, m_diffTarget.label };
    const std::string active = m_diffActive;
    StartCompare(&target);
    m_diffActive = active;   // StartCompare stops any earlier compare, which forgets the reviewed area
}

void App::EndNewTiles()
{
    if (m_diffNewLayer)
    {
        if (m_soloLayer == m_diffNewLayer) SetSolo(0);
        RemoveGhostLayer(m_diffNewLayer);
    }
    m_diffNewLayer = 0;
    m_diffNewTiles.clear();
}

void App::AddDifferenceTiles()
{
    if (m_diffNewTiles.empty()) return;
    const MpqChain& chain = m_diffTarget.source ? m_ghosts.Chain(m_diffTarget.source) : m_mpq;
    const std::string& map = m_diffs.OtherMap();
    const auto wdt = chain.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
    std::vector<TerrainAdapter::NewTile> tiles;
    std::string error;
    for (int key : m_diffNewTiles)
        if (auto t = TerrainAdapter::ReadNewTile(chain, map, key % 64, key / 64)) tiles.push_back(std::move(*t));
        else error += std::to_string(key % 64) + "_" + std::to_string(key / 64) + ": not in " + m_diffTarget.label + "; ";
    auto change = m_terrain.AddTiles(tiles, wdt && WdtBigAlpha(*wdt), error);
    if (!error.empty()) Log("Tiles not added: %s", error.c_str());
    if (!change) return;
    const size_t added = change->data.at("tiles").size();
    std::set<int> done;
    std::set<std::pair<int, int>> cells;   // every chunk of the added tiles: their landmarks come along in the same undo step
    for (const auto& e : change->data.at("tiles"))
    {
        done.insert(TileKey(e[0], e[1]));
        for (int i = 0; i < 256; ++i) cells.insert({ e[0].get<int>() * 16 + i % 16, e[1].get<int>() * 16 + i / 16 });
    }
    std::vector<Change> parts = AddPastedPois(VersionPois(chain, map, cells, 0, 0), change->label);
    if (parts.empty()) m_store.Commit(std::move(*change));
    else
    {
        const std::string label = change->label;
        parts.insert(parts.begin(), std::move(*change));
        m_store.Commit(std::move(parts), label);
    }
    // Chunks on the added tiles are done; the rest of the area (on tiles the map had) is compared as usual.
    std::vector<std::pair<int, int>> pasted, rest;
    for (const auto& cell : m_diffPending) (done.count(TileKey(cell.first / 16, cell.second / 16)) ? pasted : rest).push_back(cell);
    m_diffs.SetStatus(pasted, Differences::Status::Pasted);
    EndNewTiles();
    m_diffPending = std::move(rest);
    if (m_diffPending.empty()) m_diffActive.clear();
    std::vector<Problem> cracks;
    m_terrain.FindCracks(cracks);
    Log("Added %zu tile(s) from %s (one undo step).%s", added, m_diffTarget.label.c_str(),
        cracks.empty() ? "" : " Their edges do not meet every neighbour: see Problems (paste or sculpt across the seam).");
}

void App::ReviewDifference(const Differences::Region& r)
{
    StopCompare();
    EndNewTiles();
    m_selection.clear();
    if (m_terrain.Map() != m_diffs.BaseMap()) GoToTile(m_diffs.BaseMap(), r.x0 / 16, r.z0 / 16);
    // Look at it from the south, high enough to see all of it.
    const float cx = float(r.x0 + r.x1 + 1) * 0.5f * kChunkSize, cz = float(r.z0 + r.z1 + 1) * 0.5f * kChunkSize;
    const float span = float(std::max(r.x1 - r.x0 + 1, r.z1 - r.z0 + 1)) * kChunkSize, dist = span * 0.9f + 60.0f;
    m_camera.pos.x = cx;
    m_camera.pos.z = cz - dist;
    m_camera.pos.y = m_terrain.HeightAt(cx, cz).value_or(m_camera.pos.y - dist * 0.68f) + dist * 0.68f;
    m_camera.yaw = 0;
    m_camera.pitch = -0.6f;
    m_diffPending = r.cells;
    m_diffActive = r.key;
    m_tool = Tool::Copy;
}

void App::RejectDifference()
{
    if (const Differences::Region* r = m_diffs.Find(m_diffActive))
    {
        m_diffs.SetStatus(r->cells, Differences::Status::Rejected);
        Log("Difference at %s rejected (%zu chunks).", AreaLabel(r->area).c_str(), r->cells.size());
    }
    StopCompare();
    EndNewTiles();
    m_diffPending.clear();
    m_diffActive.clear();
}

void App::RenderDifferenceThumb(const Differences::Region& r)
{
    // The other version's area, read straight from its files (large areas get no picture: too many tiles to read).
    const std::set<int> keys = r.Tiles();
    if (keys.size() > 9) { m_diffNoThumb.insert(r.key); return; }
    const MpqChain& chain = m_diffTarget.source ? m_ghosts.Chain(m_diffTarget.source) : m_mpq;
    const std::string& map = m_diffs.OtherMap();
    const auto wdt = chain.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
    const bool bigAlpha = wdt && WdtBigAlpha(*wdt);
    std::map<int, LoadedTile> tiles;
    for (int key : keys)
        if (auto bytes = chain.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt"))
            if (auto adt = ParseAdt(*bytes, bigAlpha)) tiles.emplace(key, LoadedTile::Make(key % 64, key / 64, {}, std::move(*adt)));
    const TerrainClipboard clip = TerrainAdapter::CopyFrom(tiles, std::set<std::pair<int, int>>(r.cells.begin(), r.cells.end()));
    constexpr UINT kSize = 128;
    const std::vector<uint8_t> rgba = RenderAreaThumbnail(clip, kSize);
    if (rgba.empty()) { m_diffNoThumb.insert(r.key); return; }
    m_diffThumbs[r.key] = m_renderer.CreateRgbaTexture(kSize, kSize, rgba);
}

void App::DrawDifferences()
{
    if (!m_project || m_terrain.Map().empty()) { ImGui::TextColored(kQuiet, "Open a map first: its other versions are compared against it."); return; }

    // What against what.
    ImGui::Text("Base: %s (this project)", m_diffs.Started() ? m_diffs.BaseMap().c_str() : m_terrain.Map().c_str());
    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(320);
    const std::string current = m_diffTarget.label.empty() ? "Pick the other version" : m_diffTarget.label;
    const Differences::Progress p = m_diffs.GetProgress();
    ImGui::BeginDisabled(p.running);
    if (ImGui::BeginCombo("##other", current.c_str()))
    {
        for (const DiffCandidate& c : DiffCandidates())
            if (ImGui::Selectable(c.label.c_str(), c.label == m_diffTarget.label)) m_diffTarget = c;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (p.running)
    {
        if (ImGui::Button("Cancel scan")) { m_diffs.Cancel(); Log("Scan cancelled; what it found so far is kept."); }
    }
    else
    {
        ImGui::BeginDisabled(m_diffTarget.map.empty());
        if (ImGui::Button(m_diffs.Started() ? "Rescan" : "Scan")) StartDifferences(m_diffTarget);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Compares every tile of the other version with this map as the project has it.\n"
                              "Tiles unchanged since the last scan come from its saved results.");
    }

    if (p.total && (p.running || p.done < p.total))
    {
        const float f = float(p.done) / float(p.total);
        char overlay[160];
        const double left = p.done > p.cached ? p.seconds / double(p.done) * double(p.total - p.done) : 0;
        snprintf(overlay, sizeof overlay, "%zu / %zu tiles%s   %s elapsed%s", p.done, p.total,
                 p.cached ? (" (" + std::to_string(p.cached) + " from the last scan)").c_str() : "", Clock(p.seconds).c_str(),
                 p.running && p.done ? ("   about " + Clock(left) + " left").c_str() : p.running ? "" : "   (cancelled: Rescan to finish)");
        ImGui::ProgressBar(f, { -1, 0 }, overlay);
        if (p.running && p.tile >= 0) ImGui::TextColored(kQuiet, "Reading tile %d_%d ...  cards appear as areas are found", p.tile % 64, p.tile / 64);
    }
    if (!m_diffs.Started()) { ImGui::TextColored(kQuiet, "Pick the other version, then Scan. Each edited area becomes a card here."); return; }

    // Filters and order.
    // Terrain the map has no tile for (new islands, say) is added as whole tiles; it can be listed on its own.
    auto newTerrainOnly = [](const Differences::Region& r) { return (r.kinds & ~CellDiff::NewTerrain) == 0; };
    size_t pending = 0, rejected = 0, newTerrain = 0;
    for (const auto& r : m_diffs.Regions())
    {
        (r.status == Differences::Status::Rejected ? rejected : pending) += 1;
        newTerrain += newTerrainOnly(r) && r.status == Differences::Status::Pending;
    }
    // Whole tiles waiting to be added: buttons as well as Enter (Enter can go to whatever ImGui has focused).
    if (!m_diffNewTiles.empty())
    {
        ImGui::TextColored(kWarn, "%zu new tile(s) from %s, shown alone in the viewport:", m_diffNewTiles.size(), m_diffTarget.label.c_str());
        ImGui::SameLine();
        if (ImGui::Button(("Add " + std::to_string(m_diffNewTiles.size()) + " tile(s)  Enter").c_str())) AddDifferenceTiles();
        ImGui::SameLine();
        if (ImGui::Button("Reject  Del")) RejectDifference();
        ImGui::SameLine();
        if (ImGui::Button("Close  Esc")) { EndNewTiles(); m_diffPending.clear(); m_diffActive.clear(); }
    }
    ImGui::TextColored(kAccent, "%zu area(s) to review", pending);
    ImGui::SameLine();
    ImGui::TextColored(kQuiet, "  %zu rejected   vs %s", rejected, m_diffs.OtherLabel().c_str());
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##diffSearch", "Search area names", m_diffQuery, sizeof m_diffQuery);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    const char* sorts[] = { "Largest first", "Biggest height change", "Most new objects", "Map position" };
    ImGui::Combo("##sort", &m_diffSort, sorts, IM_ARRAYSIZE(sorts));
    ImGui::SameLine();
    ImGui::Checkbox("Rejected", &m_diffShowRejected);
    if (newTerrain)
    {
        ImGui::SameLine();
        ImGui::Checkbox(("Only new terrain (" + std::to_string(newTerrain) + ")").c_str(), &m_diffShowNewTerrain);
        ImGui::SetItemTooltip("Areas on tiles this map does not have at all (new islands, say).\nApproving one adds its tiles whole from the other version.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##diffSize", &m_thumbSize, 48, 192, "size %.0f");
    if (m_diffs.BaseMap() != m_terrain.Map())
        ImGui::TextColored(kWarn, "Scanned against %s: reviewing a card goes back to it.", m_diffs.BaseMap().c_str());

    std::string query = m_diffQuery;
    std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<const Differences::Region*> shown;
    for (const auto& r : m_diffs.Regions())
    {
        if (m_diffShowNewTerrain && !newTerrainOnly(r)) continue;
        if ((r.status == Differences::Status::Rejected) != m_diffShowRejected) continue;
        if (!query.empty())
        {
            std::string name = AreaLabel(r.area);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (name.find(query) == std::string::npos) continue;
        }
        shown.push_back(&r);
    }
    std::stable_sort(shown.begin(), shown.end(), [&](const Differences::Region* a, const Differences::Region* b) {
        switch (m_diffSort)
        {
        case 1: return a->maxHeight > b->maxHeight;
        case 2: return a->newObjects > b->newObjects;
        case 3: return std::pair{ a->z0, a->x0 } < std::pair{ b->z0, b->x0 };
        default: return a->cells.size() > b->cells.size();
        }
    });

    std::string wantThumb;
    std::optional<std::pair<std::vector<std::pair<int, int>>, Differences::Status>> verdict;
    if (ImGui::BeginChild("##differences", { 0, 0 }, ImGuiChildFlags_Borders))
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float thumb = m_thumbSize, cellW = thumb + style.FramePadding.x * 2 + style.ItemSpacing.x;
        const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cellW));
        for (size_t n = 0; n < shown.size(); ++n)
        {
            const Differences::Region& r = *shown[n];
            ImGui::PushID(r.key.c_str());
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // Enter belongs to the review (add tiles, paste), not to a focused card
            ImGui::BeginGroup();
            const bool active = r.key == m_diffActive;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.75f, 1));
            const auto it = m_diffThumbs.find(r.key);
            const bool clicked = it != m_diffThumbs.end() && it->second
                                     ? ImGui::ImageButton("##thumb", ImTextureID(intptr_t(it->second.Get())), { thumb, thumb })
                                     : ImGui::Button(m_diffNoThumb.count(r.key) ? "large area" : "...", { thumb + style.FramePadding.x * 2, thumb + style.FramePadding.y * 2 });
            if (active) ImGui::PopStyleColor();
            if (it == m_diffThumbs.end() && !m_diffNoThumb.count(r.key) && wantThumb.empty() && ImGui::IsItemVisible()) wantThumb = r.key;
            if (clicked) ReviewDifference(r);
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextColored(kAccent, "%s", AreaLabel(r.area).c_str());
                ImGui::Text("%zu chunk(s), %.0f x %.0f yd, tiles %d_%d", r.cells.size(), float(r.x1 - r.x0 + 1) * kChunkSize,
                            float(r.z1 - r.z0 + 1) * kChunkSize, r.x0 / 16, r.z0 / 16);
                ImGui::Text("Changes: %s", Kinds(r.kinds).c_str());
                if (r.kinds & CellDiff::Heights) ImGui::Text("Height change up to %.1f yd", r.maxHeight);
                if (r.newObjects || r.goneObjects) ImGui::Text("Objects: +%zu new, -%zu only on this map (stay)", r.newObjects, r.goneObjects);
                ImGui::TextColored(kQuiet, r.status == Differences::Status::Rejected ? "Rejected. Right-click: back to review"
                                                                                     : "Click: fly there and show it in place (Enter approves)");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("##menu"))
            {
                if (ImGui::MenuItem("Review (fly there)")) ReviewDifference(r);
                // Verdicts regroup the cards: applied after the grid, which still walks them.
                if (r.status == Differences::Status::Rejected)
                {
                    if (ImGui::MenuItem("Back to review")) verdict = { r.cells, Differences::Status::Pending };
                }
                else if (ImGui::MenuItem("Reject"))
                {
                    if (active) StopCompare();
                    verdict = { r.cells, Differences::Status::Rejected };
                }
                ImGui::EndPopup();
            }
            std::string caption = AreaLabel(r.area);
            caption = caption.substr(0, caption.rfind(" ("));
            ImGui::TextUnformatted(Fit(caption, thumb + style.FramePadding.x * 2).c_str());
            ImGui::TextColored(kQuiet, "%zu ch  %s%s", r.cells.size(), r.kinds & CellDiff::Water ? "water " : "",
                               r.newObjects ? ("+" + std::to_string(r.newObjects) + " obj").c_str() : "");
            ImGui::EndGroup();
            ImGui::PopItemFlag();
            ImGui::PopID();
            if ((n + 1) % size_t(cols) != 0) ImGui::SameLine();
        }
        if (shown.empty()) ImGui::TextColored(kQuiet, p.running ? "Nothing found yet." : m_diffShowRejected ? "Nothing rejected." : "Nothing left to review.");
    }
    ImGui::EndChild();
    if (verdict) m_diffs.SetStatus(verdict->first, verdict->second);
    // One picture per frame, for a card on screen without one.
    if (!wantThumb.empty())
        if (const Differences::Region* r = m_diffs.Find(wantThumb)) RenderDifferenceThumb(*r);
}
