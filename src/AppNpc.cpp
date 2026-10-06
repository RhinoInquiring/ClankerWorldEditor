// NPC viewer: one creature template in a preview of its own, with its animations, skins, equipment, geosets and
// textures. The layout and feature set follow wow.export's Creatures tab (https://github.com/Kruithne/wow.export,
// MIT, Kruithne and Marlamin), rebuilt on this editor's renderer and AzerothCore's tables.
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace DirectX;

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
constexpr uint32_t kCape = 99;          // NpcView::hidden: the cape (a texture and geoset group 15, not a model)
constexpr float kFrameMs = 1000.0f / 30;   // one step of the frame buttons

uint32_t U(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 10)); }
float F(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

const char* AttachmentName(uint32_t id)
{
    switch (id)
    {
    case 0: return "Shield";
    case 1: return "Main hand";
    case 2: return "Off hand";
    case 5: return "Right shoulder";
    case 6: return "Left shoulder";
    case 11: return "Helmet";
    default: return "Item";
    }
}

/// Character model geoset groups (group = submesh id / 100), the names wow.export's GeosetMapper.js gives the
/// groups a 3.3.5 model can have.
std::string GeosetGroupName(uint32_t group)
{
    static const char* const kNames[] = { "Hair", "Facial A", "Facial B", "Facial C", "Gloves", "Boots", "Tail", "Ears", "Wrists", "Kneepads",
                                          "Chest", "Pants", "Tabard", "Trousers", "Loincloth", "Cloak", "Facial jewelry", "Eye glow", "Belt", "Bone / tail" };
    return group < std::size(kNames) ? kNames[group] : "Group " + std::to_string(group);
}
}

void App::OpenNpc(uint32_t entry)
{
    m_showNpc = true;
    NpcView& v = m_npc;
    v.focus = true;
    v.entry = entry;
    v.name.clear();
    v.models.clear();
    v.equips.clear();
    v.equip = 0;
    v.hidden.clear();
    const std::string id = std::to_string(entry);
    std::string error;
    if (const auto rows = m_db.Query("SELECT name FROM creature_template WHERE entry = " + id, error); rows && !rows->empty())
        v.name = (*rows)[0][0];
    if (const auto rows = m_db.Query("SELECT CreatureDisplayID, DisplayScale, Probability FROM creature_template_model WHERE CreatureID = " + id +
                                     " ORDER BY Idx", error))
        for (const auto& r : *rows) v.models.push_back({ U(r[0]), F(r[1]), F(r[2]) });
    if (const auto rows = m_db.Query("SELECT e.ID, e.ItemID1, e.ItemID2, e.ItemID3, COALESCE(i1.displayid, 0), COALESCE(i1.InventoryType, 0), "
                                     "COALESCE(i2.displayid, 0), COALESCE(i2.InventoryType, 0), COALESCE(i3.displayid, 0), COALESCE(i3.InventoryType, 0) "
                                     "FROM creature_equip_template e LEFT JOIN item_template i1 ON i1.entry = e.ItemID1 "
                                     "LEFT JOIN item_template i2 ON i2.entry = e.ItemID2 LEFT JOIN item_template i3 ON i3.entry = e.ItemID3 "
                                     "WHERE e.CreatureID = " + id + " ORDER BY e.ID", error))
        for (const auto& r : *rows)
        {
            NpcView::Equip e;
            e.id = U(r[0]);
            for (int k = 0; k < 3; ++k)
            {
                e.items[k] = U(r[1 + k]);
                e.displays[k] = U(r[4 + k * 2]);
                e.types[k] = U(r[5 + k * 2]);
            }
            v.equips.push_back(e);
        }
    if (!error.empty()) Log("NPC viewer: %s", error.c_str());
    if (v.models.empty())
    {
        v.displayId = 0;
        v.look.reset();
        v.info.reset();
        v.pose.reset();
        v.skins.clear();
        return;
    }
    SetNpcDisplay(v.models[0].displayId, v.models[0].scale);
}

void App::RefreshNpcLook()
{
    NpcView& v = m_npc;
    Spawn s;
    s.entry = v.entry;
    s.displayId = v.displayId;
    s.size = v.displayScale;
    if (v.equip >= 0 && size_t(v.equip) < v.equips.size())
        for (int hand = 0; hand < 2; ++hand)
        {
            s.weapons[hand] = v.equips[size_t(v.equip)].displays[hand];
            s.weaponTypes[hand] = v.equips[size_t(v.equip)].types[hand];
        }
    v.look = m_looks.SpawnLook(s);
}

void App::SetNpcDisplay(uint32_t displayId, float scale)
{
    NpcView& v = m_npc;
    v.displayId = displayId;
    v.displayScale = scale > 0.01f && scale < 100.0f ? scale : 1;
    v.skins = m_looks.SameModel(displayId);
    m_looks.SkinNames(displayId, &v.humanoid);
    v.info.reset();
    v.pose.reset();
    v.sequence = -1;
    v.geosets.clear();
    RefreshNpcLook();
    if (!v.look) return;
    v.info = m_models.Info(v.look->look.model, m_mpq);
    if (!v.info) return;
    v.geosets = v.look->look.geosets.empty() ? v.info->geosets : v.look->look.geosets;
    if (const auto& skel = v.info->skeleton; skel && !skel->sequences.empty())
    {
        int stand = 0;
        for (size_t i = 0; i < skel->sequences.size(); ++i)
            if (skel->sequences[i].id == 0) { stand = int(i); break; }
        SetNpcSequence(stand);
    }
    if (v.autoCamera)
    {
        const float s = v.look->scale;
        const XMFLOAT3 &lo = v.info->boundsMin, &hi = v.info->boundsMax;
        v.center[0] = (lo.x + hi.x) * 0.5f * s;
        v.center[1] = (lo.y + hi.y) * 0.5f * s;
        v.center[2] = (lo.z + hi.z) * 0.5f * s;
        const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
        v.distance = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) * s * 2.6f, 0.5f);
    }
}

void App::SetNpcSequence(int sequence)
{
    NpcView& v = m_npc;
    v.sequence = sequence;
    v.timeMs = 0;
    v.pose = v.look ? LoadSkeleton(M2Name(v.look->look.model), [this](const std::string& path) { return m_mpq.Read(path); }, sequence) : nullptr;
}

void App::DrawNpcPreview(const ImVec2& size)
{
    NpcView& v = m_npc;
    const UINT w = UINT(std::max(size.x, 8.0f)), h = UINT(std::max(size.y, 8.0f));
    if (w != v.width || h != v.height || !v.rtv)
    {
        v.width = w;
        v.height = h;
        v.color.Reset();
        v.depth.Reset();
        v.rtv.Reset();
        v.srv.Reset();
        v.dsv.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        m_device->CreateTexture2D(&d, nullptr, &v.color);
        if (v.color)
        {
            m_device->CreateRenderTargetView(v.color.Get(), nullptr, &v.rtv);
            m_device->CreateShaderResourceView(v.color.Get(), nullptr, &v.srv);
        }
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        m_device->CreateTexture2D(&d, nullptr, &v.depth);
        if (v.depth) m_device->CreateDepthStencilView(v.depth.Get(), nullptr, &v.dsv);
    }
    if (!v.rtv || !v.dsv) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (v.pose && v.pose->duration && !v.paused) v.timeMs = std::fmod(v.timeMs + io.DeltaTime * 1000.0f * v.speed, float(v.pose->duration));

    // The scene: the body posed by the chosen animation, its items on the posed attachment points.
    std::vector<ModelRenderer::Part> parts;
    if (v.look)
    {
        const XMMATRIX world = XMMatrixScaling(v.look->scale, v.look->scale, v.look->scale);
        ModelRenderer::Part body{ v.look->look, {}, v.pose.get(), uint32_t(v.timeMs) };
        XMStoreFloat4x4(&body.world, world);
        body.look.geosets = v.geosets;
        std::sort(body.look.geosets.begin(), body.look.geosets.end());
        if (body.look.geosets.empty()) body.look.geosets.push_back(0xFFFF);   // none ticked: draw no submesh (empty means all)
        if (v.hidden.count(kCape))
        {
            body.look.textures.erase(2);
            for (uint16_t& g : body.look.geosets)
                if (g / 100 == 15) g = 1501;
        }
        parts.push_back(body);
        for (const auto& item : v.look->items)
            if (!v.hidden.count(item.attachment))
                if (const auto at = m_models.AttachmentMatrix(v.look->look.model, item.attachment, m_mpq, v.pose.get(), uint32_t(v.timeMs)))
                {
                    ModelRenderer::Part p{ item.look, {} };
                    XMStoreFloat4x4(&p.world, XMLoadFloat4x4(&*at) * world);
                    parts.push_back(std::move(p));
                }
    }
    const XMVECTOR center = XMVectorSet(v.center[0], v.center[1], v.center[2], 0);
    const XMVECTOR eye = XMVectorAdd(center, XMVectorScale(XMVectorSet(std::cos(v.pitch) * std::cos(v.yaw), std::sin(v.pitch),
                                                                       std::cos(v.pitch) * std::sin(v.yaw), 0), v.distance));
    const XMMATRIX view = XMMatrixLookAtRH(eye, center, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX viewProj = view * XMMatrixPerspectiveFovRH(XMConvertToRadians(40.0f), float(w) / float(h), v.distance * 0.01f, v.distance * 50.0f);

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDsv;
    m_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    UINT viewports = 1;
    D3D11_VIEWPORT oldVp{};
    m_context->RSGetViewports(&viewports, &oldVp);
    const float bg[4] = { v.background[0], v.background[1], v.background[2], 1 };
    const D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
    m_context->OMSetRenderTargets(1, v.rtv.GetAddressOf(), v.dsv.Get());
    m_context->RSSetViewports(1, &vp);
    m_context->ClearRenderTargetView(v.rtv.Get(), bg);
    m_context->ClearDepthStencilView(v.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    m_models.DrawParts(parts, viewProj, m_mpq);
    m_context->OMSetRenderTargets(1, oldRtv.GetAddressOf(), oldDsv.Get());
    if (viewports) m_context->RSSetViewports(1, &oldVp);

    // Shown without blending, like the main viewport: the alpha channel holds texture masks, not coverage.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd* cmd) {
        static_cast<ID3D11DeviceContext*>(cmd->UserCallbackData)->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }, m_context);
    dl->AddImage(ImTextureID(intptr_t(v.srv.Get())), origin, { origin.x + size.x, origin.y + size.y });
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    // Orbit camera: left drag turns, right drag pans, the wheel zooms.
    ImGui::InvisibleButton("##npcscene", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0))
    {
        v.yaw += io.MouseDelta.x * 0.01f;
        v.pitch = std::clamp(v.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0))
    {
        XMFLOAT3 right, up;
        XMStoreFloat3(&right, XMVector3Normalize(XMVector3Cross(XMVectorSubtract(center, eye), XMVectorSet(0, 1, 0, 0))));
        XMStoreFloat3(&up, XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&right), XMVectorSubtract(center, eye))));
        const float k = v.distance * 0.0015f;
        v.center[0] -= (right.x * io.MouseDelta.x - up.x * io.MouseDelta.y) * k;
        v.center[1] -= (right.y * io.MouseDelta.x - up.y * io.MouseDelta.y) * k;
        v.center[2] -= (right.z * io.MouseDelta.x - up.z * io.MouseDelta.y) * k;
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) v.distance = std::clamp(v.distance * std::pow(0.88f, io.MouseWheel), 0.2f, 2000.0f);
    if (!v.look)
        dl->AddText({ origin.x + 12, origin.y + 10 }, IM_COL32(200, 200, 200, 255),
                    v.entry ? "This template has no model the client can show." : "Pick a creature on the left.");
}

void App::DrawNpcViewer()
{
    if (!m_showNpc) return;
    NpcView& v = m_npc;
    ImGui::SetNextWindowSize({ 1180, 700 }, ImGuiCond_FirstUseEver);
    if (v.focus) { ImGui::SetNextWindowFocus(); v.focus = false; }
    if (!ImGui::Begin("NPC viewer", &m_showNpc)) { ImGui::End(); return; }
    if (!m_creatures.Connected())
    {
        ImGui::TextColored(kQuiet, "Connect the world database (File > Server setup) to view creature templates.");
        ImGui::End();
        return;
    }

    // Left: every creature template, filtered by name or entry.
    const auto& templates = UnitTemplates(SpawnKind::Creature);
    if (ImGui::BeginChild("##npclist", { 270, 0 }, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##npcfilter", "Filter: name or entry", &v.filter);
        if (v.listedFor != v.filter || v.listed.size() > templates.size())
        {
            v.listedFor = v.filter;
            v.listed.clear();
            std::string query = v.filter;
            std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            for (size_t i = 0; i < templates.size(); ++i)
            {
                if (!query.empty())
                {
                    std::string name = templates[i].name;
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    if (name.find(query) == std::string::npos && std::to_string(templates[i].entry) != query) continue;
                }
                v.listed.push_back(i);
            }
        }
        ImGui::TextColored(kQuiet, "%zu creatures", v.listed.size());
        if (ImGui::BeginChild("##npcrows"))
        {
            ImGuiListClipper clip;
            clip.Begin(int(v.listed.size()));
            while (clip.Step())
                for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
                {
                    const SpawnAdapter::Template& t = templates[v.listed[size_t(row)]];
                    ImGui::PushID(int(t.entry));
                    if (ImGui::Selectable(t.name.c_str(), t.entry == v.entry)) OpenNpc(t.entry);
                    ImGui::SameLine();
                    ImGui::TextColored(kQuiet, "%u", t.entry);
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Middle: the preview and the animation bar.
    const float sideWidth = 320;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float barHeight = ImGui::GetFrameHeightWithSpacing() * 2 + style.ItemSpacing.y;
    if (ImGui::BeginChild("##npcmain", { std::max(ImGui::GetContentRegionAvail().x - sideWidth - style.ItemSpacing.x, 100.0f), 0 }))
    {
        if (v.entry)
        {
            ImGui::Text("%s", v.name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(kQuiet, "#%u   display %u%s", v.entry, v.displayId, v.humanoid ? "   character model" : "");
        }
        else ImGui::TextColored(kQuiet, "No creature open.");
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.y = std::max(size.y - barHeight, 50.0f);
        DrawNpcPreview(size);

        const ModelSkeleton* skel = v.info && v.info->skeleton ? v.info->skeleton.get() : nullptr;
        if (!skel || skel->sequences.empty()) ImGui::TextColored(kQuiet, "No animations.");
        else
        {
            auto label = [&](int i) {
                const ModelSequence& q = skel->sequences[size_t(i)];
                std::string s = m_looks.AnimationName(q.id);
                if (q.variation) s += " " + std::to_string(q.variation + 1);
                if (q.flags & 0x40) s += "  (alias)";
                return s + "##" + std::to_string(i);
            };
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo("##anim", v.sequence >= 0 ? label(v.sequence).c_str() : "Animation", ImGuiComboFlags_HeightLarge))
            {
                for (int i = 0; i < int(skel->sequences.size()); ++i)
                {
                    if (ImGui::Selectable(label(i).c_str(), i == v.sequence)) SetNpcSequence(i);
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
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: what the model is made of.
    if (ImGui::BeginChild("##npcside", { 0, 0 }, ImGuiChildFlags_Borders))
    {
        if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Auto camera", &v.autoCamera);
            ImGui::SetItemTooltip("Frame each model as it opens");
            ImGui::SameLine();
            if (ImGui::SmallButton("Frame now"))
            {
                const bool keep = v.autoCamera;
                v.autoCamera = true;
                SetNpcDisplay(v.displayId, v.displayScale);
                v.autoCamera = keep;
            }
            ImGui::ColorEdit3("Background", v.background, ImGuiColorEditFlags_NoInputs);
            ImGui::TextColored(kQuiet, "Left drag turns, right drag pans, wheel zooms.");
        }
        if (ImGui::CollapsingHeader("Models", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (v.models.empty()) ImGui::TextColored(kQuiet, "creature_template_model has no rows.");
            for (size_t i = 0; i < v.models.size(); ++i)
            {
                const auto& m = v.models[i];
                char text[96];
                snprintf(text, sizeof text, "Display %u   x%.2f   %.0f%%##m%zu", m.displayId, m.scale, m.probability * 100, i);
                if (ImGui::RadioButton(text, m.displayId == v.displayId)) SetNpcDisplay(m.displayId, m.scale);
            }
        }
        if (ImGui::CollapsingHeader("Skins", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextColored(kQuiet, "Displays drawing the same model (preview only).");
            if (ImGui::BeginChild("##skins", { 0, std::min(float(v.skins.size()) + 0.5f, 8.0f) * ImGui::GetTextLineHeightWithSpacing() }))
                for (uint32_t id : v.skins)
                {
                    bool humanoid = false;
                    std::string text = std::to_string(id);
                    const auto names = m_looks.SkinNames(id, &humanoid);
                    for (const auto& n : names) text += "   " + n;
                    if (humanoid) text += "   (character)";
                    if (ImGui::Selectable((text + "##s" + std::to_string(id)).c_str(), id == v.displayId)) SetNpcDisplay(id, v.displayScale);
                }
            ImGui::EndChild();
        }
        if (ImGui::CollapsingHeader("Equipment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto setLabel = [&](int i) {
                if (i < 0 || size_t(i) >= v.equips.size()) return std::string("None");
                const auto& e = v.equips[size_t(i)];
                return "Set " + std::to_string(e.id) + ":  " + std::to_string(e.items[0]) + " / " + std::to_string(e.items[1]) + " / " +
                       std::to_string(e.items[2]);
            };
            if (v.equips.empty()) ImGui::TextColored(kQuiet, "creature_equip_template has no sets.");
            else
            {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##equip", setLabel(v.equip).c_str()))
                {
                    for (int i = -1; i < int(v.equips.size()); ++i)
                        if (ImGui::Selectable(setLabel(i).c_str(), i == v.equip)) { v.equip = i; RefreshNpcLook(); }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Main hand / off hand / ranged item entries (the ranged one is not drawn)");
            }
            bool any = false;
            auto toggle = [&](uint32_t key, const char* name) {
                bool shown = !v.hidden.count(key);
                if (ImGui::Checkbox(name, &shown)) { if (shown) v.hidden.erase(key); else v.hidden.insert(key); }
                any = true;
            };
            if (v.look)
            {
                for (const auto& item : v.look->items) toggle(item.attachment, (std::string(AttachmentName(item.attachment)) + "##a" + std::to_string(item.attachment)).c_str());
                if (v.humanoid && v.look->look.textures.count(2)) toggle(kCape, "Cape");
            }
            if (!any) ImGui::TextColored(kQuiet, "Nothing carried.");
        }
        if (ImGui::CollapsingHeader("Geosets", ImGuiTreeNodeFlags_DefaultOpen) && v.info)
        {
            if (ImGui::SmallButton("All")) v.geosets = v.info->geosets;
            ImGui::SameLine();
            if (ImGui::SmallButton("None")) v.geosets.clear();
            ImGui::SameLine();
            if (ImGui::SmallButton("As spawned") && v.look) v.geosets = v.look->look.geosets.empty() ? v.info->geosets : v.look->look.geosets;
            // One "Group: variant" dropdown per group with a choice to make (wow.export's customization layout); a
            // group of one submesh, and the base mesh (id 0), are plain checkboxes.
            auto shown = [&](uint16_t g) { return std::find(v.geosets.begin(), v.geosets.end(), g) != v.geosets.end(); };
            auto setShown = [&](uint16_t g, bool on) {
                if (on && !shown(g)) v.geosets.push_back(g);
                if (!on) std::erase(v.geosets, g);
            };
            std::map<uint32_t, std::vector<uint16_t>> groups;
            for (uint16_t g : v.info->geosets)
                if (g) groups[g / 100u].push_back(g);
            auto name = [&](uint32_t group) { return v.humanoid ? GeosetGroupName(group) : "Group " + std::to_string(group); };
            const float labelWidth = 110;
            if (std::count(v.info->geosets.begin(), v.info->geosets.end(), uint16_t(0)))
            {
                bool on = shown(0);
                if (ImGui::Checkbox("Base mesh##g0", &on)) setShown(0, on);
            }
            for (const auto& [group, ids] : groups)
            {
                ImGui::PushID(int(group));
                if (ids.size() == 1)
                {
                    bool on = shown(ids[0]);
                    if (ImGui::Checkbox((name(group) + "  " + std::to_string(ids[0] % 100)).c_str(), &on)) setShown(ids[0], on);
                    ImGui::SetItemTooltip("Geoset %u", ids[0]);
                    ImGui::PopID();
                    continue;
                }
                std::vector<uint16_t> on;
                for (uint16_t g : ids)
                    if (shown(g)) on.push_back(g);
                const std::string current = on.empty() ? "None" : on.size() == ids.size() ? "All" : on.size() == 1 ? std::to_string(on[0] % 100) : "Mixed";
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(name(group).c_str());
                ImGui::SameLine(labelWidth);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##group", current.c_str()))
                {
                    auto only = [&](std::optional<uint16_t> keep) { for (uint16_t g : ids) setShown(g, keep && g == *keep); };
                    if (ImGui::Selectable("None", on.empty())) only(std::nullopt);
                    for (uint16_t g : ids)
                    {
                        if (ImGui::Selectable((std::to_string(g % 100) + "##" + std::to_string(g)).c_str(), on.size() == 1 && on[0] == g)) only(g);
                        ImGui::SameLine(80);
                        ImGui::TextColored(kQuiet, "geoset %u", g);
                    }
                    if (ImGui::Selectable("All", on.size() == ids.size())) for (uint16_t g : ids) setShown(g, true);
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        }
        if (ImGui::CollapsingHeader("Textures") && v.look)
        {
            ImGui::TextColored(kQuiet, "Click a name to copy its path.");
            auto show = [&](const std::string& label, const std::string& path) {
                ID3D11ShaderResourceView* tex = m_renderer.TextureFor(path, m_mpq);
                if (tex) ImGui::Image(ImTextureID(intptr_t(tex)), { 40, 40 });
                else ImGui::Dummy({ 40, 40 });
                if (tex && ImGui::BeginItemTooltip())
                {
                    ImGui::Image(ImTextureID(intptr_t(tex)), { 256, 256 });
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextColored(kQuiet, "%s", label.c_str());
                if (ImGui::Selectable((path + "##" + label).c_str())) ImGui::SetClipboardText(path.c_str());
                ImGui::EndGroup();
            };
            static const std::map<uint32_t, const char*> kTypes = { { 1, "Body" }, { 2, "Cape" }, { 6, "Hair" }, { 8, "Fur" },
                                                                    { 11, "Skin 1" }, { 12, "Skin 2" }, { 13, "Skin 3" } };
            for (const auto& [type, path] : v.look->look.textures)
            {
                const auto name = kTypes.find(type);
                show(name != kTypes.end() ? std::string(name->second) : "Type " + std::to_string(type), path);
            }
            if (v.info)
                for (const auto& path : v.info->textures) show("Fixed", path);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
