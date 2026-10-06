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
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
constexpr uint32_t kCape = 99;          // NpcView::hidden: the cape (a texture and geoset group 15, not a model)
constexpr float kFrameMs = 1000.0f / 30;   // one step of the frame buttons

uint32_t U(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 10)); }
float F(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

/// An item quality's colour (0 poor ... 7 heirloom), as the game shows names.
ImVec4 QualityColor(uint32_t quality)
{
    static const ImVec4 kColors[] = { { 0.62f, 0.62f, 0.62f, 1 }, { 1, 1, 1, 1 }, { 0.12f, 1, 0, 1 }, { 0, 0.44f, 0.87f, 1 },
                                      { 0.64f, 0.21f, 0.93f, 1 }, { 1, 0.5f, 0, 1 }, { 0.9f, 0.8f, 0.5f, 1 }, { 0.9f, 0.8f, 0.5f, 1 } };
    return quality < std::size(kColors) ? kColors[quality] : kColors[1];
}

/// A column of a row read from the database (column -> text); empty for NULL or missing.
std::string Col(const nlohmann::json& row, const char* col)
{
    if (!row.is_object()) return {};
    const auto it = row.find(col);
    return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}

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
    m_npc.focus = true;
    if (m_npc.dirty && entry != m_npc.entry) { m_npc.pendingOpen = entry; return; }   // asked first (DrawNpcViewer)
    m_npc.hidden.clear();
    m_npc.equip = 0;
    LoadNpc(entry, true);
}

void App::LoadNpc(uint32_t entry, bool frame)
{
    NpcView& v = m_npc;
    const bool same = v.entry == entry;
    v.entry = entry;
    const auto rows = m_npcTemplates.Rows(entry);
    v.editTemplate = rows.empty() ? nlohmann::json() : rows[0];
    v.editModels = m_npcModels.Rows(entry);
    v.editEquips = m_npcEquips.Rows(entry);
    v.editRevision = m_store.Revision();
    v.dirty = false;
    v.appearanceId = 0;
    v.editDisplay = v.editExtra = nullptr;
    for (auto& loot : v.loot) loot = {};
    v.dialogue = {};
    v.name = Col(v.editTemplate, "name");
    RebuildNpcLists();
    // Keep showing the same display while it is still one of the template's (after an edit or an undo), else the first.
    uint32_t display = v.models.empty() ? 0 : v.models[0].displayId;
    float scale = v.models.empty() ? 1 : v.models[0].scale;
    for (const auto& m : v.models)
        if (same && m.displayId == v.displayId) { display = m.displayId; scale = m.scale; }
    if (!display)
    {
        v.displayId = 0;
        v.look.reset();
        v.info.reset();
        v.pose.reset();
        v.skins.clear();
        return;
    }
    const bool sameDisplay = same && display == v.displayId;
    const int sequence = v.sequence;
    const bool autoCamera = v.autoCamera;
    v.autoCamera = frame && autoCamera;
    SetNpcDisplay(display, scale);
    v.autoCamera = autoCamera;
    if (sameDisplay && sequence >= 0 && v.info && v.info->skeleton && size_t(sequence) < v.info->skeleton->sequences.size()) SetNpcSequence(sequence);
}

void App::RebuildNpcLists()
{
    NpcView& v = m_npc;
    v.models.clear();
    for (const auto& r : v.editModels) v.models.push_back({ U(Col(r, "CreatureDisplayID")), F(Col(r, "DisplayScale")), F(Col(r, "Probability")) });
    v.equips.clear();
    for (const auto& r : v.editEquips)
    {
        NpcView::Equip e;
        e.id = U(Col(r, "ID"));
        for (int k = 0; k < 3; ++k)
        {
            e.items[k] = U(Col(r, ("ItemID" + std::to_string(k + 1)).c_str()));
            const NpcView::Item& item = NpcItem(e.items[k]);
            e.displays[k] = item.display;
            e.types[k] = item.type;
        }
        v.equips.push_back(e);
    }
    if (v.equip >= int(v.equips.size())) v.equip = v.equips.empty() ? -1 : 0;
    for (const auto& m : v.models)
        if (m.displayId == v.displayId && m.scale > 0.01f) v.displayScale = m.scale;
    if (v.displayId) RefreshNpcLook();
}

const App::NpcView::Item& App::NpcItem(uint32_t entry)
{
    static const NpcView::Item kNone;
    if (!entry) return kNone;
    if (auto it = m_npc.items.find(entry); it != m_npc.items.end()) return it->second;
    NpcView::Item& item = m_npc.items[entry];
    std::string error;
    if (const auto rows = m_db.Query("SELECT name, displayid, InventoryType, Quality FROM item_template WHERE entry = " + std::to_string(entry), error); rows && !rows->empty())
        item = { (*rows)[0][0], U((*rows)[0][1]), U((*rows)[0][2]), true, U((*rows)[0][3]) };
    return item;
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
    if (v.entry && !v.dirty && v.editRevision != m_store.Revision()) LoadNpc(v.entry, false);   // an undo, a redo, another tool's edit
    if (v.pendingOpen) ImGui::OpenPopup("Unapplied NPC edits");
    if (ImGui::BeginPopupModal("Unapplied NPC edits", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s (%u) has edits that are not applied.", v.name.c_str(), v.entry);
        const uint32_t next = v.pendingOpen;
        auto go = [&](bool apply) {
            if (apply) ApplyNpc();
            v.dirty = false;
            v.pendingOpen = 0;
            OpenNpc(next);
            ImGui::CloseCurrentPopup();
        };
        if (ImGui::Button("Apply")) go(true);
        ImGui::SameLine();
        if (ImGui::Button("Discard")) go(false);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { v.pendingOpen = 0; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
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
    const float sideWidth = 400;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float barHeight = ImGui::GetFrameHeightWithSpacing() * 2 + style.ItemSpacing.y;
    // Its right edge drags: a narrower preview gives the side panel room for every tab. Remembered with the layout.
    if (ImGui::BeginChild("##npcmain", { std::max(ImGui::GetContentRegionAvail().x - sideWidth - style.ItemSpacing.x, 100.0f), 0 }, ImGuiChildFlags_ResizeX))
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
        if (v.dirty)
        {
            ImGui::TextColored(kWarn, "Edits not applied");
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply")) ApplyNpc();
            ImGui::SetItemTooltip("Write them to the world database as one undo step");
            ImGui::SameLine();
            if (ImGui::SmallButton("Revert")) LoadNpc(v.entry, false);
        }
        if (ImGui::BeginTabBar("##npctabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
        {
            if (ImGui::BeginTabItem("View")) { DrawNpcViewTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Template")) { DrawNpcTemplateTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Models & gear")) { DrawNpcGearTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Appearance")) { DrawNpcAppearanceTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Loot")) { DrawNpcLootTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Dialogue")) { DrawNpcDialogueTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}


void App::DrawNpcViewTab()
{
    NpcView& v = m_npc;
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

// ---------------------------------------------------------------------------------------------- editing

void App::ApplyNpc()
{
    NpcView& v = m_npc;
    if (!v.entry || !v.dirty) return;
    const std::string label = "Edit NPC " + std::to_string(v.entry) + " " + v.name;
    // Idx follows the list order (the order AzerothCore reads the models in).
    for (size_t i = 0; i < v.editModels.size(); ++i) v.editModels[i]["Idx"] = std::to_string(i);
    std::vector<Change> parts;
    auto add = [&](TableRowsAdapter& table, const std::vector<nlohmann::json>& after) {
        const auto before = table.Rows(v.entry);
        if (before == after) return false;
        Change c = table.MakeChange(v.entry, before, after, label);
        table.Apply(c);
        if (!table.LastError().empty()) Log("%s", table.LastError().c_str());
        parts.push_back(std::move(c));
        return true;
    };
    bool appearanceChanged = false;
    if (v.appearanceId && v.editDisplay.is_object() && v.editExtra.is_object())
        for (auto [table, row] : { std::pair{ &m_extraRows, &v.editExtra }, std::pair{ &m_displayRows, &v.editDisplay } })
        {
            const uint32_t id = row->value("ID", 0u);
            const nlohmann::json before = table->Row(id);
            if (before == *row) continue;
            Change c = table->MakeChange(id, before, *row, label);
            table->Apply(c);
            parts.push_back(std::move(c));
            appearanceChanged = true;
        }
    const bool templateChanged = v.editTemplate.is_object() && add(m_npcTemplates, { v.editTemplate });
    // Loot: rows of each kind under the loot id the (edited) template names.
    TableRowsAdapter* lootTables[3] = { &m_lootDrops, &m_lootPickpocket, &m_lootSkinning };
    bool lootChanged[3] = {};
    for (int k = 0; k < 3; ++k)
        if (const NpcView::Loot& loot = v.loot[k]; loot.id && loot.id != ~0u)
        {
            const auto before = lootTables[k]->Rows(loot.id);
            if (before == loot.rows) continue;
            Change c = lootTables[k]->MakeChange(loot.id, before, loot.rows, label);
            lootTables[k]->Apply(c);
            if (!lootTables[k]->LastError().empty()) Log("%s", lootTables[k]->LastError().c_str());
            parts.push_back(std::move(c));
            lootChanged[k] = true;
        }
    const bool modelsChanged = add(m_npcModels, v.editModels);
    const bool equipChanged = add(m_npcEquips, v.editEquips);
    std::set<std::string> dialogueReloads;
    DialogueChanges(parts, label, dialogueReloads);
    v.dirty = false;
    if (parts.empty()) return;
    m_store.Commit(std::move(parts), label);
    m_unitTemplatesRead[0] = false;   // the catalog shows names and models
    v.listedFor = "\x01";
    m_creatures.Refresh();            // spawns in the world take the new look
    if (templateChanged && RunServerCommand(".reload creature_template " + std::to_string(v.entry))) Log("%s: creature_template reloaded on the server.", label.c_str());
    if (modelsChanged || equipChanged) Log("%s: models and equipment reach a running worldserver after a restart.", label.c_str());
    for (int k = 0; k < 3; ++k)
        if (lootChanged[k] && RunServerCommand(".reload " + lootTables[k]->Table())) Log("%s: %s reloaded on the server.", label.c_str(), lootTables[k]->Table().c_str());
    for (const std::string& table : dialogueReloads)
        if (table == "npc_text") Log("%s: npc_text is read when worldserver starts: restart it to see new or changed gossip texts.", label.c_str());
        else if (RunServerCommand(".reload " + table)) Log("%s: %s reloaded on the server.", label.c_str(), table.c_str());
    if (appearanceChanged)
        Log("%s: the appearance is CreatureDisplayInfo / CreatureDisplayInfoExtra rows: export (and build the patch), copy out/server/dbc to the "
            "server, then restart the client and worldserver.", label.c_str());
    LoadNpc(v.entry, false);
}

void App::DuplicateNpc()
{
    NpcView& v = m_npc;
    if (!m_project || !v.editTemplate.is_object() || v.dirty) return;
    const Project::IdRange r = m_project->Range("creature_template.entry");
    const auto id = m_npcTemplates.NextKey(r.first, r.last);
    if (!id) { Log("No free creature_template entry in the project's range %u-%u (File > Project settings).", r.first, r.last); return; }
    const std::string entry = std::to_string(*id), label = "New NPC " + entry + " (copy of " + std::to_string(v.entry) + ")";
    nlohmann::json t = v.editTemplate;
    t["entry"] = entry;
    t["name"] = v.name + " (copy)";
    std::vector<nlohmann::json> models = v.editModels, equips = v.editEquips;
    for (auto& m : models) m["CreatureID"] = entry;
    for (auto& e : equips) e["CreatureID"] = entry;
    std::vector<Change> parts;
    const std::pair<TableRowsAdapter*, std::vector<nlohmann::json>> tables[] = { { &m_npcTemplates, { t } }, { &m_npcModels, models }, { &m_npcEquips, equips } };
    for (const auto& [table, rows] : tables)
    {
        if (rows.empty()) continue;
        Change c = table->MakeChange(*id, {}, rows, label);
        table->Apply(c);
        if (!table->LastError().empty()) Log("%s", table->LastError().c_str());
        parts.push_back(std::move(c));
    }
    m_store.Commit(std::move(parts), label);
    m_unitTemplatesRead[0] = false;
    v.listedFor = "\x01";
    Log("%s. Place it from the Catalog (Creatures); a running worldserver knows it after a restart.", label.c_str());
    OpenNpc(*id);
}

void App::DeleteNpc()
{
    NpcView& v = m_npc;
    if (!m_project || !m_project->Owns("creature_template.entry", v.entry)) return;
    const std::string label = "Delete NPC " + std::to_string(v.entry) + " " + v.name;
    std::vector<Change> parts;
    for (TableRowsAdapter* table : { &m_npcTemplates, &m_npcModels, &m_npcEquips })
    {
        const auto before = table->Rows(v.entry);
        if (before.empty()) continue;
        Change c = table->MakeChange(v.entry, before, {}, label);
        table->Apply(c);
        if (!table->LastError().empty()) Log("%s", table->LastError().c_str());
        parts.push_back(std::move(c));
    }
    if (parts.empty()) return;
    m_store.Commit(std::move(parts), label);
    m_unitTemplatesRead[0] = false;
    v.listedFor = "\x01";
    Log("%s (undo brings it back; its spawns stay in the creature table).", label.c_str());
    LoadNpc(v.entry, false);
}

void App::DrawNpcTemplateTab()
{
    NpcView& v = m_npc;
    if (!v.entry) { ImGui::TextColored(kQuiet, "No creature open."); return; }
    if (!v.editTemplate.is_object()) { ImGui::TextColored(kQuiet, "Entry %u has no creature_template row.", v.entry); return; }
    const bool owned = m_project && m_project->Owns("creature_template.entry", v.entry);
    ImGui::BeginDisabled(v.dirty || !m_project);
    if (ImGui::Button("Duplicate as new NPC")) DuplicateNpc();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(v.dirty ? "Apply or revert the edits first"
                                  : "Copies the template, its models and equipment sets to the next entry of the project's range (File > Project settings)");
    ImGui::SameLine();
    if (owned)
    {
        if (ImGui::Button("Delete")) ImGui::OpenPopup("##deletenpc");
        if (ImGui::BeginPopup("##deletenpc"))
        {
            ImGui::Text("Delete %s (%u), its models and equipment?", v.name.c_str(), v.entry);
            ImGui::TextColored(kQuiet, "Undo brings it back. Its spawns are not deleted.");
            if (ImGui::Button("Delete")) { DeleteNpc(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::TextColored(kQuiet, "stock entry");
        ImGui::SetItemTooltip("Not in the project's entry range: edits are kept as project changes, exported as SQL with a revert file");
    }

    nlohmann::json& t = v.editTemplate;
    auto get = [&](const char* col) { return Col(t, col); };
    auto set = [&](const char* col, const std::string& value) {
        if (get(col) == value) return;
        t[col] = value;
        v.dirty = true;
        if (std::string_view(col) == "name") v.name = value;
    };
    auto textField = [&](const char* label, const char* col, const char* hint = "") {
        std::string s = get(col);
        if (ImGui::InputTextWithHint(label, hint, &s)) set(col, s);
    };
    auto intField = [&](const char* label, const char* col, long long lo, long long hi) {
        int x = int(std::clamp<long long>(std::strtoll(get(col).c_str(), nullptr, 10), INT32_MIN, INT32_MAX));
        if (ImGui::InputInt(label, &x, 0)) set(col, std::to_string(std::clamp<long long>(x, lo, hi)));
    };
    auto floatField = [&](const char* label, const char* col, float lo, float hi) {
        float x = F(get(col));
        if (ImGui::InputFloat(label, &x, 0, 0, "%.3g"))
        {
            char text[32];
            snprintf(text, sizeof text, "%g", std::clamp(x, lo, hi));
            set(col, text);
        }
    };
    auto combo = [&](const char* label, const char* col, const std::vector<std::pair<int, std::string>>& options) {
        const int x = std::atoi(get(col).c_str());
        std::string current = std::to_string(x);
        for (const auto& [value, name] : options)
            if (value == x) current = name;
        if (ImGui::BeginCombo(label, current.c_str()))
        {
            for (const auto& [value, name] : options)
                if (ImGui::Selectable(name.c_str(), value == x)) set(col, std::to_string(value));
            ImGui::EndCombo();
        }
    };
    auto names = [&](const char* label, const char* col, std::initializer_list<const char*> options) {
        const std::string s = get(col);
        if (ImGui::BeginCombo(label, s.empty() ? "(none)" : s.c_str()))
        {
            if (ImGui::Selectable("(none)", s.empty())) set(col, "");
            for (const char* o : options)
                if (ImGui::Selectable(o, s == o)) set(col, o);
            ImGui::EndCombo();
        }
    };
    auto flags = [&](const char* label, const char* col, std::initializer_list<std::pair<uint32_t, const char*>> bits) {
        const uint32_t x = uint32_t(std::strtoul(get(col).c_str(), nullptr, 10));
        std::string summary;
        for (const auto& [bit, name] : bits)
            if (x & bit) summary += (summary.empty() ? "" : ", ") + std::string(name);
        if (ImGui::BeginCombo(label, summary.empty() ? "None" : summary.c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (const auto& [bit, name] : bits)
                if (bool on = (x & bit) != 0; ImGui::Checkbox(name, &on)) set(col, std::to_string(on ? x | bit : x & ~bit));
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s = %u", col, x);
    };

    ImGui::PushItemWidth(-120);
    ImGui::SeparatorText("Identity");
    textField("Name", "name");
    textField("Subname", "subname", "e.g. Weapon Merchant");
    names("Cursor", "IconName", { "Directions", "Gunner", "vehicleCursor", "Driver", "Attack", "Buy", "Speak", "Pickup", "Interact", "Trainer", "Taxi",
                                  "Repair", "LootAll", "Quest", "PickLock" });
    ImGui::SeparatorText("Level and kind");
    intField("Min level", "minlevel", 1, 255);
    intField("Max level", "maxlevel", 1, 255);
    combo("Expansion", "exp", { { 0, "Classic" }, { 1, "The Burning Crusade" }, { 2, "Wrath of the Lich King" } });
    combo("Rank", "rank", { { 0, "Normal" }, { 1, "Elite" }, { 2, "Rare elite" }, { 3, "Boss" }, { 4, "Rare" } });
    combo("Class", "unit_class", { { 1, "Warrior" }, { 2, "Paladin" }, { 4, "Rogue" }, { 8, "Mage" } });
    {
        std::vector<std::pair<int, std::string>> types;
        for (int i = 0; CreatureTypeName(uint32_t(i)); ++i) types.push_back({ i, CreatureTypeName(uint32_t(i)) });
        combo("Type", "type", types);
    }
    intField("Faction", "faction", 0, 65535);
    if (const std::string faction = m_looks.FactionName(U(get("faction"))); !faction.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, "%s", faction.c_str());
    }
    ImGui::SeparatorText("Services");
    flags("NPC flags", "npcflag", { { 0x1, "Gossip" }, { 0x2, "Quest giver" }, { 0x10, "Trainer" }, { 0x20, "Class trainer" }, { 0x40, "Profession trainer" },
                                    { 0x80, "Vendor" }, { 0x100, "Ammo vendor" }, { 0x200, "Food vendor" }, { 0x400, "Poison vendor" },
                                    { 0x800, "Reagent vendor" }, { 0x1000, "Repairs" }, { 0x2000, "Flight master" }, { 0x4000, "Spirit healer" },
                                    { 0x8000, "Spirit guide" }, { 0x10000, "Innkeeper" }, { 0x20000, "Banker" }, { 0x40000, "Petitioner" },
                                    { 0x80000, "Tabard designer" }, { 0x100000, "Battlemaster" }, { 0x200000, "Auctioneer" }, { 0x400000, "Stable master" },
                                    { 0x800000, "Guild banker" }, { 0x1000000, "Spell click" }, { 0x4000000, "Mailbox" } });
    intField("Gossip menu", "gossip_menu_id", 0, INT32_MAX);
    ImGui::SeparatorText("Movement");
    combo("Movement", "MovementType", { { 0, "Stay in place" }, { 1, "Wander" }, { 2, "Waypoint path" } });
    floatField("Walk speed", "speed_walk", 0, 50);
    floatField("Run speed", "speed_run", 0, 50);
    floatField("Aggro range", "detection_range", 0, 200);
    ImGui::SeparatorText("Combat");
    floatField("Health x", "HealthModifier", 0, 100000);
    floatField("Mana x", "ManaModifier", 0, 100000);
    floatField("Armor x", "ArmorModifier", 0, 100000);
    floatField("Damage x", "DamageModifier", 0, 100000);
    floatField("Experience x", "ExperienceModifier", 0, 100000);
    intField("Attack time ms", "BaseAttackTime", 0, 100000);
    if (bool regen = get("RegenHealth") != "0"; ImGui::Checkbox("Regenerates health", &regen)) set("RegenHealth", regen ? "1" : "0");
    ImGui::SeparatorText("Loot and money");
    intField("Loot id", "lootid", 0, INT32_MAX);
    intField("Min copper", "mingold", 0, INT32_MAX);
    intField("Max copper", "maxgold", 0, INT32_MAX);
    ImGui::SeparatorText("Scripts");
    names("AI", "AIName", { "SmartAI", "NullCreatureAI", "TriggerAI", "AggressorAI", "ReactorAI", "PassiveAI", "CritterAI", "GuardAI", "PetAI", "TotemAI",
                            "CombatAI", "ArcherAI", "TurretAI", "VehicleAI" });
    textField("Script", "ScriptName", "C++ ScriptName");
    ImGui::PopItemWidth();

    if (get("name").empty()) ImGui::TextColored(kWarn, "The name is empty.");
    if (U(get("minlevel")) > U(get("maxlevel"))) ImGui::TextColored(kWarn, "Min level is above max level.");

    if (ImGui::CollapsingHeader("All columns"))
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##colfilter", "Filter columns", &v.columnFilter);
        std::string filter = v.columnFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ImGui::BeginTable("##columns", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            for (auto& [col, value] : t.items())
            {
                std::string lower = col;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(col.c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                std::string s = value.is_string() ? value.get<std::string>() : std::string();
                ImGui::BeginDisabled(col == "entry");
                if (ImGui::InputText(("##" + col).c_str(), &s)) set(col.c_str(), s);
                ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
    }
}

void App::DrawNpcGearTab()
{
    NpcView& v = m_npc;
    if (!v.entry) { ImGui::TextColored(kQuiet, "No creature open."); return; }
    bool changed = false;
    const std::string entry = std::to_string(v.entry);

    ImGui::SeparatorText("Models");
    ImGui::TextColored(kQuiet, "Each spawn gets one, picked by chance.");
    std::optional<size_t> remove;
    if (!v.editModels.empty() && ImGui::BeginTable("##models", 5, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Display");
        ImGui::TableSetupColumn("Scale");
        ImGui::TableSetupColumn("Chance");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < v.editModels.size(); ++i)
        {
            nlohmann::json& m = v.editModels[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const uint32_t display = U(Col(m, "CreatureDisplayID"));
            if (ImGui::RadioButton("##show", display == v.displayId)) SetNpcDisplay(display, F(Col(m, "DisplayScale")));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (int x = int(display); ImGui::InputInt("##display", &x, 0)) { m["CreatureDisplayID"] = std::to_string(std::max(x, 0)); changed = true; }
            if (m_looks.SameModel(display).empty()) ImGui::SetItemTooltip("Display %u is not in the client's CreatureDisplayInfo.dbc", display);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (float x = F(Col(m, "DisplayScale")); ImGui::InputFloat("##scale", &x, 0, 0, "%.2f")) { m["DisplayScale"] = std::to_string(std::clamp(x, 0.0f, 100.0f)); changed = true; }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (float x = F(Col(m, "Probability")) * 100; ImGui::InputFloat("##chance", &x, 0, 0, "%.0f%%"))
            {
                m["Probability"] = std::to_string(std::clamp(x, 0.0f, 100.0f) / 100);
                changed = true;
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::SetItemTooltip("Remove this model");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove) { v.editModels.erase(v.editModels.begin() + std::ptrdiff_t(*remove)); changed = true; }
    const bool listed = std::any_of(v.editModels.begin(), v.editModels.end(), [&](const nlohmann::json& m) { return U(Col(m, "CreatureDisplayID")) == v.displayId; });
    ImGui::BeginDisabled(listed || !v.displayId);
    if (ImGui::Button("Add the shown display"))
    {
        nlohmann::json row = { { "CreatureID", entry }, { "CreatureDisplayID", std::to_string(v.displayId) }, { "DisplayScale", "1" },
                               { "Probability", "1" }, { "VerifiedBuild", "0" } };
        v.editModels.push_back(std::move(row));
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(listed ? "Display %u is listed already: pick another under View > Skins to add it"
                                 : "Adds display %u (pick another under View > Skins first to add that one)", v.displayId);
    float chances = 0;
    for (const auto& m : v.editModels) chances += F(Col(m, "Probability"));
    if (v.editModels.empty()) ImGui::TextColored(kWarn, "No model: the creature cannot spawn.");
    else if (chances <= 0) ImGui::TextColored(kWarn, "The chances add up to 0.");

    ImGui::SeparatorText("Equipment sets");
    ImGui::TextColored(kQuiet, "A spawn's equipment_id picks one: 1 the first, -1 random, 0 none.");
    static const char* const kSlots[3] = { "Main hand", "Off hand", "Ranged" };
    bool openPicker = false;
    remove.reset();
    for (size_t i = 0; i < v.editEquips.size(); ++i)
    {
        nlohmann::json& e = v.editEquips[i];
        ImGui::PushID(int(i) + 1000);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Set");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (int id = int(U(Col(e, "ID"))); ImGui::InputInt("##id", &id, 1)) { e["ID"] = std::to_string(std::clamp(id, 1, 255)); changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Preview", v.equip == int(i))) { v.equip = int(i); RefreshNpcLook(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove set")) remove = i;
        for (int k = 0; k < 3; ++k)
        {
            ImGui::PushID(k);
            const std::string col = "ItemID" + std::to_string(k + 1);
            const uint32_t item = U(Col(e, col.c_str()));
            const NpcView::Item& info = NpcItem(item);
            const std::string icon = m_looks.ItemIcon(info.display);
            if (ID3D11ShaderResourceView* tex = icon.empty() ? nullptr : m_renderer.TextureFor(icon, m_mpq))
                ImGui::Image(ImTextureID(intptr_t(tex)), { 22, 22 });
            else ImGui::Dummy({ 22, 22 });
            ImGui::SameLine();
            const std::string text = !item ? std::string("(empty)") : info.found ? info.name : "#" + std::to_string(item) + " (not in item_template)";
            if (ImGui::Button((text + "##pick").c_str(), { -80, 0 }))
            {
                v.pickSet = int(i);
                v.pickSlot = k;
                v.pickArmor = -1;
                v.pickLoot = false;
                openPicker = true;
            }
            ImGui::SetItemTooltip("%s: item %u. Click to choose another.", kSlots[k], item);
            ImGui::SameLine();
            ImGui::TextColored(kQuiet, "%s", kSlots[k]);
            ImGui::PopID();
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (remove) { v.editEquips.erase(v.editEquips.begin() + std::ptrdiff_t(*remove)); changed = true; }
    if (ImGui::Button("Add set"))
    {
        uint32_t id = 0;
        for (const auto& e : v.editEquips) id = std::max(id, U(Col(e, "ID")));
        v.editEquips.push_back({ { "CreatureID", entry }, { "ID", std::to_string(std::min(id + 1, 255u)) }, { "ItemID1", "0" }, { "ItemID2", "0" },
                                 { "ItemID3", "0" }, { "VerifiedBuild", "0" } });
        changed = true;
    }
    if (openPicker) ImGui::OpenPopup("##itempick");
    DrawItemPicker();
    if (changed)
    {
        v.dirty = true;
        RebuildNpcLists();
    }
}

void App::DrawItemPicker()
{
    NpcView& v = m_npc;
    ImGui::SetNextWindowSize({ 440, 420 });
    if (!ImGui::BeginPopup("##itempick")) return;
    bool search = ImGui::IsWindowAppearing();
    if (search) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##query", v.pickLoot ? "Any item: name or entry" : v.pickArmor >= 0 ? "Armour for this slot: name or entry" : "Weapons, shields, held items: name or entry", &v.pickQuery))
        search = true;
    if (search)
    {
        v.pickHits.clear();
        // What creature_equip_template accepts (weapons, shields, held-in-off-hand items, ranged weapons), or what an
        // armour slot of CreatureDisplayInfoExtra shows.
        static const char* const kArmourTypes[11] = { "1", "3", "4", "5, 20", "6", "7", "8", "9", "10", "19", "16" };
        std::string where = v.pickLoot ? std::string("1 = 1")
                          : v.pickArmor >= 0 && v.pickArmor < 11 ? std::string("InventoryType IN (") + kArmourTypes[v.pickArmor] + ")"
                                                                  : "InventoryType IN (13, 14, 15, 17, 21, 22, 23, 25, 26)";
        if (!v.pickQuery.empty())
        {
            const bool number = std::all_of(v.pickQuery.begin(), v.pickQuery.end(), [](unsigned char c) { return std::isdigit(c); });
            where += " AND (name LIKE " + m_db.Quote("%" + v.pickQuery + "%") + (number ? " OR entry = " + v.pickQuery : "") + ")";
        }
        std::string error;
        if (const auto rows = m_db.Query("SELECT entry, name, displayid, InventoryType, Quality FROM item_template WHERE " + where + " ORDER BY name LIMIT 200", error))
            for (const auto& r : *rows)
            {
                const NpcView::Item item{ r[1], U(r[2]), U(r[3]), true, U(r[4]) };
                v.items[U(r[0])] = item;
                v.pickHits.push_back({ U(r[0]), item });
            }
        if (!error.empty()) Log("Item search: %s", error.c_str());
    }
    std::optional<uint32_t> chosen;
    if (ImGui::BeginChild("##hits"))
    {
        if (ImGui::Selectable("(empty slot)")) chosen = 0;
        for (const auto& [entry, item] : v.pickHits)
        {
            ImGui::PushID(int(entry));
            const std::string icon = m_looks.ItemIcon(item.display);
            if (ID3D11ShaderResourceView* tex = icon.empty() ? nullptr : m_renderer.TextureFor(icon, m_mpq)) ImGui::Image(ImTextureID(intptr_t(tex)), { 20, 20 });
            else ImGui::Dummy({ 20, 20 });
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, QualityColor(item.quality));
            const bool picked = ImGui::Selectable(item.name.c_str());
            ImGui::PopStyleColor();
            if (picked) chosen = entry;
            ImGui::SameLine(360);
            ImGui::TextColored(kQuiet, "%u", entry);
            ImGui::PopID();
        }
        if (v.pickHits.size() == 200) ImGui::TextColored(kQuiet, "First 200 shown: type more of the name.");
    }
    ImGui::EndChild();
    if (chosen && *chosen && v.pickLoot)
    {
        NpcView::Loot& loot = NpcLoot(v.lootKind);
        loot.rows.push_back({ { "Entry", std::to_string(loot.id) }, { "Item", std::to_string(*chosen) }, { "Reference", "0" }, { "Chance", "10" },
                              { "QuestRequired", "0" }, { "LootMode", "1" }, { "GroupId", "0" }, { "MinCount", "1" }, { "MaxCount", "1" },
                              { "Comment", Col(v.editTemplate, "name") + " - " + NpcItem(*chosen).name } });
        v.dirty = true;
        ImGui::CloseCurrentPopup();
    }
    else if (chosen && v.pickArmor >= 0 && v.editExtra.is_object())
    {
        const uint32_t display = *chosen ? NpcItem(*chosen).display : 0;
        v.editExtra["NPCItemDisplay[" + std::to_string(v.pickArmor) + "]"] = display;
        v.dirty = true;
        RefreshNpcDisplay();
        ImGui::CloseCurrentPopup();
    }
    else if (chosen && v.pickSet >= 0 && size_t(v.pickSet) < v.editEquips.size())
    {
        v.editEquips[size_t(v.pickSet)]["ItemID" + std::to_string(v.pickSlot + 1)] = std::to_string(*chosen);
        v.dirty = true;
        RebuildNpcLists();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------- appearance

void App::RefreshNpcDisplay()
{
    NpcView& v = m_npc;
    const std::string model = v.look ? v.look->look.model : std::string();
    const int sequence = v.sequence;
    const bool autoCamera = v.autoCamera;
    v.autoCamera = false;
    SetNpcDisplay(v.displayId, v.displayScale);
    v.autoCamera = autoCamera;
    if (v.look && v.look->look.model == model && sequence >= 0 && v.info && v.info->skeleton && size_t(sequence) < v.info->skeleton->sequences.size())
        SetNpcSequence(sequence);
}

void App::NewNpcAppearance(uint32_t from)
{
    NpcView& v = m_npc;
    if (!m_project || !v.entry) return;
    if (v.appearanceId && v.editDisplay.is_object() && m_displayRows.Row(v.appearanceId).is_null())
    {
        Log("Apply the new appearance first (it has no rows yet, so a second one would take the same id).");
        return;
    }
    const Project::IdRange rd = m_project->Range("creaturedisplayinfo.id"), re = m_project->Range("creaturedisplayinfoextra.id");
    const uint32_t displayId = m_displayRows.FreeId(rd.first, rd.last), extraId = m_extraRows.FreeId(re.first, re.last);
    if (!displayId || !extraId)
    {
        Log("No free CreatureDisplayInfo / CreatureDisplayInfoExtra id in the project's ranges (File > Project settings).");
        return;
    }
    nlohmann::json d = from ? m_displayRows.Row(from) : nlohmann::json();
    nlohmann::json e = d.is_object() && d.value("ExtendedDisplayInfoID", 0u) ? m_extraRows.Row(d.value("ExtendedDisplayInfoID", 0u)) : nlohmann::json();
    if (!e.is_object())
    {
        // A new character: the race's own player display as the template row (sounds, blood, size class).
        const uint32_t race = 1, sex = 0;   // human male
        d = m_displayRows.Row(m_looks.RaceDisplay(race, sex));
        if (!d.is_object()) { Log("ChrRaces has no display for race %u.", race); return; }
        e = { { "DisplayRaceID", race }, { "DisplaySexID", sex }, { "SkinID", 0u }, { "FaceID", 0u }, { "HairStyleID", 0u }, { "HairColorID", 0u },
              { "FacialHairID", 0u }, { "Flags", 0u }, { "BakeName", "" } };
        for (int i = 0; i < 11; ++i) e["NPCItemDisplay[" + std::to_string(i) + "]"] = 0u;
    }
    d["ID"] = displayId;
    d["ExtendedDisplayInfoID"] = extraId;
    d["PortraitTextureName"] = "";
    e["ID"] = extraId;
    e["BakeName"] = "";   // the client composites the skin from the fields instead
    v.appearanceId = displayId;
    v.editDisplay = std::move(d);
    v.editExtra = std::move(e);
    v.dirty = true;
    // The template shows the new display instead of the one it came from.
    bool replaced = false;
    for (auto& m : v.editModels)
        if (from && U(Col(m, "CreatureDisplayID")) == from) { m["CreatureDisplayID"] = std::to_string(displayId); replaced = true; }
    if (!replaced)
        v.editModels.push_back({ { "CreatureID", std::to_string(v.entry) }, { "CreatureDisplayID", std::to_string(displayId) }, { "DisplayScale", "1" },
                                 { "Probability", "1" }, { "VerifiedBuild", "0" } });
    v.displayId = displayId;
    RebuildNpcLists();
    SetNpcDisplay(displayId, v.displayScale);
    Log("New appearance: display %u + extra %u (applied with the NPC's other edits).", displayId, extraId);
}

void App::DrawNpcAppearanceTab()
{
    NpcView& v = m_npc;
    if (!v.entry) { ImGui::TextColored(kQuiet, "No creature open."); return; }
    const bool owned = m_project && m_project->Owns("creaturedisplayinfo.id", v.displayId);
    bool humanoid = false;
    m_looks.SkinNames(v.displayId, &humanoid);
    // Edit the project's own display in place; read its rows when it comes up (unless other edits are pending).
    if (owned && humanoid && v.appearanceId != v.displayId && !v.dirty)
    {
        v.appearanceId = v.displayId;
        v.editDisplay = m_displayRows.Row(v.displayId);
        v.editExtra = m_extraRows.Row(v.editDisplay.value("ExtendedDisplayInfoID", 0u));
    }
    if (!owned || !humanoid || v.appearanceId != v.displayId || !v.editExtra.is_object())
    {
        if (!v.displayId) ImGui::TextColored(kQuiet, "No display shown.");
        else if (!humanoid) ImGui::TextWrapped("Display %u is not a character model: its look is its skins (View > Skins).", v.displayId);
        else if (!owned) ImGui::TextWrapped("Display %u belongs to the client and other creatures may use it: edit a copy.", v.displayId);
        else ImGui::TextWrapped("Apply or revert the other edits first.");
        ImGui::BeginDisabled(!m_project);
        if (humanoid && ImGui::Button("Make an editable copy")) NewNpcAppearance(v.displayId);
        if (ImGui::Button("New character appearance")) NewNpcAppearance(0);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("New CreatureDisplayInfo + CreatureDisplayInfoExtra rows in the project's ranges (File > Project settings)");
        return;
    }

    nlohmann::json& e = v.editExtra;
    bool changed = false;
    auto get = [&](const char* f) { return e.value(f, 0u); };
    auto set = [&](const std::string& f, uint32_t x) {
        if (e.value(f, 0u) == x) return;
        e[f] = x;
        changed = true;
    };
    ImGui::TextColored(kQuiet, "Display %u, extra %u. The client draws these fields itself (no baked texture).", v.displayId, get("ID"));
    const float labelWidth = 110;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-1);
    };

    // Race and sex pick the character model.
    const auto races = m_looks.Races();
    const uint32_t race = get("DisplayRaceID"), sex = get("DisplaySexID");
    std::string raceName = std::to_string(race);
    for (const auto& r : races)
        if (r.id == race && !r.name.empty()) raceName = r.name;
    row("Race");
    if (ImGui::BeginCombo("##race", raceName.c_str(), ImGuiComboFlags_HeightLarge))
    {
        for (const auto& r : races)
            if (!r.name.empty() && m_looks.CharacterModel(r.id, sex) && ImGui::Selectable((r.name + "##" + std::to_string(r.id)).c_str(), r.id == race))
                set("DisplayRaceID", r.id);
        ImGui::EndCombo();
    }
    row("Body");
    if (ImGui::BeginCombo("##sex", sex ? "Female" : "Male"))
    {
        for (uint32_t s = 0; s < 2; ++s)
            if (m_looks.CharacterModel(race, s) && ImGui::Selectable(s ? "Female" : "Male", s == sex)) set("DisplaySexID", s);
        ImGui::EndCombo();
    }
    if (changed)
        if (const uint32_t model = m_looks.CharacterModel(get("DisplayRaceID"), get("DisplaySexID"))) v.editDisplay["ModelID"] = model;

    // One "Option: value" dropdown each, like wow.export's character tab. A value the new race lacks moves to its first.
    auto choices = m_looks.CharacterChoices(get("DisplayRaceID"), get("DisplaySexID"), get("SkinID"), get("HairStyleID"));
    auto option = [&](const char* label, const char* field, const std::vector<uint32_t>& values) {
        const uint32_t x = get(field);
        if (!values.empty() && std::find(values.begin(), values.end(), x) == values.end()) set(field, values.front());
        row(label);
        if (ImGui::BeginCombo((std::string("##") + field).c_str(), values.empty() ? "(none)" : std::to_string(get(field) + 1).c_str()))
        {
            for (uint32_t value : values)
                if (ImGui::Selectable(std::to_string(value + 1).c_str(), value == get(field))) set(field, value);
            ImGui::EndCombo();
        }
    };
    option("Skin color", "SkinID", choices.skins);
    choices = m_looks.CharacterChoices(get("DisplayRaceID"), get("DisplaySexID"), get("SkinID"), get("HairStyleID"));   // faces follow the skin
    option("Face", "FaceID", choices.faces);
    option("Hair style", "HairStyleID", choices.hairStyles);
    choices = m_looks.CharacterChoices(get("DisplayRaceID"), get("DisplaySexID"), get("SkinID"), get("HairStyleID"));   // colours follow the style
    option("Hair color", "HairColorID", choices.hairColors);
    option("Facial hair", "FacialHairID", choices.facialHair);

    ImGui::SeparatorText("Armour");
    static const char* const kSlots[11] = { "Head", "Shoulders", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrists", "Hands", "Tabard", "Back" };
    bool openPicker = false;
    for (int slot = 0; slot < 11; ++slot)
    {
        ImGui::PushID(slot);
        const std::string field = "NPCItemDisplay[" + std::to_string(slot) + "]";
        const uint32_t display = e.value(field, 0u);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kSlots[slot]);
        ImGui::SameLine(80);
        const std::string icon = m_looks.ItemIcon(display);
        if (ID3D11ShaderResourceView* tex = icon.empty() ? nullptr : m_renderer.TextureFor(icon, m_mpq)) ImGui::Image(ImTextureID(intptr_t(tex)), { 22, 22 });
        else ImGui::Dummy({ 22, 22 });
        ImGui::SameLine();
        if (ImGui::Button(display ? ("Item display " + std::to_string(display) + "##pick").c_str() : "(empty)##pick", { -30, 0 }))
        {
            v.pickArmor = slot;
            v.pickSet = -1;
            v.pickLoot = false;
            openPicker = true;
        }
        ImGui::SetItemTooltip("Choose an item for the %s slot (its display is stored)", kSlots[slot]);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) set(field, 0);
        ImGui::SetItemTooltip("Empty the slot");
        ImGui::PopID();
    }
    if (openPicker) ImGui::OpenPopup("##itempick");
    DrawItemPicker();
    if (changed)
    {
        v.dirty = true;
        RefreshNpcDisplay();
    }
}

// ---------------------------------------------------------------------------------------------- loot

App::NpcView::Loot& App::NpcLoot(int kind)
{
    NpcView& v = m_npc;
    static const char* const kFields[3] = { "lootid", "pickpocketloot", "skinloot" };
    TableRowsAdapter* tables[3] = { &m_lootDrops, &m_lootPickpocket, &m_lootSkinning };
    NpcView::Loot& loot = v.loot[kind];
    const uint32_t id = U(Col(v.editTemplate, kFields[kind]));
    if (loot.id != id)
    {
        loot.id = id;
        loot.rows = id ? tables[kind]->Rows(id) : std::vector<nlohmann::json>{};
    }
    return loot;
}

void App::DrawNpcLootTab()
{
    NpcView& v = m_npc;
    if (!v.entry || !v.editTemplate.is_object()) { ImGui::TextColored(kQuiet, "No creature open."); return; }
    static const char* const kKinds[3] = { "Drops", "Pickpocketing", "Skinning" };
    static const char* const kFields[3] = { "lootid", "pickpocketloot", "skinloot" };
    for (int k = 0; k < 3; ++k)
    {
        if (k) ImGui::SameLine();
        if (ImGui::RadioButton(kKinds[k], v.lootKind == k)) v.lootKind = k;
    }
    NpcView::Loot& loot = NpcLoot(v.lootKind);
    const std::string field = kFields[v.lootKind];

    // The loot id: the template's column; several creatures may share one.
    if (!loot.id)
    {
        ImGui::TextWrapped("This creature has no %s loot (%s = 0).", kKinds[v.lootKind], field.c_str());
        if (ImGui::Button("Give it loot of its own"))
        {
            v.editTemplate[field] = std::to_string(v.entry);   // AzerothCore's convention: the loot id is the entry
            v.dirty = true;
        }
        ImGui::SetItemTooltip("Sets %s to %u (its entry); then add items", field.c_str(), v.entry);
        return;
    }
    std::string error;
    const auto shared = m_db.Query("SELECT COUNT(*) FROM creature_template WHERE " + field + " = " + std::to_string(loot.id) + " AND entry <> " + std::to_string(v.entry), error);
    const int others = shared && !shared->empty() ? std::atoi((*shared)[0][0].c_str()) : 0;
    ImGui::TextColored(kQuiet, "%s = %u", field.c_str(), loot.id);
    if (others)
    {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "shared with %d other creature(s)", others);
        ImGui::SetItemTooltip("Edits change their loot too. Give this one its own to keep them apart.");
        if (loot.id != v.entry && ImGui::SmallButton("Give it loot of its own (copy)"))
        {
            auto rows = loot.rows;
            for (auto& r : rows) r["Entry"] = std::to_string(v.entry);
            v.editTemplate[field] = std::to_string(v.entry);
            NpcLoot(v.lootKind).rows = std::move(rows);   // re-read for the new id, then replaced by the copy
            v.dirty = true;
            return;
        }
    }

    // One row per item or reference. Items in a group (1+) drop one of them; group 0 rolls each on its own.
    bool changed = false;
    std::optional<size_t> remove;
    std::map<uint32_t, float> groupChance;
    std::map<uint32_t, int> groupZero;   // rows with chance 0 in a group: they share what is left
    if (ImGui::BeginTable("##loot", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          { 0, std::min(ImGui::GetTextLineHeightWithSpacing() * (float(loot.rows.size()) + 2.5f), 420.0f) }))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Chance", ImGuiTableColumnFlags_WidthFixed, 62);
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 38);
        ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 32);
        ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthFixed, 38);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < loot.rows.size(); ++i)
        {
            nlohmann::json& r = loot.rows[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const uint32_t item = U(Col(r, "Item")), reference = U(Col(r, "Reference"));
            if (reference)
            {
                // A reference: the rows of reference_loot_template it names, rolled MinCount..MaxCount times.
                const bool open = ImGui::TreeNodeEx("##ref", ImGuiTreeNodeFlags_SpanAvailWidth, "Reference %u  %s", reference, Col(r, "Comment").c_str());
                if (open)
                {
                    auto [it, added] = v.references.try_emplace(reference);
                    if (added)
                        if (const auto rows = m_db.Query("SELECT r.Item, COALESCE(i.name, ''), r.Chance, r.GroupId, COALESCE(i.Quality, 1) FROM reference_loot_template r "
                                                         "LEFT JOIN item_template i ON i.entry = r.Item WHERE r.Entry = " + std::to_string(reference) + " ORDER BY r.GroupId, r.Chance DESC",
                                                         error))
                            it->second = *rows;
                    for (const auto& ref : it->second)
                    {
                        ImGui::TextColored(QualityColor(U(ref[4])), "%s", ref[1].empty() ? ("#" + ref[0]).c_str() : ref[1].c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(kQuiet, "%s%%  group %s", ref[2].c_str(), ref[3].c_str());
                    }
                    if (it->second.empty()) ImGui::TextColored(kQuiet, "(no rows)");
                    ImGui::TreePop();
                }
            }
            else
            {
                const NpcView::Item& info = NpcItem(item);
                const std::string icon = m_looks.ItemIcon(info.display);
                if (ID3D11ShaderResourceView* tex = icon.empty() ? nullptr : m_renderer.TextureFor(icon, m_mpq)) ImGui::Image(ImTextureID(intptr_t(tex)), { 18, 18 });
                else ImGui::Dummy({ 18, 18 });
                ImGui::SameLine();
                ImGui::TextColored(QualityColor(info.quality), "%s", info.found ? info.name.c_str() : ("#" + std::to_string(item) + " (not in item_template)").c_str());
                ImGui::SetItemTooltip("Item %u   %s", item, Col(r, "Comment").c_str());
            }
            auto number = [&](const char* id, const char* col, float lo, float hi, const char* format) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                float x = F(Col(r, col));
                if (ImGui::DragFloat(id, &x, std::string(col) == "Chance" ? 0.5f : 0.1f, lo, hi, format, ImGuiSliderFlags_AlwaysClamp))
                {
                    char text[32];
                    snprintf(text, sizeof text, std::string(col) == "Chance" ? "%g" : "%.0f", x);
                    r[col] = text;
                    changed = true;
                }
            };
            number("##chance", "Chance", 0, 100, "%.1f%%");
            ImGui::SetItemTooltip("100 always; 0 in a group: an equal share of what the group's other rows leave");
            number("##group", "GroupId", 0, 255, "%.0f");
            ImGui::SetItemTooltip("0: rolled on its own. 1 and up: at most one row of the group drops");
            number("##min", "MinCount", 1, 255, "%.0f");
            number("##max", "MaxCount", 1, 255, "%.0f");
            ImGui::TableNextColumn();
            if (bool quest = Col(r, "QuestRequired") != "0" && !Col(r, "QuestRequired").empty(); ImGui::Checkbox("##quest", &quest))
            {
                r["QuestRequired"] = quest ? "1" : "0";
                changed = true;
            }
            ImGui::SetItemTooltip("Drops only for players on a quest that needs it");
            number("##mode", "LootMode", 0, 65535, "%.0f");
            ImGui::SetItemTooltip("Loot mode bit mask (1 normal; scripts switch others on, e.g. hard modes)");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = i;
            const uint32_t group = U(Col(r, "GroupId"));
            if (group)
            {
                groupChance[group] += F(Col(r, "Chance"));
                if (F(Col(r, "Chance")) == 0) ++groupZero[group];
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove) { loot.rows.erase(loot.rows.begin() + std::ptrdiff_t(*remove)); changed = true; }
    if (loot.rows.empty()) ImGui::TextColored(kQuiet, "Nothing yet.");
    for (const auto& [group, chance] : groupChance)
        if (chance > 100.001f) ImGui::TextColored(kWarn, "Group %u: chances add up to %.1f%% (over 100: some rows can never drop).", group, chance);
        else if (groupZero.count(group)) ImGui::TextColored(kQuiet, "Group %u: %.1f%% fixed, %d row(s) share the remaining %.1f%%.", group, chance, groupZero[group], 100 - chance);

    if (ImGui::Button("Add item..."))
    {
        v.pickLoot = true;
        v.pickArmor = v.pickSet = -1;
        ImGui::OpenPopup("##itempick");
    }
    ImGui::SameLine();
    static int newReference = 0;
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##ref", &newReference, 0);
    ImGui::SameLine();
    if (ImGui::Button("Add reference") && newReference > 0)
    {
        loot.rows.push_back({ { "Entry", std::to_string(loot.id) }, { "Item", "0" }, { "Reference", std::to_string(newReference) }, { "Chance", "100" },
                              { "QuestRequired", "0" }, { "LootMode", "1" }, { "GroupId", "0" }, { "MinCount", "1" }, { "MaxCount", "1" },
                              { "Comment", Col(v.editTemplate, "name") + " - (ReferenceTable)" } });
        changed = true;
    }
    ImGui::SetItemTooltip("A reference_loot_template entry: a shared table (\"Grey 1-5\", \"Small Pouch\" ...) rolled as one row");
    if (v.lootKind == 0)
    {
        const uint32_t lo = U(Col(v.editTemplate, "mingold")), hi = U(Col(v.editTemplate, "maxgold"));
        ImGui::TextColored(kQuiet, "Money: %u - %u copper (Template tab)", lo, hi);
    }
    DrawItemPicker();
    if (changed) v.dirty = true;
    v.pickLoot = v.pickLoot && ImGui::IsPopupOpen("##itempick");
}
