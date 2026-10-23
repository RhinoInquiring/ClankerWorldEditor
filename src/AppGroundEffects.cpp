// Ground effects: the client's detail doodads (grass, flowers, pebbles), shown around the camera and edited per
// texture layer (which effect a texture grows) and per cell (where none grows).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 0.95f, 0.75f, 0.30f, 1.00f };

/// The client's graphics levels (FrameXML GraphicsQualityLevels.lua: groundEffectDensity, groundEffectDist).
struct Quality { const char* name; int density; float distance; };
constexpr Quality kQualities[] = { { "Low", 16, 70 }, { "Fair", 40, 110 }, { "Medium", 64, 160 }, { "High", 96, 200 }, { "Ultra", 128, 260 } };

std::string FileOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
}

void App::UpdateGroundEffects()
{
    const auto changed = m_terrain.TakeEffectChanges();
    auto drop = [&](int key) { m_models.RemoveTile(key); m_groundFxChunks.erase(key); };
    if (!m_showGroundFx || m_terrain.Map().empty() || m_terrain.Map() != m_groundFxMap)
    {
        for (auto it = m_groundFxChunks.begin(); it != m_groundFxChunks.end();) { m_models.RemoveTile(it->first); it = m_groundFxChunks.erase(it); }
        m_groundFxMap = m_terrain.Map();
        if (!m_showGroundFx || m_groundFxMap.empty()) return;
    }
    if (!m_groundFxRead)
    {
        m_groundFxRead = true;
        if (!m_groundFx.Load(m_mpq)) Log("This client has no ground effect tables (GroundEffectTexture.dbc): no grass to show.");
        m_groundFxList.clear();
        for (const GroundEffect& e : m_groundFx.All())
            if (const std::string names = m_groundFx.Describe(e); names != "none")
                m_groundFxList.push_back({ e.id, std::to_string(e.id) + "  " + names + "  (" + std::to_string(e.density) + ")" });
    }
    if (!m_groundFx.Loaded()) return;

    // Edits since the last frame: those chunks again; any committed change may have moved the ground, so all of them,
    // each kept on screen until its rebuild comes round.
    for (const ChunkRef& ref : changed)
        if (auto it = m_groundFxChunks.find(kGroundFxKey - (ref.tile * 256 + ref.chunk)); it != m_groundFxChunks.end()) it->second = true;
    if (m_store.Revision() != m_groundFxRevision)
    {
        m_groundFxRevision = m_store.Revision();
        for (auto& [key, stale] : m_groundFxChunks) stale = true;
    }

    const Quality& q = kQualities[std::clamp(m_groundFxQuality, 0, int(std::size(kQualities)) - 1)];
    const XMFLOAT3 eye = ViewEye();
    std::vector<std::pair<float, std::pair<int, int>>> wanted;   // distance, (tile, chunk)
    for (const auto& [key, tile] : m_terrain.Tiles())
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            const AdtChunk& c = tile.adt.chunks[ci];
            const float dx = std::max({ c.baseX - eye.x, 0.0f, eye.x - (c.baseX + kChunkSize) });
            const float dz = std::max({ c.baseZ - eye.z, 0.0f, eye.z - (c.baseZ + kChunkSize) });
            if (dx * dx + dz * dz <= q.distance * q.distance) wanted.push_back({ dx * dx + dz * dz, { key, int(ci) } });
        }
    std::set<int> keep;
    for (const auto& [d, ref] : wanted) keep.insert(kGroundFxKey - (ref.first * 256 + ref.second));
    for (auto it = m_groundFxChunks.begin(); it != m_groundFxChunks.end();)
        if (!keep.count(it->first)) { m_models.RemoveTile(it->first); it = m_groundFxChunks.erase(it); }
        else ++it;

    // Nearest first, a few milliseconds a frame.
    std::sort(wanted.begin(), wanted.end());
    const auto start = std::chrono::steady_clock::now();
    for (const auto& [d, ref] : wanted)
    {
        const int key = kGroundFxKey - (ref.first * 256 + ref.second);
        const auto it = m_groundFxChunks.find(key);
        if (it != m_groundFxChunks.end() && !it->second && m_models.HasTile(key)) continue;
        if (std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count() > 3.0f) break;
        const LoadedTile& tile = m_terrain.Tiles().at(ref.first);
        const AdtChunk* c = m_terrain.Chunk({ ref.first, ref.second });
        if (!c) { drop(key); continue; }
        const int gx = tile.x * 16 + int(c->indexX), gz = tile.y * 16 + int(c->indexY);
        Adt adt;
        adt.doodads = ScatterGroundEffects(*c, UnpackEffectMap(c->effectMap), m_groundFx, q.density, uint32_t(gz * 1024 + gx),
                                           [&](float x, float z) { return m_terrain.ShownHeightAt(x, z); });
        m_models.RemoveTile(key);
        m_models.AddTile(key, adt, m_mpq, ModelRenderer::kDetailLayer);
        m_groundFxChunks[key] = false;
    }
}

void App::DrawGroundEffectsView(float w)
{
    ImGui::Checkbox("Ground effects", &m_showGroundFx);
    ImGui::SetItemTooltip("Grass, flowers and pebbles the client scatters near the camera (detail doodads).\n"
                          "Shown as the quality below would: the client's own pattern differs, what grows where does not.");
    if (!m_showGroundFx) return;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, w - ImGui::GetCursorPosX() - 10));
    if (ImGui::BeginCombo("##gfxquality", kQualities[m_groundFxQuality].name))
    {
        for (int i = 0; i < int(std::size(kQualities)); ++i)
            if (ImGui::Selectable(kQualities[i].name, i == m_groundFxQuality))
            {
                m_groundFxQuality = i;
                for (auto& [key, stale] : m_groundFxChunks) stale = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The client's graphics level: how many grow per cell and how far away (%.0f yd).", kQualities[m_groundFxQuality].distance);
}

void App::DrawGroundEffectsPanel(float w)
{
    if (m_groundFxRead && !m_groundFx.Loaded())
    {
        ImGui::TextColored(kWarn, "This client has no GroundEffectTexture.dbc.");
        return;
    }
    auto describe = [&](uint32_t id) {
        if (!id) return std::string("none");
        const GroundEffect* e = m_groundFx.Effect(id);
        return std::to_string(id) + ": " + (e ? m_groundFx.Describe(*e) : std::string("not in this client"));
    };

    if (Section("Paint"))
    {
        if (m_hover)
            if (const auto cell = m_terrain.EffectAt(m_hover->pos.x, m_hover->pos.z))
            {
                ImGui::TextColored(kQuiet, "Under the cursor: %s", cell->texture.empty() ? "?" : FileOf(cell->texture).c_str());
                ImGui::TextColored(kQuiet, "  grows %s%s", describe(cell->effect).c_str(), cell->hidden ? "  (hidden here)" : "");
            }
        ImGui::Separator();
        if (ImGui::RadioButton("Set a texture's effect", !m_groundFxHide)) m_groundFxHide = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Hide cells", m_groundFxHide)) m_groundFxHide = true;
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Radius##gfx", &m_groundFxRadius, 1.0f, 150.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);

        if (m_groundFxHide)
            ImGui::TextColored(kQuiet, "Left drag: no ground effects in these cells\nCtrl+drag: let them grow again\n"
                                       "Red squares: hidden cells (4.2 yd)\nCtrl+wheel: radius");
        else
        {
            const auto inUse = m_terrain.EffectsInUse();
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::BeginCombo("Texture", m_groundFxTexture.empty() ? "pick one (Alt+click)" : FileOf(m_groundFxTexture).c_str()))
            {
                for (const auto& [texture, effects] : inUse)
                    if (ImGui::Selectable((FileOf(texture) + "##" + texture).c_str(), Catalog::Normalize(m_groundFxTexture) == texture))
                    {
                        m_groundFxTexture = texture;
                        size_t most = 0;   // starts from the effect it carries most often
                        for (const auto& [id, n] : effects) if (n > most) { most = n; m_groundFxEffect = id; }
                    }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Textures on the loaded tiles. Alt+click the ground takes the one under the cursor.");
            ImGui::TextWrapped("Effect %s", describe(m_groundFxEffect).c_str());
            if (ImGui::SmallButton("None")) m_groundFxEffect = 0;
            ImGui::SameLine();
            ImGui::TextColored(kQuiet, "pick others in the Effects tab");

            if (const auto it = inUse.find(Catalog::Normalize(m_groundFxTexture)); it != inUse.end())
            {
                ImGui::SeparatorText("This texture here");
                for (const auto& [id, chunks] : it->second)
                {
                    const std::string label = describe(id) + "  (" + std::to_string(chunks) + " chunk" + (chunks == 1 ? "" : "s") + ")##here" + std::to_string(id);
                    if (ImGui::Selectable(label.c_str(), id == m_groundFxEffect)) m_groundFxEffect = id;
                }
            }

            ImGui::Spacing();
            const bool ready = !m_groundFxTexture.empty();
            ImGui::BeginDisabled(!ready);
            auto apply = [&](const std::vector<ChunkRef>& chunks, const char* where) {
                if (auto change = m_terrain.SetEffect(m_groundFxTexture, m_groundFxEffect, chunks,
                                                      "Ground effect " + std::to_string(m_groundFxEffect) + " on " + FileOf(m_groundFxTexture)))
                {
                    Log("%s %s (%zu chunks)", change->label.c_str(), where, change->data.at("layers").size());
                    m_store.Commit(std::move(*change));
                }
                else Log("Nothing to change %s: the texture already grows that, or is not there.", where);
            };
            const float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2;
            if (ImGui::Button("Tile under the camera", { half, 0 }))
            {
                std::vector<ChunkRef> chunks;
                const int key = TileKey(int(std::floor(m_camera.pos.x / kTileSize)), int(std::floor(m_camera.pos.z / kTileSize)));
                if (const auto t = m_terrain.Tiles().find(key); t != m_terrain.Tiles().end())
                    for (size_t ci = 0; ci < t->second.adt.chunks.size(); ++ci) chunks.push_back({ key, int(ci) });
                apply(chunks, "on the tile under the camera");
            }
            ImGui::SameLine();
            if (ImGui::Button("Every loaded tile", { half, 0 }))
            {
                std::vector<ChunkRef> chunks;
                for (const auto& [key, tile] : m_terrain.Tiles())
                    for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) chunks.push_back({ key, int(ci) });
                apply(chunks, "on every loaded tile");
            }
            ImGui::EndDisabled();
            ImGui::TextColored(kQuiet, "Left drag: the texture grows the effect there\nCtrl+drag: it grows none\n"
                                       "Alt+click: take texture and effect\nCtrl+wheel: radius");
        }
        ImGui::TextColored(kQuiet, "Each cell grows the effect of the texture\nthat covers most of it.");
    }

    if (Section("Effects"))
    {
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##gfxfilter", "search doodad names or ids (ElwGra, Snow...)", &m_groundFxFilter);
        std::string filter = m_groundFxFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        std::vector<const std::pair<uint32_t, std::string>*> shown;
        for (const auto& e : m_groundFxList)
        {
            if (filter.empty()) { shown.push_back(&e); continue; }
            std::string lower = e.second;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (lower.find(filter) != std::string::npos) shown.push_back(&e);
        }
        ImGui::TextColored(kQuiet, "%zu effects with doodads; (n) = density", shown.size());
        if (ImGui::BeginChild("##gfxlist", { w, ImGui::GetContentRegionAvail().y }, ImGuiChildFlags_Borders))
        {
            ImGuiListClipper clipper;
            clipper.Begin(int(shown.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    if (ImGui::Selectable(shown[size_t(i)]->second.c_str(), shown[size_t(i)]->first == m_groundFxEffect))
                    {
                        m_groundFxEffect = shown[size_t(i)]->first;
                        m_groundFxHide = false;
                    }
        }
        ImGui::EndChild();
    }
}

void App::GroundEffectsViewport()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
    {
        if (io.KeyAlt)
        {
            if (const auto cell = m_terrain.EffectAt(m_hover->pos.x, m_hover->pos.z); cell && !cell->texture.empty())
            {
                m_groundFxTexture = cell->texture;
                m_groundFxEffect = cell->effect;
                m_groundFxHide = false;
                Log("Ground effect: %s grows %u here.", FileOf(cell->texture).c_str(), cell->effect);
            }
        }
        else if (!m_groundFxHide && m_groundFxTexture.empty())
            Log("Pick a texture first: Alt+click the ground, or the Texture list.");
        else
            m_terrain.BeginPaint();
    }
    if (m_terrain.Painting() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
    {
        if (m_groundFxHide) m_terrain.HideEffectStep(m_hover->pos, m_groundFxRadius, !io.KeyCtrl);
        else m_terrain.EffectStep(m_hover->pos, m_groundFxRadius, m_groundFxTexture, io.KeyCtrl ? 0 : m_groundFxEffect);
    }
    if (m_terrain.Painting() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const std::string label = m_groundFxHide ? (io.KeyCtrl ? "Show ground effects" : "Hide ground effects")
                                                 : "Ground effect " + std::to_string(io.KeyCtrl ? 0 : m_groundFxEffect) + " on " + FileOf(m_groundFxTexture);
        if (auto change = m_terrain.EndPaint(label))
        {
            Log("%s (%zu chunks)", change->label.c_str(), change->data.at("layers").size());
            m_store.Commit(std::move(*change));
        }
    }
}

void App::BuildGroundEffectOverlay(std::vector<LineVertex>& lines) const
{
    // Hidden cells near the cursor: red squares on the ground.
    if (!m_hover) return;
    const XMFLOAT4 red{ 1.0f, 0.25f, 0.2f, 0.9f };
    const float reach = std::max(m_groundFxRadius * 2, 80.0f);
    for (const auto& [key, tile] : m_terrain.Tiles())
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            const AdtChunk* c = ShownChunk({ key, int(ci) });
            if (!c || !c->noEffect) continue;
            if (std::fabs(c->baseX + kChunkSize / 2 - m_hover->pos.x) > reach || std::fabs(c->baseZ + kChunkSize / 2 - m_hover->pos.z) > reach) continue;
            auto p = [&](int row, int col) {
                return XMFLOAT3{ c->baseX + col * kUnitSize, c->baseY + c->heights[size_t(row * 17 + col)] + 0.3f, c->baseZ + row * kUnitSize };
            };
            for (int cy = 0; cy < 8; ++cy)
                for (int cx = 0; cx < 8; ++cx)
                    if (c->noEffect >> (cy * 8 + cx) & 1)
                    {
                        const XMFLOAT3 a = p(cy, cx), b = p(cy, cx + 1), d = p(cy + 1, cx + 1), e = p(cy + 1, cx);
                        for (const auto& [from, to] : { std::pair{ a, b }, std::pair{ b, d }, std::pair{ d, e }, std::pair{ e, a }, std::pair{ a, d } })
                        {
                            lines.push_back({ from, red });
                            lines.push_back({ to, red });
                        }
                    }
        }
}
