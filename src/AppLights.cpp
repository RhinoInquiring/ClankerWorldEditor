// Atmosphere: light volumes (Light.dbc) and the colour, fog and sky sets they point at (LightParams, LightIntBand,
// LightFloatBand, LightSkybox); the viewport's game-lighting preview.
#include "App.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
constexpr float kTwoPi = 6.2831853f;
constexpr int kKeyNear = 30;   // half-minutes: an edit within 15 minutes of a key changes that key

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

std::string Clock(int halfMinutes)
{
    char text[16];
    snprintf(text, sizeof text, "%02d:%02d", halfMinutes / 120, (halfMinutes / 2) % 60);
    return text;
}

/// A band row with `raw` at `time`: the key within 15 minutes of it changes, else a new key goes in (the nearest one
/// is replaced when all 16 are used). A missing row is made (`floats`: LightFloatBand).
nlohmann::json SetBandAt(nlohmann::json band, uint32_t id, bool floats, int time, uint32_t raw)
{
    if (band.is_null())
    {
        band = { { "ID", id }, { "Num", 0u } };
        for (int i = 0; i < 16; ++i)
        {
            band["Time[" + std::to_string(i) + "]"] = 0;
            band["Data[" + std::to_string(i) + "]"] = floats ? nlohmann::json(0.0f) : nlohmann::json(0u);
        }
    }
    auto keys = Lights::Keys(band);
    auto distance = [&](int t) { const int d = std::abs(t - time); return std::min(d, kDayHalfMinutes - d); };
    auto nearest = std::min_element(keys.begin(), keys.end(), [&](const auto& a, const auto& b) { return distance(a.first) < distance(b.first); });
    if (nearest != keys.end() && (distance(nearest->first) <= kKeyNear || keys.size() >= 16)) *nearest = { distance(nearest->first) <= kKeyNear ? nearest->first : time, raw };
    else keys.push_back({ time, raw });
    return Lights::SetKeys(band, keys);
}

/// The band without its key nearest `time` (within 15 minutes); unchanged when there is none there.
nlohmann::json RemoveBandKey(nlohmann::json band, int time)
{
    auto keys = Lights::Keys(band);
    std::erase_if(keys, [&](const auto& k) { const int d = std::abs(k.first - time); return std::min(d, kDayHalfMinutes - d) <= kKeyNear; });
    return Lights::SetKeys(band, keys);
}
}

const std::vector<LightVolume>& App::LightsOnMap()
{
    uint64_t version = m_lightDraftRevision;
    for (DbcTable* t : m_lights.Tables()) version += t->Version();
    const auto key = std::tuple{ CurrentMapId(), version, m_lightDraft.size() };
    if (key != m_lightsOnMapKey)
    {
        m_lightsOnMap = m_lights.OnMap(CurrentMapId(), &m_lightDraft);
        m_lightsOnMapKey = key;
    }
    return m_lightsOnMap;
}

void App::UpdateSceneLight()
{
    if (!m_gameLight || m_terrain.Map().empty())
    {
        m_renderer.SetSceneLight({});
        m_sceneLightKey = {};
        return;
    }
    const auto& all = LightsOnMap();
    uint64_t version = m_lightDraftRevision;
    for (DbcTable* t : m_lights.Tables()) version += t->Version();
    // Recomputed when the camera moves a yard, the time or slot changes, or the tables do.
    const auto key = std::tuple{ CurrentMapId(), int(std::floor(m_camera.pos.x)), int(std::floor(m_camera.pos.y)), int(std::floor(m_camera.pos.z)),
                                 m_lightTime, m_lightSlot, version };
    if (key == m_sceneLightKey) return;
    m_sceneLightKey = key;
    const LightState s = m_lights.At(all, m_camera.pos, m_lightTime, m_lightSlot, &m_lightDraft);
    SceneLight l;
    l.on = true;
    l.diffuse = s.colors[0];
    l.ambient = s.colors[1];
    l.fog = s.colors[7];
    l.fogEnd = s.floats[0] > 1 ? s.floats[0] : 1000;
    l.fogStart = l.fogEnd * std::clamp(s.floats[1], 0.0f, 0.99f);
    for (size_t i = 0; i < 5; ++i) l.sky[i] = s.colors[2 + i];
    m_renderer.SetSceneLight(l);
}

void App::LightDraftSet(DbcTable& table, uint32_t id, nlohmann::json row, const std::string& label)
{
    m_lightDraft[{ table.Name(), id }] = std::move(row);
    m_lightDraftLabel = label;
    ++m_lightDraftRevision;
}

void App::LightDraftCommit()
{
    if (m_lightDraft.empty()) return;
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows;
    for (auto& [key, row] : m_lightDraft)
        for (DbcTable* t : m_lights.Tables())
            if (t->Name() == key.first) rows.push_back({ t, key.second, row });
    m_lightDraft.clear();
    ++m_lightDraftRevision;
    CommitDbc(std::move(rows), m_lightDraftLabel.empty() ? "Edit light" : m_lightDraftLabel, "export, then restart the client (light DBCs)");
}

void App::FlyToLight(const LightVolume& v)
{
    float x, y, z;
    EditorToServer(v.pos, x, y, z);
    FlyTo(CurrentMapId(), x, y, z);
    const float back = std::max(v.outer, 60.0f);   // far enough back to see the whole sphere
    m_camera.pos = { v.pos.x, v.pos.y + back * 0.4f, v.pos.z - back };
    Log("Camera to light %u (%.0f, %.0f).", v.id, x, y);
}

void App::AddLight(const XMFLOAT3& at)
{
    if (!m_project) return;
    const Project::IdRange lr = m_project->Range("light.id"), pr = m_project->Range("lightparams.id");
    const uint32_t id = m_lights.light.FreeId(lr.first, lr.last);
    const uint32_t params = m_lights.FreeParamsId(pr.first, pr.last);
    if (!id || !params) { Log("No free light or light params id in the project's ranges (File > Project settings)."); return; }
    // Starts as the place looks now: the strongest light here lends its sets, and its clear-weather set is copied so
    // this light's colours are its own.
    const auto& all = LightsOnMap();
    const LightVolume* source = nullptr;
    float best = -1;
    for (const LightVolume& v : all)
    {
        float w = v.Global() ? 0.01f : 0;
        if (!v.Global())
        {
            const float dx = at.x - v.pos.x, dy = at.y - v.pos.y, dz = at.z - v.pos.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < v.outer) w = d <= v.inner ? 1.0f : 1.0f - (d - v.inner) / std::max(v.outer - v.inner, 1.0f);
        }
        if (w > best) { best = w; source = &v; }
    }
    LightVolume v;
    if (source) v.params = source->params;
    else if (const nlohmann::json& fallback = m_lights.light.Row(1); !fallback.is_null()) v.params = Lights::FromRow(fallback).params;
    v.id = id;
    v.map = CurrentMapId();
    v.pos = at;
    v.inner = 30;
    v.outer = 80;
    auto rows = m_lights.CopyParams(v.params[0], params);
    if (rows.empty()) { Log("Light %u has no colour set to copy.", source ? source->id : 1u); return; }
    v.params[0] = params;
    rows.push_back({ &m_lights.light, id, Lights::ToRow(v, nlohmann::json::object()) });
    CommitDbc(std::move(rows), "Add light " + std::to_string(id), "export, then restart the client (light DBCs)");
    m_lightSel = id;
}

void App::LightsViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    const ImGuiIO& io = ImGui::GetIO();
    m_lightHover.reset();
    if (ImGui::IsItemHovered())
    {
        float best = 14.0f;   // pixels from the centre marker
        for (const LightVolume& v : LightsOnMap())
            if (!v.Global())
                if (const auto s = ToScreen(viewProj, v.pos, origin, size))
                    if (const float d = std::hypot(s->x - io.MousePos.x, s->y - io.MousePos.y); d < best) { best = d; m_lightHover = v.id; }
    }
    if (!ImGui::IsItemActivated() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (m_lightPlace)
    {
        if (!m_hover) return;
        if (!io.KeyShift) m_lightPlace = false;
        AddLight({ m_hover->pos.x, m_hover->pos.y + 5, m_hover->pos.z });
        return;
    }
    if (io.KeyAlt && m_lightSel && m_hover)   // Alt+click: the selected light goes there
    {
        MoveSelectionTo({ m_hover->pos.x, m_hover->pos.y + 5, m_hover->pos.z });
        return;
    }
    m_lightSel = m_lightHover.value_or(0);
    if (m_lightSel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        for (const LightVolume& v : LightsOnMap())
            if (v.id == m_lightSel) FlyToLight(v);
}

void App::BuildLightOverlay(std::vector<LineVertex>& lines) const
{
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& c) { lines.push_back({ a, c }); lines.push_back({ b, c }); };
    // Three great circles of a sphere: about the vertical, and the two upright ones.
    auto sphere = [&](const XMFLOAT3& p, float r, const XMFLOAT4& c) {
        constexpr int n = 48;
        for (int i = 0; i < n; ++i)
        {
            const float a0 = kTwoPi * i / n, a1 = kTwoPi * (i + 1) / n;
            const float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r, c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;
            line({ p.x + c0, p.y, p.z + s0 }, { p.x + c1, p.y, p.z + s1 }, c);
            line({ p.x + c0, p.y + s0, p.z }, { p.x + c1, p.y + s1, p.z }, c);
            line({ p.x, p.y + s0, p.z + c0 }, { p.x, p.y + s1, p.z + c1 }, c);
        }
    };
    for (const LightVolume& v : m_lightsOnMap)
    {
        if (v.Global()) continue;
        const bool selected = v.id == m_lightSel, hovered = m_lightHover == v.id;
        const float dx = v.pos.x - m_camera.pos.x, dz = v.pos.z - m_camera.pos.z;
        if (!selected && dx * dx + dz * dz > std::pow(v.outer + 1500.0f, 2.0f)) continue;
        const XMFLOAT4 c = selected ? XMFLOAT4{ 1, 1, 1, 1 } : hovered ? XMFLOAT4{ 1, 0.9f, 0.4f, 1 } : XMFLOAT4{ 1, 0.85f, 0.35f, 0.55f };
        // A star at the centre, the outer radius always, the inner one when selected or hovered.
        for (const XMFLOAT3& d : { XMFLOAT3{ 3, 0, 0 }, XMFLOAT3{ 0, 3, 0 }, XMFLOAT3{ 0, 0, 3 } })
            line({ v.pos.x - d.x, v.pos.y - d.y, v.pos.z - d.z }, { v.pos.x + d.x, v.pos.y + d.y, v.pos.z + d.z }, c);
        sphere(v.pos, v.outer, c);
        if (selected || hovered) sphere(v.pos, v.inner, { c.x, c.y, c.z, c.w * 0.6f });
    }
}

void App::DrawLightsPanel(float w)
{
    const bool editable = m_project && !m_terrain.Map().empty();
    const auto& all = LightsOnMap();
    const LightVolume* sel = nullptr;
    for (const LightVolume& v : all)
        if (v.id == m_lightSel) sel = &v;
    if (!sel) m_lightSel = 0;

    if (Section("Preview"))
    {
        ImGui::Checkbox("Game lighting in the viewport", &m_gameLight);
        ImGui::SetItemTooltip("Sky, fog, sun and ambient colours of the light where the camera is.\nView > Game lighting does the same.");
        int minutes = m_lightTime / 2;
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::SliderInt("Time", &minutes, 0, 24 * 60 - 1, Clock(m_lightTime).c_str())) m_lightTime = minutes * 2;
        ImGui::SetNextItemWidth(w - 90);
        ImGui::Combo("Weather", &m_lightSlot, kLightSlotNames, 5);
        ImGui::SetItemTooltip("Which of a light's sets shows: clear, storm, under water, dead.");
        ImGui::TextColored(kQuiet, "Not shown here: the skybox model, clouds,\nthe sun and moon, water colours.");
    }

    if (Section("Lights"))
    {
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button(m_lightPlace ? "Click the ground... (Esc)" : "New light", { w, 0 })) m_lightPlace = !m_lightPlace;
        ImGui::SetItemTooltip("Click the ground to add a light there (Shift: keep adding). It starts as the place looks now.");
        ImGui::EndDisabled();
        if (ImGui::BeginListBox("##lights", { w, std::max(120.0f, ImGui::GetContentRegionAvail().y - 4) }))
        {
            std::vector<std::pair<float, const LightVolume*>> sorted;
            for (const LightVolume& v : all)
            {
                const float dx = v.pos.x - m_camera.pos.x, dy = v.pos.y - m_camera.pos.y, dz = v.pos.z - m_camera.pos.z;
                sorted.push_back({ v.Global() ? -1.0f : std::sqrt(dx * dx + dy * dy + dz * dz), &v });
            }
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [d, v] : sorted)
            {
                char text[128];
                if (v->Global()) snprintf(text, sizeof text, "Whole map  #%u", v->id);
                else snprintf(text, sizeof text, "#%u   %.0f yd away   r %.0f / %.0f%s", v->id, d, v->inner, v->outer, d < v->outer ? "   (here)" : "");
                if (ImGui::Selectable(text, v->id == m_lightSel, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_lightSel = v->id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !v->Global()) FlyToLight(*v);
                }
            }
            ImGui::EndListBox();
        }
        ImGui::TextColored(kQuiet, "%zu light(s). Click: select   Double-click: fly there", all.size());
    }

    if (!sel)
    {
        LightDraftCommit();
        return;
    }
    const LightVolume v = *sel;
    const std::string name = v.Global() ? "the whole map" : "light " + std::to_string(v.id);

    if (Section("Selected", false))
    {
        ImGui::BeginDisabled(!editable);
        if (v.Global())
            ImGui::TextColored(kQuiet, "The map's default light: everywhere no other\nlight reaches. It has no place or radius.");
        else
        {
            DrawTransformBar(w);
            float pos[3] = { v.pos.x, v.pos.y, v.pos.z };
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::InputFloat3("Centre", pos, "%.1f"))
            {
                LightVolume e = v;
                e.pos = { pos[0], pos[1], pos[2] };
                LightDraftSet(m_lights.light, v.id, Lights::ToRow(e, m_lights.Row(m_lights.light, v.id, &m_lightDraft)), "Move " + name);
            }
            float inner = v.inner, outer = v.outer;
            ImGui::SetNextItemWidth(w - 90);
            const bool a = ImGui::SliderFloat("Full inside", &inner, 0.0f, outer, "%.0f yd");
            ImGui::SetItemTooltip("Inside this radius the light is all there is.");
            ImGui::SetNextItemWidth(w - 90);
            const bool b = ImGui::SliderFloat("Fades out at", &outer, std::max(inner, 1.0f), 3000.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("Between the two radii it blends into the light around it.");
            if (a || b)
            {
                LightVolume e = v;
                e.inner = std::min(inner, outer);
                e.outer = outer;
                LightDraftSet(m_lights.light, v.id, Lights::ToRow(e, m_lights.Row(m_lights.light, v.id, &m_lightDraft)), "Resize " + name);
            }
        }

        // The sets of the eight slots; the one the preview shows is edited below.
        ImGui::SeparatorText("Colour sets");
        for (int s = 0; s < 5; ++s)
        {
            const uint32_t p = v.params[size_t(s)];
            const size_t users = p ? m_lights.Users(p, &m_lightDraft) : 0;
            char text[128];
            snprintf(text, sizeof text, "%-20s %5u%s", kLightSlotNames[s], p, users > 1 ? ("   shared by " + std::to_string(users)).c_str() : "");
            if (ImGui::Selectable(text, s == m_lightSlot)) m_lightSlot = s;
        }
        const uint32_t p = v.params[size_t(m_lightSlot)];
        const size_t users = p ? m_lights.Users(p, &m_lightDraft) : 0;
        const bool owned = m_project && m_project->Owns("lightparams.id", p);
        if (p && (users > 1 || !owned))
        {
            ImGui::PushTextWrapPos(w);
            ImGui::TextColored(kWarn, users > 1 ? "Set %u is shared by %zu lights: editing it changes all of them." : "Set %u is the client's own.", p, users);
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Give this light its own copy", { w, 0 }))
            {
                const Project::IdRange r = m_project->Range("lightparams.id");
                if (const uint32_t to = m_lights.FreeParamsId(r.first, r.last))
                {
                    LightDraftCommit();
                    auto rows = m_lights.CopyParams(p, to);
                    LightVolume e = v;
                    e.params[size_t(m_lightSlot)] = to;
                    rows.push_back({ &m_lights.light, v.id, Lights::ToRow(e, m_lights.light.Row(v.id)) });
                    CommitDbc(std::move(rows), "Own colour set " + std::to_string(to) + " for " + name, "export, then restart the client (light DBCs)");
                }
                else Log("No free light params id in the project's range (File > Project settings).");
            }
        }
        if (!v.Global() && ImGui::Button("Delete this light", { w, 0 }))
        {
            LightDraftCommit();
            CommitDbc({ { &m_lights.light, v.id, nullptr } }, "Delete " + name, "export, then restart the client (light DBCs)");
            m_lightSel = 0;
        }
        ImGui::EndDisabled();
    }

    const uint32_t params = v.params[size_t(m_lightSlot)];
    if (params && Section(("Colours at " + Clock(m_lightTime) + "###colours").c_str()))
    {
        ImGui::BeginDisabled(!editable);
        const LightState s = m_lights.Params(params, m_lightTime, &m_lightDraft);
        ImGui::TextColored(kQuiet, "Set %u, %s. Editing sets the colour at this time\n(the key within 15 minutes, or a new one).", params,
                           kLightSlotNames[m_lightSlot]);
        for (int i = 0; i < 18; ++i)
        {
            if (i == 8 || i == 13) continue;   // unused by the 3.3.5 client
            const uint32_t id = params * 18 - 17 + uint32_t(i);
            const nlohmann::json& band = m_lights.Row(m_lights.intBands, id, &m_lightDraft);
            float c[3] = { s.colors[size_t(i)].x, s.colors[size_t(i)].y, s.colors[size_t(i)].z };
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(w - 170);
            if (ImGui::ColorEdit3(kLightColorNames[i], c, ImGuiColorEditFlags_NoInputs))
                LightDraftSet(m_lights.intBands, id, SetBandAt(band, id, false, m_lightTime, Lights::Raw({ c[0], c[1], c[2] })),
                              std::string(kLightColorNames[i]) + " of set " + std::to_string(params));
            const auto keys = Lights::Keys(band);
            ImGui::SameLine();
            ImGui::TextColored(kQuiet, "%zu key(s)", keys.size());
            if (ImGui::BeginPopupContextItem("keys"))
            {
                for (const auto& [t, raw] : keys)
                {
                    const XMFLOAT3 k = Lights::Color(raw);
                    ImGui::ColorButton(("##k" + std::to_string(t)).c_str(), { k.x, k.y, k.z, 1 }, ImGuiColorEditFlags_NoTooltip, { 14, 14 });
                    ImGui::SameLine();
                    if (ImGui::MenuItem(("Go to " + Clock(t)).c_str())) m_lightTime = t;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove the key at this time"))
                    LightDraftSet(m_lights.intBands, id, RemoveBandKey(band, m_lightTime), "Remove a key of " + std::string(kLightColorNames[i]));
                if (ImGui::MenuItem("This colour all day"))
                    LightDraftSet(m_lights.intBands, id, Lights::SetKeys(band.is_null() ? SetBandAt(band, id, false, 0, 0) : band, { { 0, Lights::Raw(s.colors[size_t(i)]) } }),
                                  std::string(kLightColorNames[i]) + " all day");
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TextColored(kQuiet, "Right-click a colour: its keys, remove one,\nor keep one colour all day.");
        ImGui::EndDisabled();
    }

    if (params && Section("Fog and sky"))
    {
        ImGui::BeginDisabled(!editable);
        const LightState s = m_lights.Params(params, m_lightTime, &m_lightDraft);
        auto floatBand = [&](int i, float value, float lo, float hi, const char* format, float scale, ImGuiSliderFlags flags = 0) {
            const uint32_t id = params * 6 - 5 + uint32_t(i);
            ImGui::SetNextItemWidth(w - 150);
            if (ImGui::SliderFloat(kLightFloatNames[i], &value, lo, hi, format, flags))
                LightDraftSet(m_lights.floatBands, id,
                              SetBandAt(m_lights.Row(m_lights.floatBands, id, &m_lightDraft), id, true, m_lightTime, Lights::FloatBits(value * scale)),
                              std::string(kLightFloatNames[i]) + " of set " + std::to_string(params));
        };
        ImGui::TextColored(kQuiet, "At %s (keys like the colours).", Clock(m_lightTime).c_str());
        floatBand(0, s.floats[0], 20.0f, 5000.0f, "%.0f yd", 36.0f, ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Everything past this distance disappears in the fog.");
        floatBand(1, s.floats[1], 0.0f, 0.99f, "%.2f", 1.0f);
        ImGui::SetItemTooltip("Where the fog begins, as a share of the fog distance.");
        floatBand(2, s.floats[2], 0.0f, 1.0f, "%.2f", 1.0f);
        floatBand(3, s.floats[3], 0.0f, 1.0f, "%.2f", 1.0f);

        ImGui::SeparatorText("Whole set");
        nlohmann::json row = m_lights.Row(m_lights.params, params, &m_lightDraft);
        if (!row.is_null())
        {
            bool changed = false;
            const auto boxes = m_lights.skyboxes.Rows();
            const uint32_t box = row.value("LightSkyboxID", 0u);
            std::string current = box ? "#" + std::to_string(box) : "None";
            if (auto it = boxes.find(box); it != boxes.end()) current = it->second.value("Name", current);
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::BeginCombo("Skybox", current.c_str()))
            {
                if (ImGui::Selectable("None", box == 0)) { row["LightSkyboxID"] = 0u; changed = true; }
                for (const auto& [id, b] : boxes)
                    if (ImGui::Selectable((b.value("Name", std::string()) + "##" + std::to_string(id)).c_str(), id == box)) { row["LightSkyboxID"] = id; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("A sky model drawn around the camera (nebulae, auroras); not shown in the editor.");
            auto slider = [&](const char* field, const char* label) {
                float f = row.value(field, 0.0f);
                ImGui::SetNextItemWidth(w - 150);
                if (ImGui::SliderFloat(label, &f, 0.0f, 1.0f, "%.2f")) { row[field] = f; changed = true; }
            };
            slider("Glow", "Glow");
            slider("WaterShallowAlpha", "River, shallow");
            slider("WaterDeepAlpha", "River, deep");
            slider("OceanShallowAlpha", "Ocean, shallow");
            slider("OceanDeepAlpha", "Ocean, deep");
            ImGui::SetItemTooltip("How see-through water is (the colours are in the Colours tab).");
            if (changed) LightDraftSet(m_lights.params, params, row, "Edit colour set " + std::to_string(params));
        }
        ImGui::EndDisabled();
    }

    // The draft becomes one undo step once nothing is being dragged or typed.
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) LightDraftCommit();
}
