// Roads: editor-only splines that paint and grade the terrain under them, live, until baked into ordinary edits.
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

std::string NameOf(const Road& r) { return r.name.empty() ? "Road " + std::to_string(r.id) : r.name; }

std::string FileOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

float Length(const Road& r)
{
    const auto s = SampleRoad(r);
    float len = 0;
    for (size_t i = 1; i < s.size(); ++i) len += std::hypot(s[i].pos.x - s[i - 1].pos.x, s[i].pos.z - s[i - 1].pos.z);
    return len;
}

/// Where a new point goes between existing ones: after the point that starts the nearest straight leg.
size_t InsertAt(const Road& r, const XMFLOAT3& p)
{
    size_t best = r.points.size();
    float bestD = 1e30f;
    for (size_t i = 0; i + 1 < r.points.size(); ++i)
    {
        const XMFLOAT3 &a = r.points[i].pos, &b = r.points[i + 1].pos;
        const float ex = b.x - a.x, ez = b.z - a.z, len2 = ex * ex + ez * ez;
        const float u = len2 > 1e-6f ? std::clamp(((p.x - a.x) * ex + (p.z - a.z) * ez) / len2, 0.0f, 1.0f) : 0.0f;
        const float dx = a.x + ex * u - p.x, dz = a.z + ez * u - p.z, d = dx * dx + dz * dz;
        if (d < bestD) { bestD = d; best = i + 1; }
    }
    return best;
}
}

void App::EditRoad(const Road& edited, bool commit, const std::string& label)
{
    if (!commit)
    {
        // A slow preview (a long road, a setting that redraws all of it) waits a few times its own cost before the next,
        // so dragging stays smooth; the last one waits in m_roadPending.
        const auto now = std::chrono::steady_clock::now();
        if (now - m_roadPreviewAt < std::chrono::duration<float, std::milli>(std::min(m_roadPreviewMs * 3.0f, 150.0f)))
        {
            m_roadPending = edited;
            return;
        }
        m_roadPending.reset();
        m_roads.Preview(&edited);
        m_roadPreviewAt = std::chrono::steady_clock::now();
        m_roadPreviewMs = std::chrono::duration<float, std::milli>(m_roadPreviewAt - now).count();
        return;
    }
    m_roadPending.reset();
    const Road* saved = m_roads.Saved(edited.id);
    const std::optional<Road> before = saved ? std::optional(*saved) : std::nullopt;
    if (before && before->ToJson() == edited.ToJson()) { m_roads.Preview(nullptr); return; }
    m_roads.Commit(before ? &*before : nullptr, &edited, label);
    m_roadDefaults = edited;   // the next road starts like this one
    m_roadDefaults.points.clear();
}

void App::BakeSelectedRoad()
{
    const Road* saved = m_roads.Saved(m_roadSel);
    if (!saved) return;
    const Road road = *saved;
    const std::string label = "Bake " + NameOf(road) + " into the terrain";
    auto terrain = m_terrain.BakeRoad(road, label);
    std::vector<Change> parts;
    if (terrain) parts.push_back(std::move(*terrain));   // already applied to the tiles
    Change remove = m_roads.MakeChange(&road, nullptr, label);
    m_roads.Apply(remove);
    parts.push_back(std::move(remove));
    m_store.Commit(std::move(parts), label);
    m_roadSel = 0;
    m_roadPoint.reset();
    Log("%s: the road is now ordinary terrain edits (undo brings the road back).", label.c_str());
}

void App::RoadsViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    const ImGuiIO& io = ImGui::GetIO();
    m_roadHover.reset();
    const auto roads = m_roads.OnMap(m_terrain.Map());
    if (ImGui::IsItemHovered() && m_roadLines)   // hidden points cannot be picked
    {
        float best = 12.0f;
        for (const Road* r : roads)
            for (size_t i = 0; i < r->points.size(); ++i)
                if (const auto s = ToScreen(viewProj, r->points[i].pos, origin, size))
                    if (const float d = std::hypot(s->x - io.MousePos.x, s->y - io.MousePos.y); d < best) { best = d; m_roadHover = { r->id, i }; }
    }
    if (m_roadDraw && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) m_roadDraw = false;
    if (!ImGui::IsItemActivated() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    const Road* selected = m_roads.Saved(m_roadSel);
    if ((m_roadDraw || io.KeyCtrl) && m_hover && m_project && !m_roadHover)
    {
        // Drawing: each click adds a point at the end (Ctrl+click: between the two nearest points). One undo step each.
        Road r;
        if (selected) r = *selected;
        else
        {
            r = m_roadDefaults;
            r.id = m_roads.NextId();
            r.map = m_terrain.Map();
            if (r.texture.empty()) r.texture = m_activeTexture;
        }
        const RoadPoint p{ m_hover->pos, 0 };
        size_t at = r.points.size();
        if (io.KeyCtrl && r.points.size() >= 2) at = InsertAt(r, p.pos);
        else if (m_roadPoint && *m_roadPoint == 0 && r.points.size() >= 2) at = 0;   // the first point selected: extend from that end
        r.points.insert(r.points.begin() + std::ptrdiff_t(at), p);
        EditRoad(r, true, std::string(selected ? "Add a point to " : "Start ") + NameOf(r));
        m_roadSel = r.id;
        m_roadPoint = at;
        if (!selected) m_roadDraw = true;
        return;
    }
    if (io.KeyAlt && m_roadSel && m_hover)
    {
        MoveSelectionTo(m_hover->pos);
        return;
    }
    if (m_roadHover)
    {
        m_roadSel = m_roadHover->first;
        m_roadPoint = m_roadHover->second;
    }
    else if (!m_roadDraw)
    {
        // Clicking near a road's line selects the whole road.
        m_roadPoint.reset();
        m_roadSel = 0;
        if (m_hover)
            for (const Road* r : roads)
            {
                const auto samples = SampleRoad(*r);
                for (const RoadSample& s : samples)
                    if (std::hypot(s.pos.x - m_hover->pos.x, s.pos.z - m_hover->pos.z) < s.width / 2 + 1) { m_roadSel = r->id; break; }
                if (m_roadSel) break;
            }
    }
}

void App::BuildRoadOverlay(std::vector<LineVertex>& lines) const
{
    if (!m_roadLines) return;   // the road as it will look
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& c) { lines.push_back({ a, c }); lines.push_back({ b, c }); };
    for (const Road* r : m_roads.OnMap(m_terrain.Map()))
    {
        const bool selected = r->id == m_roadSel;
        const XMFLOAT4 c = selected ? XMFLOAT4{ 1, 1, 1, 0.95f } : XMFLOAT4{ 1, 0.7f, 0.3f, 0.7f };
        const XMFLOAT4 edge{ c.x, c.y, c.z, c.w * 0.45f };
        const auto samples = SampleRoad(*r);
        for (size_t i = 0; i + 1 < samples.size(); ++i)
        {
            const RoadSample &a = samples[i], &b = samples[i + 1];
            const float dx = b.pos.x - a.pos.x, dz = b.pos.z - a.pos.z, len = std::max(std::hypot(dx, dz), 1e-4f);
            const float nx = -dz / len, nz = dx / len;   // to the side
            line({ a.pos.x, a.pos.y + 0.4f, a.pos.z }, { b.pos.x, b.pos.y + 0.4f, b.pos.z }, c);
            if (!selected) continue;
            for (float side : { -1.0f, 1.0f })
                line({ a.pos.x + nx * side * a.width / 2, a.pos.y + 0.4f, a.pos.z + nz * side * a.width / 2 },
                     { b.pos.x + nx * side * b.width / 2, b.pos.y + 0.4f, b.pos.z + nz * side * b.width / 2 }, edge);
        }
        // Points: a post with a diamond; the selected one bigger.
        for (size_t i = 0; i < r->points.size(); ++i)
        {
            const XMFLOAT3& p = r->points[i].pos;
            const bool on = selected && m_roadPoint == i, hovered = m_roadHover && m_roadHover->first == r->id && m_roadHover->second == i;
            const float s = on ? 1.6f : 1.0f;
            const XMFLOAT4 pc = on ? XMFLOAT4{ 0.4f, 1, 0.5f, 1 } : hovered ? XMFLOAT4{ 1, 1, 0.6f, 1 } : c;
            const XMFLOAT3 top{ p.x, p.y + 3, p.z };
            line(p, top, pc);
            const XMFLOAT3 k[4] = { { p.x - s, top.y, p.z }, { p.x, top.y + s, p.z }, { p.x + s, top.y, p.z }, { p.x, top.y - s, p.z } };
            for (int j = 0; j < 4; ++j) line(k[j], k[(j + 1) % 4], pc);
        }
    }
}

void App::DrawRoadsPanel(float w)
{
    const bool editable = m_project && !m_terrain.Map().empty();
    if (m_roadPending && m_roadPending->id == m_roadSel) EditRoad(Road(*m_roadPending), false, {});   // a spaced-out preview's turn
    const auto roads = m_roads.OnMap(m_terrain.Map());

    if (Section("Roads"))
    {
        ImGui::Checkbox("Show lines and points", &m_roadLines);
        ImGui::SetItemTooltip("Off: only the road itself, as it will look in game (handles hidden too).\n"
                              "Other tools always show roads without lines. Also in View.");
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button(m_roadDraw && !m_roadSel ? "Click the ground... (Enter: done)" : "New road", { w, 0 }))
        {
            m_roadSel = 0;
            m_roadPoint.reset();
            m_roadDraw = true;
        }
        ImGui::SetItemTooltip("Click the ground for each point; Enter or right-click finishes.");
        ImGui::EndDisabled();
        if (ImGui::BeginListBox("##roads", { w, std::min(200.0f, 26.0f + 20.0f * float(roads.size())) }))
        {
            for (const Road* r : roads)
            {
                char text[160];
                snprintf(text, sizeof text, "%s   %zu points, %.0f yd", NameOf(*r).c_str(), r->points.size(), Length(*r));
                if (ImGui::Selectable(text, r->id == m_roadSel, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_roadSel = r->id;
                    m_roadPoint.reset();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !r->points.empty())
                    {
                        float x, y, z;
                        EditorToServer(r->points[r->points.size() / 2].pos, x, y, z);
                        FlyTo(CurrentMapId(), x, y, z);
                    }
                }
            }
            ImGui::EndListBox();
        }
        ImGui::TextColored(kQuiet, "Roads are the editor's only: the game gets the\nground they leave (export), or Bake writes them\n"
                                   "into the terrain for good.");
    }

    const Road* shown = m_roadPending && m_roadPending->id == m_roadSel ? &*m_roadPending : m_roads.Find(m_roadSel);   // the newest values
    if (!shown)
    {
        m_roadSel = 0;
        return;
    }
    Road r = *shown;
    if (m_roadPoint && *m_roadPoint >= r.points.size()) m_roadPoint.reset();

    if (Section("Selected", false))
    {
        ImGui::BeginDisabled(!editable);
        DrawTransformBar(w);
        bool live = false, done = false;
        std::string label = "Edit " + NameOf(r);
        auto track = [&](bool changed, const char* what) {
            if (changed) { live = true; label = std::string(what) + " of " + NameOf(r); }
            done |= ImGui::IsItemDeactivatedAfterEdit();
        };
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::InputText("Name", &r.name)) { live = true; label = "Rename " + NameOf(*shown); }
        done |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::Button(m_roadDraw ? "Adding points... (Enter: done)" : "Add points", { w, 0 })) m_roadDraw = !m_roadDraw;
        ImGui::SetItemTooltip("Clicks add points at the end (with the first point selected: at the start).\nCtrl+click: a point between the nearest two.");

        ImGui::SeparatorText("Shape");
        ImGui::SetNextItemWidth(w - 110);
        track(ImGui::SliderFloat("Width", &r.width, 1.0f, 40.0f, "%.1f yd"), "Width");
        if (ImGui::Checkbox("Follow the ground", &r.followGround)) { live = done = true; label = "Follow the ground: " + NameOf(r); }
        ImGui::SetItemTooltip("On: the road rides the ground, smoothed along its length (hills stay).\n"
                              "Off: it runs straight between its points' heights (ramps, cuttings); move points up and down.");
        ImGui::SetNextItemWidth(w - 110);
        track(ImGui::SliderFloat("Flatten", &r.grade, 0.0f, 1.0f, "%.2f"), "Flatten");
        ImGui::SetItemTooltip("How far the ground is pulled to the road's own height: level side to side, smooth along it.");
        ImGui::SetNextItemWidth(w - 110);
        track(ImGui::SliderFloat("Sink", &r.sink, 0.0f, 2.0f, "%.2f yd"), "Sink");
        ImGui::SetItemTooltip("How far the road sits below the ground beside it (worn paths sit a little low).");

        ImGui::SeparatorText("Look");
        auto texture = [&](const char* what, std::string& name) {
            ImGui::PushID(what);
            if (!name.empty())
            {
                ImGui::Image(ImTextureID(intptr_t(m_renderer.TextureFor(name, m_mpq))), { 40, 40 });
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::Text("%s: %s", what, name.empty() ? "none" : FileOf(name).c_str());
            ImGui::BeginDisabled(m_activeTexture.empty() || m_activeTexture == name);
            if (ImGui::SmallButton("Use the picked texture")) { name = m_activeTexture; live = done = true; label = std::string(what) + " of " + NameOf(r); }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", m_activeTexture.empty() ? "Pick one in Catalog > Ground textures first." : m_activeTexture.c_str());
            if (!name.empty())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("None")) { name.clear(); live = done = true; label = std::string(what) + " of " + NameOf(r); }
            }
            ImGui::EndGroup();
            ImGui::PopID();
        };
        texture("Centre", r.texture);
        texture("Shoulder", r.shoulderTexture);
        ImGui::SetNextItemWidth(w - 110);
        track(ImGui::SliderFloat("Shoulder", &r.shoulder, 0.0f, 15.0f, "%.1f yd"), "Shoulder");
        ImGui::SetItemTooltip("Width of the worn edge beside the road, each side.");
        ImGui::SetNextItemWidth(w - 110);
        track(ImGui::SliderFloat("Ragged edges", &r.noise, 0.0f, 1.0f, "%.2f"), "Ragged edges");
        ImGui::TextColored(kQuiet, "Pick textures in Catalog > Ground textures,\nthen Use the picked texture.");

        if (m_roadPoint)
        {
            ImGui::SeparatorText(("Point " + std::to_string(*m_roadPoint + 1) + " of " + std::to_string(r.points.size())).c_str());
            RoadPoint& p = r.points[*m_roadPoint];
            float pw = p.width > 0 ? p.width : r.width;
            ImGui::SetNextItemWidth(w - 110);
            if (ImGui::SliderFloat("Width here", &pw, 1.0f, 40.0f, "%.1f yd")) { p.width = pw; live = true; label = "Width at a point of " + NameOf(r); }
            done |= ImGui::IsItemDeactivatedAfterEdit();
            if (p.width > 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Same as road")) { p.width = 0; live = done = true; label = "Width at a point of " + NameOf(r); }
            }
            if (ImGui::Button("Delete this point", { w, 0 }))
            {
                r.points.erase(r.points.begin() + std::ptrdiff_t(*m_roadPoint));
                m_roadPoint.reset();
                live = done = true;
                label = "Delete a point of " + NameOf(r);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Bake into the terrain", { w, 0 })) BakeSelectedRoad();
        ImGui::SetItemTooltip("Writes the road into the ground as ordinary height and texture edits and removes the spline.\nUndo brings it back.");
        if (ImGui::Button("Delete road", { w, 0 }))
        {
            if (const Road* saved = m_roads.Saved(r.id)) m_roads.Commit(saved, nullptr, "Delete " + NameOf(r));
            m_roadSel = 0;
            m_roadPoint.reset();
        }
        ImGui::EndDisabled();
        if (m_roadSel && (live || done) && editable) EditRoad(r, done, label);
    }
}
