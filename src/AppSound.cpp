// Atmosphere: zone sound (the ambience, music and intro an AreaTable row points at) and sound emitters
// (SoundEmitters.dbc: sounds placed in the world).
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
constexpr float kTwoPi = 6.2831853f;
const char* const kNote = "export, then restart the client (sound DBCs)";

std::optional<ImVec2> ToScreen(FXMMATRIX viewProj, const XMFLOAT3& p, const ImVec2& origin, const ImVec2& size)
{
    const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProj);
    const float w = XMVectorGetW(c);
    if (w <= 0.1f) return std::nullopt;
    return ImVec2{ origin.x + (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(c) / w * 0.5f) * size.y };
}

bool Contains(const std::string& text, const std::string& filter)
{
    if (filter.empty()) return true;
    return std::search(text.begin(), text.end(), filter.begin(), filter.end(),
                       [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }) != text.end();
}
}

void App::PlayEntry(uint32_t id)
{
    const auto s = m_sounds.Entry(id);
    if (!s || s->files.empty()) { Log("Sound %u has no files.", id); return; }
    std::string error;
    if (PlaySoundFile(m_mpq, s->files[0], error)) Log("Playing %s (%s).", s->name.c_str(), s->files[0].c_str());
    else Log("Cannot play %s: %s", s->name.c_str(), error.c_str());
}

bool App::SoundPicker(const char* label, uint32_t& id, float width)
{
    if (m_soundNames.empty())
        for (const auto& [i, row] : m_sounds.entries.Rows()) m_soundNames.push_back({ i, row.value("Name", std::string()) });
    const auto s = m_sounds.Entry(id);
    bool picked = false;
    ImGui::PushID(label);
    if (ImGui::Button(((s ? s->name : std::string("(none)")) + "##pick").c_str(), { width - 60, 0 })) ImGui::OpenPopup("sounds");
    ImGui::SameLine();
    ImGui::BeginDisabled(!s);
    if (ImGui::Button("Play", { 52, 0 })) PlayEntry(id);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    if (ImGui::BeginPopup("sounds"))
    {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(420);
        ImGui::InputTextWithHint("##filter", "Search sounds", &filter);
        std::vector<size_t> shown;
        for (size_t i = 0; i < m_soundNames.size(); ++i)
            if (Contains(m_soundNames[i].second, filter) || Contains(std::to_string(m_soundNames[i].first), filter)) shown.push_back(i);
        if (ImGui::BeginChild("list", { 420, 320 }))
        {
            ImGuiListClipper clip;
            clip.Begin(int(shown.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
                {
                    const auto& [sid, name] = m_soundNames[shown[size_t(r)]];
                    ImGui::PushID(int(sid));
                    if (ImGui::SmallButton("Play")) PlayEntry(sid);
                    ImGui::SameLine();
                    if (ImGui::Selectable((name + "   #" + std::to_string(sid)).c_str(), sid == id))
                    {
                        id = sid;
                        picked = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
        ImGui::TextColored(kQuiet, "%zu of %zu sounds", shown.size(), m_soundNames.size());
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return picked;
}

std::vector<SoundEmitter> App::EmittersOnMap() const
{
    return m_terrain.Map().empty() ? std::vector<SoundEmitter>{} : m_sounds.OnMap(CurrentMapId());
}

void App::CommitEmitter(uint32_t id, const std::optional<SoundEmitter>& after, const std::string& label)
{
    CommitDbc({ { &m_sounds.emitters, id, after ? Sounds::ToRow(*after, m_sounds.emitters.Row(id).is_null() ? nlohmann::json::object() : m_sounds.emitters.Row(id))
                                                : nlohmann::json() } },
              label, kNote);
}

void App::SoundViewport(const ImVec2& origin, const ImVec2& size, FXMMATRIX viewProj)
{
    const ImGuiIO& io = ImGui::GetIO();
    m_emitterHover.reset();
    const auto emitters = EmittersOnMap();
    if (ImGui::IsItemHovered())
    {
        float best = 14.0f;
        for (const SoundEmitter& e : emitters)
            if (const auto s = ToScreen(viewProj, { e.pos.x, e.pos.y + 4, e.pos.z }, origin, size))
                if (const float d = std::hypot(s->x - io.MousePos.x, s->y - io.MousePos.y); d < best) { best = d; m_emitterHover = e.id; }
    }
    if (!ImGui::IsItemActivated() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (m_emitterPlace)
    {
        if (!m_hover || !m_project) return;
        if (!io.KeyShift) m_emitterPlace = false;
        const Project::IdRange r = m_project->Range("soundemitter.id");
        const uint32_t id = m_sounds.emitters.FreeId(r.first, r.last);
        if (!id) { Log("No free sound emitter id in the project's range %u-%u (File > Project settings).", r.first, r.last); return; }
        SoundEmitter e;
        e.id = id;
        e.map = CurrentMapId();
        e.sound = m_emitterSound;
        e.pos = { m_hover->pos.x, m_hover->pos.y + 2, m_hover->pos.z };
        if (const auto s = m_sounds.Entry(e.sound)) e.name = s->name;
        CommitEmitter(id, e, "Add sound emitter " + std::to_string(id));
        m_emitterSel = id;
        return;
    }
    if (io.KeyAlt && m_emitterSel && m_hover)
    {
        MoveSelectionTo({ m_hover->pos.x, m_hover->pos.y + 2, m_hover->pos.z });
        return;
    }
    m_emitterSel = m_emitterHover.value_or(0);
    if (m_emitterSel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        if (const auto s = m_sounds.Entry(Sounds::FromRow(m_sounds.emitters.Row(m_emitterSel)).sound)) PlayEntry(s->id);
}

void App::BuildSoundOverlay(std::vector<LineVertex>& lines) const
{
    auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& c) { lines.push_back({ a, c }); lines.push_back({ b, c }); };
    for (const SoundEmitter& saved : EmittersOnMap())
    {
        const SoundEmitter& e = saved.id == m_emitterSel && m_emitterEdit.id == saved.id ? m_emitterEdit : saved;
        const float dx = e.pos.x - m_camera.pos.x, dz = e.pos.z - m_camera.pos.z;
        const bool selected = e.id == m_emitterSel, hovered = m_emitterHover == e.id;
        if (!selected && dx * dx + dz * dz > 1200.0f * 1200.0f) continue;
        const XMFLOAT4 c = selected ? XMFLOAT4{ 1, 1, 1, 1 } : hovered ? XMFLOAT4{ 0.6f, 1, 0.8f, 1 } : XMFLOAT4{ 0.45f, 0.95f, 0.75f, 0.7f };
        const XMFLOAT3 top{ e.pos.x, e.pos.y + 4, e.pos.z };
        line(e.pos, top, c);
        // A speaker: two arcs beside the pole.
        for (float r : { 1.0f, 1.8f })
            for (int i = 0; i < 6; ++i)
            {
                const float a0 = -0.7f + 1.4f * i / 6, a1 = -0.7f + 1.4f * (i + 1) / 6;
                line({ top.x + std::cos(a0) * r, top.y + std::sin(a0) * r, top.z }, { top.x + std::cos(a1) * r, top.y + std::sin(a1) * r, top.z }, c);
            }
        if (!selected && !hovered) continue;
        // How far it is heard (SoundEntries DistanceCutoff), as a ring on the ground.
        float reach = 0;
        if (const nlohmann::json& s = m_sounds.entries.Row(e.sound); !s.is_null()) reach = s.value("DistanceCutoff", 0.0f);
        if (reach <= 0) continue;
        for (int i = 0; i < 48; ++i)
        {
            const float a0 = kTwoPi * i / 48, a1 = kTwoPi * (i + 1) / 48;
            line({ e.pos.x + std::cos(a0) * reach, e.pos.y, e.pos.z + std::sin(a0) * reach },
                 { e.pos.x + std::cos(a1) * reach, e.pos.y, e.pos.z + std::sin(a1) * reach }, c);
        }
    }
}

void App::DrawSoundPanel(float w)
{
    const bool editable = m_project && !m_terrain.Map().empty();

    if (Section("Zone sound"))
    {
        // The area under the camera, or its zone.
        uint32_t areaId = 0;
        if (const auto ref = m_terrain.ChunkAtGrid(int(std::floor(m_camera.pos.x / kChunkSize)), int(std::floor(m_camera.pos.z / kChunkSize))))
            if (const AdtChunk* c = m_terrain.Chunk(*ref)) areaId = c->areaId;
        const uint32_t zoneId = areaId ? m_areas.ZoneOf(areaId) : 0;
        if (!areaId)
            ImGui::TextColored(kQuiet, "Fly over the terrain: the sound of the area\nunder the camera shows here.");
        else
        {
            ImGui::Text("Here: %s", AreaLabel(areaId).c_str());
            if (zoneId != areaId)
            {
                ImGui::Text("Zone: %s", AreaLabel(zoneId).c_str());
                if (ImGui::RadioButton("Edit this area", !m_soundZone)) m_soundZone = false;
                ImGui::SameLine();
                if (ImGui::RadioButton("Edit the zone", m_soundZone)) m_soundZone = true;
            }
            const uint32_t target = m_soundZone ? zoneId : areaId;
            nlohmann::json row = m_areas.Row(target);
            if (!row.is_null())
            {
                ImGui::BeginDisabled(!editable);
                ImGui::TextColored(kQuiet, "0 / None: the client uses the zone's (sub-areas).");
                bool changed = false;
                std::string what;

                // Ambience: day and night loops.
                const uint32_t amb = row.value("AmbienceID", 0u);
                const nlohmann::json& ambRow = m_sounds.ambience.Row(amb);
                auto entryName = [&](uint32_t id) { const auto s = m_sounds.Entry(id); return s ? s->name : std::string("-"); };
                auto ambLabel = [&](uint32_t id, const nlohmann::json& r) {
                    return r.is_null() ? std::string("None")
                                       : entryName(r.value("AmbienceID[0]", 0u)) + " / " + entryName(r.value("AmbienceID[1]", 0u)) + "##" + std::to_string(id);
                };
                ImGui::SetNextItemWidth(w - 90);
                if (ImGui::BeginCombo("Ambience", ambLabel(amb, ambRow).c_str(), ImGuiComboFlags_HeightLarge))
                {
                    if (ImGui::Selectable("None", amb == 0)) { row["AmbienceID"] = 0u; changed = true; what = "ambience"; }
                    for (const auto& [id, r] : m_sounds.ambience.Rows())
                        if (ImGui::Selectable(ambLabel(id, r).c_str(), id == amb)) { row["AmbienceID"] = id; changed = true; what = "ambience"; }
                    ImGui::EndCombo();
                }
                if (!ambRow.is_null())
                {
                    if (ImGui::SmallButton("Play day##amb")) PlayEntry(ambRow.value("AmbienceID[0]", 0u));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Play night##amb")) PlayEntry(ambRow.value("AmbienceID[1]", 0u));
                }

                // Music: day and night pieces, silences between.
                const uint32_t mus = row.value("ZoneMusic", 0u);
                const nlohmann::json& musRow = m_sounds.music.Row(mus);
                auto musLabel = [&](uint32_t id, const nlohmann::json& r) {
                    return r.is_null() ? std::string("None") : r.value("SetName", std::string()) + "##" + std::to_string(id);
                };
                ImGui::SetNextItemWidth(w - 90);
                if (ImGui::BeginCombo("Music", musLabel(mus, musRow).c_str(), ImGuiComboFlags_HeightLarge))
                {
                    if (ImGui::Selectable("None", mus == 0)) { row["ZoneMusic"] = 0u; changed = true; what = "music"; }
                    for (const auto& [id, r] : m_sounds.music.Rows())
                        if (ImGui::Selectable(musLabel(id, r).c_str(), id == mus)) { row["ZoneMusic"] = id; changed = true; what = "music"; }
                    ImGui::EndCombo();
                }
                if (!musRow.is_null())
                {
                    if (ImGui::SmallButton("Play day##mus")) PlayEntry(musRow.value("Sounds[0]", 0u));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Play night##mus")) PlayEntry(musRow.value("Sounds[1]", 0u));
                    ImGui::SameLine();
                    ImGui::TextColored(kQuiet, "silence %u-%u s", musRow.value("SilenceIntervalMin[0]", 0u) / 1000, musRow.value("SilenceIntervalMax[0]", 0u) / 1000);
                }

                // Intro: once, on entering.
                const uint32_t in = row.value("IntroSound", 0u);
                const nlohmann::json& inRow = m_sounds.intro.Row(in);
                auto inLabel = [&](uint32_t id, const nlohmann::json& r) {
                    return r.is_null() ? std::string("None") : r.value("Name", std::string()) + "##" + std::to_string(id);
                };
                ImGui::SetNextItemWidth(w - 90);
                if (ImGui::BeginCombo("Intro", inLabel(in, inRow).c_str(), ImGuiComboFlags_HeightLarge))
                {
                    if (ImGui::Selectable("None", in == 0)) { row["IntroSound"] = 0u; changed = true; what = "intro"; }
                    for (const auto& [id, r] : m_sounds.intro.Rows())
                        if (ImGui::Selectable(inLabel(id, r).c_str(), id == in)) { row["IntroSound"] = id; changed = true; what = "intro"; }
                    ImGui::EndCombo();
                }
                if (!inRow.is_null() && ImGui::SmallButton("Play##intro")) PlayEntry(inRow.value("SoundID", 0u));
                ImGui::EndDisabled();
                if (changed) CommitDbc({ { &m_areas, target, row } }, "Set " + what + " of " + AreaLabel(target), kNote);
            }
        }
        if (ImGui::Button("Stop playing", { w, 0 })) StopSound();
    }

    if (Section("Emitters"))
    {
        const auto all = EmittersOnMap();
        ImGui::BeginDisabled(!editable);
        SoundPicker("New emitters play", m_emitterSound, w - 120);
        ImGui::BeginDisabled(!m_emitterSound);
        if (ImGui::Button(m_emitterPlace ? "Click the ground... (Esc)" : "New emitter", { w, 0 })) m_emitterPlace = !m_emitterPlace;
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        std::vector<std::pair<float, const SoundEmitter*>> sorted;
        for (const SoundEmitter& e : all)
        {
            const float dx = e.pos.x - m_camera.pos.x, dy = e.pos.y - m_camera.pos.y, dz = e.pos.z - m_camera.pos.z;
            sorted.push_back({ std::sqrt(dx * dx + dy * dy + dz * dz), &e });
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (ImGui::BeginListBox("##emitters", { w, std::min(240.0f, 26.0f + 20.0f * float(sorted.size())) }))
        {
            for (const auto& [d, e] : sorted)
            {
                char text[200];
                snprintf(text, sizeof text, "%s   %.0f yd   #%u", e->name.empty() ? "(no name)" : e->name.c_str(), d, e->id);
                if (ImGui::Selectable(text, e->id == m_emitterSel, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_emitterSel = e->id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        float x, y, z;
                        EditorToServer(e->pos, x, y, z);
                        FlyTo(CurrentMapId(), x, y, z);
                    }
                }
            }
            ImGui::EndListBox();
        }
        ImGui::TextColored(kQuiet, "%zu on this map. Double-click: fly there\n(in the viewport: play it).", all.size());
        ImGui::TextColored(kQuiet, "Northrend also places sounds in its tiles\n(MCSE); those are not listed here.");

        const nlohmann::json& row = m_sounds.emitters.Row(m_emitterSel);
        if (m_emitterSel && !row.is_null())
        {
            ImGui::SeparatorText(("Emitter " + std::to_string(m_emitterSel)).c_str());
            ImGui::BeginDisabled(!editable);
            DrawTransformBar(w);
            SoundEmitter e = Sounds::FromRow(row);
            std::string name = e.name;
            ImGui::SetNextItemWidth(w - 90);
            ImGui::InputText("Name", &name);
            if (ImGui::IsItemDeactivatedAfterEdit() && name != e.name)
            {
                e.name = name;
                CommitEmitter(e.id, e, "Rename sound emitter " + std::to_string(e.id));
            }
            uint32_t sound = e.sound;
            if (SoundPicker("Sound", sound, w - 40))
            {
                e.sound = sound;
                CommitEmitter(e.id, e, "Sound of emitter " + std::to_string(e.id));
            }
            if (ImGui::Button("Delete emitter", { w, 0 }))
            {
                CommitEmitter(e.id, std::nullopt, "Delete sound emitter " + std::to_string(e.id));
                m_emitterSel = 0;
            }
            ImGui::EndDisabled();
        }
    }
}
