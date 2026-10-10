#include "App.hpp"
#include "Minimap.hpp"
#include "Assets.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <fstream>
#include <random>

using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
    const char* kOldConfigFile = "wow-world-editor.cfg";   // where older builds kept the last project, next to the exe
    const ImVec4 kBad{ 1.00f, 0.42f, 0.38f, 1.00f };

    /// How a log line reads: 2 an error, 1 something to act on, 0 information.
    /// ponytail: keyword match on the text; a level per Log call if the guess misfires.
    int LogLevel(const std::string& line)
    {
        std::string l = line;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const char* w : { "fail", "error", "cannot", "can't", "not written", "refus", "not connected", "missing" })
            if (l.find(w) != std::string::npos) return 2;
        for (const char* w : { "restart", "warning", "first", "no free" })
            if (l.find(w) != std::string::npos) return 1;
        return 0;
    }

    const ImVec4 kAccent{ 0.30f, 0.62f, 1.00f, 1.00f };
    const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };
    const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };

    std::string Narrow(const wchar_t* w)
    {
        char buf[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
        return buf;
    }

}

std::optional<std::string> PickFolder(HWND owner, const wchar_t* title)
{
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    if (FAILED(dlg->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::string result = Narrow(path);
    CoTaskMemFree(path);
    return result;
}

namespace
{

    bool ContainsNoCase(const std::string& text, const char* query)
    {
        if (!*query) return true;
        auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };
        return lower(text).find(lower(query)) != std::string::npos;
    }

    std::string FileOf(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void ApplyStyle(float dpi)
    {
        ImGuiStyle& s = ImGui::GetStyle();
        ImGui::StyleColorsDark(&s);
        s.WindowRounding = 6;
        s.ChildRounding = 4;
        s.FrameRounding = 4;
        s.PopupRounding = 6;
        s.GrabRounding = 4;
        s.TabRounding = 4;
        s.ScrollbarRounding = 6;
        s.WindowPadding = { 10, 10 };
        s.FramePadding = { 8, 5 };
        s.ItemSpacing = { 8, 6 };
        s.ItemInnerSpacing = { 6, 4 };
        s.IndentSpacing = 16;
        s.WindowBorderSize = 1;
        s.FrameBorderSize = 0;
        s.TabBorderSize = 0;
        s.DockingSeparatorSize = 2;

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg] = { 0.11f, 0.12f, 0.13f, 1 };
        c[ImGuiCol_ChildBg] = { 0.09f, 0.10f, 0.11f, 1 };
        c[ImGuiCol_PopupBg] = { 0.12f, 0.13f, 0.15f, 0.98f };
        c[ImGuiCol_Border] = { 0.22f, 0.24f, 0.27f, 1 };
        c[ImGuiCol_FrameBg] = { 0.17f, 0.18f, 0.20f, 1 };
        c[ImGuiCol_FrameBgHovered] = { 0.22f, 0.24f, 0.27f, 1 };
        c[ImGuiCol_FrameBgActive] = { 0.26f, 0.28f, 0.32f, 1 };
        c[ImGuiCol_TitleBg] = { 0.09f, 0.10f, 0.11f, 1 };
        c[ImGuiCol_TitleBgActive] = { 0.11f, 0.12f, 0.14f, 1 };
        c[ImGuiCol_MenuBarBg] = { 0.09f, 0.10f, 0.11f, 1 };
        c[ImGuiCol_Header] = { 0.20f, 0.36f, 0.58f, 0.55f };
        c[ImGuiCol_HeaderHovered] = { 0.24f, 0.44f, 0.70f, 0.70f };
        c[ImGuiCol_HeaderActive] = { 0.26f, 0.50f, 0.80f, 0.85f };
        c[ImGuiCol_Button] = { 0.19f, 0.21f, 0.24f, 1 };
        c[ImGuiCol_ButtonHovered] = { 0.25f, 0.29f, 0.34f, 1 };
        c[ImGuiCol_ButtonActive] = { 0.24f, 0.46f, 0.75f, 1 };
        c[ImGuiCol_Tab] = { 0.13f, 0.14f, 0.16f, 1 };
        c[ImGuiCol_TabHovered] = { 0.24f, 0.44f, 0.70f, 1 };
        c[ImGuiCol_TabSelected] = { 0.18f, 0.22f, 0.28f, 1 };
        c[ImGuiCol_TabDimmed] = { 0.11f, 0.12f, 0.13f, 1 };
        c[ImGuiCol_TabDimmedSelected] = { 0.15f, 0.17f, 0.20f, 1 };
        c[ImGuiCol_DockingPreview] = { 0.30f, 0.62f, 1.00f, 0.55f };
        c[ImGuiCol_SliderGrab] = kAccent;
        c[ImGuiCol_CheckMark] = kAccent;
        s.ScaleAllSizes(dpi);
        s.FontScaleDpi = dpi;
    }

    /// A row of mutually exclusive options drawn as joined toggle buttons; returns true on change.
    template <class E>
    bool Segmented(const char* id, E& value, const std::vector<std::pair<E, std::string>>& items, float width = 0)
    {
        bool changed = false;
        ImGui::PushID(id);
        const float each = width > 0 ? (width - ImGui::GetStyle().ItemSpacing.x * (items.size() - 1)) / items.size() : 0;
        int i = 0;
        for (const auto& [v, label] : items)
        {
            if (i++) ImGui::SameLine();
            const bool on = value == v;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.42f, 0.70f, 1));
            if (ImGui::Button(label.c_str(), ImVec2(each, 0))) { value = v; changed = true; }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
        return changed;
    }
}

// ---------------------------------------------------------------------------------------------- setup

XMVECTOR App::Camera::Forward() const
{
    if (topDown) return XMVectorSet(0, -1, 0, 0);
    return XMVectorSet(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw), 0);
}

XMMATRIX App::Camera::View() const
{
    return XMMatrixLookToRH(XMLoadFloat3(&pos), Forward(), topDown ? Heading() : XMVectorSet(0, 1, 0, 0));
}

XMVECTOR App::Camera::Heading() const
{
    return XMVectorSet(std::sin(yaw), 0, std::cos(yaw), 0);
}

XMFLOAT3 App::CameraTarget() const
{
    // The view ray of the angled camera (pitch, even in top-down) onto the ground; not loaded yet -> a guess
    // 40 yd down, as FlyTo frames things.
    Camera angled = m_camera;
    angled.topDown = false;
    const XMVECTOR p0 = XMLoadFloat3(&m_camera.pos), dir = angled.Forward();
    XMFLOAT3 t;
    if (const auto hit = m_terrain.Pick(p0, dir, false)) return hit->pos;
    const float drop = m_camera.pos.y - m_terrain.HeightAt(m_camera.pos.x, m_camera.pos.z).value_or(m_camera.pos.y - 40.0f);
    const float along = angled.pitch < -0.05f ? std::max(drop, 0.0f) / std::tan(-angled.pitch) : 0.0f;
    XMStoreFloat3(&t, XMVectorAdd(p0, XMVectorScale(m_camera.Heading(), along)));
    t.y = m_camera.pos.y - drop;
    return t;
}

XMMATRIX App::Projection(float aspect, float nearZ, float farZ) const
{
    if (!m_camera.topDown) return XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), aspect, nearZ, farZ);
    const float tall = 2.0f * m_topHeight * std::tan(XMConvertToRadians(30.0f));
    return XMMatrixOrthographicRH(tall * aspect, tall, -3000.0f, std::max(farZ, m_topHeight + 3000.0f));
}

XMFLOAT3 App::ViewEye() const
{
    XMFLOAT3 e = m_camera.pos;
    if (m_camera.topDown) e.y = m_terrain.HeightAt(e.x, e.z).value_or(e.y - m_topHeight);   // the ground, however far the map was panned
    return e;
}

void App::SetTopDown(bool on)
{
    if (on == m_camera.topDown) return;
    const XMFLOAT3 t = CameraTarget();
    m_camera.topDown = on;
    m_topSeen.reset();
    m_topGoal = m_topHeight;
    m_topVel = { 0, 0 };
    if (on)
    {
        m_camera.yaw = m_topYaw;
        m_camera.pos = { t.x, m_camera.pos.y, t.z };
        FollowTopDown();
        return;
    }
    // Back to the angled camera, framing the same spot.
    const float dist = std::min(m_topHeight, 300.0f);
    XMStoreFloat3(&m_camera.pos, XMVectorSubtract(XMLoadFloat3(&t), XMVectorScale(m_camera.Heading(), dist)));
    m_camera.pos.y = t.y + dist * 0.68f;
    m_camera.pitch = -0.6f;
}

void App::FrameArea(float cx, float cz, float span)
{
    const float ground = m_terrain.HeightAt(cx, cz).value_or(m_camera.pos.y - 100.0f);
    if (m_camera.topDown)   // straight over its middle, zoomed so all of it is on screen
    {
        m_topHeight = m_topGoal = std::clamp((span * 1.3f + 40.0f) / (2.0f * std::tan(XMConvertToRadians(30.0f))), 30.0f, 3000.0f);
        m_camera.pos = { cx, ground + m_topHeight, cz };
        m_topVel = { 0, 0 };
        m_topSeen = { m_camera.pos, m_camera.pitch };   // placed exactly: nothing to follow
        return;
    }
    // From the south, high enough to see all of it.
    const float dist = span * 0.9f + 60.0f;
    m_camera.pos = { cx, ground + dist * 0.68f, cz - dist };
    m_camera.yaw = 0;
    m_camera.pitch = -0.6f;
}

void App::FollowTopDown()
{
    if (!m_camera.topDown) return;
    // Something else moved the camera (fly-to, differences, focus tile...): over the spot it aimed at. Only its
    // height changed (ground streamed in): stay put. Then sit m_topHeight above the ground there.
    if (m_topSeen)
    {
        const XMFLOAT3& was = m_topSeen->first;
        if (was.x != m_camera.pos.x || was.z != m_camera.pos.z || m_topSeen->second != m_camera.pitch)
        {
            const XMFLOAT3 t = CameraTarget();
            m_camera.pos.x = t.x;
            m_camera.pos.z = t.z;
            m_camera.pos.y = t.y + m_topHeight;
            m_camera.yaw = m_topYaw;   // features aim from the south; the map keeps its turn
        }
        else if (was.y != m_camera.pos.y)
            m_camera.pos.y = m_terrain.HeightAt(m_camera.pos.x, m_camera.pos.z).value_or(m_camera.pos.y) + m_topHeight;
    }
    else if (const auto h = m_terrain.HeightAt(m_camera.pos.x, m_camera.pos.z))
        m_camera.pos.y = *h + m_topHeight;
    m_topSeen = { m_camera.pos, m_camera.pitch };
}

bool App::Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, bool firstRun)
{
    m_hwnd = hwnd;
    m_device = device;
    m_context = context;
    m_buildLayout = firstRun;
    SetCdnAsync(true);   // CASC files not on disk: fetched in the background, the window never waits on the network

    // Gizmo: bold handles with clear arrowheads, a little larger than ImGuizmo's default, and arrows that
    // always point along +X/+Y/+Z instead of flipping towards the camera.
    ImGuizmo::Style& gizmo = ImGuizmo::GetStyle();
    gizmo.TranslationLineThickness = 6.0f;
    gizmo.TranslationLineArrowSize = 16.0f;
    gizmo.RotationLineThickness = 5.0f;
    gizmo.RotationOuterLineThickness = 5.0f;
    gizmo.ScaleLineThickness = 6.0f;
    gizmo.ScaleLineCircleSize = 10.0f;
    gizmo.HatchedAxisLineThickness = 0.0f;
    gizmo.CenterCircleSize = 8.0f;
    ImGuizmo::SetGizmoSizeClipSpace(0.15f);
    ImGuizmo::AllowAxisFlip(false);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    ApplyStyle(dpi);

    std::string error;
    if (!m_renderer.Init(device, context, error))
    {
        MessageBoxA(hwnd, error.c_str(), "Shader error", MB_ICONERROR);
        return false;
    }
    if (!m_models.Init(device, context, m_renderer, error))
    {
        MessageBoxA(hwnd, error.c_str(), "Shader error", MB_ICONERROR);
        return false;
    }
    m_drawOptions.showObjects = false;   // real models replace the placement boxes
    m_renderer.SetLoader(&m_loader);
    m_models.SetLoader(&m_loader);
    m_terrain.SetLoader(&m_loader);
    m_store.Register(m_terrain);
    m_store.Register(m_roads);
    m_terrain.SetRoads(&m_roads);
    m_roads.onChanged = [this](const std::string& map, const RoadStore::Cells& cells) { m_terrain.RefreshCells(map, cells); };
    m_store.Register(m_creatures);
    m_store.Register(m_raceRows);
    m_store.Register(m_gameobjects);
    for (DbcTable* table : DbcTables()) m_store.Register(*table);
    for (TableRowsAdapter* table : TableAdapters())
    {
        m_store.Register(*table);
        table->SetDb(&m_db);
    }
    m_creatures.SetDb(&m_db);
    m_gameobjects.SetDb(&m_db);
    // Displays: the NPC editor's unapplied appearance first, then the project's rows, then the client's.
    m_looks.SetRowOverride([this](int table, uint32_t id) -> const nlohmann::json* {
        const NpcView& v = m_npc;
        if (v.appearanceId && v.editDisplay.is_object())
        {
            if (table == 0 && id == v.appearanceId) return &v.editDisplay;
            if (table == 1 && v.editExtra.is_object() && id == v.editExtra.value("ID", 0u)) return &v.editExtra;
        }
        return (table == 0 ? m_displayRows : m_extraRows).Edited(id);
    });
    m_looks.SetUpload([this](const std::string& name, const BlpImage& image) { m_renderer.CacheTexture(name, image); });

    char user[64] = {};
    DWORD size = sizeof user;
    GetUserNameA(user, &size);
    strncpy_s(m_newAuthor, user, _TRUNCATE);

    auto hasProject = [this] { return m_project.has_value(); };
    auto always = [] { return true; };
    m_commands = {
        { "New project...", "Ctrl+N", [this] { NewProject(); }, always },
        { "Open project...", "Ctrl+O", [this] { OpenProjectDialog(); }, always },
        { "Save", "Ctrl+S", [this] { Save(); }, hasProject },
        { "Export client files", "Ctrl+E", [this] { Export(false); }, hasProject },
        { "Play test (export to client overlay)", "F5", [this] { Export(true); }, hasProject },
        { "Build patch MPQ", "Ctrl+Shift+E", [this] { BuildPatch(false); }, hasProject },
        { "Build patch MPQ and install it into the client", "", [this] { BuildPatch(true); }, hasProject },
        { "Undo", "Ctrl+Z", [this] { Undo(); }, [this] { return m_store.CanUndo(); } },
        { "Redo", "Ctrl+Y", [this] { Redo(); }, [this] { return m_store.CanRedo(); } },
        { "Group: Terrain", "F1", [this] { SetGroup(Group::Terrain); }, always },
        { "Group: Objects", "F2", [this] { SetGroup(Group::Objects); }, always },
        { "Group: Units", "F3", [this] { SetGroup(Group::Units); }, always },
        { "Group: Regions", "F4", [this] { SetGroup(Group::Regions); }, always },
        { "Group: Atmosphere", "F6", [this] { SetGroup(Group::Atmosphere); }, always },
        { "Copy selected chunks", "Ctrl+C", [this] { CopySelection(); }, [this] { return !m_selection.empty(); } },
        { "Paste at cursor", "Ctrl+V", [this] { PasteAtCursor(); }, [this] { return !m_clipboard.Empty(); } },
        { "Start / stop placing the clipboard", "P", [this] { m_tool = Tool::Copy; m_placing = !m_placing; m_pin.reset(); }, [this] { return !m_clipboard.Empty(); } },
        { "Commit pinned paste", "Enter", [this] { CommitPlacement(); }, [this] { return m_pin.has_value(); } },
        { "Cancel pinned paste", "Esc", [this] { CancelPin(); }, [this] { return m_pin.has_value(); } },
        { "Rotate clipboard clockwise", "R", [this] { RotateClipboard(1); }, [this] { return !m_clipboard.Empty(); } },
        { "Rotate clipboard counter-clockwise", "Shift+R", [this] { RotateClipboard(3); }, [this] { return !m_clipboard.Empty(); } },
        { "Rotate selection in place", "", [this] { RotateSelectionInPlace(); }, [this] { return !m_selection.empty(); } },
        { "Revert selection to client", "", [this] { RevertSelectionToClient(); }, [this] { return !m_selection.empty(); } },
        { "Clear selection", "Esc", [this] { m_selection.clear(); }, [this] { return !m_selection.empty(); } },
        { "Sculpt: Raise", "1", [this] { m_tool = Tool::Sculpt; m_brush.mode = Brush::Mode::Raise; }, always },
        { "Sculpt: Lower", "2", [this] { m_tool = Tool::Sculpt; m_brush.mode = Brush::Mode::Lower; }, always },
        { "Sculpt: Flatten", "3", [this] { m_tool = Tool::Sculpt; m_brush.mode = Brush::Mode::Flatten; }, always },
        { "Sculpt: Smooth", "4", [this] { m_tool = Tool::Sculpt; m_brush.mode = Brush::Mode::Smooth; }, always },
        { "Sculpt: Select vertices", "5", [this] { m_tool = Tool::Sculpt; m_brush.mode = Brush::Mode::Vertices; }, always },
        { "Focus camera on tile", "F", [this] { FocusTile(); }, [this] { return !m_terrain.Tiles().empty(); } },
        { "View: top-down", "Ctrl+T", [this] { SetTopDown(!m_camera.topDown); }, always },
        { "Toggle wireframe", "", [this] { m_drawOptions.wireframe = !m_drawOptions.wireframe; }, always },
        { "Toggle object boxes", "", [this] { m_drawOptions.showObjects = !m_drawOptions.showObjects; }, always },
        { "Reset panel layout", "", [this] { m_buildLayout = true; }, always },
        { "Close project", "", [this] { GuardUnsaved([this] { CloseProject(); }); }, hasProject },
        { "Project settings...", "", [this] { OpenProjectSettings(); }, hasProject },
        { "New map...", "", [this] { OpenNewMap(); }, hasProject },
        { "Server setup...", "", [this] { OpenSetup(); }, hasProject },
        { "Server: build server data (maps, vmaps, mmaps)", "", [this] { m_showServerData = true; }, hasProject },
        { "Check for problems", "", [this] { RunChecks(); }, hasProject },
        { "Window: NPC viewer", "", [this] { m_showNpc = true; m_npc.focus = true; }, always },
        { "Window: Sources", "", [this] { m_showSources = true; }, hasProject },
        { "Window: Races", "", [this] { m_showRaces = true; }, hasProject },
        { "Window: Keyboard shortcuts", "Ctrl+/", [this] { m_showShortcuts = true; }, always },
        { "View: toggle far terrain", "", [this] { m_drawOptions.farTerrain = !m_drawOptions.farTerrain; }, always },
        { "View: toggle terrain level of detail", "", [this] { m_drawOptions.lod = !m_drawOptions.lod; }, always },
        { "View: toggle creatures", "", [this] { m_showSpawns[0] = !m_showSpawns[0]; }, always },
        { "View: toggle gameobjects", "", [this] { m_showSpawns[1] = !m_showSpawns[1]; }, always },
    };
    for (const ToolInfo& t : kTools)
        m_commands.push_back({ std::string("Tool: ") + t.name + " (" + t.about + ")", t.key, [this, tool = t.tool] { m_tool = tool; }, always });

    // Recent projects; the list older builds kept next to the exe (one project) joins it once.
    if (std::ifstream f(SettingsDir() / "recent.txt"); f)
        for (std::string line; std::getline(f, line);)
            if (!line.empty()) m_recent.push_back(line);
    if (std::ifstream old(kOldConfigFile); old)
        if (std::string line; std::getline(old, line) && !line.empty() && std::find(m_recent.begin(), m_recent.end(), line) == m_recent.end())
            m_recent.push_back(line);
    if (!m_recent.empty() && fs::exists(fs::path(m_recent.front()) / "project.json")) OpenProject(m_recent.front());
    else Log("Welcome. Create a project (File > New project) or open one to start.");
    return true;
}

void App::RememberProject(const std::string& dir)
{
    std::erase(m_recent, dir);
    m_recent.insert(m_recent.begin(), dir);
    if (m_recent.size() > 10) m_recent.resize(10);
    std::ofstream f(SettingsDir() / "recent.txt");
    for (const std::string& d : m_recent) f << d << "\n";
}

void App::Autosave()
{
    if (!m_project || !m_store.Dirty()) return;
    const double now = ImGui::GetTime();
    if (m_store.Revision() != m_autosaveRevision)   // an edit just landed: write a moment later, once it settles
    {
        m_autosaveRevision = m_store.Revision();
        m_autosaveAt = now + 2.0;
        return;
    }
    if (now < m_autosaveAt) return;
    if (!Save(true)) m_autosaveAt = now + 30.0;   // failing: try again later, not every frame
}

void App::Log(const char* fmt, ...)
{
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[16];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    m_log.push_back(std::string(stamp) + "  " + text);
    if (const int level = LogLevel(text))
    {
        m_toast = text;
        m_toastError = level == 2;
        m_toastUntil = ImGui::GetCurrentContext() ? ImGui::GetTime() + (level == 2 ? 12.0 : 8.0) : 0;
    }
}

// ---------------------------------------------------------------------------------------------- actions

void App::GuardUnsaved(std::function<void()> then)
{
    if (!m_store.Dirty()) { then(); return; }
    m_afterUnsaved = std::move(then);
    m_unsavedOpen = true;
}

void App::NewProject()
{
    GuardUnsaved([this] {
        m_newName[0] = 0;
        if (m_project) strncpy_s(m_newClient, m_project->clientDir.c_str(), _TRUNCATE);
        m_newProjectOpen = true;
    });
}

void App::OpenProjectDialog()
{
    GuardUnsaved([this] {
        if (auto dir = PickFolder(m_hwnd, L"Open project folder")) OpenProject(*dir);
    });
}

bool App::OpenProject(const std::string& dir)
{
    std::string error;
    auto project = Project::Load(dir, error);
    if (!project) { Log("Could not open project: %s", error.c_str()); return false; }

    CloseProject();
    const size_t archives = m_mpq.Open(project->base.layers);
    m_mpq.SetMapsFromLowestLayer(true);   // each map as its own source has it (the client's), mods' copies are other versions
    for (size_t i = 0; i < project->base.layers.size() && i < m_mpq.Report().size(); ++i)
        if (!m_mpq.Report()[i].note.empty())
            Log("Base layer %s: %s", project->base.layers[i].path.c_str(), m_mpq.Report()[i].note.c_str());
    m_mpq.SetOverlay(project->dir / "overlay");   // tiles the project added (rebuilt once its changes are loaded)
    m_terrain.SetProjectDir(project->dir);
    if (!archives) Log("The project's base files have no archives: check Sources (View > Sources).");
    if (auto dbc = m_mpq.Read("DBFilesClient\\Map.dbc")) m_maps = ParseMapDbc(*dbc);
    m_mapListVersion = ~0ull;   // the Maps window rebuilds the list (project rows, source labels) on its next draw
    m_wdtArchives.clear();
    std::vector<std::string> blueprintErrors;
    m_blueprints = Blueprint::LoadAll(project->BlueprintsDir(), blueprintErrors);
    m_blueprintThumbs.clear();
    for (const auto& e : blueprintErrors) Log("Blueprint not loaded: %s", e.c_str());
    std::vector<std::string> sourceErrors;
    {
        std::vector<std::pair<std::string, std::vector<MpqLayer>>> compare;
        for (const Project::Source& s : project->compare) compare.push_back({ s.name, s.layers });
        m_ghosts.Reset(&m_mpq, project->name, compare, sourceErrors);
    }
    m_ghosts.StartWorker();
    for (const auto& e : sourceErrors) Log("Source not attached: %s", e.c_str());
    UpdateFallbacks();
    const auto catalogStart = std::chrono::steady_clock::now();
    m_catalog.Build(m_mpq);
    Log("Catalog: %zu doodads, %zu WMOs, %zu ground textures, %zu other textures (%.0f ms).", m_catalog.Count(Catalog::Kind::Doodad),
        m_catalog.Count(Catalog::Kind::Wmo), m_catalog.Count(Catalog::Kind::GroundTexture), m_catalog.Count(Catalog::Kind::Texture),
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - catalogStart).count());
    std::sort(m_maps.begin(), m_maps.end(), [](const MapEntry& a, const MapEntry& b) { return a.id < b.id; });

    m_project = std::move(*project);
    m_loader.Start(&m_mpq);
    m_store.author = m_project->author;
    if (!m_store.Load(m_project->ChangesDir(), error)) Log("Changes not fully loaded: %s", error.c_str());
    m_terrain.RebuildOverlay();
    RememberProject(m_project->dir.string());
    Log("Opened project '%s': %zu archives, %zu maps, %zu changes.", m_project->name.c_str(), archives, m_maps.size(), m_store.Done().size());
    ConnectServer();
    if (m_project->serverProfile.empty()) OpenSetup();
    RunChecks();

    // Reopen where the last edit happened.
    for (auto it = m_store.Done().rbegin(); it != m_store.Done().rend(); ++it)
        if (it->domain == m_terrain.Domain() && !it->data.at("edits").empty())
        {
            const std::string map = it->data.at("map");
            const auto& e = it->data.at("edits")[0];
            for (size_t i = 0; i < m_maps.size(); ++i)
                if (m_maps[i].directory == map) SelectMap(i);
            GoToTile(map, e[0], e[1]);
            break;
        }
    return true;
}

void App::CloseProject()
{
    m_terrain.Unload();
    m_models.Clear();
    m_objSel.clear();
    m_races = RacesView{};   // its sources' chains are about to close
    m_raceRows.Clear();
    m_catalog.Clear();
    m_catalogKey.clear();
    m_catalogItems.clear();
    m_armed.reset();
    m_diffs.Cancel();   // its worker reads the archives about to close
    m_diffs.Clear();
    m_diffThumbs.clear();
    m_diffNoThumb.clear();
    m_diffPending.clear();
    m_diffNewTiles.clear();
    m_diffNewLayer = 0;
    m_diffTarget = {};
    m_mpq.SetFallbacks({});
    m_ghosts = Ghosts{};
    m_soloLayer = m_copyLayer = 0;
    m_compare.clear();   // its layers went with the ghosts
    m_comparing = false;
    m_blueprints.clear();
    m_blueprintThumbs.clear();
    m_versions.clear();
    m_versionsKey.clear();
    m_store.Clear();
    m_loader.Stop();   // before the archives it reads from close
    m_renderer.ClearFar();
    m_mpq.SetOverlay({});
    m_terrain.SetProjectDir({});
    m_mpq.Close();
    m_project.reset();
    m_db.Close();
    m_soapOk.reset();
    m_commandLog.clear();
    m_problems.clear();
    m_problemsChecked = false;
    m_spawnView.clear();
    m_spawnSel.clear();
    m_spawnHover.reset();
    m_path.reset();
    m_pathView.clear();
    m_pathViewGuid = 0;
    m_spawnArmed.reset();
    m_spawnResults.clear();
    m_looks.Reset();
    m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;
    m_models.RemoveTile(-3);
    m_models.RemoveTile(-4);
    m_models.RemoveTile(-5);
    for (DbcTable* table : DbcTables()) table->Reset();
    m_unitTemplatesRead[0] = m_unitTemplatesRead[1] = false;
    m_mapJob.reset();
    m_flightNode = m_flightPath = 0;
    m_flightPoint.reset();
    m_flightPlace = false;
    m_poiSel = 0;
    m_poiPick = PoiPick::None;
    m_triggerSel = 0;
    m_triggerPick = TriggerPick::None;
    m_teleportViewRevision = ~0ull;
    m_wmoKeys.clear();
    m_maps.clear();
    m_mapIndex = -1;
    m_mapTiles.clear();
    m_selection.clear();
    m_hover.reset();
    m_focusTile.reset();
    m_flyGround.reset();
}

bool App::Save(bool quiet)
{
    if (!m_project) return false;
    std::string error;
    if (!m_project->Save(error) || !m_store.Save(m_project->ChangesDir(), error))
    {
        Log("%s failed: %s", quiet ? "Autosave" : "Save", error.c_str());
        return false;
    }
    if (!quiet) Log("Saved %zu changes to %s", m_store.Done().size(), m_project->ChangesDir().string().c_str());
    return true;
}

void App::Export(bool playTest)
{
    if (!m_project || !Save()) return;
    std::string error;
    const fs::path out = m_project->ClientOutDir();
    std::vector<fs::path> tiles;
    {
        // Every export starts empty, so a file an undone edit made earlier never lingers into the patch or the overlay.
        std::error_code clearEc;
        fs::remove_all(out, clearEc);
        if (clearEc) { Log("Export failed: cannot clear %s: %s", out.string().c_str(), clearEc.message().c_str()); return; }
    }
    // Tiles that fail a check are skipped and listed in Problems; the rest are written.
    m_problems.clear();
    m_problemsChecked = true;
    RenderMinimaps();
    const size_t written = m_terrain.Export(out, error, &tiles, &m_problems);
    m_terrain.FindCracks(m_problems);
    if (!error.empty()) { Log("Export failed: %s", error.c_str()); return; }
    Log("Exported %zu file(s) to %s", written, out.string().c_str());
    for (const Problem& p : m_problems)
        if (p.severity == Problem::Severity::Error) Log("Not exported: %s", p.message.c_str());
    // Textures and models the edits use that only another client has go into the patch too.
    if (const size_t dropped = m_terrain.TakeDroppedEffects())
        Log("%zu texture layer(s) had ground effects this client does not know (from another map); exported without them.", dropped);
    SetCdnAsync(false);   // the patch must be complete: wait for CDN files
    const AssetReport assets = CopyMissingAssets(m_mpq, tiles, out);
    SetCdnAsync(true);
    if (!assets.copied.empty()) Log("Added %zu file(s) from other clients that this client lacks (e.g. %s).", assets.copied.size(), assets.copied[0].c_str());
    if (!assets.converted.empty()) Log("Converted %zu newer-client file(s) to 3.3.5a; %zu change(s) noted in Problems.", assets.converted.size(), assets.notes.size());
    for (const auto& n : assets.notes) m_problems.push_back({ Problem::Severity::Warning, "Assets", "3.3.5a conversion: " + n });
    for (const SpawnAdapter* spawns : { &m_creatures, &m_gameobjects })
    {
        if (!spawns->ExportSql(m_project->dir / "out" / "server", error)) Log("%s", error.c_str());
        else if (size_t a, c, d; spawns->Counts(a, c, d), a + c + d)
            Log("Spawns: out/server/%s_spawns.sql (+%zu, ~%zu, -%zu) and %s_spawns_revert.sql.", spawns->Table(), a, c, d, spawns->Table());
    }
    for (const TableRowsAdapter* table : TableAdapters())
    {
        if (!table->ExportSql(m_project->dir / "out" / "server", error)) Log("%s", error.c_str());
        else if (const size_t n = table->Count()) Log("out/server/%s.sql (%zu changed) and %s_revert.sql.", table->Table().c_str(), n, table->Table().c_str());
    }
    for (const DbcTable* table : DbcTables())
    {
        if (!table->Export({ out / "DBFilesClient", m_project->dir / "out" / "server" / "dbc" }, m_project->dir / "out" / "dbc", error))
            Log("%s", error.c_str());
        else if (table->Count())
            Log("%s: %zu row(s) in DBFilesClient/%s.dbc, out/server/dbc and out/dbc/%s.json (mod-dbc-patch); client and worldserver read DBCs "
                "only at start, so restart them.", table->Name().c_str(), table->Count(), table->Name().c_str(), table->Name().c_str());
    }
    // Races: the race tables (client and server) and the files the project's races bring from their clients. Run with no
    // race too: it takes away race tables an earlier export left in out/server/dbc.
    {
        RaceCatalog races;
        std::string raceError;
        if (races.Load(m_mpq, raceError))
        {
            for (const auto& [id, package] : m_raceRows.Packages()) races.Apply(id, package);
            std::vector<std::string> notes;
            auto sourceNamed = [this](const std::string& client) -> const MpqChain* {
                for (const Ghosts::Source& s : m_ghosts.Sources())
                    if (s.name == client) return s.mpq;
                return nullptr;
            };
            if (!ExportRaces(races, m_raceRows.Packages(), sourceNamed, { out / "DBFilesClient", m_project->dir / "out" / "server" / "dbc" }, out, notes, error))
                Log("Races: %s", error.c_str());
            else if (!m_raceRows.Packages().empty())
                Log("Races: %zu race(s) of the project in DBFilesClient and out/server/dbc (ChrRaces, CharSections, CharHairGeosets, "
                    "CharacterFacialHairStyles, CharBaseInfo, CharStartOutfit: whole tables, mod-dbc-patch cannot express them); restart the "
                    "client and the worldserver.", m_raceRows.Packages().size());
            for (const std::string& n : notes)
            {
                Log("Races: %s", n.c_str());
                m_problems.push_back({ Problem::Severity::Warning, "Races", n });
            }
        }
        else if (!m_raceRows.Packages().empty()) Log("Races: %s", raceError.c_str());
    }
    // Generated pictures (world maps) go in as they are.
    size_t generated = 0;
    std::error_code assetsEc;
    for (const auto& entry : fs::recursive_directory_iterator(m_project->AssetsDir(), assetsEc))
        if (entry.is_regular_file())
        {
            const fs::path target = out / fs::relative(entry.path(), m_project->AssetsDir(), assetsEc);
            fs::create_directories(target.parent_path(), assetsEc);
            generated += fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, assetsEc);
        }
    if (generated) Log("Copied %zu generated file(s) from assets/.", generated);
    if (AreasPainted())
        Log("Area ids were painted: extract the server's maps again from the exported tiles, or the server keeps the old areas.");
    for (const auto& m : assets.missing)
    {
        Log("Missing everywhere, the client will not find it: %s", m.c_str());
        m_problems.push_back({ Problem::Severity::Error, "Assets", "Referenced by an edited tile, found in no client: " + m });
    }
    if (!playTest) return;

    std::error_code ec;
    const fs::path overlay = m_project->OverlayDir();
    if (!fs::exists(overlay.parent_path(), ec))
    {
        Log("Play test needs the wxl-editor-poc extension in the client (%s missing).", overlay.parent_path().string().c_str());
        return;
    }
    // The overlay mirrors this export: what the last one had and this one does not goes (an undone tile, say).
    size_t removed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(overlay, ec))
        if (entry.is_regular_file() && !fs::exists(out / fs::relative(entry.path(), overlay, ec), ec)) removed += fs::remove(entry.path(), ec);
    ec.clear();
    if (removed) Log("Play test: %zu file(s) no longer exported were taken out of the overlay.", removed);
    // Each file lands whole: copied beside its target, then renamed over it, so a running client never reads half a tile.
    size_t copied = 0;
    for (const auto& entry : fs::recursive_directory_iterator(out, ec))
    {
        if (!entry.is_regular_file()) continue;
        const fs::path target = overlay / fs::relative(entry.path(), out, ec);
        const fs::path temp = target.string() + ".partial";
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(entry.path(), temp, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::rename(temp, target, ec);
        if (ec) { Log("Copy to the client overlay failed at %s: %s", target.string().c_str(), ec.message().c_str()); return; }
        ++copied;
    }
    Log("Play test ready: %zu file(s) in the overlay; relog in the client to see the changes.", copied);
}

void App::BuildPatch(bool install)
{
    if (!m_project) return;
    Export(false);
    const fs::path out = m_project->ClientOutDir(), patch = m_project->PatchOutPath();
    std::error_code ec;
    if (!fs::exists(out, ec) || fs::is_empty(out, ec)) { Log("Patch not built: the export wrote nothing (no edits yet?)."); return; }
    std::string error;
    size_t files = 0;
    if (!WriteMpq(patch, out, error, &files)) { Log("Patch not built: %s", error.c_str()); return; }
    Log("Patch built: %s, %zu file(s), %.1f MB.", patch.string().c_str(), files, double(fs::file_size(patch, ec)) / (1024.0 * 1024.0));

    // Archives in the client that load after it would hide its files where they overlap (a later module patch, say).
    std::vector<std::string> above;
    for (const fs::path& dir : { m_project->DataDir(), m_project->PatchInstallPath().parent_path() })
        for (const auto& entry : fs::directory_iterator(dir, ec))
            if (const std::string name = entry.path().filename().string();
                entry.is_regular_file() && entry.path().extension().string().size() == 4 && LoadsAfter(name, m_project->patchName) &&
                _stricmp(entry.path().extension().string().c_str(), ".mpq") == 0)
                above.push_back(name);
    ec.clear();
    for (const auto& name : above)
    {
        const std::string text = name + " in the client loads after " + m_project->patchName + ": where both have a file, the client uses its";
        Log("Warning: %s.", text.c_str());
        m_problems.push_back({ Problem::Severity::Warning, "Patch", text });
    }
    if (!install) return;
    const fs::path target = m_project->PatchInstallPath(), temp = target.string() + ".partial";
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(patch, temp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(temp, target, ec);
    if (ec)
    {
        fs::remove(temp, ec);
        Log("Patch not installed into %s: close the client first (it holds its archives open).", target.string().c_str());
        return;
    }
    Log("Patch installed: %s. Start the client to see it (also restart the worldserver if DBCs changed).", target.string().c_str());
}

void App::Undo()
{
    if (m_terrain.Stroking() || m_terrain.HoleStroking() || m_terrain.AreaStroking() || m_terrain.Watering() || !m_store.CanUndo()) return;
    ClearPlacementView();
    Log("Undo: %s", m_store.Done().back().label.c_str());
    m_store.Undo();
}

void App::Redo()
{
    if (m_terrain.Stroking() || m_terrain.HoleStroking() || m_terrain.AreaStroking() || m_terrain.Watering() || !m_store.CanRedo()) return;
    ClearPlacementView();
    Log("Redo: %s", m_store.Undone().back().label.c_str());
    m_store.Redo();
}

void App::GoToTile(const std::string& map, int x, int y)
{
    if (map != m_terrain.Map())
    {
        StopCompare();   // its versions are of the old map
        std::string error;
        if (!m_terrain.SetMap(map, error)) { Log("%s", error.c_str()); return; }
        const auto wdl = m_mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdl");
        m_renderer.LoadFar(wdl ? ParseWdl(*wdl) : std::vector<std::vector<int16_t>>{});   // the whole map at low detail
        m_planKey.clear();   // SetMap dropped every tile, the preview included
        m_models.Clear();
        m_spawnModelVersion[0] = m_spawnModelVersion[1] = ~0u;   // gameobject models are rebuilt for the new map
        m_ghosts.ClearTiles();   // the renderer dropped them with the map; they stream in again
        m_objSel.clear();
        m_pin.reset();
        m_selection.clear();
        Log("Map %s: tiles stream in around the camera.", map.c_str());
    }
    m_camera.pos = { (x + 0.5f) * kTileSize, m_camera.pos.y, (y - 0.1f) * kTileSize };
    m_camera.yaw = 0;
    m_camera.pitch = -0.45f;
    m_focusTile = TileKey(x, y);
    if (auto it = m_terrain.Tiles().find(TileKey(x, y)); it != m_terrain.Tiles().end())
    {
        m_camera.pos.y = it->second.maxHeight + 120.0f;
        m_focusTile.reset();
    }
}

void App::GoToWmoMap(const std::string& map)
{
    GoToTile(map, 32, 32);
    const auto& w = m_terrain.GlobalWmo();
    if (m_terrain.Map() != map || !w) return;
    m_focusTile.reset();
    m_camera.pos = { (w->extMin[0] + w->extMax[0]) / 2, w->extMax[1] + 40.0f, (w->extMin[2] + w->extMax[2]) / 2 + 60.0f };
    m_camera.yaw = 0;
    m_camera.pitch = -0.7f;
    Log("%s is a WMO-only map (%s): right-drag and WASD to fly in.", map.c_str(), w->model.c_str());
}

std::optional<float> App::GroundAt(float x, float z, float fromY) const
{
    const std::optional<float> terrain = m_terrain.HeightAt(x, z);
    std::optional<float> best = terrain && *terrain <= fromY + 2 ? terrain : std::nullopt;
    ModelRenderer::DrawSettings wmos;
    wmos.doodads = false;
    const float top = fromY + 2;
    if (const auto hit = m_models.Pick(XMVectorSet(x, top, z, 0), XMVectorSet(0, -1, 0, 0), 2000.0f, wmos); hit && hit->wmo)
        if (const float y = top - hit->distance; !best || y > *best) best = y;
    return best ? best : terrain;
}

void App::FocusTile()
{
    const int key = TileKey(int(std::floor(m_camera.pos.x / kTileSize)), int(std::floor(m_camera.pos.z / kTileSize)));
    auto it = m_terrain.Tiles().find(key);
    if (it == m_terrain.Tiles().end()) return;
    GoToTile(m_terrain.Map(), it->second.x, it->second.y);
}

void App::CopySelection()
{
    if (m_selection.empty()) return;
    if (const Ghosts::Layer* layer = m_copyLayer ? m_ghosts.Find(m_copyLayer) : nullptr)
    {
        std::set<std::pair<int, int>> cells;
        for (ChunkRef r : m_selection) cells.insert(m_terrain.GridOf(r));
        m_clipboard = TerrainAdapter::CopyFrom(layer->tiles, cells);
        m_clipboard.pois = VersionPois(m_ghosts.Chain(layer->source), layer->map.empty() ? m_terrain.Map() : layer->map, cells, m_clipboard.originX,
                                       m_clipboard.originZ);
        Log("Copying from ghost layer %s (%zu landmark(s) on it).", layer->label.c_str(), m_clipboard.pois.size());
        if (m_clipboard.Empty()) { Log("That layer has nothing loaded under the selection."); return; }
    }
    else
        m_clipboard = m_terrain.Copy(m_selection);
    ++m_clipVersion;
    m_pasteOffset = 0;
    m_placing = true;
    m_pin.reset();
    if (m_copyLayer && m_ghosts.Find(m_copyLayer))
    {
        // From another version: it goes to the same spot on the map, pinned with its own heights; Enter commits.
        SetSolo(0);
        PasteInPlace();
        Log("Copied %zu chunk(s), %zu doodad(s), %zu WMO(s) from the ghost, pinned in place: Enter commits, arrows or a click move it, Esc cancels.",
            m_clipboard.chunks.size(), m_clipboard.doodads.size(), m_clipboard.wmos.size());
        return;
    }
    if (m_heightBeforeInPlace) { m_pasteHeightMode = *m_heightBeforeInPlace; m_heightBeforeInPlace.reset(); }
    Log("Copied %zu chunk(s), %zu doodad(s), %zu WMO(s). Click to pin it, Enter to commit, Esc to cancel.", m_clipboard.chunks.size(),
        m_clipboard.doodads.size(), m_clipboard.wmos.size());
}

void App::PasteAtCursor()
{
    if (m_clipboard.Empty() || m_terrain.Stroking()) return;
    if (!m_pin)
    {
        if (!m_hover) { Log("Point at the terrain where the paste should go, then press Ctrl+V."); return; }
        m_pin = m_terrain.GridOf(m_hover->chunk);
    }
    m_placing = true;
    CommitPlacement();
}

void App::CommitPlacement()
{
    if (!m_pin || m_clipboard.Empty()) return;
    m_compareCycledAt = -1e9;   // a compare commits the blended plan, never the quick one shown while cycling
    // A paste over tiles still loading would silently skip them; they are pinned, so this is a short wait.
    if (const size_t missing = m_terrain.MissingTiles(PasteTiles()))
    {
        Log("Still loading %zu tile(s) under the paste; press Enter again in a moment.", missing);
        return;
    }
    const Tool tool = m_tool;
    m_tool = Tool::Copy;   // the plan is only built for the Copy tool
    m_planKey.clear();
    UpdatePlacement();     // make sure the plan matches the current settings
    m_tool = tool;
    auto change = m_terrain.ApplyPlan(m_plan, "Paste " + std::to_string(m_clipboard.chunks.size()) + " chunk(s)" +
                                                  (m_blend ? ", blended" : ""));
    m_renderer.UnloadTile(-1);
    m_models.RemoveTile(-1);
    m_planKey.clear();
    m_pin.reset();
    if (m_heightBeforeInPlace) { m_pasteHeightMode = *m_heightBeforeInPlace; m_heightBeforeInPlace.reset(); }
    if (!change) { Log("Nothing pasted: the target chunks are not loaded."); return; }
    Log("%s (%zu chunks changed, %zu objects added).", change->label.c_str(), m_plan.chunks.size(),
        ChangeStore::List(change->data, "objects").size());
    // Landmarks of another version come along in the same undo step.
    std::vector<Change> parts = AddPastedPois(m_plan.pois, change->label);
    if (parts.empty()) { m_store.Commit(std::move(*change)); return; }
    const std::string label = change->label;
    parts.insert(parts.begin(), std::move(*change));
    m_store.Commit(std::move(parts), label);
}

void App::CancelPin()
{
    m_pin.reset();
    if (m_heightBeforeInPlace) { m_pasteHeightMode = *m_heightBeforeInPlace; m_heightBeforeInPlace.reset(); }
    ClearPlacementView();
}

float App::PasteOffsetAt(int gx, int gz) const
{
    switch (m_pasteHeightMode)
    {
    case PasteHeight::FollowGround:
        if (const auto ground = m_terrain.FootprintMean(m_clipboard, gx, gz)) return *ground - m_clipboard.MeanHeight() + m_pasteOffset;
        break;
    case PasteHeight::FollowSlope:
        if (const auto plane = m_terrain.FitSlope(m_clipboard, gx, gz)) return (*plane)[0] + m_pasteOffset;
        break;
    case PasteHeight::LowestPoint:
        if (const auto ground = m_terrain.FootprintMin(m_clipboard, gx, gz)) return *ground - m_clipboard.MinHeight() + m_pasteOffset;
        break;
    case PasteHeight::Absolute:
        break;
    }
    return m_pasteOffset;
}

void App::PasteSlopeAt(int gx, int gz, PasteOptions& options) const
{
    options.slopeX = options.slopeZ = 0;
    if (m_pasteHeightMode != PasteHeight::FollowSlope) return;
    if (const auto plane = m_terrain.FitSlope(m_clipboard, gx, gz))
    {
        options.slopeX = (*plane)[1];
        options.slopeZ = (*plane)[2];
    }
}

void App::RotateClipboard(int quarterTurns)
{
    if (m_clipboard.Empty()) return;
    for (int i = 0; i < quarterTurns % 4; ++i) m_clipboard.RotateClockwise();
    ++m_clipVersion;
}

void App::RevertSelectionToClient()
{
    if (m_selection.empty() || m_terrain.Stroking()) return;
    ClearPlacementView();
    std::set<std::pair<int, int>> cells;
    for (ChunkRef ref : m_selection) cells.insert(m_terrain.GridOf(ref));
    size_t skipped = 0;
    auto change = RevertToClient(m_terrain, m_mpq, cells, skipped);
    if (skipped) Log("%zu chunk(s) are on tiles the project added; the client has nothing to revert them to.", skipped);
    if (!change) { Log("Nothing to revert: the selection is as the client has it."); return; }
    Log("%s", change->label.c_str());
    m_store.Commit(std::move(*change));
}

void App::RotateSelectionInPlace()
{
    if (m_selection.empty() || m_terrain.Stroking()) return;
    ClearPlacementView();
    std::vector<ChunkRef> footprint;
    if (auto change = m_terrain.RotateInPlace(m_selection, &footprint))
    {
        Log("%s", change->label.c_str());
        m_store.Commit(std::move(*change));
        m_selection = std::set<ChunkRef>(footprint.begin(), footprint.end());
    }
}

namespace
{
    std::mt19937& Random()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }
    float RandomUnit() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(Random()); }

    /// `text` cut with an ellipsis to fit `width` pixels.
    std::string Fit(const std::string& text, float width)
    {
        if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
        std::string s = text;
        while (s.size() > 1 && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
        return s + "...";
    }
}

void App::PlaceFromCatalog(bool keepArmed)
{
    if (!m_armed || !m_hover) return;
    const float scale = std::clamp(m_placeScale * (1.0f + m_placeScaleJitter * (RandomUnit() * 2 - 1)), 1.0f / 1024.0f, 63.0f);
    std::vector<DoodadPlacement> doodads;
    std::vector<WmoPlacement> wmos;
    if (m_armed->wmo) wmos.push_back({ m_armed->path, { m_hover->pos.x, m_hover->pos.y, m_hover->pos.z }, { 0, m_placeYaw, 0 }, {}, {}, 0, 0 });
    else doodads.push_back({ m_armed->path, { m_hover->pos.x, m_hover->pos.y, m_hover->pos.z }, { 0, m_placeYaw, 0 }, scale, 0 });
    std::vector<ObjectRef> placed;
    if (auto change = m_terrain.PlaceObjects(doodads, wmos, "Place " + FileOf(m_armed->path), &placed))
    {
        Log("%s", change->label.c_str());
        m_store.Commit(std::move(*change));
        m_objSel = std::set<ObjectRef>(placed.begin(), placed.end());
    }
    if (m_placeRandomYaw) m_placeYaw = RandomUnit() * 360.0f;
    if (!keepArmed) m_armed.reset();
}

void App::DrawCatalog()
{
    if (!ImGui::Begin("Catalog")) { ImGui::End(); return; }
    if (m_catalog.Empty())
    {
        ImGui::TextColored(kQuiet, "Open a project: the catalog lists every model and texture in its client's MPQs.");
        ImGui::End();
        return;
    }
    using K = Catalog::Kind;
    struct Tab { K kind; const char* name; };
    const Tab tabs[] = { { K::Doodad, "Doodads" }, { K::Wmo, "Buildings" }, { K::GroundTexture, "Ground textures" }, { K::Texture, "Other textures" },
                         { K::Count, "All files" } };
    // Each tool group shows the tabs it places from (Zones has none of its own: every tab but the portals).
    const Group group = GroupOf(m_tool);
    auto shown = [&](int tab) {
        switch (group)
        {
        case Group::Terrain: return tab == int(K::GroundTexture) || tab == kBlueprintTab || tab == kDifferencesTab;
        case Group::Objects: return tab == int(K::Doodad) || tab == int(K::Wmo) || tab == int(K::Texture) || tab == int(K::Count);
        case Group::Units: return tab == kCreatureTab || tab == kGameobjectTab;
        default: return m_tool == Tool::Triggers ? tab == kPortalTab : tab != kPortalTab;
        }
    };
    if (!shown(m_catalogTab))   // the group changed: its first tab until the tab bar picks one
        m_catalogTab = group == Group::Terrain ? int(K::GroundTexture) : group == Group::Objects ? int(K::Doodad)
                     : m_tool == Tool::Triggers ? kPortalTab : group == Group::Regions ? int(K::Doodad) : kCreatureTab;
    if (ImGui::BeginTabBar("##catalogTabs"))
    {
        char blueprintLabel[64];
        snprintf(blueprintLabel, sizeof blueprintLabel, "Blueprints  %zu###Blueprints", m_blueprints.size());
        if (shown(kBlueprintTab) && ImGui::BeginTabItem(blueprintLabel))
        {
            m_catalogTab = kBlueprintTab;
            ImGui::EndTabItem();
        }
        {
            size_t pending = 0;
            for (const auto& r : m_diffs.Regions()) pending += r.status == Differences::Status::Pending;
            char label[64];
            snprintf(label, sizeof label, "Differences  %zu###Differences", pending);
            const bool bringForward = m_catalogShowTab == kDifferencesTab;
            if (shown(kDifferencesTab) && ImGui::BeginTabItem(label, nullptr, bringForward ? ImGuiTabItemFlags_SetSelected : 0))
            {
                if (bringForward) m_catalogShowTab.reset();
                m_catalogTab = kDifferencesTab;
                ImGui::EndTabItem();
            }
        }
        for (const auto& [tab, name] : { std::pair{ kCreatureTab, "Creatures" }, std::pair{ kGameobjectTab, "Gameobjects" }, std::pair{ kPortalTab, "Portal effects" } })
        {
            if (!shown(tab)) continue;
            const bool bringForward = m_catalogShowTab == tab;
            if (ImGui::BeginTabItem(name, nullptr, bringForward ? ImGuiTabItemFlags_SetSelected : 0))
            {
                if (bringForward) m_catalogShowTab.reset();
                m_catalogTab = tab;
                ImGui::EndTabItem();
            }
        }
        for (const Tab& t : tabs)
        {
            if (!shown(int(t.kind))) continue;
            char label[64];
            snprintf(label, sizeof label, "%s  %zu###%s", t.name, t.kind == K::Count ? m_catalog.Tree(K::Count).count : m_catalog.Count(t.kind), t.name);
            const bool bringForward = m_catalogShowTab == int(t.kind);
            if (ImGui::BeginTabItem(label, nullptr, bringForward ? ImGuiTabItemFlags_SetSelected : 0))
            {
                if (bringForward) m_catalogShowTab.reset();
                if (m_catalogTab != int(t.kind)) { m_catalogTab = int(t.kind); m_catalogFolder.clear(); }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    if (m_catalogTab == kBlueprintTab) { DrawBlueprints(); ImGui::End(); return; }
    if (m_catalogTab == kDifferencesTab) { DrawDifferences(); ImGui::End(); return; }
    if (m_catalogTab == kPortalTab) { DrawPortalCatalog(); ImGui::End(); return; }
    if (m_catalogTab == kCreatureTab || m_catalogTab == kGameobjectTab)
    {
        DrawUnitCatalog(m_catalogTab == kCreatureTab ? SpawnKind::Creature : SpawnKind::GameObject);
        ImGui::End();
        return;
    }
    const K kind = K(m_catalogTab);
    const bool models = kind == K::Doodad || kind == K::Wmo;

    // Toolbar: search, what is used nearby, thumbnail size; placement options while a model is armed.
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##search", "Search: words in the path, e.g. elwynn tree", m_catalogQuery, sizeof m_catalogQuery);
    ImGui::SameLine();
    ImGui::Checkbox("Used nearby", &m_catalogNearby);
    ImGui::SetItemTooltip("Only what the loaded tiles use; the count shows on each item");
    if (kind != K::Count)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("##size", &m_thumbSize, 48, 192, "size %.0f");
    }
    if (m_armed)
    {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(kAccent, "Placing %s", FileOf(m_armed->path).c_str());
        ImGui::SameLine();
        ImGui::Checkbox("Random turn", &m_placeRandomYaw);
        if (!m_armed->wmo)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderFloat("##scale", &m_placeScale, 0.1f, 10.0f, "scale %.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderFloat("##jitter", &m_placeScaleJitter, 0.0f, 0.9f, "+/- %.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Random scale variation per placement, as a fraction");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop  Esc")) m_armed.reset();
    }

    // Usage on the loaded tiles, refreshed when tiles stream or changes land.
    const size_t stamp = m_terrain.Tiles().size() * 1000003u + m_store.Done().size() * 7919u + m_terrain.Tiles().empty();
    if (stamp != m_usageStamp)
    {
        m_usageStamp = stamp;
        m_usage.clear();
        for (const auto& [key, tile] : m_terrain.Tiles())
        {
            for (const auto& d : tile.adt.doodads) ++m_usage[Catalog::Normalize(d.model)];
            for (const auto& w : tile.adt.wmos) ++m_usage[Catalog::Normalize(w.model)];
            for (const auto& t : tile.adt.textures) ++m_usage[Catalog::Normalize(t)];
        }
        m_catalogKey.clear();
    }
    const std::string key = std::to_string(m_catalogTab) + "|" + m_catalogFolder + "|" + m_catalogQuery + "|" + (m_catalogNearby ? "1" : "0");
    if (key != m_catalogKey)
    {
        m_catalogKey = key;
        m_catalogItems = m_catalog.Filter(kind, m_catalogFolder, m_catalogQuery);
        if (m_catalogNearby) std::erase_if(m_catalogItems, [&](const Catalog::Item* i) { return !m_usage.count(i->lower); });
    }

    // Left: folders of this tab. Right: the items.
    const float treeWidth = 240;
    if (ImGui::BeginChild("##folders", { treeWidth, 0 }, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
    {
        const Catalog::Folder& root = m_catalog.Tree(kind);
        if (ImGui::Selectable(("Everything  (" + std::to_string(root.count) + ")").c_str(), m_catalogFolder.empty())) m_catalogFolder.clear();
        std::function<void(const Catalog::Folder&)> node = [&](const Catalog::Folder& f) {
            for (const auto& [lower, child] : f.children)
            {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (child.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
                if (m_catalogFolder == child.prefix) flags |= ImGuiTreeNodeFlags_Selected;
                const bool open = ImGui::TreeNodeEx(child.prefix.c_str(), flags, "%s  (%zu)", child.name.c_str(), child.count);
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_catalogFolder = child.prefix;
                if (open) { node(child); ImGui::TreePop(); }
            }
        };
        node(root);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    auto activate = [&](const Catalog::Item& item) {
        if ((item.kind == K::Doodad || item.kind == K::Wmo) && m_armed && m_armed->path == item.path)
            m_armed.reset();   // clicking the model being placed again stops placing it
        else if (item.kind == K::Doodad || item.kind == K::Wmo)
        {
            m_armed = Armed{ item.path, item.kind == K::Wmo };
            m_tool = Tool::Objects;
            if (m_placeRandomYaw) m_placeYaw = RandomUnit() * 360.0f;
        }
        else if (item.kind == K::GroundTexture)
        {
            PickTexture(item.path);
            if (m_tool != Tool::Roads) m_tool = Tool::Paint;   // Roads takes it from there for its centre or shoulder
        }
        else
        {
            ImGui::SetClipboardText(item.path.c_str());
            Log("Copied the path %s", item.path.c_str());
        }
    };
    auto tooltip = [&](const Catalog::Item& item) {
        if (!ImGui::BeginItemTooltip()) return;
        ImGui::TextUnformatted(item.path.c_str());
        const auto use = m_usage.find(item.lower);
        ImGui::TextColored(kQuiet, "%s   used %d time(s) on the loaded tiles", m_mpq.Names()[item.archive].c_str(), use == m_usage.end() ? 0 : use->second);
        ImGui::TextColored(kQuiet, "%s", item.kind == K::Doodad || item.kind == K::Wmo ? "Click to place it (Objects tool)"
                                         : item.kind == K::GroundTexture ? "Click to pick it for painting" : "Click to copy the path");
        ImGui::EndTooltip();
    };

    if (ImGui::BeginChild("##items", { 0, 0 }, ImGuiChildFlags_Borders))
    {
        if (m_catalogItems.empty()) ImGui::TextColored(kQuiet, "Nothing matches.");
        if (kind == K::Count)
        {
            // All files: a list.
            if (ImGui::BeginTable("##files", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn("Archive", ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableHeadersRow();
                static const char* kKinds[] = { "Doodad", "Building", "Ground texture", "Texture", "" };
                ImGuiListClipper clip;
                clip.Begin(int(m_catalogItems.size()));
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
                    {
                        const Catalog::Item& item = *m_catalogItems[size_t(i)];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        const bool armed = m_armed && m_armed->path == item.path;
                        if (ImGui::Selectable((item.path + "##" + std::to_string(i)).c_str(), armed, ImGuiSelectableFlags_SpanAllColumns)) activate(item);
                        tooltip(item);
                        ImGui::TableNextColumn();
                        ImGui::TextColored(kQuiet, "%s", kKinds[size_t(item.kind)]);
                        ImGui::TableNextColumn();
                        ImGui::TextColored(kQuiet, "%s", m_mpq.Names()[item.archive].c_str());
                    }
                ImGui::EndTable();
            }
        }
        else
        {
            // A grid of thumbnails, made a few per frame (8 ms budget) so scrolling stays smooth.
            const auto start = std::chrono::steady_clock::now();
            auto budgetLeft = [&] { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count() < 8.0f; };
            const ImGuiStyle& style = ImGui::GetStyle();
            const float thumb = m_thumbSize, cellW = thumb + style.FramePadding.x * 2 + style.ItemSpacing.x;
            const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cellW));
            const int rows = int((m_catalogItems.size() + cols - 1) / cols);
            ImGuiListClipper clip;
            clip.Begin(rows, thumb + style.FramePadding.y * 2 + ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y);
            while (clip.Step())
                for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
                    for (int col = 0; col < cols; ++col)
                    {
                        const size_t i = size_t(row) * cols + col;
                        if (i >= m_catalogItems.size()) break;
                        const Catalog::Item& item = *m_catalogItems[i];
                        ID3D11ShaderResourceView* tex = nullptr;
                        if (models) tex = m_models.Thumbnail(item.path, item.kind == K::Wmo, m_mpq, budgetLeft());
                        else
                        {
                            tex = m_renderer.CachedTexture(item.path);
                            if (!tex && budgetLeft()) tex = m_renderer.TextureFor(item.path, m_mpq);
                        }
                        ImGui::PushID(int(i));
                        ImGui::BeginGroup();
                        const bool picked = (m_armed && m_armed->path == item.path) || (item.kind == K::GroundTexture && m_activeTexture == item.path);
                        if (picked) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1));
                        const bool clicked = tex ? ImGui::ImageButton("##thumb", ImTextureID(intptr_t(tex)), { thumb, thumb })
                                                 : ImGui::Button(models && m_models.ThumbnailFailed(item.path) ? "cannot load" : "...",
                                                                 { thumb + style.FramePadding.x * 2, thumb + style.FramePadding.y * 2 });
                        if (picked) ImGui::PopStyleColor();
                        tooltip(item);
                        if (clicked) activate(item);
                        std::string name = FileOf(item.path);
                        if (const auto use = m_usage.find(item.lower); use != m_usage.end()) name = std::to_string(use->second) + "x " + name;
                        ImGui::TextUnformatted(Fit(name, thumb + style.FramePadding.x * 2).c_str());
                        ImGui::EndGroup();
                        ImGui::PopID();
                        if (col + 1 < cols) ImGui::SameLine();
                    }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

const std::vector<SpawnAdapter::Template>& App::UnitTemplates(SpawnKind kind)
{
    const int k = kind == SpawnKind::Creature ? 0 : 1;
    SpawnAdapter& adapter = kind == SpawnKind::Creature ? m_creatures : m_gameobjects;
    if (!m_unitTemplatesRead[k] && adapter.Connected())
    {
        m_unitTemplatesRead[k] = true;
        const char* table = kind == SpawnKind::Creature ? "creature_template" : "gameobject_template";
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        m_unitTemplates[k] = adapter.All(error);
        if (!error.empty()) Log("%s: %s", table, error.c_str());
        else Log("Catalog: %zu %s rows in %.0f ms.", m_unitTemplates[k].size(), table,
                 std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return m_unitTemplates[k];
}

void App::DrawUnitCatalog(SpawnKind kind)
{
    const int k = kind == SpawnKind::Creature ? 0 : 1;
    SpawnAdapter& adapter = kind == SpawnKind::Creature ? m_creatures : m_gameobjects;
    const char* table = kind == SpawnKind::Creature ? "creature_template" : "gameobject_template";
    if (!adapter.Connected())
    {
        ImGui::TextColored(kQuiet, "Connect the world database (File > Server setup) to list %s.", table);
        return;
    }
    UnitTemplates(kind);
    // Folders: creature types, gameobject types.
    auto categoryName = [&](uint32_t c) -> std::string {
        if (kind == SpawnKind::GameObject) return GameObjectTypeName(c);
        const char* name = CreatureTypeName(c);
        return name ? name : "Type " + std::to_string(c);
    };
    std::map<uint32_t, size_t> counts;
    for (const auto& t : m_unitTemplates[k]) ++counts[t.category];
    std::map<uint32_t, int> nearby;   // entry -> spawns around the camera
    for (const Spawn& s : m_spawnView)
        if (s.kind == kind) ++nearby[s.entry];

    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##unitsearch", "Search: name or entry", m_catalogQuery, sizeof m_catalogQuery);
    ImGui::SameLine();
    ImGui::Checkbox("Used nearby", &m_catalogNearby);
    ImGui::SetItemTooltip("Only templates spawned around the camera; the count shows on each");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##size", &m_thumbSize, 48, 192, "size %.0f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) { m_unitTemplatesRead[k] = false; return; }
    ImGui::SetItemTooltip("Read %s again (after editing it elsewhere)", table);
    if (m_spawnArmed && m_spawnKind == kind)
    {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(kAccent, "Placing %s", m_spawnArmed->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop  Esc")) m_spawnArmed.reset();
    }

    if (ImGui::BeginChild("##unitfolders", { 240, 0 }, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
    {
        if (ImGui::Selectable(("Everything  (" + std::to_string(m_unitTemplates[k].size()) + ")").c_str(), m_unitCategory[k] == ~0u)) m_unitCategory[k] = ~0u;
        for (const auto& [c, n] : counts)
            if (ImGui::Selectable((categoryName(c) + "  (" + std::to_string(n) + ")##" + std::to_string(c)).c_str(), m_unitCategory[k] == c))
                m_unitCategory[k] = c;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    std::string query = m_catalogQuery;
    std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<const SpawnAdapter::Template*> items;
    for (const auto& t : m_unitTemplates[k])
    {
        if (m_unitCategory[k] != ~0u && t.category != m_unitCategory[k]) continue;
        if (m_catalogNearby && !nearby.count(t.entry)) continue;
        if (!query.empty())
        {
            std::string name = t.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (name.find(query) == std::string::npos && std::to_string(t.entry) != query) continue;
        }
        items.push_back(&t);
    }

    if (ImGui::BeginChild("##unititems", { 0, 0 }, ImGuiChildFlags_Borders))
    {
        if (items.empty()) ImGui::TextColored(kQuiet, "Nothing matches.");
        const auto start = std::chrono::steady_clock::now();
        auto budgetLeft = [&] { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count() < 8.0f; };
        const ImGuiStyle& style = ImGui::GetStyle();
        const float thumb = m_thumbSize, cellW = thumb + style.FramePadding.x * 2 + style.ItemSpacing.x;
        const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cellW));
        const int rows = int((items.size() + cols - 1) / cols);
        ImGuiListClipper clip;
        clip.Begin(rows, thumb + style.FramePadding.y * 2 + ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y);
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
                for (int col = 0; col < cols; ++col)
                {
                    const size_t i = size_t(row) * cols + col;
                    if (i >= items.size()) break;
                    const SpawnAdapter::Template& t = *items[i];
                    // The model as it spawns: skin, hair, armour and weapons, scaled like the spawn, seen from the front.
                    const std::string thumbKey = (kind == SpawnKind::Creature ? "c" : "g") + std::to_string(t.entry);
                    ID3D11ShaderResourceView* tex = m_models.LookThumbnail(thumbKey, {}, m_mpq, false);
                    if (!tex && !m_models.ThumbnailFailed("look:" + thumbKey) && budgetLeft())
                    {
                        Spawn s;
                        s.kind = kind;
                        s.entry = t.entry;
                        s.displayId = t.displayId;
                        s.size = t.size;
                        std::copy(std::begin(t.weapons), std::end(t.weapons), s.weapons);
                        std::copy(std::begin(t.weaponTypes), std::end(t.weaponTypes), s.weaponTypes);
                        std::vector<std::pair<ModelLook, XMFLOAT4X4>> parts;
                        if (const auto model = m_looks.SpawnLook(s))
                        {
                            const XMMATRIX world = XMMatrixScaling(model->scale, model->scale, model->scale);
                            XMFLOAT4X4 m;
                            XMStoreFloat4x4(&m, world);
                            parts.push_back({ model->look, m });
                            for (const auto& item : model->items)
                                if (const auto at = m_models.AttachmentMatrix(model->look.model, item.attachment, m_mpq))
                                {
                                    XMStoreFloat4x4(&m, XMLoadFloat4x4(&*at) * world);
                                    parts.push_back({ item.look, m });
                                }
                        }
                        tex = m_models.LookThumbnail(thumbKey, parts, m_mpq, true);
                    }
                    ImGui::PushID(int(i));
                    ImGui::BeginGroup();
                    const bool picked = m_spawnArmed && m_spawnKind == kind && m_spawnArmed->entry == t.entry;
                    if (picked) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1));
                    const bool clicked = tex ? ImGui::ImageButton("##thumb", ImTextureID(intptr_t(tex)), { thumb, thumb })
                                             : ImGui::Button(m_models.ThumbnailFailed("look:" + thumbKey) ? "no model" : "...",
                                                             { thumb + style.FramePadding.x * 2, thumb + style.FramePadding.y * 2 });
                    if (picked) ImGui::PopStyleColor();
                    if (kind == SpawnKind::Creature && ImGui::IsItemClicked(ImGuiMouseButton_Right)) OpenNpc(t.entry);
                    if (ImGui::BeginItemTooltip())
                    {
                        ImGui::Text("%s  #%u", t.name.c_str(), t.entry);
                        ImGui::TextColored(kQuiet, "%s   %s   display %u", t.detail.c_str(), categoryName(t.category).c_str(), t.displayId);
                        if (const auto n = nearby.find(t.entry); n != nearby.end()) ImGui::TextColored(kQuiet, "%d spawned around the camera", n->second);
                        ImGui::TextColored(kQuiet, kind == SpawnKind::Creature ? "Click to place it, right-click for the NPC viewer" : "Click to place it");
                        ImGui::EndTooltip();
                    }
                    if (clicked && picked) m_spawnArmed.reset();   // clicking the one being placed again stops placing it
                    else if (clicked)
                    {
                        m_spawnKind = kind;
                        m_tool = kind == SpawnKind::Creature ? Tool::Creatures : Tool::Gameobjects;
                        m_spawnArmed = t;
                    }
                    std::string name = t.name;
                    if (const auto n = nearby.find(t.entry); n != nearby.end()) name = std::to_string(n->second) + "x " + name;
                    ImGui::TextUnformatted(Fit(name, thumb + style.FramePadding.x * 2).c_str());
                    ImGui::EndGroup();
                    ImGui::PopID();
                    if (col + 1 < cols) ImGui::SameLine();
                }
    }
    ImGui::EndChild();
}

void App::UpdateFallbacks()
{
    std::vector<const MpqChain*> chains;
    for (size_t i = 1; i < m_ghosts.Sources().size(); ++i) chains.push_back(m_ghosts.Sources()[i].mpq);
    m_diffs.Cancel();  // so is the differences scan's (its results stay; Rescan finishes it from the saved ones)
    if (m_diffTarget.source) { m_diffs.Clear(); m_diffTarget = {}; }   // its source may be the one removed
    m_loader.Stop();   // the fallback list is read by the loader's thread: change it while that is idle
    m_mpq.SetFallbacks(std::move(chains));
    if (m_project) m_loader.Start(&m_mpq);
    m_renderer.ForgetMissingTextures();
    m_models.ForgetFailed();
}

void App::PickTexture(const std::string& path)
{
    m_activeTexture = path;
    std::erase(m_recentTextures, path);
    m_recentTextures.insert(m_recentTextures.begin(), path);
    if (m_recentTextures.size() > 12) m_recentTextures.resize(12);
    Log("Painting with %s.", FileOf(path).c_str());
}

void App::OpenSaveBlueprint()
{
    if (m_selection.empty() || !m_project) { Log("Select the chunks to keep first (Select or Copy tool)."); return; }
    m_saveBlueprintOpen = true;
    m_blueprintName[0] = 0;
    m_blueprintNotes[0] = 0;
}

void App::DrawSaveBlueprintModal()
{
    if (m_saveBlueprintOpen) { ImGui::OpenPopup("Save blueprint"); m_saveBlueprintOpen = false; }
    ImGui::SetNextWindowSize({ 460, 0 }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Save blueprint", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const Ghosts::Layer* from = m_copyLayer ? m_ghosts.Find(m_copyLayer) : nullptr;
    ImGui::TextColored(kQuiet, "%zu chunk(s) from %s%s", m_selection.size(), m_terrain.Map().c_str(), from ? (" (ghost: " + from->label + ")").c_str() : "");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("Name", "e.g. Ruined watchtower, Elwynn", m_blueprintName, sizeof m_blueprintName);
    ImGui::InputTextMultiline("Notes", m_blueprintNotes, sizeof m_blueprintNotes, { 0, 60 });
    const bool ok = m_blueprintName[0] != 0;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Save", { 120, 0 }) || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
    {
        std::set<std::pair<int, int>> cells;
        for (ChunkRef r : m_selection) cells.insert(m_terrain.GridOf(r));
        Blueprint b;
        b.name = m_blueprintName;
        b.notes = m_blueprintNotes;
        b.map = m_terrain.Map() + (from ? " / " + from->label : "");
        b.clip = from ? TerrainAdapter::CopyFrom(from->tiles, cells) : m_terrain.Copy(m_selection);
        if (from)
            b.clip.pois = VersionPois(m_ghosts.Chain(from->source), from->map.empty() ? m_terrain.Map() : from->map, cells, b.clip.originX, b.clip.originZ);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &local);
        b.created = when;
        b.thumbSize = 128;
        b.thumb = RenderAreaThumbnail(b.clip, b.thumbSize);
        std::string error;
        if (b.clip.Empty()) Log("Nothing to keep: no loaded chunks under the selection.");
        else if (!b.Save(m_project->BlueprintsDir(), error)) Log("%s", error.c_str());
        else
        {
            Log("Blueprint '%s' saved: %zu chunks, %zu objects (%s).", b.name.c_str(), b.clip.chunks.size(), b.clip.doodads.size() + b.clip.wmos.size(),
                b.file.filename().string().c_str());
            m_blueprints.push_back(std::move(b));
            std::sort(m_blueprints.begin(), m_blueprints.end(), [](const Blueprint& x, const Blueprint& y) { return x.name < y.name; });
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

std::vector<uint8_t> App::RenderAreaThumbnail(const TerrainClipboard& clip, UINT size)
{
    std::vector<uint8_t> rgba;
    if (clip.Empty()) return rgba;
    // The area as a private layer, drawn alone from straight above with an orthographic camera.
    constexpr int kLayer = 9999, kKey = -9;
    const Adt adt = clip.ToAdt();
    m_renderer.LoadTile(kKey, adt, m_mpq, kLayer);
    m_models.AddTile(kKey, adt, m_mpq, kLayer);
    float top = -1e9f;
    for (const auto& e : clip.chunks) top = std::max(top, *std::max_element(e.heights.begin(), e.heights.end()));
    const float spanX = clip.Width() * kChunkSize, spanZ = clip.Depth() * kChunkSize, span = std::max(spanX, spanZ);
    const float cx = clip.originX * kChunkSize + spanX / 2, cz = clip.originZ * kChunkSize + spanZ / 2;
    rgba = RenderOrtho(cx - span / 2, cz - span / 2, span, span, top, size, size, kLayer);
    m_renderer.UnloadTile(kKey);
    m_models.RemoveTile(kKey);
    return rgba;
}

void App::RenderMinimaps()
{
    if (!m_project || m_terrain.Map().empty()) return;
    const std::string& map = m_terrain.Map();
    const fs::path dir = m_project->dir / "minimaps";
    std::error_code ec;
    fs::create_directories(dir, ec);
    nlohmann::json index = nlohmann::json::object();   // tile -> edit hash its picture shows
    if (std::ifstream f(dir / "index.json"); f)
        try { index = nlohmann::json::parse(f); } catch (const std::exception&) {}
    ClearPlacementView();   // the terrain as it is, not a pinned paste
    DrawOptions look = m_drawOptions;
    ModelRenderer::DrawSettings models = m_modelSettings;
    look.solo = models.layer = 0;
    MinimapLook(look, models);
    for (const auto& l : m_ghosts.Layers()) m_renderer.SetLayerStyle(l.id, l.tint, false);
    size_t rendered = 0;
    for (const auto& [key, hash] : TerrainAdapter::EditHashes(m_store.Done(), map))
    {
        const int x = key % 64, y = key / 64;
        const std::string name = map + "_" + std::to_string(x) + "_" + std::to_string(y);
        if (index.value(name, size_t(0)) == hash && fs::exists(dir / (name + ".blp"), ec)) continue;
        std::string error;
        if (!m_terrain.LoadNow(x, y, error)) continue;   // a tile taken away again: no picture
        const LoadedTile& tile = m_terrain.Tiles().at(key);
        if (!m_models.HasTile(key)) m_models.AddTile(key, tile.adt, m_mpq);
        const std::vector<uint8_t> rgba = MinimapFromTopDown(RenderTopDown(m_device, m_context, m_renderer, m_models, look, models, x * kTileSize,
                                                                           y * kTileSize, kTileSize, kTileSize, tile.maxHeight + 50.0f,
                                                                           kMinimapSize, kMinimapSize, true),
                                                             kMinimapSize);
        if (rgba.empty()) continue;
        const std::vector<uint8_t> blp = WriteBlp(kMinimapSize, kMinimapSize, rgba.data());
        std::ofstream(dir / (name + ".blp"), std::ios::binary).write(reinterpret_cast<const char*>(blp.data()), std::streamsize(blp.size()));
        index[name] = hash;
        ++rendered;
    }
    std::ofstream(dir / "index.json") << index.dump(1);
    if (rendered) Log("Minimaps: %zu edited tile(s) of %s drawn again.", rendered, map.c_str());
    // Tiles edited on other maps keep their last picture (or the client's) until their map is open at an export.
}

std::vector<uint8_t> App::RenderOrtho(float x0, float z0, float spanX, float spanZ, float top, UINT width, UINT height, int layer)
{
    // Only `layer`: ghost layers are hidden for this draw (the frame sets their styles again).
    if (layer == 0)
        for (const auto& l : m_ghosts.Layers()) m_renderer.SetLayerStyle(l.id, l.tint, false);
    DrawOptions options = m_drawOptions;
    options.solo = layer;
    ModelRenderer::DrawSettings models = m_modelSettings;
    models.layer = layer;
    return RenderTopDown(m_device, m_context, m_renderer, m_models, options, models, x0, z0, spanX, spanZ, top, width, height, true);
}

void App::UseBlueprint(const Blueprint& b, bool inPlace)
{
    m_clipboard = b.clip;
    ++m_clipVersion;
    m_pasteOffset = 0;
    m_pin.reset();
    if (m_heightBeforeInPlace) { m_pasteHeightMode = *m_heightBeforeInPlace; m_heightBeforeInPlace.reset(); }
    m_tool = Tool::Copy;
    m_placing = true;
    if (inPlace) PasteInPlace();
    Log("Blueprint '%s': %s", b.name.c_str(), inPlace ? "pinned where it was taken from; Enter commits." : "follows the cursor; click to pin, Enter commits.");
}

void App::DrawBlueprints()
{
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##search", "Search names and notes", m_catalogQuery, sizeof m_catalogQuery);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##size", &m_thumbSize, 48, 192, "size %.0f");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selection.empty());
    if (ImGui::SmallButton("Save selection...  Ctrl+B")) OpenSaveBlueprint();
    ImGui::EndDisabled();
    if (m_blueprints.empty())
    {
        ImGui::TextColored(kQuiet, "No blueprints yet. Select chunks on the map, then Save selection (Ctrl+B).");
        return;
    }

    std::string query = m_catalogQuery;
    std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<size_t> shown;
    for (size_t i = 0; i < m_blueprints.size(); ++i)
    {
        std::string hay = m_blueprints[i].name + " " + m_blueprints[i].notes + " " + m_blueprints[i].map;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (query.empty() || hay.find(query) != std::string::npos) shown.push_back(i);
    }

    if (ImGui::BeginChild("##blueprints", { 0, 0 }, ImGuiChildFlags_Borders))
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float thumb = m_thumbSize, cellW = thumb + style.FramePadding.x * 2 + style.ItemSpacing.x;
        const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cellW));
        for (size_t n = 0; n < shown.size(); ++n)
        {
            const Blueprint& b = m_blueprints[shown[n]];
            auto& srv = m_blueprintThumbs[b.file.string()];
            if (!srv && b.thumbSize) srv = m_renderer.CreateRgbaTexture(b.thumbSize, b.thumbSize, b.thumb);
            ImGui::PushID(int(shown[n]));
            ImGui::BeginGroup();
            const bool clicked = srv ? ImGui::ImageButton("##thumb", ImTextureID(intptr_t(srv.Get())), { thumb, thumb })
                                     : ImGui::Button("no picture", { thumb + style.FramePadding.x * 2, thumb + style.FramePadding.y * 2 });
            if (clicked) UseBlueprint(b, false);
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextColored(kAccent, "%s", b.name.c_str());
                ImGui::Text("%d x %d chunks (%.0f x %.0f yd), %zu doodads, %zu WMOs", b.clip.Width(), b.clip.Depth(), b.clip.Width() * kChunkSize,
                            b.clip.Depth() * kChunkSize, b.clip.doodads.size(), b.clip.wmos.size());
                ImGui::TextColored(kQuiet, "From %s, %s", b.map.c_str(), b.created.c_str());
                if (!b.notes.empty()) ImGui::TextWrapped("%s", b.notes.c_str());
                ImGui::TextColored(kQuiet, "Click: place it (follows the cursor)   Right-click: more");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("##menu"))
            {
                if (ImGui::MenuItem("Place (follows the cursor)")) UseBlueprint(b, false);
                if (ImGui::MenuItem("Place where it was taken from")) UseBlueprint(b, true);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete...")) m_blueprintToDelete = shown[n];
                ImGui::EndPopup();
            }
            ImGui::TextUnformatted(Fit(b.name, thumb + style.FramePadding.x * 2).c_str());
            ImGui::EndGroup();
            ImGui::PopID();
            if ((n + 1) % size_t(cols) != 0) ImGui::SameLine();
        }
    }
    ImGui::EndChild();

    // Deleting removes the file; it is asked first.
    if (m_blueprintToDelete) ImGui::OpenPopup("Delete blueprint?");
    if (ImGui::BeginPopupModal("Delete blueprint?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const size_t i = m_blueprintToDelete.value_or(0);
        if (i < m_blueprints.size())
        {
            ImGui::Text("Delete '%s'? Its file is removed:", m_blueprints[i].name.c_str());
            ImGui::TextColored(kQuiet, "%s", m_blueprints[i].file.string().c_str());
            if (ImGui::Button("Delete", { 120, 0 }))
            {
                std::error_code ec;
                std::filesystem::remove(m_blueprints[i].file, ec);
                Log("Blueprint '%s' deleted.", m_blueprints[i].name.c_str());
                m_blueprintThumbs.erase(m_blueprints[i].file.string());
                m_blueprints.erase(m_blueprints.begin() + std::ptrdiff_t(i));
                m_blueprintToDelete.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Keep", { 120, 0 })) { m_blueprintToDelete.reset(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void App::SetSolo(int layer)
{
    if (m_soloLayer)
        if (const Ghosts::Layer* old = m_ghosts.Find(m_soloLayer))
            for (const auto& [key, tile] : old->tiles) m_models.RemoveTile(Ghosts::Key(old->id, key));
    m_soloLayer = layer;
    if (const Ghosts::Layer* now = layer ? m_ghosts.Find(layer) : nullptr)
        for (const auto& [key, tile] : now->tiles) m_models.AddTile(Ghosts::Key(now->id, key), tile.adt, m_ghosts.Chain(now->source), now->id);
}

void App::RemoveGhostLayer(int layer)
{
    if (m_soloLayer == layer) SetSolo(0);
    if (m_copyLayer == layer) m_copyLayer = 0;
    m_renderer.UnloadLayer(layer);
    m_ghosts.RemoveLayer(layer);
}

const AdtChunk* App::ShownChunk(ChunkRef ref) const
{
    const AdtChunk* c = m_terrain.Chunk(ref);
    const Ghosts::Layer* solo = m_soloLayer ? const_cast<Ghosts&>(m_ghosts).Find(m_soloLayer) : nullptr;
    if (!c || !solo) return c;
    const auto [gx, gz] = m_terrain.GridOf(ref);
    auto it = solo->tiles.find(TileKey(gx / 16, gz / 16));
    if (it == solo->tiles.end()) return nullptr;
    const int16_t i = it->second.byGrid[size_t((gz % 16) * 16 + gx % 16)];
    return i < 0 ? nullptr : &it->second.adt.chunks[size_t(i)];
}

void App::PasteInPlace()
{
    if (m_clipboard.Empty() || m_terrain.Stroking()) return;
    m_tool = Tool::Copy;
    m_placing = true;
    if (!m_heightBeforeInPlace) m_heightBeforeInPlace = m_pasteHeightMode;
    m_pasteHeightMode = PasteHeight::Absolute;
    m_pasteOffset = 0;
    m_pin = std::pair{ m_clipboard.originX + (m_clipboard.Width() - 1) / 2, m_clipboard.originZ + (m_clipboard.Depth() - 1) / 2 };
    Log("Pasting in place with the original heights (height mode: Absolute). Enter commits, Esc cancels.");
}

void App::DrawVersions()
{
    if (!ImGui::Begin("Versions")) { ImGui::End(); return; }
    if (!m_project) { ImGui::TextColored(kQuiet, "Open a project first."); ImGui::End(); return; }
    DrawCompare();
    if (ImGui::Button("Find differences with another version..."))
    {
        SetGroup(Group::Terrain);
        m_catalogShowTab = kDifferencesTab;
    }
    ImGui::SetItemTooltip("Scans a whole other version of this map and lists every edited area as a card (Catalog > Differences)");

    // Sources: the project's base files plus other versions to compare against (edited in the Sources window).
    ImGui::SeparatorText("Sources");
    for (size_t i = 0; i < m_ghosts.Sources().size(); ++i)
    {
        const auto& s = m_ghosts.Sources()[i];
        ImGui::TextUnformatted(i == 0 ? (s.name + " (project base)").c_str() : s.name.c_str());
        ImGui::SetItemTooltip("%zu archive(s) and unpacked folder(s)", s.mpq->Names().size());
    }
    if (ImGui::Button("Sources...")) m_showSources = true;
    ImGui::SetItemTooltip("Folders of MPQs, single MPQs and unpacked folders: the project's base files and versions to compare against");

    if (m_terrain.Map().empty()) { ImGui::TextColored(kQuiet, "Open a map to see its versions."); ImGui::End(); return; }

    // Versions of the tile under the camera: every archive that holds it, identical copies merged.
    const int tx = int(std::floor(m_camera.pos.x / kTileSize)), ty = int(std::floor(m_camera.pos.z / kTileSize));
    const std::string key = m_terrain.Map() + " " + std::to_string(tx) + "_" + std::to_string(ty) + " " + std::to_string(m_ghosts.Sources().size());
    if (key != m_versionsKey)
    {
        m_versionsKey = key;
        const auto main = m_terrain.Tiles().find(TileKey(tx, ty));
        m_versions = m_ghosts.Versions(m_terrain.Map(), tx, ty, main == m_terrain.Tiles().end() ? nullptr : &main->second);
    }
    ImGui::SeparatorText(("Tile " + std::to_string(tx) + "_" + std::to_string(ty) + " (under the camera)").c_str());
    auto toggle = [&](size_t source, int archive, const std::string& label) {
        Ghosts::Layer* layer = m_ghosts.Find(source, archive);
        bool on = layer != nullptr;
        if (ImGui::Checkbox(("##ghost" + label).c_str(), &on))
        {
            if (on) { Ghosts::Layer& l = m_ghosts.AddLayer(source, archive, label); Log("Ghost layer %s added.", l.label.c_str()); }
            else RemoveGhostLayer(layer->id);
        }
        ImGui::SetItemTooltip("Show this version as a ghost layer");
    };
    if (ImGui::BeginTable("##versions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH))
    {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Height diff", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        // Whole other clients, resolved like their own client would.
        for (size_t s = 1; s < m_ghosts.Sources().size(); ++s)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = m_ghosts.Sources()[s].name + " (whole client)";
            toggle(s, -1, label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label.c_str());
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        }
        for (const Ghosts::Version& v : m_versions)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            toggle(v.source, v.archive, v.label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.label.c_str());
            if (v.inUse) { ImGui::SameLine(); ImGui::TextColored(kAccent, "in use"); }
            if (!v.alsoIn.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored(kQuiet, "+%zu", v.alsoIn.size());
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::TextUnformatted("Identical copies in:");
                    for (const auto& a : v.alsoIn) ImGui::BulletText("%s", a.c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::TableNextColumn();
            if (v.heightDiff) ImGui::Text("%.2f yd", *v.heightDiff); else ImGui::TextColored(kQuiet, "-");
            ImGui::TableNextColumn();
            ImGui::Text("%zu", v.doodads + v.wmos);
            ImGui::SetItemTooltip("%zu doodads, %zu WMOs", v.doodads, v.wmos);
        }
        ImGui::EndTable();
    }
    if (m_versions.empty()) ImGui::TextColored(kQuiet, "No source has this tile.");

    // Any other map as a ghost of this one: the module copies (Azeroth_Epoch, ...) keep the original coordinates.
    ImGui::SeparatorText("Another map as ghost");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##ghostMap", "Pick a map to overlay...", ImGuiComboFlags_HeightLarge))
    {
        static char filter[64] = {};
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); filter[0] = 0; }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "Filter: name or folder", filter, sizeof filter);
        std::string f = filter;
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const MapEntry& m : m_maps)
        {
            if (m.directory == m_terrain.Map()) continue;
            std::string hay = m.name + " " + m.directory;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (!f.empty() && hay.find(f) == std::string::npos) continue;
            const std::string item = m.name + "  (" + m.directory + ", " + std::to_string(m.id) + ")";
            const bool shown = m_ghosts.Find(0, -1, m.directory) != nullptr;
            if (ImGui::Selectable(item.c_str(), shown) && !shown)
            {
                Ghosts::Layer& l = m_ghosts.AddLayer(0, -1, m.name + " (" + m.directory + ")", m.directory);
                Log("Ghost layer %s added over %s.", l.label.c_str(), m_terrain.Map().c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Shows that map's tiles at the same coordinates, e.g. Azeroth_Epoch over Azeroth");

    // Layers: colour, visibility, solo, and which one Ctrl+C copies from.
    ImGui::SeparatorText("Layers");
    if (ImGui::RadioButton("Copy from the map", m_copyLayer == 0)) m_copyLayer = 0;
    std::vector<int> remove;
    for (auto& l : m_ghosts.Layers())
    {
        ImGui::PushID(l.id);
        ImGui::ColorEdit4("##tint", &l.tint.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
        ImGui::SetItemTooltip("Tint and opacity");
        ImGui::SameLine();
        ImGui::Checkbox("##visible", &l.visible);
        ImGui::SetItemTooltip("Show or hide");
        ImGui::SameLine();
        ImGui::TextUnformatted(l.label.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, "%zu tiles", l.tiles.size());
        ImGui::Indent(56);
        if (ImGui::RadioButton("Copy from", m_copyLayer == l.id)) m_copyLayer = l.id;
        ImGui::SameLine();
        bool solo = m_soloLayer == l.id;
        if (ImGui::Checkbox("Solo", &solo)) SetSolo(solo ? l.id : 0);
        ImGui::SetItemTooltip("Show only this version, with its objects, as the client would");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove.push_back(l.id);
        ImGui::Unindent(56);
        ImGui::PopID();
    }
    for (int id : remove) RemoveGhostLayer(id);
    if (m_ghosts.Layers().empty()) ImGui::TextColored(kQuiet, "Tick a version above to show it as a ghost.");
    else
        ImGui::TextColored(kQuiet, "Select chunks, pick a layer to copy from, Ctrl+C.\nCtrl+V places it like any copy;\n"
                                   "Ctrl+Shift+V pastes it in place with its own heights.");
    ImGui::End();
}

XMFLOAT4X4 App::GizmoFrame() const
{
    XMFLOAT4X4 out;
    if (m_objSel.size() == 1)
    {
        const ObjectRef& r = *m_objSel.begin();
        if (r.wmo) { if (auto w = m_terrain.FindWmo(r.uid)) { XMStoreFloat4x4(&out, PlacementMatrix(w->pos, w->rot, 1.0f)); return out; } }
        else if (auto d = m_terrain.FindDoodad(r.uid)) { XMStoreFloat4x4(&out, PlacementMatrix(d->pos, d->rot, d->scale)); return out; }
    }
    XMFLOAT3 c{};
    size_t n = 0;
    for (const ObjectRef& r : m_objSel)
        if (auto p = ObjectPosition(r)) { c.x += p->x; c.y += p->y; c.z += p->z; ++n; }
    if (n) { c.x /= n; c.y /= n; c.z /= n; }
    XMStoreFloat4x4(&out, XMMatrixTranslation(c.x, c.y, c.z));
    return out;
}

std::optional<XMFLOAT3> App::ObjectPosition(const ObjectRef& ref) const
{
    if (ref.wmo) { if (auto w = m_terrain.FindWmo(ref.uid)) return XMFLOAT3{ w->pos[0], w->pos[1], w->pos[2] }; }
    else if (auto d = m_terrain.FindDoodad(ref.uid)) return XMFLOAT3{ d->pos[0], d->pos[1], d->pos[2] };
    return std::nullopt;
}

void App::PreviewObjects(const std::function<void(float*, float*, float*)>& fn)
{
    m_terrain.PreviewObjectEdit([&](DoodadPlacement& d) { fn(d.pos, d.rot, &d.scale); }, [&](WmoPlacement& w) { fn(w.pos, w.rot, nullptr); });
}

void App::EndObjectEdit(const std::string& label)
{
    if (auto change = m_terrain.EndObjectEdit(label))
    {
        Log("%s", change->label.c_str());
        m_store.Commit(std::move(*change));
    }
}

void App::EditObjects(const std::string& label, const std::function<void(float*, float*, float*)>& fn)
{
    if (m_objSel.empty()) return;
    m_terrain.BeginObjectEdit(m_objSel);
    PreviewObjects(fn);
    EndObjectEdit(label);
}

void App::DeleteSelectedObjects()
{
    if (auto change = m_terrain.DeleteObjects(m_objSel))
    {
        Log("%s", change->label.c_str());
        m_store.Commit(std::move(*change));
    }
    m_objSel.clear();
}

void App::DrawTransformBar(float pw)
{
    // The same bar in every tool that moves things: handle mode, world / local, snapping, and what this selection allows.
    const float x0 = ImGui::GetCursorPosX();
    Segmented("gizmo", m_gizmo, { { Gizmo::Move, "Move 1" }, { Gizmo::Rotate, "Rotate 2" }, { Gizmo::Scale, "Scale 3" } }, pw);
    Segmented("space", m_gizmoLocal, { { false, "World" }, { true, "Local  X" } }, pw);
    ImGui::SetItemTooltip("World: handles follow the map axes. Local: they follow the selection's own axes.");
    ImGui::Checkbox("Snap", &m_gizmoSnap);
    ImGui::SetItemTooltip("Hold Ctrl while dragging a handle to flip this");
    ImGui::SameLine();
    const float left = pw - (ImGui::GetCursorPosX() - x0);
    ImGui::SetNextItemWidth(left / 3 - 4);
    ImGui::SliderFloat("##snapMove", &m_snapMove, 0.1f, 100.0f, "%.1f yd", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(left / 3 - 4);
    ImGui::SliderFloat("##snapRotate", &m_snapRotate, 1.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(left / 3 - 4);
    ImGui::SliderFloat("##snapScale", &m_snapScale, 0.01f, 1.0f, "x%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    if (const auto t = ActiveTransform())
    {
        const bool blocked = (m_gizmo == Gizmo::Rotate && !t->rotate) || (m_gizmo == Gizmo::Scale && t->scale == Transformable::Scale::None);
        if (blocked) ImGui::TextColored(kWarn, "%s Pick Move (1).", t->limits.c_str());
        else if (!t->limits.empty()) ImGui::TextColored(kQuiet, "%s", t->limits.c_str());
        ImGui::TextColored(kQuiet, "PgUp/PgDn height (Shift: fine)  G ground  Alt+click: move here");
    }
    ImGui::Separator();
}

void App::DrawObjectPanel()
{
    if (m_tool != Tool::Objects)
    {
        ImGui::TextColored(kQuiet, "Pick the Objects tool (O) to select and edit objects.");
        if (ImGui::Button("Objects tool  O")) m_tool = Tool::Objects;
        return;
    }
    DrawTransformBar(ImGui::GetContentRegionAvail().x);

    if (m_objSel.empty())
    {
        if (!m_objHover) { ImGui::TextColored(kQuiet, "Click an object to select it."); return; }
        ImGui::TextColored(kQuiet, "Hovered");
    }
    if (m_objSel.size() > 1)
    {
        ImGui::TextColored(kAccent, "%zu objects selected", m_objSel.size());
        ImGui::Spacing();
        size_t shown = 0;
        for (const ObjectRef& r : m_objSel)
        {
            if (++shown > 12) { ImGui::TextColored(kQuiet, "..."); break; }
            const std::string name = r.wmo ? (m_terrain.FindWmo(r.uid) ? m_terrain.FindWmo(r.uid)->model : "?")
                                           : (m_terrain.FindDoodad(r.uid) ? m_terrain.FindDoodad(r.uid)->model : "?");
            ImGui::TextUnformatted(FileOf(name).c_str());
            ImGui::SetItemTooltip("%s  (id %u)", name.c_str(), r.uid);
        }
        if (ImGui::Button("Delete")) DeleteSelectedObjects();
        ImGui::SameLine();
        if (ImGui::Button("Clear selection")) m_objSel.clear();
        return;
    }

    const ObjectRef ref = m_objSel.empty() ? *m_objHover : *m_objSel.begin();
    std::string model;
    float pos[3], rot[3], scale = 1;
    if (ref.wmo)
    {
        const auto w = m_terrain.FindWmo(ref.uid);
        if (!w) { ImGui::TextColored(kQuiet, "Not loaded."); return; }
        model = w->model;
        std::copy(w->pos, w->pos + 3, pos);
        std::copy(w->rot, w->rot + 3, rot);
    }
    else
    {
        const auto d = m_terrain.FindDoodad(ref.uid);
        if (!d) { ImGui::TextColored(kQuiet, "Not loaded."); return; }
        model = d->model;
        std::copy(d->pos, d->pos + 3, pos);
        std::copy(d->rot, d->rot + 3, rot);
        scale = d->scale;
    }
    ImGui::TextColored(kAccent, "%s", FileOf(model).c_str());
    ImGui::SetItemTooltip("%s", model.c_str());
    ImGui::TextColored(kQuiet, "%s   id %u%s", ref.wmo ? "WMO (building)" : "M2 (doodad)", ref.uid, ref.uid >= 200'000'000 && ref.uid != 0xFFFFFFFFu ? "  (added in this project)" : "");
    if (m_objSel.empty()) return;   // hovered only: no editing
    ImGui::Spacing();

    // Typed or dragged values preview live and commit as one change when the field is let go.
    auto field = [&](bool changed, const std::function<void(float*, float*, float*)>& set) {
        if (changed)
        {
            if (!m_terrain.ObjectEditing()) m_terrain.BeginObjectEdit(m_objSel);
            PreviewObjects(set);
        }
        if (ImGui::IsItemDeactivated() && m_terrain.ObjectEditing()) EndObjectEdit("Edit " + FileOf(model));
    };
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w - 70);
    field(ImGui::DragFloat3("Position", pos, 0.1f, 0, 0, "%.2f"), [&](float* p, float*, float*) { std::copy(pos, pos + 3, p); });
    ImGui::SetNextItemWidth(w - 70);
    field(ImGui::SliderFloat3("Rotation", rot, -360, 360, "%.1f", ImGuiSliderFlags_AlwaysClamp), [&](float*, float* r, float*) { std::copy(rot, rot + 3, r); });
    ImGui::SetItemTooltip("Degrees as stored: tilt, turn (yaw), roll");
    if (!ref.wmo)
    {
        ImGui::SetNextItemWidth(w - 70);
        field(ImGui::SliderFloat("Scale", &scale, 1.0f / 1024.0f, 63.0f, "%.3f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp),
              [&](float*, float*, float* s) { if (s) *s = scale; });
    }
    // A WMO's doodad set (its furniture: set 0 always shows, the placement adds one more) and swapping the model.
    const auto& global = m_terrain.GlobalWmo();
    const bool isGlobal = ref.wmo && global && global->uniqueId == ref.uid;
    auto setWmo = [&](const std::string& label, const std::function<void(WmoPlacement&)>& fn) {
        m_terrain.BeginObjectEdit(m_objSel);
        m_terrain.PreviewObjectEdit([](DoodadPlacement&) {}, fn);
        EndObjectEdit(label);
    };
    if (ref.wmo)
        if (const auto placed = m_terrain.FindWmo(ref.uid))
            if (const auto info = m_models.Info(placed->model, m_mpq); info && info->doodadSets.size() > 1)
            {
                auto setName = [&](size_t i) { return std::to_string(i) + "  " + (info->doodadSets[i].empty() ? "(unnamed)" : info->doodadSets[i]); };
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
                if (ImGui::BeginCombo("Doodads", placed->doodadSet < info->doodadSets.size() ? setName(placed->doodadSet).c_str() : std::to_string(placed->doodadSet).c_str()))
                {
                    for (size_t i = 0; i < info->doodadSets.size(); ++i)
                        if (ImGui::Selectable(setName(i).c_str(), i == placed->doodadSet))
                            setWmo("Doodad set " + std::to_string(i) + " for " + FileOf(model), [i](WmoPlacement& p) { p.doodadSet = uint16_t(i); });
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("The furniture set shown with set 0 (which always shows)");
            }
    if (m_armed && m_armed->wmo == ref.wmo && _stricmp(m_armed->path.c_str(), model.c_str()) != 0)
    {
        if (ImGui::Button(("Replace with " + FileOf(m_armed->path)).c_str()))
        {
            const std::string path = m_armed->path, label = "Replace " + FileOf(model) + " with " + FileOf(path);
            if (ref.wmo) setWmo(label, [path](WmoPlacement& p) { p.model = path; p.doodadSet = 0; });
            else
            {
                m_terrain.BeginObjectEdit(m_objSel);
                m_terrain.PreviewObjectEdit([path](DoodadPlacement& d) { d.model = path; }, [](WmoPlacement&) {});
                EndObjectEdit(label);
            }
            m_armed.reset();
        }
        ImGui::SetItemTooltip("Same place and turn, the model picked in the Catalog");
    }
    else ImGui::TextColored(kQuiet, "To swap the model: pick one in the Catalog, then Replace here.");
    if (isGlobal) ImGui::TextColored(kQuiet, "This map's own WMO: kept in its WDT, exported with it.");
    ImGui::Spacing();
    if (ImGui::Button("Drop to ground"))
        EditObjects("Drop object to the ground", [&](float* p, float*, float*) { if (const auto h = m_terrain.HeightAt(p[0], p[2])) p[1] = *h; });
    ImGui::SameLine();
    ImGui::BeginDisabled(isGlobal);
    if (ImGui::Button("Delete")) DeleteSelectedObjects();
    ImGui::EndDisabled();
    if (isGlobal) ImGui::SetItemTooltip("A WMO-only map is its WMO: replace it instead");
}

void App::ClearPlacementView()
{
    if (m_planKey.empty()) return;
    m_terrain.PreviewPlan(nullptr);
    m_renderer.UnloadTile(-1);
    m_models.RemoveTile(-1);
    m_planKey.clear();
}

void App::UpdatePlacement()
{
    if (m_tool != Tool::Copy || !m_placing || m_clipboard.Empty() || m_boxing || (!m_pin && !m_hover)) { ClearPlacementView(); return; }
    const auto [gx, gz] = PasteAnchor();
    const float offset = PasteOffsetAt(gx, gz);
    PasteOptions options;
    options.heights = m_pasteHeights;
    options.textures = m_pasteTextures;
    options.holes = m_pasteHoles;
    options.water = m_pasteWater;
    options.mapWater = m_pasteMapWater;
    options.objects = m_pasteObjects;
    options.blend = m_pin && m_blend;   // the floating ghost stays cheap; pinning shows the full blend
    if (m_comparing && ImGui::GetTime() - m_compareCycledAt < kCompareSettle) options.blend = false;   // flicking through versions
    // A compare clip is known by what it was built from, so its plans can be kept and found again.
    const bool compareClip = m_comparing && m_clipVersion == m_compareClipVersion;
    const size_t clipId = compareClip ? std::hash<std::string>{}(m_compareClipTag) : size_t(m_clipVersion);
    options.widthYards = m_blendAuto ? 0.0f : m_blendWidth;
    PasteSlopeAt(gx, gz, options);

    char key[240];
    snprintf(key, sizeof key, "%d %d %.2f %.4f %.4f %zu %d %d %d %d %.1f %d %zu", gx, gz, offset, options.slopeX, options.slopeZ, clipId,
             int(m_pasteHeights), int(m_pasteTextures) | int(m_pasteHoles) << 1 | int(m_pasteObjects) << 2 | int(m_pasteWater) << 3 | int(m_pasteMapWater) << 4, int(m_pin.has_value()), int(options.blend), options.widthYards,
             int(m_ghostPreview), m_terrain.MissingTiles(PasteTiles()));   // replan as the footprint's tiles arrive
    if (m_planKey == key) return;
    m_planKey = key;

    if (auto hit = compareClip ? m_planCache.find(key) : m_planCache.end(); hit != m_planCache.end())
        m_plan = hit->second;
    else
    {
        m_plan = m_terrain.PlanPaste(m_clipboard, gx, gz, offset, options);
        if (compareClip)
        {
            if (m_planCache.size() >= 24) m_planCache.clear();   // ponytail: whole-cache reset; an LRU if it thrashes
            m_planCache[key] = m_plan;
        }
    }
    if (m_pin)
    {
        m_renderer.UnloadTile(-1);
        m_terrain.PreviewPlan(&m_plan);   // the terrain itself shows the result, blend band included
    }
    else
    {
        m_terrain.PreviewPlan(nullptr);
        if (m_ghostPreview) m_renderer.LoadTile(-1, m_terrain.BuildGhost(m_plan), m_mpq, -1);
        else m_renderer.UnloadTile(-1);
    }
    Adt objects;   // the copied objects at their planned spots, drawn like the rest of the world
    objects.doodads = m_plan.doodads;
    objects.wmos = m_plan.wmos;
    m_models.RemoveTile(-1);
    m_models.AddTile(-1, objects, m_mpq);
}

std::pair<int, int> App::PasteAnchor() const
{
    if (!m_pin && !m_hover) return { -1, -1 };
    const auto [cx, cz] = m_pin ? *m_pin : m_terrain.GridOf(m_hover->chunk);
    return { cx - (m_clipboard.Width() - 1) / 2, cz - (m_clipboard.Depth() - 1) / 2 };
}

std::set<int> App::PasteTiles() const
{
    std::set<int> tiles;
    if (!m_placing || m_clipboard.Empty() || (!m_pin && !m_hover)) return tiles;
    const auto [gx, gz] = PasteAnchor();
    // Blend band: as wide as PlanPaste makes it (4 chunks automatic, else the set width) plus one.
    const int band = m_blend ? (m_blendAuto ? 4 : int(std::ceil(m_blendWidth / kChunkSize)) + 1) + 1 : 1;
    for (int z = std::max(0, gz - band); z <= std::min(1023, gz + m_clipboard.Depth() - 1 + band); z += 1)
        for (int x = std::max(0, gx - band); x <= std::min(1023, gx + m_clipboard.Width() - 1 + band); x += 1)
            tiles.insert(TileKey(x / 16, z / 16));
    // A carried object is added to the tile its origin stands on, which can lie beyond the paste (a cave's): loaded too.
    auto origin = [&](const float pos[3]) {
        const int tx = int(std::floor((gx * kChunkSize + pos[0]) / kTileSize)), tz = int(std::floor((gz * kChunkSize + pos[2]) / kTileSize));
        if (tx >= 0 && tz >= 0 && tx < 64 && tz < 64) tiles.insert(TileKey(tx, tz));
    };
    for (const auto& d : m_clipboard.doodads) origin(d.pos);
    for (const auto& w : m_clipboard.wmos) origin(w.pos);
    return tiles;
}

void App::ApplySelection(const std::set<ChunkRef>& hits)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl)
        for (ChunkRef r : hits) m_selection.erase(r);
    else if (io.KeyShift)
        m_selection.insert(hits.begin(), hits.end());
    else
        m_selection = hits;
}

const App::MapVersion* App::SelectedMapVersion() const
{
    if (m_mapIndex < 0 || size_t(m_mapIndex) >= m_mapVersions.size()) return nullptr;
    const auto& versions = m_mapVersions[size_t(m_mapIndex)];
    return size_t(m_mapVersion) < versions.size() ? &versions[size_t(m_mapVersion)] : nullptr;
}

void App::SelectMap(size_t index, int version)
{
    m_mapIndex = int(index);
    m_mapVersion = version;
    const auto& m = m_maps[index];
    const MapVersion* v = SelectedMapVersion();
    if (!v) m_mapVersion = 0;
    // The version's own files: a lower base layer resolved alone, a compare source as its client would.
    auto read = [&](const std::string& ext) -> std::optional<std::vector<uint8_t>> {
        const std::string dir = v ? v->directory : m.directory, name = "World\\Maps\\" + dir + "\\" + dir + ext;
        if (!v || v->edited) return m_mpq.Read(name);
        if (v->source == 0) return m_mpq.ReadFromLayer(size_t(v->archive), name);
        return m_ghosts.Chain(v->source).Read(name);
    };
    const auto wdt = read(".wdt");
    m_mapTiles = wdt ? WdtTiles(*wdt) : std::vector<bool>();
    m_mapGlobal = wdt ? WdtGlobalWmo(*wdt) : std::nullopt;
    const auto wdl = read(".wdl");
    m_mapWdl = wdl ? ParseWdl(*wdl) : std::vector<std::vector<int16_t>>{};
    m_mapPreviewDir = !v || v->edited ? m.directory : std::string();   // other versions never take the open map's edits
    m_mapPreviewKey = ~0ull;
    m_mapPreview.Reset();
    m_mapPreviewTexture.Reset();
}

void App::RefreshMapList()
{
    // The maps: the client's Map.dbc (the lowest layer's, never a pack's) with the maps the project made, then the maps
    // each source's own Map.dbc adds (ids the client lacks). A source is a base layer, or the layers sharing its label
    // (a mod's MPQ and its loose files); compare sources are read too, for their versions.
    m_mapListVersion = m_mapRows.Version();
    const std::string selected = m_mapIndex >= 0 && size_t(m_mapIndex) < m_maps.size() ? m_maps[size_t(m_mapIndex)].directory : std::string();
    const int selectedVersion = m_mapVersion;
    std::vector<MapEntry> maps;
    for (const auto& [id, row] : m_mapRows.Rows())
        maps.push_back({ id, row.value("Directory", std::string()), row.value("MapName_lang", std::string()) });
    if (!maps.empty()) m_maps = std::move(maps);   // else not the 3.3.5 layout: keep what ParseMapDbc read

    struct Group { std::string label; std::set<size_t> layers; bool hasDbc = false; std::map<uint32_t, MapEntry> rows; };
    std::vector<Group> groups;
    if (m_project)
        for (size_t li = 0; li < m_project->base.layers.size(); ++li)
        {
            const MpqLayer& l = m_project->base.layers[li];
            if (!l.enabled) continue;
            const std::string label = l.label.empty() ? fs::path(l.path).filename().string() : l.label;
            auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return !l.label.empty() && x.label == label; });
            if (g == groups.end()) g = groups.insert(groups.end(), Group{ label });
            g->layers.insert(li);
        }
    const std::string mapDbc = "DBFilesClient\\Map.dbc";
    for (Group& g : groups)   // the topmost archive of the group holding a Map.dbc
        for (size_t a = 0; a < m_mpq.Names().size() && !g.hasDbc; ++a)
            if (g.layers.count(m_mpq.ArchiveLayer(a)) && m_mpq.Has(a, mapDbc))
                if (const auto dbc = m_mpq.ReadFrom(a, mapDbc))
                {
                    g.hasDbc = true;
                    for (const MapEntry& e : ParseMapDbc(*dbc)) g.rows[e.id] = e;
                }
    std::set<uint32_t> known;
    for (const MapEntry& m : m_maps) known.insert(m.id);
    for (const Group& g : groups)
        for (const auto& [id, e] : g.rows)
            if (known.insert(id).second) m_maps.push_back(e);
    std::sort(m_maps.begin(), m_maps.end(), [](const MapEntry& a, const MapEntry& b) { return a.id < b.id; });

    std::vector<std::map<uint32_t, std::string>> compareDirs(m_ghosts.Sources().size());
    for (size_t s = 1; s < compareDirs.size(); ++s)
        if (const auto dbc = m_ghosts.Chain(s).Read(mapDbc))
            for (const MapEntry& e : ParseMapDbc(*dbc)) compareDirs[s][e.id] = e.directory;
    auto wdtOf = [](const std::string& dir) { return "World\\Maps\\" + dir + "\\" + dir + ".wdt"; };
    auto wdtArchives = [&](const std::string& dir) -> const std::vector<size_t>& {
        // Archives holding a WDT, worked out once per set of sources (a scan of every archive takes about a second).
        auto [it, fresh] = m_wdtArchives.try_emplace(dir);
        if (fresh)
            for (size_t a = 0; a < m_mpq.Names().size(); ++a)
                if (m_mpq.Has(a, wdtOf(dir))) it->second.push_back(a);
        return it->second;
    };

    m_mapVersions.assign(m_maps.size(), {});
    for (size_t i = 0; i < m_maps.size(); ++i)
    {
        MapEntry& m = m_maps[i];
        auto& versions = m_mapVersions[i];
        const std::string wdt = wdtOf(m.directory);
        std::error_code ec;
        const auto layer = m_mpq.LayerOf(wdt);   // the lowest layer holding the map: where it is edited from
        if (!layer && fs::exists(m_mpq.OverlayPath(wdt), ec)) m.source = "Project";
        else
        {
            m.source.clear();
            for (const Group& g : groups)
                if (layer && g.layers.count(*layer)) m.source = g.label;
        }
        if (m.source == "Project") versions.push_back({ 0, -1, m.directory, "Project", true });
        const auto home = m_mpq.MapHome(wdt);
        for (const Group& g : groups)
        {
            // What this source calls the map (its Map.dbc; a source without one is taken to have the same folder).
            const auto row = g.rows.find(m.id);
            if (g.hasDbc && row == g.rows.end()) continue;
            const std::string dir = g.hasDbc ? row->second.directory : m.directory;
            for (size_t a : wdtArchives(dir))
                if (g.layers.count(m_mpq.ArchiveLayer(a)))
                {
                    const bool edited = m.source != "Project" && dir == m.directory && home && m_mpq.ArchiveLayer(a) == m_mpq.ArchiveLayer(*home);
                    versions.push_back({ 0, int(a), dir, g.label + (dir == m.directory ? "" : " (" + dir + ")"), edited });
                    break;
                }
        }
        std::stable_partition(versions.begin(), versions.end(), [](const MapVersion& v) { return v.edited; });   // first row: the edited one
        for (size_t s = 1; s < compareDirs.size(); ++s)
            if (const auto it = compareDirs[s].find(m.id); it != compareDirs[s].end() && m_ghosts.Chain(s).HasOwn(wdtOf(it->second)))
                versions.push_back({ s, -1, it->second, m_ghosts.Sources()[s].name + (it->second == m.directory ? "" : " (" + it->second + ")"), false });
    }
    m_mapIndex = -1;
    for (size_t i = 0; i < m_maps.size(); ++i)
        if (m_maps[i].directory == selected) SelectMap(i, selectedVersion);
}

void App::OpenNewMap()
{
    if (!m_project) return;
    m_newMap = NewMapForm{};
    m_newMap.texture = m_activeTexture.empty() ? "Tileset\\Elwynn\\ElwynnGrassBase.blp" : m_activeTexture;
    m_newMapError.clear();
    m_newMapOpen = true;
}

bool App::CreateNewMap(std::string& error)
{
    NewMapForm& f = m_newMap;
    if (f.directory.empty() || f.directory.size() > 48 ||
        !std::all_of(f.directory.begin(), f.directory.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_'; }))
    { error = "Folder name: letters, digits and _ only (it becomes World\\Maps\\<name>\\)."; return false; }
    auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };
    for (const MapEntry& m : m_maps)
        if (lower(m.directory) == lower(f.directory)) { error = "Map " + std::to_string(m.id) + " already uses the folder " + m.directory + "."; return false; }
    if (m_mpq.Read("World\\Maps\\" + f.directory + "\\" + f.directory + ".wdt")) { error = "The game files already have a map in that folder."; return false; }
    if (f.texture.empty() || !m_mpq.Read(f.texture)) { error = "Ground texture not found: " + f.texture; return false; }
    const int last = std::min(63, f.x + f.size - 1), lastY = std::min(63, f.y + f.size - 1);
    if (f.x < 0 || f.y < 0 || f.size < 1) { error = "Tiles out of the 64 x 64 grid."; return false; }

    const Project::IdRange range = m_project->Range("map.id");
    const uint32_t id = range.first ? m_mapRows.FreeId(range.first, range.last) : 0;
    if (!id) { error = "No free Map.dbc id in the project's map.id range (File > Project settings)."; return false; }
    const bool instance = f.kind != 0;
    uint32_t difficultyId = 0;
    if (instance)
    {
        const Project::IdRange r = m_project->Range("mapdifficulty.id");
        difficultyId = r.first ? m_mapDifficulty.FreeId(r.first, r.last) : 0;
        if (!difficultyId) { error = "No free MapDifficulty id in the project's mapdifficulty.id range (File > Project settings)."; return false; }
    }

    // Map.dbc: the "like" map's loading screen, minimap icon scale, expansion and time of day; ours for the rest.
    nlohmann::json row = m_mapRows.Row(f.like);
    if (row.is_null()) row = nlohmann::json::object();
    row["ID"] = id;
    row["Directory"] = f.directory;
    row["MapName_lang"] = f.name;
    row["InstanceType"] = uint32_t(f.kind);
    row["MapDescription0_lang"] = "";
    row["MapDescription1_lang"] = "";
    row["AreaTableID"] = 0u;
    row["CorpseMapID"] = 0xFFFFFFFFu;   // -1: no corpse entrance yet (Triggers > Entrances sets one)
    row["Corpse[0]"] = 0.0f;
    row["Corpse[1]"] = 0.0f;
    row["MaxPlayers"] = instance ? uint32_t(f.maxPlayers) : 0u;
    row["RaidOffset"] = 0u;
    const std::string label = "New map " + f.directory + " (" + std::to_string(id) + ")";
    std::vector<Change> parts;
    parts.push_back(m_mapRows.MakeChange(id, nullptr, row, label));
    m_mapRows.Apply(parts.back());
    if (instance)   // AzerothCore looks up an instance's difficulty (players, reset) here
    {
        const nlohmann::json d = { { "ID", difficultyId }, { "MapID", id }, { "Difficulty", 0u }, { "Message_lang", "" },
                                   { "RaidDuration", f.kind == 2 ? 604800u : 0u }, { "MaxPlayers", uint32_t(f.maxPlayers) }, { "Difficultystring", "" } };
        parts.push_back(m_mapDifficulty.MakeChange(difficultyId, nullptr, d, label));
        m_mapDifficulty.Apply(parts.back());
        if (m_db.Connected())
        {
            parts.push_back(m_instances.MakeChange(id, {}, { { { "map", std::to_string(id) }, { "parent", "0" }, { "script", "" }, { "allowMount", "0" } } }, label));
            m_instances.Apply(parts.back());
        }
    }

    // The WDT and WDL, then flat tiles through the added-tiles path (overlay, export, minimap, server data).
    parts.push_back(m_terrain.CreateMap(f.directory, f.bigAlpha ? 0x4u : 0u));
    std::string terrainError;
    GoToTile(f.directory, f.x + (last - f.x) / 2, f.y + (lastY - f.y) / 2);   // opens it (empty until the tiles below)
    if (m_terrain.Map() != f.directory) error = "The new map did not open.";
    std::vector<TerrainAdapter::NewTile> tiles;
    for (int ty = f.y; ty <= lastY; ++ty)
        for (int tx = f.x; tx <= last; ++tx)
            tiles.push_back({ tx, ty, BlankAdt(tx, ty, f.height, f.texture, f.area), {} });
    if (error.empty())
        if (auto added = m_terrain.AddTiles(tiles, f.bigAlpha, terrainError)) parts.push_back(std::move(*added));
        else error = "Tiles not added: " + terrainError;
    if (!error.empty())
    {
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)   // nothing half made
            for (Adapter* a : std::initializer_list<Adapter*>{ &m_mapRows, &m_mapDifficulty, &m_instances, &m_terrain })
                if (it->domain == a->Domain()) a->Revert(*it);
        RefreshMapList();
        return false;
    }
    m_store.Commit(std::move(parts), label);
    RefreshMapList();
    Log("%s: %zu flat tile(s) at %.1f. Export writes its Map.dbc row, WDT and tiles; restart the client and worldserver to use it.",
        label.c_str(), tiles.size(), f.height);
    return true;
}

void App::DrawNewMapModal()
{
    if (m_newMapOpen) { ImGui::OpenPopup("New map"); m_newMapOpen = false; }
    ImGui::SetNextWindowSize({ 560, 0 }, ImGuiCond_Appearing);
    if (!m_project || !ImGui::BeginPopupModal("New map", nullptr, ImGuiWindowFlags_NoResize)) return;
    NewMapForm& f = m_newMap;
    const float field = ImGui::GetContentRegionAvail().x - 150;
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Folder", &f.directory);
    ImGui::SetItemTooltip("World\\Maps\\<folder>\\: letters, digits and _, unique.");
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Name", &f.name);
    ImGui::SetNextItemWidth(field);
    const char* kinds[] = { "World (open map)", "Dungeon", "Raid" };
    ImGui::Combo("Kind", &f.kind, kinds, 3);
    if (f.kind)
    {
        ImGui::SetNextItemWidth(field);
        ImGui::SliderInt("Players", &f.maxPlayers, 1, 40);
    }
    std::string likeName = MapLabel(f.like);
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("Like", likeName.c_str()))
    {
        for (const MapEntry& m : m_maps)
            if (ImGui::Selectable((std::to_string(m.id) + "  " + (m.name.empty() ? m.directory : m.name)).c_str(), m.id == f.like)) f.like = m.id;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Loading screen, minimap icon scale, expansion and flags are copied from this map.");

    ImGui::SeparatorText("Ground");
    ImGui::SetNextItemWidth(field);
    ImGui::SliderInt("Tiles across", &f.size, 1, 8);
    ImGui::SetItemTooltip("A square of size x size tiles (533 yd each).");
    ImGui::SetNextItemWidth(field / 2 - 4);
    ImGui::SliderInt("##tx", &f.x, 0, 64 - f.size);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field / 2 - 4);
    ImGui::SliderInt("First tile x, y##ty", &f.y, 0, 64 - f.size);
    ImGui::SetNextItemWidth(field);
    ImGui::InputFloat("Height", &f.height, 1.0f, 10.0f, "%.1f yd");
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Texture", &f.texture);
    ImGui::SetItemTooltip("Ground texture of every chunk. Pick one in Catalog > Ground textures first to have it here.");
    ImGui::Checkbox("8-bit alpha (finer texture blends, as Northrend)", &f.bigAlpha);
    int area = int(f.area);
    ImGui::SetNextItemWidth(field);
    if (ImGui::InputInt("Area id", &area)) f.area = uint32_t(std::max(area, 0));
    ImGui::SetItemTooltip("AreaTable id for every chunk (0 = none). Zones > Paint sets areas later.");

    if (!m_newMapError.empty()) ImGui::TextColored(kWarn, "%s", m_newMapError.c_str());
    ImGui::Separator();
    if (ImGui::Button("Create", { 120, 0 }))
    {
        m_newMapError.clear();
        if (CreateNewMap(m_newMapError)) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120, 0 })) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::RefreshMapPreview()
{
    if (m_mapWdl.size() != 4096) return;
    // Loaded tiles of this map draw with their current heights (edits and undo show at once); the rest from the WDL.
    const bool current = m_terrain.Map() == m_mapPreviewDir;
    uint64_t key = current ? m_store.Revision() * 1000003ull : 0;
    if (current)
        for (const auto& [k, tile] : m_terrain.Tiles()) key += uint64_t(k + 1) * 2654435761ull;
    if (key == m_mapPreviewKey && m_mapPreview) return;
    m_mapPreviewKey = key;
    if (current)
        for (const auto& [k, tile] : m_terrain.Tiles())
        {
            // The WDL's 17 x 17 grid is every chunk corner: chunk (row, col) outer vertex (0 | 8, 0 | 8).
            std::vector<int16_t>& grid = m_mapWdl[size_t(tile.y) * 64 + tile.x];
            if (grid.size() != 289) grid.assign(289, 0);
            for (int r = 0; r < 17; ++r)
                for (int c = 0; c < 17; ++c)
                {
                    const int chunk = tile.byGrid[size_t(std::min(r, 15)) * 16 + std::min(c, 15)];
                    if (chunk < 0) continue;
                    const AdtChunk& ch = tile.adt.chunks[size_t(chunk)];
                    const float h = ch.baseY + ch.heights[size_t(r == 16 ? 8 : 0) * 17 + (c == 16 ? 8 : 0)];
                    grid[size_t(r) * 17 + c] = int16_t(std::clamp(h, -32768.0f, 32767.0f));
                }
        }

    constexpr UINT kPx = 16, kSide = 64 * kPx;
    const std::vector<uint8_t> image = MapPreview(m_mapWdl, kPx);
    if (!m_mapPreviewTexture)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kSide;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&d, nullptr, &m_mapPreviewTexture))) return;
        m_device->CreateShaderResourceView(m_mapPreviewTexture.Get(), nullptr, &m_mapPreview);
    }
    m_context->UpdateSubresource(m_mapPreviewTexture.Get(), 0, nullptr, image.data(), kSide * 4, 0);
}

// ---------------------------------------------------------------------------------------------- frame

void App::Frame(float dt)
{
    m_fps = m_fps * 0.95f + (dt > 0 ? 1.0f / dt : 0) * 0.05f;
    Autosave();
    if (m_closeRequested)
    {
        m_closeRequested = false;
        GuardUnsaved([this] { m_quit = true; });
    }

    // A tool of another group (button or shortcut): that group's panels come forward; each group remembers its tool.
    if (const Group g = GroupOf(m_tool); g != GroupOf(m_lastTool))
    {
        const std::vector<const char*> panels[kGroups] = { { "Inspector", "Catalog" }, { "Object", "Catalog" }, { "Inspector", "Catalog" },
                                                           { "Inspector", "Problems" }, { "Inspector" } };
        for (const char* name : panels[int(g)]) ImGui::SetWindowFocus(name);
        ImGui::SetWindowFocus("Tools");
        if (g == Group::Terrain) m_catalogShowTab = int(Catalog::Kind::GroundTexture);
        if (g == Group::Objects) m_catalogShowTab = int(Catalog::Kind::Doodad);
        if (g == Group::Units) m_catalogShowTab = m_tool == Tool::Gameobjects ? kGameobjectTab : kCreatureTab;
    }
    if (m_tool == Tool::Triggers && m_lastTool != Tool::Triggers) m_catalogShowTab = kPortalTab;
    m_groupTool[int(GroupOf(m_tool))] = m_tool;
    m_lastTool = m_tool;

    UpdateSpawnView();   // cached: queries the database only when the camera changes tile or the project changes
    UpdateSpawnModels();

    // Selected chunks and the clipboard's footprint stay loaded wherever the camera goes.
    std::set<int> pinned = PasteTiles();
    for (ChunkRef r : m_selection) pinned.insert(r.tile);
    for (const auto& [gx, gz] : m_diffPending) pinned.insert(TileKey(gx / 16, gz / 16));   // a difference about to be reviewed
    if (m_mapJob) pinned.insert(m_mapJob->tiles.begin(), m_mapJob->tiles.end());   // a world map waiting to be rendered
    m_terrain.SetPinned(std::move(pinned));

    // Tiles stream in around the camera: prepared in the background, uploaded here within one shared budget per
    // frame (terrain first, then a ghost tile if time is left, then models).
    const auto streamStart = std::chrono::steady_clock::now();
    auto streamMs = [&] { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - streamStart).count(); };
    std::string error;
    for (const auto& [loaded, stats] : m_terrain.Stream(m_camera.pos.x, m_camera.pos.z, m_loadRadius, error, 4.0f))
    {
        if (m_focusTile == loaded)
        {
            m_camera.pos.y = stats.maxHeight + 120.0f;
            m_focusTile.reset();
        }
        if (stats.texturesMissing) Log("Tile %d_%d: %zu texture(s) missing.", loaded % 64, loaded / 64, stats.texturesMissing);
        if (m_flyGround)
            if (const auto h = m_terrain.HeightAt(m_flyGround->first, m_flyGround->second))
            {
                m_camera.pos.y = *h + 40;
                m_flyGround.reset();
            }
    }
    if (!error.empty()) Log("%s", error.c_str());

    // Files fetched from the CDN in the background since the last look: ask again for what was missing.
    if (const uint64_t arrived = CdnArrivals(); arrived != m_cdnArrivalsSeen && ImGui::GetTime() - m_cdnRefreshAt > 1.5)
    {
        m_cdnArrivalsSeen = arrived;
        m_cdnRefreshAt = ImGui::GetTime();
        m_renderer.ForgetMissingTextures();
        m_models.ForgetFailed();
        m_models.RefreshTextures(m_mpq);
        for (Ghosts::Layer& layer : m_ghosts.Layers()) layer.missing.clear();
        for (auto it = m_ghostTilesMissingTextures.begin(); it != m_ghostTilesMissingTextures.end();)
        {
            const Ghosts::Layer* layer = m_ghosts.Find(it->first);
            const auto tile = layer ? layer->tiles.find(it->second) : decltype(layer->tiles)::const_iterator{};
            if (layer && tile != layer->tiles.end() &&
                m_renderer.LoadTile(Ghosts::Key(it->first, it->second), tile->second.adt, m_ghosts.Chain(layer->source), it->first).texturesMissing)
                ++it;
            else
                it = m_ghostTilesMissingTextures.erase(it);
        }
    }

    // Ghost layers stream around the camera like the map; the solo layer brings its objects along.
    if (!m_terrain.Map().empty() && !m_ghosts.Layers().empty())
    {
        const auto streamed = m_ghosts.Stream(m_terrain.Map(), m_camera.pos.x, m_camera.pos.z, m_loadRadius, streamMs() < 4.0f ? 1 : 0);
        for (const auto& [name, image] : m_ghosts.TakeImages()) m_renderer.CacheTexture(name, image);   // decoded on the ghost worker
        for (const auto& [id, key] : streamed.unloaded)
        {
            m_renderer.UnloadTile(Ghosts::Key(id, key));
            m_models.RemoveTile(Ghosts::Key(id, key));
        }
        for (const auto& [id, key] : streamed.loaded)
            if (const Ghosts::Layer* layer = m_ghosts.Find(id))
            {
                const LoadedTile& tile = layer->tiles.at(key);
                if (m_renderer.LoadTile(Ghosts::Key(id, key), tile.adt, m_ghosts.Chain(layer->source), id).texturesMissing)
                    m_ghostTilesMissingTextures.insert({ id, key });
                if (id == m_soloLayer) m_models.AddTile(Ghosts::Key(id, key), tile.adt, m_ghosts.Chain(layer->source), id);
            }
    }
    for (const auto& layer : m_ghosts.Layers()) m_renderer.SetLayerStyle(layer.id, layer.tint, layer.visible);
    m_drawOptions.solo = m_soloLayer;
    m_modelSettings.layer = m_soloLayer;

    // Models follow the terrain: drop tiles that streamed out, add one newly loaded tile per frame.
    for (int key : m_models.TileKeys())
        if (key >= 0 && key != kGlobalWmoKey && !m_terrain.Tiles().count(key)) m_models.RemoveTile(key);   // negative keys: paste previews
    if (const auto& global = m_terrain.GlobalWmo(); global && !m_models.HasTile(kGlobalWmoKey))   // a WMO-only map's one WMO
    {
        Adt adt;
        adt.wmos.push_back(*global);
        m_models.AddTile(kGlobalWmoKey, adt, m_mpq);
    }
    for (int key : m_terrain.TakeObjectChanges())   // edited, pasted or undone objects: reload that tile's models now
        if (key == kGlobalWmoKey) m_models.RemoveTile(key);   // added back below from the moved placement
        else if (auto it = m_terrain.Tiles().find(key); it != m_terrain.Tiles().end() && m_models.HasTile(key))
        {
            m_models.RemoveTile(key);
            m_models.AddTile(key, it->second.adt, m_mpq);
        }
    for (const auto& [key, tile] : m_terrain.Tiles())   // models of newly loaded tiles, while the frame's budget lasts
        if (!m_models.HasTile(key))
        {
            m_models.AddTile(key, tile.adt, m_mpq);
            if (streamMs() > 8.0f) break;
        }
    RunMapJob();

    ImGuizmo::BeginFrame();
    HandleShortcuts();
    DrawMenuBar();
    DrawToolbar();
    DrawStatusBar();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), ImGui::GetMainViewport());
    if (m_buildLayout) BuildDefaultLayout(dockspace);

    DrawViewport(dt);
    DrawToolsPanel();
    DrawMapsPanel();
    DrawInspector();
    // The Object window docks with the Inspector the first time it appears and comes forward with the Objects tool.
    if (ImGuiWindow* inspector = ImGui::FindWindowByName("Inspector"); inspector && inspector->DockId)
        ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Object")) DrawObjectPanel();
    ImGui::End();
    if (ImGuiWindow* log = ImGui::FindWindowByName("Log"); log && log->DockId) ImGui::SetNextWindowDockID(log->DockId, ImGuiCond_FirstUseEver);
    if (m_catalogShowTab) ImGui::SetNextWindowFocus();
    DrawCatalog();
    if (ImGuiWindow* inspector = ImGui::FindWindowByName("Inspector"); inspector && inspector->DockId)
        ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
    DrawVersions();
    DrawSources();
    DrawRaces();
    UpdateNpc();
    DrawNpcViewer();
    DrawShortcuts();
    DrawServerDataWindow();
    DrawChangesPanel();
    DrawProblemsPanel();
    if (ImGuiWindow* log = ImGui::FindWindowByName("Log"); log && log->DockId) ImGui::SetNextWindowDockID(log->DockId, ImGuiCond_FirstUseEver);
    DrawServerPanel();
    DrawLogPanel();
    DrawNewProjectModal();
    DrawSetupModal();
    DrawProjectSettingsModal();
    DrawNewMapModal();
    DrawUnsavedModal();
    DrawSaveBlueprintModal();
    DrawPalette();
}

void App::HandleShortcuts()
{
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) { m_paletteOpen = true; m_paletteQuery[0] = 0; m_paletteSelected = 0; }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Slash, global)) m_showShortcuts = !m_showShortcuts;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) NewProject();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) OpenProjectDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) Save();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, global)) Export(false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, global)) BuildPatch(false);
    if (ImGui::Shortcut(ImGuiKey_F5, global)) Export(true);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) Undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) Redo();

    if (ImGui::GetIO().WantTextInput) return;   // the rest only while not typing
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, global)) CopySelection();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, global)) PasteInPlace();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, global)) OpenSaveBlueprint();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_T, global)) SetTopDown(!m_camera.topDown);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, global)) PasteAtCursor();
    if (ImGui::GetIO().KeyCtrl) return;
    for (const ToolInfo& t : kTools)
        if (t.imKey != ImGuiKey_None && ImGui::IsKeyPressed(t.imKey, false)) m_tool = t.tool;
    for (int g = 0; g < kGroups; ++g)   // F1..F4, F6: tool groups
        if (ImGui::IsKeyPressed(GroupKey(g), false)) SetGroup(Group(g));
    const bool pathMode = m_path && m_tool == Tool::Creatures;
    if (pathMode && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) SavePathEdit();
    TransformKeys();   // every tool that moves things: handles, height, scale, ground, delete
    if (!TransformTool() && ImGui::IsKeyPressed(ImGuiKey_R, false)) RotateClipboard(ImGui::GetIO().KeyShift ? 3 : 1);
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FocusTile();
    if (ImGui::IsKeyPressed(ImGuiKey_P, false) && !m_clipboard.Empty()) { m_tool = Tool::Copy; m_placing = !m_placing; m_pin.reset(); }
    // Compare: [ and ] start it on the selection and cycle the versions; Enter pastes the shown one and ends it.
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) || ImGui::IsKeyPressed(ImGuiKey_RightBracket, false))
    {
        const int step = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) ? -1 : 1;
        if (m_comparing) CycleCompare(step);
        else
        {
            StartCompare();
            if (step < 0) CycleCompare(-1);
        }
    }
    if (m_comparing && m_tool == Tool::Copy && m_pin && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        CommitCompare();
    if ((m_comparing || !m_diffNewTiles.empty()) && !m_diffActive.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) RejectDifference();
    if (!m_diffNewTiles.empty() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) AddDifferenceTiles();
    if (m_tool == Tool::Copy && m_pin)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) CommitPlacement();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) m_pin->first -= 1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) m_pin->first += 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_pin->second -= 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_pin->second += 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        if (m_armed) m_armed.reset();                 // stop placing a catalog model
        else if (m_triggerPick != TriggerPick::None) m_triggerPick = TriggerPick::None;   // a pending trigger pick
        else if (m_tool == Tool::Triggers && m_triggerSel) m_triggerSel = 0;
        else if (m_poiPick != PoiPick::None) m_poiPick = PoiPick::None;
        else if (m_tool == Tool::Pois && m_poiSel) m_poiSel = 0;
        else if (m_flightPlace) m_flightPlace = false;
        else if (m_tool == Tool::Flights && (m_flightNode || m_flightPath)) { m_flightNode = m_flightPath = 0; m_flightPoint.reset(); }
        else if (m_lightPlace) m_lightPlace = false;
        else if (m_tool == Tool::Lights && m_lightSel) m_lightSel = 0;
        else if (m_emitterPlace) m_emitterPlace = false;
        else if (m_tool == Tool::Sound && m_emitterSel) m_emitterSel = 0;
        else if (m_roadDraw) m_roadDraw = false;
        else if (m_tool == Tool::Roads && m_roadPoint) m_roadPoint.reset();
        else if (m_tool == Tool::Roads && m_roadSel) m_roadSel = 0;
        else if (m_comparing) StopCompare();          // the map comes back as it is
        else if (!m_diffPending.empty()) { EndNewTiles(); m_diffPending.clear(); m_diffActive.clear(); }   // a review still on its way
        else if (m_pin) CancelPin();                  // first Esc: unpin, terrain goes back
        else if (m_placing) m_placing = false;        // second: stop placing
        else if (m_tool == Tool::Objects && !m_objSel.empty()) m_objSel.clear();
        else if (m_tool == Tool::Sculpt && m_terrain.SelectedVertices()) m_terrain.ClearVertexSelection();
        else if (pathMode) CancelPathEdit();          // leave path editing, nothing saved
        else if (m_spawnArmed) m_spawnArmed.reset();
        else if (SpawnTool() && !m_spawnSel.empty()) m_spawnSel.clear();
        else m_selection.clear();                     // then: clear the selection
    }
    const std::pair<ImGuiKey, Brush::Mode> modes[] = { { ImGuiKey_1, Brush::Mode::Raise }, { ImGuiKey_2, Brush::Mode::Lower },
                                                       { ImGuiKey_3, Brush::Mode::Flatten }, { ImGuiKey_4, Brush::Mode::Smooth },
                                                       { ImGuiKey_5, Brush::Mode::Vertices } };
    for (const auto& [key, mode] : modes)
        if (!TransformTool() && ImGui::IsKeyPressed(key, false)) { m_tool = Tool::Sculpt; m_brush.mode = mode; }
    if (m_tool == Tool::Sculpt && m_terrain.SelectedVertices())
    {
        const float step = m_vertexStep * (ImGui::GetIO().KeyShift ? 5.0f : 1.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) EditVertices(TerrainAdapter::VertexOp::Move, step);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) EditVertices(TerrainAdapter::VertexOp::Move, -step);
    }
}

void App::EditVertices(TerrainAdapter::VertexOp op, float amount)
{
    if (auto change = m_terrain.EditSelectedVertices(op, amount))
    {
        Log("%s", change->label.c_str());
        m_store.Commit(std::move(*change));
    }
}

void App::DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New project...", "Ctrl+N")) NewProject();
        if (ImGui::MenuItem("Open project...", "Ctrl+O")) OpenProjectDialog();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, m_project.has_value())) Save();
        ImGui::Separator();
        if (ImGui::MenuItem("Export client files", "Ctrl+E", false, m_project.has_value())) Export(false);
        if (ImGui::MenuItem("Play test", "F5", false, m_project.has_value())) Export(true);
        if (ImGui::MenuItem("Build patch MPQ", "Ctrl+Shift+E", false, m_project.has_value())) BuildPatch(false);
        if (ImGui::MenuItem("Build patch MPQ and install into client", nullptr, false, m_project.has_value())) BuildPatch(true);
        if (ImGui::MenuItem("Build server data (maps, vmaps, mmaps)...", nullptr, false, m_project.has_value())) m_showServerData = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Project settings...", nullptr, false, m_project.has_value())) OpenProjectSettings();
        if (ImGui::MenuItem("New map...", nullptr, false, m_project.has_value())) OpenNewMap();
        if (ImGui::MenuItem("Server setup...", nullptr, false, m_project.has_value())) OpenSetup();
        ImGui::Separator();
        if (ImGui::MenuItem("Close project", nullptr, false, m_project.has_value())) GuardUnsaved([this] { CloseProject(); });
        if (ImGui::MenuItem("Exit", "Alt+F4")) RequestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_store.CanUndo())) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_store.CanRedo())) Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Copy chunks", "Ctrl+C", false, !m_selection.empty())) CopySelection();
        if (ImGui::MenuItem("Save selection as blueprint...", "Ctrl+B", false, !m_selection.empty())) OpenSaveBlueprint();
        if (ImGui::MenuItem("Paste at cursor", "Ctrl+V", false, !m_clipboard.Empty())) PasteAtCursor();
        if (ImGui::MenuItem("Clear selection", "Esc", false, !m_selection.empty())) m_selection.clear();
        ImGui::Separator();
        if (ImGui::MenuItem("Rotate selection in place", nullptr, false, !m_selection.empty())) RotateSelectionInPlace();
        if (ImGui::MenuItem("Revert selection to client", nullptr, false, !m_selection.empty())) RevertSelectionToClient();
        ImGui::Separator();
        if (ImGui::MenuItem("Command palette...", "Ctrl+P")) { m_paletteOpen = true; m_paletteQuery[0] = 0; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Wireframe", nullptr, &m_drawOptions.wireframe);
        ImGui::MenuItem("Object boxes", nullptr, &m_drawOptions.showObjects);
        ImGui::MenuItem("Far terrain (whole map, low detail)", nullptr, &m_drawOptions.farTerrain);
        ImGui::MenuItem("Terrain level of detail", nullptr, &m_drawOptions.lod);
        ImGui::MenuItem("Game lighting (time of day in Atmosphere > Lights)", nullptr, &m_gameLight);
        ImGui::MenuItem("Road lines and points (Roads tool)", nullptr, &m_roadLines);
        ImGui::MenuItem("Creatures", nullptr, &m_showSpawns[int(SpawnKind::Creature)]);
        ImGui::MenuItem("Gameobjects", nullptr, &m_showSpawns[int(SpawnKind::GameObject)]);
        DrawEventFilter(330);
        if (ImGui::MenuItem("Top-down view", "Ctrl+T", m_camera.topDown)) SetTopDown(!m_camera.topDown);
        if (ImGui::MenuItem("Focus camera on tile", "F", false, !m_terrain.Tiles().empty())) FocusTile();
        ImGui::Separator();
        ImGui::MenuItem("Sources", nullptr, &m_showSources);
        ImGui::MenuItem("NPC viewer", nullptr, &m_showNpc);
        ImGui::MenuItem("Races", nullptr, &m_showRaces);
        if (ImGui::MenuItem("Reset panel layout")) m_buildLayout = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        ImGui::MenuItem("Keyboard shortcuts", "Ctrl+/", &m_showShortcuts);
        if (ImGui::MenuItem("Command palette...", "Ctrl+P")) { m_paletteOpen = true; m_paletteQuery[0] = 0; }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::DrawToolbar()
{
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    const float height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2 - 6;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 10, 5 });
    if (ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags))
    {
        // Tool groups: each has its own tools and brings its panels forward.
        const char* tips[] = { "Sculpt, paint, holes, copy and paste", "Doodads and WMOs: place from the Catalog, move, turn, scale",
                               "Creatures, gameobjects and their paths", "Zones and areas, buildings, the world map",
                               "Lights: sky, fog and sun colours by time of day" };
        for (int g = 0; g < kGroups; ++g)
        {
            const bool on = int(GroupOf(m_tool)) == g;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.42f, 0.70f, 1));
            if (ImGui::Button((std::string(kGroupNames[g]) + "  F" + std::to_string(g < 4 ? g + 1 : g + 2)).c_str())) SetGroup(Group(g));
            if (on) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tips[g]);
            ImGui::SameLine();
        }
        ImGui::TextColored(kQuiet, "|");
        ImGui::SameLine();
        if (m_showNpc) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.42f, 0.70f, 1));
        const bool npcPressed = ImGui::Button("NPCs");
        if (m_showNpc) ImGui::PopStyleColor();
        if (npcPressed) { m_showNpc = !m_showNpc; m_npc.focus = m_showNpc; }
        ImGui::SetItemTooltip("NPC viewer: creature templates, models, equipment, appearance");
        ImGui::SameLine();

        const float playWidth = 120;
        ImGui::SameLine(ImGui::GetWindowWidth() - playWidth - 10);
        ImGui::BeginDisabled(!m_project);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.28f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.58f, 0.34f, 1));
        if (ImGui::Button("Play test  F5", ImVec2(playWidth, 0))) Export(true);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Save, export and copy the project's client files into the WXL client's overlay.\nRelog in the client to see them.");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::DrawStatusBar()
{
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##Status", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags))
    {
        if (ImGui::BeginMenuBar())
        {
            if (m_project)
            {
                ImGui::TextUnformatted(m_project->name.c_str());
                ImGui::SameLine();
                if (m_store.Dirty()) ImGui::TextColored(kWarn, "  unsaved changes");
                else ImGui::TextColored(kQuiet, "  saved");
            }
            else
                ImGui::TextColored(kQuiet, "No project");
            if (m_project && m_problemsChecked)
            {
                const auto errors = std::count_if(m_problems.begin(), m_problems.end(), [](const Problem& p) { return p.severity == Problem::Severity::Error; });
                const auto warnings = std::ptrdiff_t(m_problems.size()) - errors;
                ImGui::SameLine(0, 24);
                char text[96];
                const char* stale = m_problemsRevision != m_store.Revision() ? "  (before the last edits)" : "";
                if (m_problems.empty()) snprintf(text, sizeof text, "no problems%s", stale);
                else snprintf(text, sizeof text, "%zd error(s), %zd warning(s)%s", errors, warnings, stale);
                ImGui::PushStyleColor(ImGuiCol_Text, errors ? kBad : warnings ? kWarn : kQuiet);
                if (ImGui::Selectable(text, false, 0, ImGui::CalcTextSize(text))) ImGui::SetWindowFocus("Problems");
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Open the Problems panel (Check now runs the checks again)");
            }
            if (ImGui::GetTime() < m_toastUntil)
            {
                ImGui::SameLine(0, 24);
                const std::string text = Fit(m_toast, 520);
                ImGui::PushStyleColor(ImGuiCol_Text, m_toastError ? kBad : kWarn);
                if (ImGui::Selectable(text.c_str(), false, 0, ImGui::CalcTextSize(text.c_str()))) ImGui::SetWindowFocus("Log");
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s\n(click for the Log)", m_toast.c_str());
            }
            if (m_project && !m_project->serverProfile.empty())
            {
                ImGui::SameLine(0, 24);
                if (m_db.Connected()) ImGui::TextColored(kQuiet, "server %s", m_project->serverProfile.c_str());
                else ImGui::TextColored(kWarn, "server %s offline", m_project->serverProfile.c_str());
            }
            if (!m_terrain.Map().empty())
            {
                ImGui::SameLine(0, 24);
                ImGui::TextColored(kQuiet, "%s   %zu tiles loaded", m_terrain.Map().c_str(), m_terrain.Tiles().size());
            }
            if (!m_selection.empty())
            {
                ImGui::SameLine(0, 24);
                ImGui::TextColored(kAccent, "%zu chunk(s) selected", m_selection.size());
            }
            if (const Differences::Progress p = m_diffs.GetProgress(); p.running && p.total)
            {
                ImGui::SameLine(0, 24);
                ImGui::TextColored(kQuiet, "Differences vs %s", m_diffs.OtherLabel().c_str());
                ImGui::SameLine();
                char overlay[48];
                snprintf(overlay, sizeof overlay, "%zu / %zu tiles", p.done, p.total);
                ImGui::ProgressBar(float(p.done) / float(p.total), { 160, ImGui::GetTextLineHeight() }, overlay);
            }
            if (m_hover)
                if (const AdtChunk* c = m_terrain.Chunk(m_hover->chunk))
                {
                    ImGui::SameLine(0, 24);
                    ImGui::TextColored(kQuiet, "cursor %.1f, %.1f, %.1f   tile %d_%d   chunk %u,%u   area %s", m_hover->pos.x, m_hover->pos.y,
                                       m_hover->pos.z, m_hover->chunk.tile % 64, m_hover->chunk.tile / 64, c->indexX, c->indexY, AreaLabel(c->areaId).c_str());
                }
                else
                {
                    float sx, sy, sz;   // on a WMO floor: no chunk; the server position is what spawns and points take
                    EditorToServer(m_hover->pos, sx, sy, sz);
                    ImGui::SameLine(0, 24);
                    ImGui::TextColored(kQuiet, "cursor on a WMO   server %.1f, %.1f, %.1f", sx, sy, sz);
                }
            const std::string right = std::to_string(int(m_fps + 0.5f)) + " fps";
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 16);
            ImGui::TextColored(kQuiet, "%s", right.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void App::BuildDefaultLayout(unsigned int dockspace)
{
    m_buildLayout = false;
    ++m_panelLayoutGeneration;   // the multi-panel windows go back to their own arrangements too
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Tools", left);
    ImGui::DockBuilderDockWindow("Maps", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Object", right);
    ImGui::DockBuilderDockWindow("Versions", right);
    ImGui::DockBuilderDockWindow("Catalog", bottom);
    ImGui::DockBuilderDockWindow("Changes", bottom);
    ImGui::DockBuilderDockWindow("Problems", bottom);
    ImGui::DockBuilderDockWindow("Server", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
}

unsigned int App::PanelHost(const char* host, bool* open, const std::function<void(unsigned int)>& layout)
{
    const bool visible = ImGui::Begin(host, open);
    const ImGuiID space = ImGui::GetID("##panels");
    if (!ImGui::DockBuilderGetNode(space) || m_panelLayouts[host] != m_panelLayoutGeneration)
    {
        m_panelLayouts[host] = m_panelLayoutGeneration;
        ImGui::DockBuilderRemoveNode(space);
        ImGui::DockBuilderAddNode(space, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(space, ImGui::GetContentRegionAvail());
        layout(space);
        ImGui::DockBuilderFinish(space);
    }
    // A collapsed or hidden host keeps its panels docked (they hide with it).
    ImGui::DockSpace(space, { 0, 0 }, visible ? ImGuiDockNodeFlags_None : ImGuiDockNodeFlags_KeepAliveOnly);
    ImGui::End();
    return space;
}

bool App::BeginPanel(const char* name, unsigned int space)
{
    ImGui::SetNextWindowDockID(space, ImGuiCond_FirstUseEver);
    return ImGui::Begin(name);
}

// ---------------------------------------------------------------------------------------------- viewport

void App::EnsureViewportTarget(UINT width, UINT height)
{
    if (width == m_vpWidth && height == m_vpHeight && m_vpRtv) return;
    m_vpWidth = width;
    m_vpHeight = height;
    m_vpColor.Reset();
    m_vpRtv.Reset();
    m_vpSrv.Reset();
    m_vpDsv.Reset();

    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&d, nullptr, &m_vpColor);
    m_device->CreateRenderTargetView(m_vpColor.Get(), nullptr, &m_vpRtv);
    m_device->CreateShaderResourceView(m_vpColor.Get(), nullptr, &m_vpSrv);

    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    m_vpDepth.Reset();
    m_vpDepthStaging.Reset();
    m_device->CreateTexture2D(&d, nullptr, &m_vpDepth);
    m_device->CreateDepthStencilView(m_vpDepth.Get(), nullptr, &m_vpDsv);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m_device->CreateTexture2D(&d, nullptr, &m_vpDepthStaging);
}

void App::BuildOverlay(std::vector<LineVertex>& lines) const
{
    BuildSpawnOverlay(lines);
    BuildPathOverlay(lines);
    if (m_tool == Tool::Triggers)
    {
        BuildTriggerOverlay(lines);
        return;
    }
    if (m_tool == Tool::Pois)
    {
        BuildPoiOverlay(lines);
        return;
    }
    if (m_tool == Tool::Lights)
    {
        BuildLightOverlay(lines);
        return;
    }
    if (m_tool == Tool::Sound)
    {
        BuildSoundOverlay(lines);
        return;
    }
    if (m_tool == Tool::Roads)
    {
        BuildRoadOverlay(lines);
        return;
    }
    if (m_tool == Tool::Flights)
    {
        BuildFlightOverlay(lines);
        return;
    }
    auto chunkOutline = [&](ChunkRef ref, XMFLOAT4 color, float lift = 0.3f) {
        const AdtChunk* c = ShownChunk(ref);
        if (!c) return;
        auto p = [&](size_t row, size_t col) {
            return XMFLOAT3{ c->baseX + col * kUnitSize, c->baseY + c->heights[row * 17 + col] + lift, c->baseZ + row * kUnitSize };
        };
        for (size_t i = 0; i < 8; ++i)
        {
            lines.push_back({ p(0, i), color });     lines.push_back({ p(0, i + 1), color });
            lines.push_back({ p(8, i), color });     lines.push_back({ p(8, i + 1), color });
            lines.push_back({ p(i, 0), color });     lines.push_back({ p(i + 1, 0), color });
            lines.push_back({ p(i, 8), color });     lines.push_back({ p(i + 1, 8), color });
        }
    };

    // One hole cell (2x2 vertex cells) outlined along its own terrain.
    auto holeOutline = [&](ChunkRef ref, int bit, XMFLOAT4 color) {
        const AdtChunk* c = m_terrain.Chunk(ref);
        if (!c) return;
        const size_t r0 = size_t(bit / 4) * 2, c0 = size_t(bit % 4) * 2;
        auto p = [&](size_t row, size_t col) {
            return XMFLOAT3{ c->baseX + col * kUnitSize, c->baseY + c->heights[row * 17 + col] + 0.4f, c->baseZ + row * kUnitSize };
        };
        for (size_t i = 0; i < 2; ++i)
        {
            lines.push_back({ p(r0, c0 + i), color });     lines.push_back({ p(r0, c0 + i + 1), color });
            lines.push_back({ p(r0 + 2, c0 + i), color }); lines.push_back({ p(r0 + 2, c0 + i + 1), color });
            lines.push_back({ p(r0 + i, c0), color });     lines.push_back({ p(r0 + i + 1, c0), color });
            lines.push_back({ p(r0 + i, c0 + 2), color }); lines.push_back({ p(r0 + i + 1, c0 + 2), color });
        }
    };
    if (m_tool == Tool::Zones)
    {
        BuildZoneOverlay(lines);
        return;
    }
    if (m_tool == Tool::Holes && m_hover && m_impassMode)
    {
        for (const ChunkRef& ref : m_terrain.ChunksAt(m_hover->pos, 250.0f))   // impassable chunks nearby
            if (m_terrain.Chunk(ref)->flags & 0x2u) chunkOutline(ref, { 1, 0.25f, 0.2f, 0.8f }, 0.6f);
        const bool set = m_holeCut != ImGui::GetIO().KeyCtrl;
        for (const ChunkRef& ref : m_terrain.ChunksAt(m_hover->pos, m_holeRadius))
            chunkOutline(ref, set ? XMFLOAT4{ 1, 0.5f, 0.3f, 1 } : XMFLOAT4{ 0.35f, 1, 0.45f, 1 }, 0.9f);
        return;
    }
    if (m_tool == Tool::Holes && m_hover)
    {
        for (const auto& [ref, bit] : m_terrain.HoleCellsAt(m_hover->pos, 120.0f))   // existing holes nearby
            if (m_terrain.Chunk(ref)->holes & (1u << bit)) holeOutline(ref, bit, { 1, 0.3f, 0.3f, 0.35f });
        const bool cut = m_holeCut != ImGui::GetIO().KeyCtrl;
        for (const auto& [ref, bit] : m_terrain.HoleCellsAt(m_hover->pos, m_holeRadius))
            holeOutline(ref, bit, cut ? XMFLOAT4{ 1, 0.35f, 0.3f, 1 } : XMFLOAT4{ 0.35f, 1, 0.45f, 1 });
        return;
    }

    if (m_tool == Tool::Objects)
    {
        auto box = [&](const ObjectRef& r, XMFLOAT4 color) {
            XMFLOAT3 c[8];
            if (!m_models.Corners(r.wmo, r.uid, c)) return;
            const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (const auto& e : edges) { lines.push_back({ c[e[0]], color }); lines.push_back({ c[e[1]], color }); }
        };
        for (const ObjectRef& r : m_objSel) box(r, { 1, 0.75f, 0.2f, 1 });
        if (m_carryInside)   // what moves along with a selected building
            for (const ObjectRef& r : m_gizmoActive ? m_gizmoRiders : m_terrain.DoodadsInside(m_objSel)) box(r, { 1, 0.75f, 0.2f, 0.35f });
        if (m_objHover && !m_objSel.count(*m_objHover)) box(*m_objHover, { 1, 1, 1, 0.6f });
        return;
    }
    for (ChunkRef r : m_selection) chunkOutline(r, { 0.3f, 0.62f, 1, 1 });
    if (m_pin && m_tool == Tool::Copy && !m_planKey.empty())   // pinned: outlines stay without the mouse
    {
        for (const auto& pc : m_plan.chunks) chunkOutline(pc.ref, { 1, 0.6f, 0.2f, 0.3f }, 0.6f);
        for (ChunkRef r : m_plan.footprint) chunkOutline(r, { 1, 0.6f, 0.2f, 1.0f }, 0.8f);
        return;
    }
    if (!m_hover) return;

    if (m_tool == Tool::Select || m_tool == Tool::Copy)
    {
        if (!m_selection.count(m_hover->chunk)) chunkOutline(m_hover->chunk, { 1, 1, 1, 0.45f });
        if (m_tool == Tool::Copy && m_placing && !m_planKey.empty())   // where the paste lands, and its blend band
        {
            for (const auto& pc : m_plan.chunks) chunkOutline(pc.ref, { 1, 0.6f, 0.2f, 0.3f }, 0.6f);
            for (ChunkRef r : m_plan.footprint) chunkOutline(r, { 1, 0.6f, 0.2f, m_pin ? 1.0f : 0.9f }, 0.8f);
        }
        return;
    }

    // Brush: outer ring at the radius, inner ring where the falloff is half. Only the brush tools have one; tools that
    // pick things (units, triggers, points, nodes) show their own hover instead.
    if (m_tool == Tool::Sculpt && m_terrain.SelectedVertices())   // selected vertices: small yellow crosses
    {
        const XMFLOAT4 yellow{ 1, 0.85f, 0.2f, 1 };
        size_t shown = 0;
        for (const XMFLOAT3& p : m_terrain.SelectedVertexPositions())
        {
            if (++shown > 40000) break;   // ponytail: cap; a huge selection only needs to look selected
            lines.push_back({ { p.x - 0.8f, p.y + 0.2f, p.z }, yellow }); lines.push_back({ { p.x + 0.8f, p.y + 0.2f, p.z }, yellow });
            lines.push_back({ { p.x, p.y + 0.2f, p.z - 0.8f }, yellow }); lines.push_back({ { p.x, p.y + 0.2f, p.z + 0.8f }, yellow });
        }
    }
    if (m_tool != Tool::Sculpt && m_tool != Tool::Paint && m_tool != Tool::Shade && m_tool != Tool::Water) return;
    const XMFLOAT4 ringColors[2] = { { 1, 1, 1, 0.9f }, { 1, 1, 1, 0.35f } };
    const PaintBrush* soft = m_tool == Tool::Paint ? &m_paint : m_tool == Tool::Shade ? &m_shadeBrush : nullptr;
    const float radius = soft ? soft->radius : m_tool == Tool::Water ? m_water.radius : m_brush.radius;
    if (m_tool == Tool::Water && m_water.mode == WaterBrush::Mode::Level)   // where Level puts the surface
    {
        XMFLOAT3 prev{};
        for (int i = 0; i <= 64; ++i)
        {
            const float a = i * XM_2PI / 64;
            const XMFLOAT3 p{ m_hover->pos.x + std::cos(a) * radius, m_water.level, m_hover->pos.z + std::sin(a) * radius };
            if (i) { lines.push_back({ prev, { 0.3f, 0.7f, 1, 1 } }); lines.push_back({ p, { 0.3f, 0.7f, 1, 1 } }); }
            prev = p;
        }
    }
    const float radii[2] = { radius, soft ? std::max(radius * soft->hardness, 0.5f) : radius * 0.5f };
    for (int ring = 0; ring < 2; ++ring)
    {
        constexpr int kSegments = 64;
        XMFLOAT3 prev{};
        for (int i = 0; i <= kSegments; ++i)
        {
            const float a = i * XM_2PI / kSegments;
            const float x = m_hover->pos.x + std::cos(a) * radii[ring], z = m_hover->pos.z + std::sin(a) * radii[ring];
            const XMFLOAT3 p{ x, m_terrain.HeightAt(x, z).value_or(m_hover->pos.y) + 0.4f, z };
            if (i) { lines.push_back({ prev, ringColors[ring] }); lines.push_back({ p, ringColors[ring] }); }
            prev = p;
        }
    }
}

void App::DrawViewport(float dt)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
    const bool open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (size.x < 8 || size.y < 8) { ImGui::End(); return; }
    EnsureViewportTarget(UINT(size.x), UINT(size.y));

    // The scene image first so the gizmo draws over it. The gizmo runs before the scene button: ImGuizmo only
    // takes a click when no item is hovered, so the button is left out while the mouse is on a handle.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Drawn without blending: the scene's alpha channel holds whatever the textures carried (specular and
    // transparency masks), not coverage, and ImGui would otherwise let the window background show through it.
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd* cmd) {
        static_cast<ID3D11DeviceContext*>(cmd->UserCallbackData)->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }, m_context);
    dl->AddImage(ImTextureID(intptr_t(m_vpSrv.Get())), origin, { origin.x + size.x, origin.y + size.y });
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    const bool gizmoHot = UpdateGizmo(origin, size);
    if (gizmoHot) ImGui::Dummy(size);
    else ImGui::InvisibleButton("##scene", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = gizmoHot || ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();

    // Camera: right drag looks, WASD/QE move while the viewport has the mouse or focus.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_looking = true;
    // A right click that did not turn the camera stops placing (like Esc).
    if (m_looking && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16)
    {
        if (m_armed) { m_armed.reset(); Log("Stopped placing."); }
        else if (m_spawnArmed && SpawnTool()) { m_spawnArmed.reset(); Log("Stopped placing."); }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_looking = false;
    FollowTopDown();
    // Top-down grabs the map instead of looking around: right or middle drag pans it under the cursor (Shift+right
    // drag turns it), keys glide a screen a second whatever the zoom, and the wheel zooms smoothly towards the cursor.
    const float topYd = 2.0f * m_topHeight * std::tan(XMConvertToRadians(30.0f)) / size.y;   // top-down: yards per pixel
    const XMVECTOR heading = m_camera.Heading(), across = XMVector3Normalize(XMVector3Cross(heading, XMVectorSet(0, 1, 0, 0)));
    if (m_camera.topDown && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_panning = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_panning = false;
    if (m_camera.topDown && ((m_looking && !io.KeyShift) || m_panning))
    {
        XMVECTOR p = XMLoadFloat3(&m_camera.pos);
        p = XMVectorSubtract(p, XMVectorScale(across, io.MouseDelta.x * topYd));
        p = XMVectorAdd(p, XMVectorScale(heading, io.MouseDelta.y * topYd));
        XMStoreFloat3(&m_camera.pos, p);
        m_topVel = { 0, 0 };
    }
    else if (m_looking)
    {
        m_camera.yaw -= io.MouseDelta.x * 0.005f;
        if (m_camera.topDown) m_topYaw = m_camera.yaw;
        if (!m_camera.topDown) m_camera.pitch = std::clamp(m_camera.pitch - io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
    }
    if ((hovered || m_looking || ImGui::IsWindowFocused()) && !io.WantTextInput)
    {
        const float speed = m_camera.speed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 5.0f : 1.0f) * dt;
        const XMVECTOR fwd = m_camera.Forward();
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(fwd, XMVectorSet(0, 1, 0, 0)));
        XMVECTOR p = XMLoadFloat3(&m_camera.pos);
        if (m_camera.topDown)
        {
            const auto key = [](ImGuiKey k) { return ImGui::IsKeyDown(k) ? 1.0f : 0.0f; };
            const float pace = topYd * size.y * (io.KeyShift ? 3.0f : 1.0f) * !io.KeyCtrl;   // a screen height a second
            const float ease = 1.0f - std::exp(-dt * 12.0f);
            m_topVel.x += ((key(ImGuiKey_D) - key(ImGuiKey_A)) * pace - m_topVel.x) * ease;
            m_topVel.y += ((key(ImGuiKey_W) - key(ImGuiKey_S)) * pace - m_topVel.y) * ease;
            p = XMVectorAdd(p, XMVectorAdd(XMVectorScale(across, m_topVel.x * dt), XMVectorScale(heading, m_topVel.y * dt)));
            if (!io.KeyCtrl) m_topGoal = std::clamp(m_topGoal * std::exp((key(ImGuiKey_E) - key(ImGuiKey_Q)) * dt * 1.5f), 30.0f, 3000.0f);   // E out, Q in
        }
        else if (!io.KeyCtrl)
        {
            if (ImGui::IsKeyDown(ImGuiKey_W)) p = XMVectorAdd(p, XMVectorScale(fwd, speed));
            if (ImGui::IsKeyDown(ImGuiKey_S)) p = XMVectorSubtract(p, XMVectorScale(fwd, speed));
            if (ImGui::IsKeyDown(ImGuiKey_D)) p = XMVectorAdd(p, XMVectorScale(right, speed));
            if (ImGui::IsKeyDown(ImGuiKey_A)) p = XMVectorSubtract(p, XMVectorScale(right, speed));
            if (ImGui::IsKeyDown(ImGuiKey_E)) p = XMVectorAdd(p, XMVectorSet(0, speed, 0, 0));
            if (ImGui::IsKeyDown(ImGuiKey_Q)) p = XMVectorSubtract(p, XMVectorSet(0, speed, 0, 0));
        }
        if (hovered && io.MouseWheel != 0)
        {
            if (io.KeyCtrl && m_tool == Tool::Water) m_water.radius = std::clamp(m_water.radius * (io.MouseWheel > 0 ? 1.15f : 0.87f), 1.0f, 150.0f);
            else if (io.KeyCtrl && m_tool == Tool::Holes) m_holeRadius = std::clamp(m_holeRadius * (io.MouseWheel > 0 ? 1.2f : 0.83f), 1.0f, 60.0f);
            else if (io.KeyCtrl && m_tool == Tool::Zones) m_areaRadius = std::clamp(m_areaRadius * (io.MouseWheel > 0 ? 1.15f : 0.87f), 1.0f, 300.0f);
            else if (io.KeyCtrl && m_tool == Tool::Paint) m_paint.radius = std::clamp(m_paint.radius * (io.MouseWheel > 0 ? 1.15f : 0.87f), 1.0f, 150.0f);
            else if (io.KeyCtrl && m_tool == Tool::Shade) m_shadeBrush.radius = std::clamp(m_shadeBrush.radius * (io.MouseWheel > 0 ? 1.15f : 0.87f), 1.0f, 150.0f);
            else if (io.KeyCtrl) m_brush.radius = std::clamp(m_brush.radius * (io.MouseWheel > 0 ? 1.15f : 0.87f), 1.0f, 300.0f);
            else if (io.KeyAlt && m_tool == Tool::Copy) m_pasteOffset = std::clamp(m_pasteOffset + io.MouseWheel * (io.KeyShift ? 0.1f : 0.5f), -50.0f, 50.0f);
            else if (m_looking)   // flying: the wheel sets the speed
            {
                m_camera.speed = std::clamp(m_camera.speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 10.0f, 1000.0f);
                m_speedShownUntil = ImGui::GetTime() + 1.5;
            }
            else if (m_camera.topDown) m_topGoal = std::clamp(m_topGoal * std::pow(0.8f, io.MouseWheel), 30.0f, 3000.0f);
            else p = XMVectorAdd(p, XMVectorScale(fwd, io.MouseWheel * 20.0f));
        }
        XMStoreFloat3(&m_camera.pos, p);
    }
    else m_topVel = { 0, 0 };
    if (m_camera.topDown && m_topGoal != m_topHeight)   // the zoom eases to its goal, the ground under the cursor staying put
    {
        float h = m_topHeight + (m_topGoal - m_topHeight) * (1.0f - std::exp(-dt * 14.0f));
        if (std::fabs(m_topGoal - h) < 0.05f) h = m_topGoal;
        XMVECTOR p = XMLoadFloat3(&m_camera.pos);
        if (hovered)
        {
            const float keep = 1.0f - h / m_topHeight;
            const float ox = (io.MousePos.x - origin.x - size.x / 2) * topYd, oy = (io.MousePos.y - origin.y - size.y / 2) * topYd;
            p = XMVectorAdd(p, XMVectorSubtract(XMVectorScale(across, ox * keep), XMVectorScale(heading, oy * keep)));
        }
        XMStoreFloat3(&m_camera.pos, XMVectorAdd(p, XMVectorSet(0, h - m_topHeight, 0, 0)));
        m_topHeight = h;
    }
    if (m_camera.topDown) m_topSeen = { m_camera.pos, m_camera.pitch };   // our own moves are not a feature's

    // Picking under the mouse.
    const float aspect = size.x / size.y;
    const XMMATRIX proj = Projection(aspect, 1.0f, 6000.0f);
    const XMMATRIX viewProj = m_camera.View() * proj;
    m_hover.reset();
    if (hovered && !m_looking)
    {
        const float mx = (io.MousePos.x - origin.x) / size.x * 2 - 1, my = 1 - (io.MousePos.y - origin.y) / size.y * 2;
        const XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);
        const XMVECTOR p0 = XMVector3TransformCoord(XMVectorSet(mx, my, 0, 1), inv);
        const XMVECTOR p1 = XMVector3TransformCoord(XMVectorSet(mx, my, 1, 1), inv);
        const XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(p1, p0));
        m_hover = m_terrain.Pick(p0, dir, m_tool == Tool::Holes);
        if (const Ghosts::Layer* solo = m_soloLayer ? m_ghosts.Find(m_soloLayer) : nullptr)
        {
            // Soloing: aim at the ground on screen (the ghost's), and act on the map's chunk at that spot.
            m_hover.reset();
            if (const auto hit = TerrainAdapter::PickIn(solo->tiles, p0, dir, m_tool == Tool::Holes))
                if (const auto ref = m_terrain.ChunkAtGrid(int(std::floor(hit->pos.x / kChunkSize)), int(std::floor(hit->pos.z / kChunkSize))))
                    m_hover = TerrainHit{ hit->pos, *ref };
        }
        // Spawns, paths, triggers, POIs and flight nodes stand on WMO floors too (bridges, buildings, dungeons): the nearer
        // of the terrain and a WMO under the cursor. Terrain tools keep aiming at the terrain.
        if (SpawnTool() || m_tool == Tool::Triggers || m_tool == Tool::Pois || m_tool == Tool::Flights || m_tool == Tool::Lights || m_tool == Tool::Sound)
        {
            ModelRenderer::DrawSettings wmos = m_modelSettings;
            wmos.doodads = false;
            const float ground = m_hover ? XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&m_hover->pos), p0))) : 6000.0f;
            if (const auto hit = m_models.Pick(p0, dir, ground, wmos); hit && hit->wmo)
            {
                XMFLOAT3 pos;
                XMStoreFloat3(&pos, XMVectorAdd(p0, XMVectorScale(dir, hit->distance)));
                m_hover = TerrainHit{ pos, ChunkRef{ -1, -1 } };   // no terrain chunk here
            }
        }
        m_objHover.reset();
        if ((m_tool == Tool::Objects && !gizmoHot) || (m_tool == Tool::Zones && io.KeyShift))
        {
            const float ground = m_hover ? XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&m_hover->pos), p0))) : 6000.0f;
            if (const auto hit = m_models.Pick(p0, dir, ground, m_modelSettings)) m_objHover = ObjectRef{ hit->wmo, hit->uid };
        }
        m_spawnHover.reset();
        if (SpawnTool() && m_showSpawns[int(m_spawnKind)])
        {
            // The marker under the cursor, else the spawn's model (its body or anything it carries).
            m_spawnHover = SpawnAt(io.MousePos, origin, size, viewProj);
            const float ground = m_hover ? XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&m_hover->pos), p0))) : 6000.0f;
            if (!m_spawnHover)
                if (const auto hit = m_models.PickTile(SpawnTileKey(m_spawnKind), p0, dir, ground)) m_spawnHover = hit->uid;
        }
    }

    // The armed catalog model follows the cursor.
    m_models.RemoveTile(-2);
    if (m_armed && m_tool == Tool::Objects && m_hover)
    {
        Adt preview;
        const float pos[3] = { m_hover->pos.x, m_hover->pos.y, m_hover->pos.z }, rot[3] = { 0, m_placeYaw, 0 };
        if (m_armed->wmo) preview.wmos.push_back({ m_armed->path, { pos[0], pos[1], pos[2] }, { rot[0], rot[1], rot[2] }, {}, {}, 0, 0 });
        else preview.doodads.push_back({ m_armed->path, { pos[0], pos[1], pos[2] }, { rot[0], rot[1], rot[2] }, m_placeScale, 0 });
        m_models.AddTile(-2, preview, m_mpq);
    }

    // Tools.
    if (m_tool == Tool::Sculpt)
    {
        if (m_brush.mode == Brush::Mode::Vertices)   // drag selects, Ctrl+drag deselects
        {
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.SelectVertices(m_hover->pos, m_brush.radius, !io.KeyCtrl);
        }
        else if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover && io.KeyAlt && m_brush.mode == Brush::Mode::Flatten)
        {
            m_brush.height = m_hover->pos.y;   // Alt+click: flatten to this height
            m_brush.fixedHeight = true;
        }
        else if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.BeginStroke(*m_hover);
        if (m_terrain.Stroking() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.StrokeStep(m_hover->pos, m_brush, dt);
        if (m_terrain.Stroking() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            if (auto change = m_terrain.EndStroke(m_brush)) m_store.Commit(std::move(*change));
    }
    else if (m_tool == Tool::Paint && m_swapBrush)
    {
        // Swap: drag swaps under the brush, Alt+click picks the texture to swap out.
        const std::string to = m_swapRemove ? std::string() : m_activeTexture;
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
        {
            if (io.KeyAlt)
            {
                if (const auto t = m_terrain.TextureAt(m_hover->pos.x, m_hover->pos.z)) m_swapFrom = *t;
            }
            else if (m_swapFrom.empty() || (to.empty() && !m_swapRemove))
                Log("Swap needs a texture to swap out (Alt+click the ground) and one to put in (the active texture).");
            else
                m_terrain.BeginPaint();
        }
        if (m_terrain.Painting() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.SwapStep(m_hover->pos, m_paint.radius, m_swapFrom, to);
        if (m_terrain.Painting() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            if (auto change = m_terrain.EndPaint(SwapLabel()))
            {
                Log("%s (%zu chunks)", change->label.c_str(), change->data.at("layers").size());
                m_store.Commit(std::move(*change));
            }
    }
    else if (m_tool == Tool::Paint)
    {
        // Drag paints the active texture, Ctrl+drag removes it, Alt+click picks the texture under the cursor.
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
        {
            if (io.KeyAlt)
            {
                if (const auto t = m_terrain.TextureAt(m_hover->pos.x, m_hover->pos.z)) PickTexture(*t);
            }
            else if (m_activeTexture.empty())
                Log("Pick a ground texture first: Catalog > Ground textures, or Alt+click the ground.");
            else
                m_terrain.BeginPaint();
        }
        if (m_terrain.Painting() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
            m_terrain.PaintStep(m_hover->pos, m_paint, m_activeTexture, io.KeyCtrl, dt);
        if (m_terrain.Painting() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            if (auto change = m_terrain.EndPaint((io.KeyCtrl ? "Erase " : "Paint ") + FileOf(m_activeTexture)))
            {
                Log("%s (%zu chunks)", change->label.c_str(), change->data.at("layers").size());
                m_store.Commit(std::move(*change));
            }
    }
    else if (m_tool == Tool::Shade)
    {
        // Drag tints towards the colour, Ctrl+drag returns to neutral, Alt+click picks the colour under the cursor.
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
        {
            if (io.KeyAlt)
            {
                if (const auto c = m_terrain.ShadeAt(m_hover->pos.x, m_hover->pos.z)) m_shadeColor = *c;
            }
            else if (!m_terrain.VertexColors())
                Log("%s has no vertex shading: the client ignores vertex colours on this map.", m_terrain.Map().c_str());
            else
                m_terrain.BeginShade();
        }
        if (m_terrain.Shading() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
            m_terrain.ShadeStep(m_hover->pos, m_shadeBrush, m_shadeColor, io.KeyCtrl, dt);
        if (m_terrain.Shading() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            if (auto change = m_terrain.EndShade()) m_store.Commit(std::move(*change));
    }
    else if (SpawnTool())
    {
        SpawnsViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Water)
    {
        // Drag paints; Ctrl swaps Add and Remove; Alt+click takes the level and liquid under the cursor.
        WaterBrush b = m_water;
        if (io.KeyCtrl && b.mode != WaterBrush::Mode::Level) b.mode = b.mode == WaterBrush::Mode::Add ? WaterBrush::Mode::Remove : WaterBrush::Mode::Add;
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover)
        {
            const auto there = m_terrain.WaterAt(m_hover->pos.x, m_hover->pos.z);
            if (io.KeyAlt)
            {
                m_water.level = there ? there->first : m_hover->pos.y;
                if (there) m_water.type = there->second;
            }
            else
            {
                if (b.mode == WaterBrush::Mode::Add && m_waterFromClick)
                {
                    // Match the water the stroke starts on, or the nearest water around: same liquid, same level, no
                    // seam. Nothing near: the ground there plus the depth, in the chosen liquid.
                    const auto match = there ? there : m_terrain.NearestWater(m_hover->pos.x, m_hover->pos.z, kWaterMatchRadius);
                    m_water.level = b.level = match ? match->first : m_hover->pos.y + m_waterDepth;
                    if (match) m_water.type = b.type = match->second;
                }
                m_terrain.BeginWater(m_hover->pos);
            }
        }
        if (m_terrain.Watering() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.WaterStep(m_hover->pos, b);
        if (m_terrain.Watering() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            if (auto change = m_terrain.EndWater(b))
            {
                Log("%s", change->label.c_str());
                m_store.Commit(std::move(*change));
            }
    }
    else if (m_tool == Tool::Holes)
    {
        const bool cut = m_holeCut != io.KeyCtrl;
        if (m_impassMode)
        {
            if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.BeginImpass();
            if (m_terrain.Impassing() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.ImpassStep(m_hover->pos, m_holeRadius, cut);
            if (m_terrain.Impassing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                if (auto change = m_terrain.EndImpass())
                {
                    Log("%s", change->label.c_str());
                    m_store.Commit(std::move(*change));
                }
        }
        else if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.BeginHoles();
        if (m_terrain.HoleStroking() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_hover) m_terrain.HoleStep(m_hover->pos, m_holeRadius, cut);
        if (m_terrain.HoleStroking() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            if (auto change = m_terrain.EndHoles()) m_store.Commit(std::move(*change));
    }
    else if (m_tool == Tool::Zones)
    {
        ZonesViewport();
    }
    else if (m_tool == Tool::Triggers)
    {
        TriggersViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Pois)
    {
        PoisViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Flights)
    {
        FlightsViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Lights)
    {
        LightsViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Sound)
    {
        SoundViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Roads)
    {
        RoadsViewport(origin, size, viewProj);
    }
    else if (m_tool == Tool::Objects)
    {
        // Objects: click picks, dragging draws a box; Shift adds, Ctrl removes, Alt+click moves the selection there. The handles move, turn and scale.
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_boxStart[0] = io.MousePos.x;
            m_boxStart[1] = io.MousePos.y;
            m_boxing = false;
            m_objPress = m_objHover;
        }
        const bool leftHeld = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (leftHeld && !io.KeyAlt && std::fabs(io.MousePos.x - m_boxStart[0]) + std::fabs(io.MousePos.y - m_boxStart[1]) > 5) m_boxing = true;
        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (m_boxing)
            {
                const float x0 = std::min(m_boxStart[0], io.MousePos.x), x1 = std::max(m_boxStart[0], io.MousePos.x);
                const float y0 = std::min(m_boxStart[1], io.MousePos.y), y1 = std::max(m_boxStart[1], io.MousePos.y);
                std::set<ObjectRef> hits;
                m_models.ForEachObject([&](bool wmo, uint32_t uid, const XMFLOAT3& c) {
                    if (wmo ? !m_modelSettings.wmos : !m_modelSettings.doodads) return;
                    const XMVECTOR clip = XMVector4Transform(XMVectorSet(c.x, c.y, c.z, 1), viewProj);
                    const float w = XMVectorGetW(clip);
                    if (w <= 0 || w > m_modelSettings.distance) return;
                    const float sx = origin.x + (XMVectorGetX(clip) / w + 1) / 2 * size.x, sy = origin.y + (1 - XMVectorGetY(clip) / w) / 2 * size.y;
                    if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) hits.insert({ wmo, uid });
                });
                if (!io.KeyShift && !io.KeyCtrl) m_objSel.clear();
                for (const ObjectRef& r : hits)
                    if (io.KeyCtrl) m_objSel.erase(r); else m_objSel.insert(r);
            }
            else if (io.KeyAlt && !m_objSel.empty() && m_hover)
                MoveSelectionTo(m_hover->pos);   // Alt+click: the selection goes there, as in every tool that moves things
            else if (m_armed)
                PlaceFromCatalog(io.KeyShift);
            else if (m_objPress)
            {
                if (io.KeyCtrl) m_objSel.erase(*m_objPress);
                else if (io.KeyShift) m_objSel.insert(*m_objPress);
                else m_objSel = { *m_objPress };
            }
            else if (!io.KeyShift && !io.KeyCtrl)
                m_objSel.clear();
            m_boxing = false;
            m_objPress.reset();
        }
    }
    else
    {
        // Select and Copy: click picks one chunk, dragging draws a box. Shift adds, Ctrl removes.
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_boxStart[0] = io.MousePos.x;
            m_boxStart[1] = io.MousePos.y;
            m_boxing = false;
        }
        const bool leftHeld = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (leftHeld && std::fabs(io.MousePos.x - m_boxStart[0]) + std::fabs(io.MousePos.y - m_boxStart[1]) > 5) m_boxing = true;

        if (m_tool == Tool::Copy && m_placing && !m_comparing && !m_clipboard.Empty() && !m_boxing && ImGui::IsItemDeactivated() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (m_hover) m_pin = m_terrain.GridOf(m_hover->chunk);   // pin (or move the pin) here
        }
        else if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            std::set<ChunkRef> hits;
            if (m_boxing)
            {
                const float x0 = std::min(m_boxStart[0], io.MousePos.x), x1 = std::max(m_boxStart[0], io.MousePos.x);
                const float y0 = std::min(m_boxStart[1], io.MousePos.y), y1 = std::max(m_boxStart[1], io.MousePos.y);

                // Occlusion: last frame's depth buffer (same camera) tells what the viewport actually shows.
                std::vector<float> depth;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (m_vpDepthStaging)
                {
                    m_context->CopyResource(m_vpDepthStaging.Get(), m_vpDepth.Get());
                    if (SUCCEEDED(m_context->Map(m_vpDepthStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        depth.resize(size_t(m_vpWidth) * m_vpHeight);
                        for (UINT row = 0; row < m_vpHeight; ++row)
                            memcpy(depth.data() + size_t(row) * m_vpWidth, static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch,
                                   m_vpWidth * sizeof(float));
                        m_context->Unmap(m_vpDepthStaging.Get(), 0);
                    }
                }
                constexpr float kNear = 1.0f, kFar = 6000.0f;
                const float orthoFar = std::max(kFar, m_topHeight + 3000.0f);   // as Projection
                auto linear = [&](float d) {
                    return m_camera.topDown ? -3000.0f + d * (orthoFar + 3000.0f) : kNear * kFar / (kFar - d * (kFar - kNear));
                };

                // A chunk counts when any of five sample points is inside the box and not hidden by terrain.
                const size_t samples[5] = { 4 * 17 + 9 + 3, 1 * 17 + 9 + 1, 1 * 17 + 9 + 6, 6 * 17 + 9 + 1, 6 * 17 + 9 + 6 };
                for (const auto& [key, tile] : m_terrain.Tiles())
                    for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
                    {
                        const AdtChunk* shown = ShownChunk({ key, int(ci) });
                        if (!shown) continue;
                        const AdtChunk& c = *shown;
                        for (size_t j : samples)
                        {
                            const size_t row = j / 17, col = j % 17 - 9;
                            const XMVECTOR world = XMVectorSet(c.baseX + (col + 0.5f) * kUnitSize, c.baseY + c.heights[j] + 0.5f,
                                                               c.baseZ + (row + 0.5f) * kUnitSize, 1);
                            const XMVECTOR clip = XMVector4Transform(world, viewProj);
                            const float w = XMVectorGetW(clip);
                            if (w <= 0) continue;
                            const float sx = origin.x + (XMVectorGetX(clip) / w + 1) / 2 * size.x;
                            const float sy = origin.y + (1 - XMVectorGetY(clip) / w) / 2 * size.y;
                            if (sx < x0 || sx > x1 || sy < y0 || sy > y1) continue;
                            if (!depth.empty())
                            {
                                const int px = std::clamp(int(sx - origin.x), 0, int(m_vpWidth) - 1);
                                const int py = std::clamp(int(sy - origin.y), 0, int(m_vpHeight) - 1);
                                const float shown = linear(depth[size_t(py) * m_vpWidth + px]);
                                const float point = linear(XMVectorGetZ(clip) / w);
                                if (point > shown + 2.0f + shown * 0.01f) continue;   // something nearer covers it
                            }
                            hits.insert({ key, int(ci) });
                            break;
                        }
                    }
            }
            else if (m_hover)
                hits.insert(m_hover->chunk);
            if (m_boxing || m_hover || (!io.KeyShift && !io.KeyCtrl)) ApplySelection(hits);
            m_boxing = false;
        }
    }

    if (m_terrain.TakeFarChanged())   // tiles added or taken away: their low-detail heights changed
    {
        const auto wdl = m_mpq.Read("World\\Maps\\" + m_terrain.Map() + "\\" + m_terrain.Map() + ".wdl");
        m_renderer.LoadFar(wdl ? ParseWdl(*wdl) : std::vector<std::vector<int16_t>>{});
    }
    UpdateDifferences();
    UpdateCompare();
    UpdatePlacement();

    // Render the scene into the viewport texture.
    std::vector<LineVertex> overlay;
    BuildOverlay(overlay);
    const D3D11_VIEWPORT vp{ 0, 0, size.x, size.y, 0, 1 };
    const float sky[4] = { 0.36f, 0.52f, 0.72f, 1 };
    m_context->OMSetRenderTargets(1, m_vpRtv.GetAddressOf(), m_vpDsv.Get());
    m_context->RSSetViewports(1, &vp);
    m_context->ClearRenderTargetView(m_vpRtv.Get(), sky);
    m_context->ClearDepthStencilView(m_vpDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    UpdateSceneLight();
    m_renderer.DrawSky(viewProj);
    if (m_drawOptions.farTerrain && !m_soloLayer)
    {
        // The low-detail map first, with a projection reaching across the continent; depth then starts over for the
        // detailed scene, so the near pass keeps its precision.
        m_renderer.DrawFar(m_camera.View() * Projection(size.x / size.y, 100.0f, 40000.0f));
        m_context->ClearDepthStencilView(m_vpDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
    m_drawOptions.eye = ViewEye();
    m_renderer.Draw(viewProj, m_drawOptions);
    m_models.Draw(viewProj, ViewEye(), m_modelSettings);
    {
        std::vector<LineVertex> solids;
        BuildPathSolids(solids);
        if (m_tool == Tool::Flights) BuildFlightSolids(solids);
        m_renderer.DrawSolids(viewProj, solids);
    }
    m_renderer.DrawWater(viewProj, m_soloLayer);
    m_renderer.DrawOverlay(viewProj, overlay);

    if (m_boxing)
    {
        const ImVec2 a{ m_boxStart[0], m_boxStart[1] }, b = io.MousePos;
        const ImVec2 lo{ std::min(a.x, b.x), std::min(a.y, b.y) }, hi{ std::max(a.x, b.x), std::max(a.y, b.y) };
        const ImU32 fill = io.KeyCtrl ? IM_COL32(255, 90, 90, 40) : IM_COL32(80, 160, 255, 40);
        const ImU32 edge = io.KeyCtrl ? IM_COL32(255, 110, 110, 220) : IM_COL32(110, 180, 255, 220);
        dl->AddRectFilled(lo, hi, fill);
        dl->AddRect(lo, hi, edge, 0.0f, ImDrawFlags_None, 1.5f);
    }

    DrawSpawnLabels(dl, origin, size, viewProj);
    DrawPathLabels(dl, origin, size, viewProj);
    if (m_tool == Tool::Triggers) DrawTriggerLabels(dl, origin, size, viewProj);
    if (m_tool == Tool::Pois) DrawPoiLabels(dl, origin, size, viewProj);
    if (m_tool == Tool::Flights) DrawFlightLabels(dl, origin, size, viewProj);

    // Corner caption and empty state.
    const ImVec2 pad{ origin.x + 12, origin.y + 10 };
    if (!m_terrain.Map().empty())
    {
        const char* tools[] = { "Select", "Sculpt", "Copy", "Holes", "Objects", "Paint", "Creatures", "Gameobjects", "Zones", "Triggers", "POIs", "Flights", "Shade", "Lights", "Sound", "Roads", "Water" };
        const char* modes[] = { "Raise", "Lower", "Flatten", "Smooth", "Vertices" };
        char caption[400];
        if (m_tool == Tool::Sculpt)
            snprintf(caption, sizeof caption, "%s   World > Sculpt > %s", m_terrain.Map().c_str(), modes[int(m_brush.mode)]);
        else if (m_tool == Tool::Paint)
            snprintf(caption, sizeof caption, "%s   World > Paint > %s   %s   Ctrl: erase   Alt+click: pick   Ctrl+wheel: radius", m_terrain.Map().c_str(),
                     m_activeTexture.empty() ? "(no texture)" : FileOf(m_activeTexture).c_str(), io.KeyCtrl ? "ERASING" : "");
        else if (m_tool == Tool::Shade)
            snprintf(caption, sizeof caption, "%s   World > Shade   %s   Ctrl: back to neutral   Alt+click: pick   Ctrl+wheel: radius", m_terrain.Map().c_str(),
                     !m_terrain.VertexColors() ? "(this map has no vertex shading)" : io.KeyCtrl ? "ERASING" : "");
        else if (m_tool == Tool::Water)
        {
            const char* modes[] = { "Add", "Remove", "Level" };
            const LiquidInfo* l = m_terrain.Liquid(m_water.type);
            snprintf(caption, sizeof caption, "%s   World > Water > %s %s at %.1f   Ctrl: %s   Alt+click: pick level   Ctrl+wheel: radius", m_terrain.Map().c_str(),
                     modes[int(m_water.mode)], l ? l->name.c_str() : "?", m_water.level, m_water.mode == WaterBrush::Mode::Add ? "remove" : m_water.mode == WaterBrush::Mode::Remove ? "add" : "-");
        }
        else if (m_tool == Tool::Holes && m_impassMode)
            snprintf(caption, sizeof caption, "%s   World > Holes > Impassable: %s   (Ctrl: %s)", m_terrain.Map().c_str(),
                     m_holeCut != io.KeyCtrl ? "set" : "clear", m_holeCut != io.KeyCtrl ? "clear" : "set");
        else if (m_tool == Tool::Holes)
            snprintf(caption, sizeof caption, "%s   World > Holes > %s   (Ctrl: %s)", m_terrain.Map().c_str(),
                     m_holeCut != io.KeyCtrl ? "Cut" : "Fill", m_holeCut != io.KeyCtrl ? "fill" : "cut");
        else if (m_tool == Tool::Zones)
            snprintf(caption, sizeof caption, "%s   World > Zones > Paint %s   Alt+click: pick   Ctrl+wheel: radius", m_terrain.Map().c_str(),
                     m_activeArea ? AreaLabel(m_activeArea).c_str() : "(no area)");
        else if (m_tool == Tool::Triggers && m_triggerPick != TriggerPick::None)
        {
            const char* picks[] = { "", "click the ground: place a new trigger   Shift+click: place and keep going   Esc: stop",
                                    "click the ground: move the trigger there   Esc: stop", "click the ground: the teleport's arrival point   Esc: stop",
                                    "click the ground: where dead players' spirits appear   Esc: stop" };
            snprintf(caption, sizeof caption, "%s   World > Triggers   %s", m_terrain.Map().c_str(), picks[int(m_triggerPick)]);
        }
        else if (m_tool == Tool::Flights && m_flightPlace)
            snprintf(caption, sizeof caption, "%s   World > Flights   click the ground: place a flight node   Shift+click: place and keep going   Esc: stop", m_terrain.Map().c_str());
        else if (m_tool == Tool::Pois && m_poiPick != PoiPick::None)
        {
            const char* picks[] = { "", "click the ground: place a new point   Shift+click: place and keep going   Esc: stop",
                                    "click the ground: move the point there   Esc: stop" };
            snprintf(caption, sizeof caption, "%s   World > POIs   %s", m_terrain.Map().c_str(), picks[int(m_poiPick)]);
        }
        else if (m_tool == Tool::Roads && m_roadDraw)
            snprintf(caption, sizeof caption, "%s   World > Roads   click the ground: add a point   Ctrl+click: insert between   Enter / right-click: done", m_terrain.Map().c_str());
        else if (m_tool == Tool::Sound && m_emitterPlace)
            snprintf(caption, sizeof caption, "%s   World > Sound   click the ground: add an emitter   Shift+click: add and keep going   Esc: stop", m_terrain.Map().c_str());
        else if (m_tool == Tool::Lights && m_lightPlace)
            snprintf(caption, sizeof caption, "%s   World > Lights   click the ground: add a light   Shift+click: add and keep going   Esc: stop", m_terrain.Map().c_str());
        else if (m_tool == Tool::Objects && m_armed)
            snprintf(caption, sizeof caption, "%s   World > Objects > Place %s   click: place   Shift+click: place and keep going   Esc: stop",
                     m_terrain.Map().c_str(), FileOf(m_armed->path).c_str());
        else if (TransformTool() && !(SpawnTool() && m_spawnArmed))
        {
            // Every tool that moves things: the same caption and keys.
            const size_t selected = m_tool == Tool::Objects ? m_objSel.size() : SpawnTool() ? m_spawnSel.size()
                                  : m_tool == Tool::Triggers ? size_t(m_triggerSel != 0) : m_tool == Tool::Flights ? size_t(m_flightNode || m_flightPoint)
                                  : m_tool == Tool::Lights ? size_t(m_lightSel != 0) : m_tool == Tool::Sound ? size_t(m_emitterSel != 0)
                                  : m_tool == Tool::Roads ? size_t(m_roadSel != 0) : size_t(m_poiSel != 0);
            snprintf(caption, sizeof caption, "%s   World > %s > %s (%s)   %zu selected   %s", m_terrain.Map().c_str(), tools[int(m_tool)],
                     m_gizmo == Gizmo::Move ? "Move" : m_gizmo == Gizmo::Rotate ? "Rotate" : "Scale", m_gizmoLocal ? "local" : "world", selected,
                     TransformHint().c_str());
        }
        else
            snprintf(caption, sizeof caption, "%s   World > %s   %s", m_terrain.Map().c_str(), tools[int(m_tool)],
                     m_tool != Tool::Copy || !m_placing ? (std::to_string(m_selection.size()) + " selected").c_str()
                     : m_pin ? "PINNED: R rotate, arrows move, Alt+wheel height, Enter commit, Esc cancel"
                             : "Placing: click to pin, Esc to stop");
        dl->AddRectFilled({ pad.x - 6, pad.y - 4 }, { pad.x + ImGui::CalcTextSize(caption).x + 6, pad.y + ImGui::GetTextLineHeight() + 4 },
                          IM_COL32(0, 0, 0, 120), 4);
        if (std::string text = caption; text.find("World > ") != std::string::npos)   // the tool's group leads the path
        {
            text.replace(text.find("World > "), 8, std::string(kGroupNames[int(GroupOf(m_tool))]) + " > ");
            snprintf(caption, sizeof caption, "%s", text.c_str());
        }
        dl->AddText(pad, IM_COL32(235, 238, 242, 255), caption);
        if (const Ghosts::Layer* solo = m_soloLayer ? m_ghosts.Find(m_soloLayer) : nullptr)
        {
            const std::string line = m_soloLayer == m_diffNewLayer
                ? "NEW TILES: " + std::to_string(m_diffNewTiles.size()) + " tile(s) from " + m_diffTarget.label +
                      " this map does not have   Enter add them   Del reject   Esc close"
                : "SOLO: " + solo->label + "   (read-only view; edits go to the map; turn Solo off in Versions)";
            const ImVec2 at{ pad.x, pad.y + ImGui::GetTextLineHeight() + 10 };
            dl->AddRectFilled({ at.x - 6, at.y - 4 }, { at.x + ImGui::CalcTextSize(line.c_str()).x + 6, at.y + ImGui::GetTextLineHeight() + 4 },
                              IM_COL32(150, 70, 0, 200), 4);
            dl->AddText(at, IM_COL32(255, 240, 220, 255), line.c_str());
        }
        if (m_comparing && m_compareAt < m_compare.size())
        {
            const CompareEntry& e = m_compare[m_compareAt];
            char line[400];
            if (!m_compareAt)
                snprintf(line, sizeof line, "COMPARE %zu/%zu: %s   [ ] cycle   Esc stop", m_compareAt, m_compare.size() - 1, e.label.c_str());
            else if (!e.diff.cells)
                snprintf(line, sizeof line, "COMPARE %zu/%zu: %s   %s   [ ] cycle   Esc stop", m_compareAt, m_compare.size() - 1, e.label.c_str(),
                         e.ready ? "does not cover the selection" : "loading...");
            else
                snprintf(line, sizeof line, "COMPARE %zu/%zu: %s   %zu/%zu chunks differ   height %.1f/%.1f yd   edge %.1f yd   water %zu   objects +%zu -%zu%s   [ ] cycle   Enter paste   Esc stop",
                         m_compareAt, m_compare.size() - 1, e.label.c_str(), e.diff.changed, e.diff.cells, e.diff.meanHeight, e.diff.maxHeight,
                         e.diff.maxEdge, e.diff.water, e.diff.newDoodads.size() + e.diff.newWmos.size(), e.diff.goneDoodads + e.diff.goneWmos,
                         e.assetsMissing ? ("   " + std::to_string(e.assetsMissing) + " asset(s) missing").c_str() : "");
            const ImVec2 at{ pad.x, pad.y + 2 * (ImGui::GetTextLineHeight() + 10) };
            dl->AddRectFilled({ at.x - 6, at.y - 4 }, { at.x + ImGui::CalcTextSize(line).x + 6, at.y + ImGui::GetTextLineHeight() + 4 },
                              IM_COL32(20, 70, 140, 210), 4);
            dl->AddText(at, IM_COL32(230, 240, 255, 255), line);
        }
    }
    else
    {
        if (!m_project) DrawStartScreen(origin, size);
        else
        {
            const char* line1 = "Pick a map in the Maps panel";
            const char* line2 = "Click a tile in its grid to fly there";
            const ImVec2 s1 = ImGui::CalcTextSize(line1), s2 = ImGui::CalcTextSize(line2);
            const ImVec2 c{ origin.x + size.x / 2, origin.y + size.y / 2 };
            dl->AddText({ c.x - s1.x / 2, c.y - s1.y }, IM_COL32(240, 242, 246, 255), line1);
            dl->AddText({ c.x - s2.x / 2, c.y + 4 }, IM_COL32(200, 205, 212, 200), line2);
        }
    }
    if (ImGui::GetTime() < m_speedShownUntil)
    {
        char text[48];
        snprintf(text, sizeof text, "Camera speed %.0f yd/s", m_camera.speed);
        const ImVec2 s = ImGui::CalcTextSize(text), at{ origin.x + (size.x - s.x) / 2, origin.y + size.y - s.y - 40 };
        dl->AddRectFilled({ at.x - 8, at.y - 4 }, { at.x + s.x + 8, at.y + s.y + 4 }, IM_COL32(20, 22, 26, 210), 4);
        dl->AddText(at, IM_COL32(235, 238, 242, 255), text);
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------- side panels

void App::BeginSections(const std::string& id)
{
    m_sectionTab = false;
    m_sectionBar = ImGui::BeginTabBar(id.c_str(), ImGuiTabBarFlags_FittingPolicyShrink);
}

bool App::Section(const char* label, bool select)
{
    if (m_sectionTab) ImGui::EndTabItem();
    m_sectionTab = m_sectionBar && ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
    return m_sectionTab;
}

void App::EndSections()
{
    if (m_sectionTab) ImGui::EndTabItem();
    if (m_sectionBar) ImGui::EndTabBar();
    m_sectionBar = m_sectionTab = false;
}

void App::DrawToolsPanel()
{
    if (!ImGui::Begin("Tools")) { ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::SeparatorText(kGroupNames[int(GroupOf(m_tool))]);
    if (m_terrain.GlobalWmo() && (GroupOf(m_tool) == Group::Terrain || m_tool == Tool::Zones))
        ImGui::TextColored(kWarn, "%s has no terrain (one WMO): nothing for this tool here.", m_terrain.Map().c_str());
    {
        std::vector<std::pair<Tool, std::string>> tools;
        for (const ToolInfo& t : kTools)
            if (t.group == GroupOf(m_tool)) tools.push_back({ t.tool, std::string(t.name) + "  " + t.key });
        Segmented("tool", m_tool, tools, w);
    }
    ImGui::Spacing();

    // The tool's sections, one tab each; View is the last tab of every tool.
    BeginSections("##sections" + std::to_string(int(m_tool)));
    if (m_tool == Tool::Zones) DrawZonesPanel(w);
    if (m_tool == Tool::Triggers) DrawTriggersPanel(w);
    if (m_tool == Tool::Pois) DrawPoisPanel(w);
    if (m_tool == Tool::Flights) DrawFlightsPanel(w);
    if (m_tool == Tool::Lights) DrawLightsPanel(w);
    if (m_tool == Tool::Sound) DrawSoundPanel(w);
    if (m_tool == Tool::Roads) DrawRoadsPanel(w);
    if (m_tool == Tool::Water) DrawWaterPanel(w);
    if (SpawnTool()) DrawSpawnsPanel(w);
    if (m_tool == Tool::Objects && Section("Objects"))
    {
        if (m_armed)
        {
            ImGui::TextColored(kAccent, "Placing %s", FileOf(m_armed->path).c_str());
            if (ImGui::Button("Stop placing  (Esc / right-click)", { w, 0 })) m_armed.reset();
            ImGui::Separator();
        }
        ImGui::TextColored(kQuiet, "Click: select   Shift: add   Ctrl: remove\nDrag empty space: box select\n"
                                   "Handles: arrows move, rings turn, cubes scale\n1 / 2 / 3: move / rotate / scale   X: world / local\n"
                                   "Ctrl while dragging a handle: snap\nG: drop to ground   Del: delete   Esc: deselect\nExact values: Object window");
        ImGui::Checkbox("Move what stands inside buildings along", &m_carryInside);
        ImGui::SetItemTooltip("Doodads placed on the map inside a selected WMO (chairs, tables, crates) follow its handles.\n"
                              "Furniture that is part of the WMO file always follows.");
        ImGui::BeginDisabled(m_objSel.empty());
        if (ImGui::Button("Drop to ground  G", { (w - 8) / 2, 0 }))
            EditObjects("Drop " + std::to_string(m_objSel.size()) + " object(s) to the ground", [&](float* pos, float*, float*) {
                if (const auto h = m_terrain.HeightAt(pos[0], pos[2])) pos[1] = *h;
            });
        ImGui::SameLine();
        if (ImGui::Button("Delete  Del", { (w - 8) / 2, 0 })) DeleteSelectedObjects();
        ImGui::EndDisabled();
    }

    if (m_tool == Tool::Paint)
    {
        if (Section("Texture"))
        {
            if (!m_activeTexture.empty())
            {
                ImGui::Image(ImTextureID(intptr_t(m_renderer.TextureFor(m_activeTexture, m_mpq))), { 64, 64 });
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(FileOf(m_activeTexture).c_str());
                ImGui::PushTextWrapPos(w);
                ImGui::TextColored(kQuiet, "%s", m_activeTexture.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
            }
            else
                ImGui::TextColored(kQuiet, "No texture picked yet.");
            if (ImGui::Button("Pick in the Catalog", { w, 0 })) m_catalogShowTab = int(Catalog::Kind::GroundTexture);
            if (m_recentTextures.size() > 1)
            {
                ImGui::TextColored(kQuiet, "Recent");
                for (size_t i = 0; i < m_recentTextures.size(); ++i)
                {
                    ImGui::PushID(int(i));
                    if (ImGui::ImageButton("##recent", ImTextureID(intptr_t(m_renderer.TextureFor(m_recentTextures[i], m_mpq))), { 32, 32 }))
                        PickTexture(m_recentTextures[i]);
                    ImGui::SetItemTooltip("%s", m_recentTextures[i].c_str());
                    ImGui::PopID();
                    if ((i + 1) % 6 != 0 && i + 1 < m_recentTextures.size()) ImGui::SameLine();
                }
            }
        }
        if (Section("Brush"))
        {
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Radius", &m_paint.radius, 1.0f, 150.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Pressure", &m_paint.pressure, 0.02f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("How fast the texture builds up while you hold the button");
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Hardness", &m_paint.hardness, 0.0f, 0.95f, "%.2f");
            ImGui::SetItemTooltip("0: soft edge over the whole radius. Near 1: hard edge (the inner ring).");
            ImGui::TextColored(kQuiet, "Drag: paint   Ctrl+drag: erase\nAlt+click: pick the texture under the cursor\nCtrl+wheel: radius\n"
                                       "A chunk holds 4 textures; a 5th replaces\nthe one it shows least of.");
        }
        if (Section("Swap"))
        {
            ImGui::Checkbox("The brush swaps instead of painting", &m_swapBrush);
            ImGui::SetItemTooltip("Drag over chunks to swap there; Alt+click picks the texture to swap out.");
            ImGui::TextUnformatted("Swap out");
            ImGui::SetNextItemWidth(w);
            ImGui::InputTextWithHint("##swapfrom", "Alt+click the ground (with the box above on)", &m_swapFrom);
            ImGui::Checkbox("Remove it (no texture in its place)", &m_swapRemove);
            if (!m_swapRemove) ImGui::TextColored(kQuiet, "Put in: %s", m_activeTexture.empty() ? "(pick a texture above)" : FileOf(m_activeTexture).c_str());
            const bool ready = !m_swapFrom.empty() && (m_swapRemove || !m_activeTexture.empty()) && !m_terrain.Tiles().empty();
            ImGui::BeginDisabled(!ready);
            auto swapOn = [&](bool cameraTile) {
                std::vector<ChunkRef> chunks;
                const int under = TileKey(int(std::floor(m_camera.pos.x / kTileSize)), int(std::floor(m_camera.pos.z / kTileSize)));
                for (const auto& [key, tile] : m_terrain.Tiles())
                    if (!cameraTile || key == under)
                        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) chunks.push_back({ key, int(ci) });
                if (auto change = m_terrain.SwapTexture(m_swapFrom, m_swapRemove ? std::string() : m_activeTexture, chunks, SwapLabel()))
                {
                    Log("%s (%zu chunks)", change->label.c_str(), change->data.at("layers").size());
                    m_store.Commit(std::move(*change));
                }
                else Log("Nothing to swap: no chunk there uses %s.", FileOf(m_swapFrom).c_str());
            };
            if (ImGui::Button("Swap on the tile under the camera", { w, 0 })) swapOn(true);
            if (ImGui::Button("Swap on every loaded tile", { w, 0 })) swapOn(false);
            ImGui::EndDisabled();
            ImGui::TextColored(kQuiet, "Where a chunk already has the new texture,\nthe two merge into one layer. Removing gives\n"
                                       "the share to the chunk's other textures;\na chunk's only texture stays.");
        }
    }


    if (m_tool == Tool::Shade)
    {
        if (!m_terrain.VertexColors())
        {
            ImGui::PushTextWrapPos(w);
            ImGui::TextColored(kWarn, "%s has no vertex shading. The client reads vertex colours only on maps whose WDT turns "
                                      "them on (Northrend does; the old continents and Outland do not).", m_terrain.Map().c_str());
            ImGui::PopTextWrapPos();
        }
        if (Section("Colour"))
        {
            float rgb[3] = { m_shadeColor[0] / 255.0f, m_shadeColor[1] / 255.0f, m_shadeColor[2] / 255.0f };
            ImGui::SetNextItemWidth(w - 90);
            if (ImGui::ColorEdit3("Colour##shade", rgb))
                for (int k = 0; k < 3; ++k) m_shadeColor[size_t(k)] = uint8_t(std::lround(std::clamp(rgb[k], 0.0f, 1.0f) * 255.0f));
            if (ImGui::Button("Neutral (no change)", { w, 0 })) m_shadeColor = { 0x7F, 0x7F, 0x7F };
            ImGui::TextColored(kQuiet, "Mid grey (127) leaves the ground as it is;\ndarker darkens, brighter lightens (up to 2x).");
        }
        if (Section("Brush"))
        {
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Radius##shade", &m_shadeBrush.radius, 1.0f, 150.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Pressure##shade", &m_shadeBrush.pressure, 0.02f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("How fast the colour builds up while you hold the button");
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Hardness##shade", &m_shadeBrush.hardness, 0.0f, 0.95f, "%.2f");
            ImGui::TextColored(kQuiet, "Drag: shade   Ctrl+drag: back to neutral\nAlt+click: pick the colour under the cursor\nCtrl+wheel: radius\n"
                                       "One colour per vertex (4.2 yd apart).");
        }
    }

    if (m_tool == Tool::Sculpt && Section("Sculpt"))
    {
        Segmented("mode", m_brush.mode, { { Brush::Mode::Raise, "Raise" }, { Brush::Mode::Lower, "Lower" }, { Brush::Mode::Flatten, "Flatten" },
                                          { Brush::Mode::Smooth, "Smooth" }, { Brush::Mode::Vertices, "Vertices" } }, w);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Radius", &m_brush.radius, 1.0f, 300.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
        if (m_brush.mode != Brush::Mode::Vertices)
        {
            ImGui::SetNextItemWidth(w - 90);
            ImGui::SliderFloat("Strength", &m_brush.strength, 1.0f, 100.0f, "%.0f");
            ImGui::TextUnformatted("Falloff");
            Segmented("falloff", m_brush.falloff, { { Brush::Falloff::Smooth, "Smooth" }, { Brush::Falloff::Linear, "Linear" }, { Brush::Falloff::Flat, "Flat" },
                                                    { Brush::Falloff::Sharp, "Sharp" }, { Brush::Falloff::Gaussian, "Gauss" } }, w);
            ImGui::SetItemTooltip("How the brush weakens from its centre to its rim.\nFlat: full strength everywhere (hard edge).");
        }
        ImGui::TextColored(kQuiet, m_brush.mode == Brush::Mode::Vertices ? "Drag: select   Ctrl+drag: deselect   Esc: clear\nPgUp / PgDn: move (Shift: 5x)\nCtrl+wheel: radius"
                                                                         : "Left drag: sculpt\nCtrl+wheel: radius\n1-5: switch mode");
    }
    if (m_tool == Tool::Sculpt && m_brush.mode == Brush::Mode::Flatten && Section("Flatten"))
    {
        Segmented("only", m_brush.only, { { Brush::Only::Both, "Both" }, { Brush::Only::Raise, "Fill only" }, { Brush::Only::Lower, "Cut only" } }, w);
        ImGui::SetItemTooltip("Fill only raises ground below the target; cut only lowers ground above it.");
        ImGui::Checkbox("Fixed height", &m_brush.fixedHeight);
        ImGui::SetItemTooltip("Off: towards the height where each stroke starts.\nAlt+click the ground: take its height (turns this on).");
        ImGui::BeginDisabled(!m_brush.fixedHeight);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputFloat("Height", &m_brush.height, 0.5f, 5.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Slope", &m_brush.angle, 0.0f, 60.0f, "%.1f deg");
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Downhill", &m_brush.direction, 0.0f, 360.0f, "%.0f deg");
        ImGui::SetItemTooltip("Direction the slope falls towards: 0 = +z, 90 = +x.");
        ImGui::TextColored(kQuiet, "A slope flattens to a tilted plane through\nthe stroke's start (or the fixed height there):\nramps and roads up hills.");
    }
    const bool vertexMode = m_tool == Tool::Sculpt && m_brush.mode == Brush::Mode::Vertices;
    const bool vertexFresh = vertexMode && !std::exchange(m_vertexTabShown, vertexMode);   // switching to Vertices brings its tab forward
    if (!vertexMode) m_vertexTabShown = false;
    if (vertexMode && Section("Vertices", vertexFresh))
    {
        ImGui::Text("%zu vertices selected", m_terrain.SelectedVertices());
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Step", &m_vertexStep, 0.1f, 20.0f, "%.1f yd", ImGuiSliderFlags_Logarithmic);
        ImGui::BeginDisabled(!m_terrain.SelectedVertices());
        if (ImGui::Button("Raise", { (w - 8) / 2, 0 })) EditVertices(TerrainAdapter::VertexOp::Move, m_vertexStep);
        ImGui::SameLine();
        if (ImGui::Button("Lower", { (w - 8) / 2, 0 })) EditVertices(TerrainAdapter::VertexOp::Move, -m_vertexStep);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputFloat("##setheight", &m_vertexHeight, 0.5f, 5.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("Set height", { 82, 0 })) EditVertices(TerrainAdapter::VertexOp::SetHeight, m_vertexHeight);
        if (ImGui::Button("Even out (to their mean)", { w, 0 })) EditVertices(TerrainAdapter::VertexOp::Even, 0);
        if (ImGui::Button("Clear selection", { w, 0 })) m_terrain.ClearVertexSelection();
        ImGui::EndDisabled();
    }
    if (m_tool == Tool::Holes && Section("Holes"))
    {
        Segmented("holewhat", m_impassMode, { { false, "Holes" }, { true, "Impassable" } }, w);
        if (m_impassMode) Segmented("impassmode", m_holeCut, { { true, "Set" }, { false, "Clear" } }, w);
        else Segmented("holemode", m_holeCut, { { true, "Cut" }, { false, "Fill" } }, w);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Radius##holes", &m_holeRadius, 1.0f, 60.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
        ImGui::TextColored(kQuiet, "Left drag: cut (or fill)\nCtrl+drag: the opposite\nCtrl+wheel: radius");
        if (m_impassMode)
            ImGui::TextColored(kQuiet, "Whole chunks (33 x 33 yd), outlined red.\nThe client's impassable flag (MCNK 0x2);\nAzerothCore's extractors ignore it.");
        else
            ImGui::TextColored(kQuiet, "A hole cell is 1/16 of a chunk\n(8.3 x 8.3 yd), the finest size the\n3.3.5 client and AzerothCore support.");
    }

    if (m_tool == Tool::Select || m_tool == Tool::Copy)
    {
        if (Section(("Selection (" + std::to_string(m_selection.size()) + ")###selection").c_str()))
        {
            ImGui::Text("%zu chunk(s) selected", m_selection.size());
            ImGui::BeginDisabled(m_selection.empty());
            if (ImGui::Button("Clear", { (w - 8) / 2, 0 })) m_selection.clear();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(m_terrain.Tiles().empty());
            if (ImGui::Button("Select loaded", { (w - 8) / 2, 0 }))
                for (const auto& [key, tile] : m_terrain.Tiles())
                    for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) m_selection.insert({ key, int(ci) });
            ImGui::EndDisabled();
            ImGui::TextColored(kQuiet, "Click: select   Drag: box select\nShift: add   Ctrl: remove   Esc: clear");
        }
        if (m_tool == Tool::Copy && Section("Clipboard"))
        {
            ImGui::BeginDisabled(m_selection.empty());
            if (ImGui::Button("Copy  Ctrl+C", { (w - 8) / 2, 0 })) CopySelection();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(m_clipboard.Empty());
            if (ImGui::Button(m_placing ? "Stop placing  P" : "Place  P", { (w - 8) / 2, 0 })) { m_placing = !m_placing; m_pin.reset(); }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(m_selection.empty());
            if (ImGui::Button("Save as blueprint...  Ctrl+B", { w, 0 })) OpenSaveBlueprint();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Keep the selected area (terrain, textures, holes, objects) in the catalog's Blueprints tab");
            if (m_clipboard.Empty()) ImGui::TextColored(kQuiet, "Empty. Select chunks and copy them.");
            else ImGui::TextColored(kAccent, "%zu chunk(s)%s", m_clipboard.chunks.size(), m_pin ? "  pinned" : m_placing ? "  following the cursor" : "");
            ImGui::Separator();
            ImGui::BeginDisabled(m_selection.empty());
            if (ImGui::Button("Rotate selection in place", { w, 0 })) RotateSelectionInPlace();
            if (ImGui::Button("Revert selection to client", { w, 0 })) RevertSelectionToClient();
            ImGui::SetItemTooltip("Puts the selected chunks back as the client has them: heights, textures, holes, water and objects.\n"
                                  "One undo step on top of the edits; tiles the project added are left alone.");
            ImGui::EndDisabled();
        }
        // Placement options come forward when placing starts.
        const bool placing = m_tool == Tool::Copy && m_placing && !m_clipboard.Empty();
        if (placing && Section("Placement", !m_placementShown))
        {
            ImGui::Checkbox("Heights", &m_pasteHeights);
            ImGui::SameLine();
            ImGui::Checkbox("Textures", &m_pasteTextures);
            ImGui::SetItemTooltip("Texture layers, alpha maps and ground effects");
            ImGui::SameLine();
            ImGui::Checkbox("Holes", &m_pasteHoles);
            ImGui::SameLine();
            ImGui::Checkbox("Water", &m_pasteWater);
            ImGui::SetItemTooltip("Terrain liquid (MH2O): the copy's water replaces the pasted chunks' own; a dry copy removes it");
            ImGui::SameLine();
            ImGui::Checkbox("Objects", &m_pasteObjects);
            ImGui::SetItemTooltip("Doodads and WMOs standing on the copied chunks (%zu + %zu)", m_clipboard.doodads.size(), m_clipboard.wmos.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_pasteWater);
            ImGui::Checkbox("Map's water", &m_pasteMapWater);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The pasted water keeps its shape but takes the liquid (colour) and level of this map's water\n"
                                  "there, or the nearest within 80 yd, so it joins the lake or river around the paste");
            ImGui::SameLine();
            ImGui::Checkbox("Ghost", &m_ghostPreview);
            ImGui::SetItemTooltip("See-through ghost while the copy follows the cursor");

            ImGui::BeginDisabled(!m_pasteHeights);
            Segmented("height", m_pasteHeightMode, { { PasteHeight::FollowGround, "Ground" }, { PasteHeight::FollowSlope, "Slope" },
                                                      { PasteHeight::LowestPoint, "Lowest" }, { PasteHeight::Absolute, "Absolute" } }, w);
            ImGui::SetItemTooltip("Ground: the copy's average height matches the ground under it (default).\n"
                                  "Slope: also tilted to the incline under it; the copy keeps all its detail.\n"
                                  "Lowest: its lowest point sits on the lowest ground under it.\nAbsolute: original heights.");
            ImGui::SetNextItemWidth(w - 150);
            ImGui::SliderFloat("Fine offset", &m_pasteOffset, -50.0f, 50.0f, "%+.1f yd", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) m_pasteOffset = 0;
            ImGui::EndDisabled();

            if (ImGui::Button("Rotate left", { (w - 8) / 2, 0 })) RotateClipboard(3);
            ImGui::SameLine();
            if (ImGui::Button("Rotate right", { (w - 8) / 2, 0 })) RotateClipboard(1);

            ImGui::Checkbox("Blend edges", &m_blend);
            ImGui::SetItemTooltip("Bend the ground around the copy to meet its edge with matching slope;\n"
                                  "the copy itself is kept exactly. Textures fade over ~17 yd inside the edge.\nShown once the copy is pinned.");
            ImGui::BeginDisabled(!m_blend);
            ImGui::SameLine();
            ImGui::Checkbox("Auto width", &m_blendAuto);
            ImGui::BeginDisabled(m_blendAuto);
            ImGui::SetNextItemWidth(w - 110);
            float shownWidth = m_blendAuto && m_pin && m_plan.widthYards > 0 ? m_plan.widthYards : m_blendWidth;
            if (ImGui::SliderFloat("Blend width", &shownWidth, 8.0f, 120.0f, "%.0f yd") && !m_blendAuto) m_blendWidth = shownWidth;
            ImGui::EndDisabled();
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!m_pin);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.28f, 1));
            if (ImGui::Button("Commit  Enter", { (w - 8) / 2, 0 })) CommitPlacement();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Cancel  Esc", { (w - 8) / 2, 0 })) CancelPin();
            ImGui::EndDisabled();
            ImGui::TextColored(kQuiet, m_pin ? "Pinned: the terrain shows the result.\nR / Shift+R rotate   arrows move\nAlt+wheel fine height\nClick elsewhere to move the pin."
                                             : "Click to pin the copy here.\nR / Shift+R rotate   Alt+wheel height");
        }
        m_placementShown = placing;
    }

    if (Section("View"))
    {
        ImGui::Checkbox("Wireframe", &m_drawOptions.wireframe);
        ImGui::Checkbox("Object boxes", &m_drawOptions.showObjects);
        ImGui::SetItemTooltip("Yellow/cyan boxes at each doodad and WMO placement");
        ImGui::Checkbox("Doodads", &m_modelSettings.doodads);
        ImGui::SameLine();
        ImGui::Checkbox("WMOs", &m_modelSettings.wmos);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Object dist", &m_modelSettings.distance, 100.0f, 3000.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
        ImGui::TextColored(kQuiet, "%zu models, %zu objects, %zu drawn", m_models.ModelCount(), m_models.InstanceCount(), m_models.DrawnLastFrame());
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Cam speed", &m_camera.speed, 10.0f, 1000.0f, "%.0f yd/s", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderInt("Load radius", &m_loadRadius, 1, 8, "%d tiles");
        ImGui::SetItemTooltip("Tiles loaded around the camera in each direction (%d x %d).", m_loadRadius * 2 + 1, m_loadRadius * 2 + 1);
        ImGui::TextColored(kQuiet, "Right drag: look\nWASD, Q/E: move   Shift: fast\nWheel: dolly   F: focus tile");
    }
    EndSections();
    ImGui::End();
}

void App::DrawMapsPanel()
{
    if (!ImGui::Begin("Maps")) { ImGui::End(); return; }
    if (!m_project)
    {
        ImGui::TextWrapped("Open or create a project to browse the client's maps.");
        if (ImGui::Button("New project...")) NewProject();
        ImGui::SameLine();
        if (ImGui::Button("Open project...")) OpenProjectDialog();
        ImGui::End();
        return;
    }

    if (m_mapListVersion != m_mapRows.Version()) RefreshMapList();   // maps created or undone
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Search maps", m_mapFilter, sizeof m_mapFilter);
    const float listHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.38f);
    if (ImGui::BeginChild("##maplist", { 0, listHeight }, ImGuiChildFlags_Borders))
    {
        for (size_t i = 0; i < m_maps.size(); ++i)
        {
            const MapEntry& m = m_maps[i];
            static const std::vector<MapVersion> kNone;
            const auto& versions = i < m_mapVersions.size() ? m_mapVersions[i] : kNone;
            bool match = ContainsNoCase(m.name, m_mapFilter) || ContainsNoCase(m.directory, m_mapFilter) || ContainsNoCase(m.source, m_mapFilter);
            for (const MapVersion& v : versions) match = match || ContainsNoCase(v.label, m_mapFilter);
            if (!match) continue;
            char label[256];
            snprintf(label, sizeof label, "%s##%u", m.name.empty() ? m.directory.c_str() : m.name.c_str(), m.id);
            if (ImGui::Selectable(label, m_mapIndex == int(i) && m_mapVersion == 0)) SelectMap(i);
            ImGui::SameLine();
            ImGui::TextColored(kQuiet, "%u  %s", m.id, m.directory.c_str());
            if (!m.source.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored({ 0.55f, 0.75f, 1.0f, 1.0f }, "[%s]", m.source.c_str());
            }
            // Other versions of the same map: lower base layers and compare sources, one row each.
            for (size_t vi = 1; vi < versions.size(); ++vi)
            {
                const MapVersion& v = versions[vi];
                snprintf(label, sizeof label, "      %s##%u_%zu", v.label.c_str(), m.id, vi);
                if (ImGui::Selectable(label, m_mapIndex == int(i) && m_mapVersion == int(vi))) SelectMap(i, int(vi));
                ImGui::SetItemTooltip(v.source == 0 ? "A lower base layer's copy of this map: the layer above it wins, so this one is shown, not edited."
                                                    : "This map in a compare source (same map id): shown, not edited.");
                ImGui::SameLine();
                ImGui::TextColored(kQuiet, v.source == 0 ? "under" : "compare");
            }
        }
    }
    ImGui::EndChild();

    if (m_mapIndex < 0) { ImGui::TextColored(kQuiet, "Select a map to see its tiles."); ImGui::End(); return; }
    const MapEntry& map = m_maps[size_t(m_mapIndex)];
    if (const MapVersion* v = SelectedMapVersion(); v && !v->edited)
    {
        // Another version: its tiles and far heights, shown; the open map can show it as a ghost.
        ImGui::TextColored(kAccent, "%s", v->label.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, "(shown, not edited)");
        const bool open = m_terrain.Map() == map.directory;
        Ghosts::Layer* ghost = nullptr;
        for (Ghosts::Layer& l : m_ghosts.Layers())
            if (l.source == v->source && l.archive == v->archive && l.layerWide == (v->archive >= 0) && l.map == (v->directory == m_terrain.Map() ? "" : v->directory))
                ghost = &l;
        ImGui::BeginDisabled(!open);
        if (ImGui::Button(ghost ? "Remove the ghost" : "Show as ghost over the open map", { -1, 0 }))
        {
            if (ghost) RemoveGhostLayer(ghost->id);
            else
            {
                Ghosts::Layer& l = m_ghosts.AddLayer(v->source, v->archive, v->label, v->directory == m_terrain.Map() ? "" : v->directory);
                l.layerWide = v->archive >= 0;
                Log("Ghost layer %s added over %s.", l.label.c_str(), m_terrain.Map().c_str());
            }
        }
        ImGui::EndDisabled();
        if (!open) ImGui::SetItemTooltip("Open %s first (its first row): the ghost shows over it.", map.directory.c_str());
        if (v->source == 0) ImGui::TextColored(kQuiet, "To edit this version, move its layer to the top in Sources.");
    }
    if (m_mapGlobal)
    {
        // A dungeon or other WMO-only map: no tiles, one WMO.
        ImGui::TextWrapped("%s is one WMO, no terrain:", map.directory.c_str());
        ImGui::TextColored(kQuiet, "%s", m_mapGlobal->model.c_str());
        if (ImGui::Button(m_terrain.Map() == map.directory ? "Go to its middle" : "Open this map", { -1, 0 })) GoToWmoMap(map.directory);
        ImGui::TextColored(kQuiet, "Spawns, paths, triggers, POIs and flight nodes work on its floors.\nTerrain tools have nothing to edit here.");
        ImGui::End();
        return;
    }
    if (m_mapTiles.empty()) { ImGui::TextColored(kQuiet, "%s has no WDT tile grid.", map.directory.c_str()); ImGui::End(); return; }

    RefreshMapPreview();
    // 64x64 tile grid over the map picture (land green to brown by height, blue shallows, dark deep water):
    // tan lines = tiles, orange = edited, blue outline = loaded, white dot = camera.
    const std::set<int> edited = m_terrain.EditedTiles(map.directory);
    const MapVersion* shownVersion = SelectedMapVersion();
    const bool current = m_terrain.Map() == map.directory && (!shownVersion || shownVersion->edited);
    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 30);
    const float cell = std::floor(std::max(side, 128.0f) / 64.0f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tiles", { cell * 64, cell * 64 });
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, { o.x + cell * 64, o.y + cell * 64 }, IM_COL32(18, 20, 23, 255));
    if (m_mapPreview) dl->AddImage(ImTextureID(intptr_t(m_mapPreview.Get())), o, { o.x + cell * 64, o.y + cell * 64 });
    const ImU32 grid = IM_COL32(190, 140, 100, cell >= 5 ? 255 : 120);   // fainter when the cells are tiny
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            if (!m_mapTiles[size_t(y) * 64 + x]) continue;
            const ImVec2 a{ o.x + x * cell, o.y + y * cell }, b{ a.x + cell, a.y + cell };
            if (!m_mapPreview) dl->AddRectFilled(a, b, IM_COL32(90, 98, 110, 255));
            if (edited.count(TileKey(x, y))) dl->AddRectFilled(a, b, IM_COL32(240, 150, 50, 150));
            dl->AddRect(a, { b.x + 1, b.y + 1 }, grid);
            if (current && m_terrain.Tiles().count(TileKey(x, y)))
                dl->AddRect({ a.x - 1, a.y - 1 }, { b.x + 1, b.y + 1 }, IM_COL32(80, 160, 255, 255), 0.0f, ImDrawFlags_None, 1.5f);
        }
    if (current)
        dl->AddCircleFilled({ o.x + m_camera.pos.x / kTileSize * cell, o.y + m_camera.pos.z / kTileSize * cell }, std::max(2.5f, cell * 0.6f),
                            IM_COL32(255, 255, 255, 255));
    if (ImGui::IsItemHovered())
    {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int x = int((m.x - o.x) / cell), y = int((m.y - o.y) / cell);
        if (x >= 0 && x < 64 && y >= 0 && y < 64)
        {
            const bool exists = m_mapTiles[size_t(y) * 64 + x];
            dl->AddRect({ o.x + x * cell - 1, o.y + y * cell - 1 }, { o.x + (x + 1) * cell, o.y + (y + 1) * cell }, IM_COL32(255, 255, 255, 200));
            ImGui::SetTooltip("%s %d, %d%s", map.directory.c_str(), x, y, exists ? "\nClick to fly there" : "\nNo tile");
            if (exists && ImGui::IsItemClicked() && (!shownVersion || shownVersion->edited)) GoToTile(map.directory, x, y);
        }
    }
    ImGui::TextColored(kQuiet, "Lines: tiles   Orange: edited   Blue: loaded   Dot: camera");
    ImGui::End();
}

void App::DrawInspector()
{
    if (!ImGui::Begin("Inspector")) { ImGui::End(); return; }
    if (m_terrain.Tiles().empty() && !m_terrain.GlobalWmo()) { ImGui::TextColored(kQuiet, "Nothing loaded."); ImGui::End(); return; }
    // What the current tool works on: a spawn, an object, an area. Otherwise the terrain chunk.
    if ((SpawnTool() && InspectSpawn()) || (m_tool == Tool::Objects && InspectObject()) || (m_tool == Tool::Zones && InspectArea()))
    {
        ImGui::End();
        return;
    }

    // Several chunks: a summary. One chunk (selected, else hovered): its details.
    if (m_selection.size() > 1)
    {
        std::set<int> tiles;
        float lo = 1e9f, hi = -1e9f;
        size_t holes = 0;
        for (ChunkRef r : m_selection)
            if (const AdtChunk* c = m_terrain.Chunk(r))
            {
                tiles.insert(r.tile);
                for (float h : c->heights) { lo = std::min(lo, c->baseY + h); hi = std::max(hi, c->baseY + h); }
                holes += c->holes != 0;
            }
        ImGui::TextColored(kAccent, "%zu chunks selected", m_selection.size());
        ImGui::TextColored(kQuiet, "across %zu tile(s)", tiles.size());
        ImGui::Spacing();
        ImGui::Text("Area   %.0f x %.0f yd max", float(m_selection.size()) * kChunkSize, kChunkSize);
        ImGui::Text("Height %.1f .. %.1f", lo, hi);
        ImGui::Text("Chunks with holes  %zu", holes);
        if (ImGui::Button("Clear selection")) m_selection.clear();
        ImGui::End();
        return;
    }

    const std::optional<ChunkRef> ref = !m_selection.empty() ? std::optional<ChunkRef>(*m_selection.begin())
                                                             : (m_hover ? std::optional<ChunkRef>(m_hover->chunk) : std::nullopt);
    const AdtChunk* c = ref ? m_terrain.Chunk(*ref) : nullptr;
    if (!c) { ImGui::TextColored(kQuiet, "Hover or select terrain to inspect a chunk."); ImGui::End(); return; }
    const LoadedTile& tile = m_terrain.Tiles().at(ref->tile);

    ImGui::TextColored(kAccent, "%s chunk %u, %u", m_selection.empty() ? "Hovered" : "Selected", c->indexX, c->indexY);
    ImGui::TextColored(kQuiet, "%s tile %d_%d", m_terrain.Map().c_str(), tile.x, tile.y);
    ImGui::Spacing();
    if (ImGui::BeginTable("##props", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
    {
        auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kQuiet, "%s", key);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        const auto [lo, hi] = std::minmax_element(c->heights.begin(), c->heights.end());
        char buf[64];
        row("Area", AreaLabel(c->areaId));
        snprintf(buf, sizeof buf, "%.1f", c->baseY);
        row("Base height", buf);
        snprintf(buf, sizeof buf, "%.1f .. %.1f", c->baseY + *lo, c->baseY + *hi);
        row("Height range", buf);
        row("Holes", c->holes ? "yes" : "none");
        row("Texture layers", std::to_string(c->layerCount) + " of 4");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Textures");
    for (uint32_t l = 0; l < c->layerCount; ++l)
    {
        const std::string& name = c->textureIds[l] < tile.adt.textures.size() ? tile.adt.textures[c->textureIds[l]] : std::string("?");
        ImGui::TextColored(kQuiet, "%u", l);
        ImGui::SameLine();
        ImGui::TextUnformatted(FileOf(name).c_str());
        ImGui::SetItemTooltip("%s", name.c_str());
    }
    if (!m_selection.empty() && ImGui::Button("Clear selection")) m_selection.clear();
    ImGui::End();
}

void App::DrawChangesPanel()
{
    if (!ImGui::Begin("Changes")) { ImGui::End(); return; }
    ImGui::BeginDisabled(!m_store.CanUndo());
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_store.CanRedo());
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(kQuiet, "%zu applied, %zu undone%s", m_store.Done().size(), m_store.Undone().size(), m_store.Dirty() ? "  (unsaved)" : "");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##changefilter", "Filter", &m_changeFilter);

    if (ImGui::BeginTable("##changes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Change");
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        // Click a row: the camera goes where the change was made. Right-click: undo or redo up to it.
        std::optional<size_t> undoTo, redoTo;
        const Change* goTo = nullptr;
        auto row = [&](const Change& c, bool undone, size_t index) {
            if (!m_changeFilter.empty() && !ContainsNoCase(c.label, m_changeFilter.c_str()) && !ContainsNoCase(c.target, m_changeFilter.c_str())) return;
            ImGui::TableNextRow();
            if (undone) ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
            ImGui::TableNextColumn();
            ImGui::PushID(int(c.id));
            if (ImGui::Selectable(std::to_string(c.id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) goTo = &c;
            if (ImGui::BeginItemTooltip())
            {
                ImGui::Text("%s", c.label.c_str());
                ImGui::TextColored(kQuiet, "%s   by %s", c.domain.c_str(), c.author.c_str());
                ImGui::TextColored(kQuiet, "Click: go there   Right-click: %s to here", undone ? "redo" : "undo");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("##change"))
            {
                if (ImGui::MenuItem("Go there", nullptr, false, GoToChange(c, false))) goTo = &c;
                if (!undone && ImGui::MenuItem("Undo to here (this one included)")) undoTo = index;
                if (undone && ImGui::MenuItem("Redo to here (this one included)")) redoTo = index;
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%s%s", c.label.c_str(), undone ? "  (undone)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.target.c_str());
            std::tm tm{};
            const std::time_t t = std::time_t(c.time);
            localtime_s(&tm, &t);
            char stamp[16];
            std::strftime(stamp, sizeof stamp, "%H:%M", &tm);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(stamp);
            if (undone) ImGui::PopStyleColor();
        };
        for (size_t i = 0; i < m_store.Undone().size(); ++i) row(m_store.Undone()[i], true, i);
        for (size_t i = m_store.Done().size(); i-- > 0;) row(m_store.Done()[i], false, i);
        ImGui::EndTable();
        if (goTo && !GoToChange(*goTo, true)) Log("'%s' has no place to go to.", goTo->label.c_str());
        if (undoTo) UndoTo(*undoTo);
        if (redoTo) RedoTo(*redoTo);
    }
    ImGui::End();
}

bool App::GoToChange(const Change& c, bool go)
{
    // A batch: its first part with a place.
    std::vector<std::pair<std::string, const nlohmann::json*>> parts;
    if (c.domain == ChangeStore::kBatch && c.data.contains("changes"))
        for (const auto& p : c.data["changes"]) parts.push_back({ p.value("domain", ""), &p["data"] });
    else parts.push_back({ c.domain, &c.data });
    for (const auto& [domain, data] : parts)
    {
        const nlohmann::json& d = *data;
        if (domain == m_terrain.Domain() && d.contains("map") && d["map"].is_string())
            for (const char* key : { "edits", "layers", "holes", "liquids", "areas", "colors", "objects", "tiles" })
                if (d.contains(key) && d[key].is_array() && !d[key].empty() && d[key][0].is_array() && d[key][0].size() >= 2)
                {
                    if (go) GoToTile(d["map"].get<std::string>(), d[key][0][0].get<int>(), d[key][0][1].get<int>());
                    return true;
                }
        if (domain == m_creatures.Domain() || domain == m_gameobjects.Domain())
        {
            const nlohmann::json& r = d.contains("rows") && d["rows"].is_array() && !d["rows"].empty() ? d["rows"][0] : d;
            const nlohmann::json& row = r.contains("after") && r["after"].is_object() ? r["after"] : r.contains("before") ? r["before"] : r;
            auto text = [&](const char* k) { return row.contains(k) && row[k].is_string() ? row[k].get<std::string>() : std::string(); };
            if (row.is_object() && !text("map").empty() && !text("position_x").empty())
            {
                if (go) FlyTo(uint32_t(std::stoul(text("map"))), std::stof(text("position_x")), std::stof(text("position_y")), std::stof(text("position_z")), false);
                return true;
            }
        }
    }
    return false;
}

std::string App::SwapLabel() const
{
    return m_swapRemove ? "Remove " + FileOf(m_swapFrom) : "Swap " + FileOf(m_swapFrom) + " for " + FileOf(m_activeTexture);
}

void App::DrawWaterPanel(float w)
{
    static const char* kKinds[] = { "water", "ocean", "magma", "slime" };
    if (Section("Water"))
    {
        Segmented("watermode", m_water.mode, { { WaterBrush::Mode::Add, "Add" }, { WaterBrush::Mode::Remove, "Remove" }, { WaterBrush::Mode::Level, "Level" } }, w);
        const LiquidInfo* current = m_terrain.Liquid(m_water.type);
        char label[96];
        snprintf(label, sizeof label, "%s (%s, %u)", current ? current->name.c_str() : "unknown", current ? kKinds[int(current->kind)] : "?", m_water.type);
        ImGui::SetNextItemWidth(w - 90);
        if (ImGui::BeginCombo("Liquid", label))
        {
            for (const LiquidInfo& l : m_terrain.LiquidTypes())
            {
                snprintf(label, sizeof label, "%s (%s, %u)", l.name.c_str(), kKinds[int(l.kind)], l.id);
                if (ImGui::Selectable(label, l.id == m_water.type)) m_water.type = l.id;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("LiquidType.dbc. Ocean is stored flat (one height per chunk), as in Blizzard's tiles.");
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Radius##water", &m_water.radius, 1.0f, 150.0f, "%.0f yd", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(w - 90);
        ImGui::InputFloat("Level", &m_water.level, 0.5f, 5.0f, "%.2f");
        ImGui::SetItemTooltip("Height of the surface. Alt+click takes it (and the liquid) from the water or ground under the cursor.");
        if (m_water.mode == WaterBrush::Mode::Add)
        {
            ImGui::Checkbox("Match nearby water", &m_waterFromClick);
            ImGui::SetItemTooltip("Each stroke takes the liquid and level of the water it starts on, or of the\n"
                                  "nearest water within %.0f yd, so it joins it without a seam.\nNo water near: the ground there plus the depth below.",
                                  kWaterMatchRadius);
            if (m_waterFromClick)
            {
                ImGui::SetNextItemWidth(w - 90);
                ImGui::SliderFloat("Depth", &m_waterDepth, 0.0f, 20.0f, "%.1f yd");
            }
            ImGui::Checkbox("Take cells from other liquids", &m_water.replace);
            ImGui::SetItemTooltip("Off: the new liquid is layered over what is there.");
        }
    }
    if (Section("Slope"))
    {
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Angle", &m_water.angle, 0.0f, 45.0f, "%.1f deg");
        ImGui::SetNextItemWidth(w - 90);
        ImGui::SliderFloat("Downhill", &m_water.direction, 0.0f, 360.0f, "%.0f deg");
        ImGui::SetItemTooltip("Direction the surface falls towards: 0 = +z, 90 = +x.");
        ImGui::TextColored(kQuiet, "The surface passes through the level where\nthe stroke starts. Rivers and waterfalls.\nOcean stays flat.");
    }
    ImGui::TextColored(kQuiet, "Left drag: %s   Ctrl+drag: %s\nAlt+click: pick level and liquid\nCtrl+wheel: radius\n"
                               "Water already there keeps its height;\nLevel moves it.",
                       m_water.mode == WaterBrush::Mode::Add ? "add" : m_water.mode == WaterBrush::Mode::Remove ? "remove" : "level",
                       m_water.mode == WaterBrush::Mode::Add ? "remove" : m_water.mode == WaterBrush::Mode::Remove ? "add" : "level");
}

void App::UndoTo(size_t doneCount)
{
    if (m_terrain.Stroking() || m_terrain.HoleStroking() || m_terrain.AreaStroking() || m_terrain.Watering()) return;
    const size_t steps = m_store.Done().size() - std::min(doneCount, m_store.Done().size());
    if (!steps) return;
    ClearPlacementView();
    for (size_t i = 0; i < steps; ++i) m_store.Undo();
    Log("Undid %zu change(s).", steps);
}

void App::RedoTo(size_t undoneCount)
{
    if (m_terrain.Stroking() || m_terrain.HoleStroking() || m_terrain.AreaStroking() || m_terrain.Watering()) return;
    const size_t steps = m_store.Undone().size() - std::min(undoneCount, m_store.Undone().size());
    if (!steps) return;
    ClearPlacementView();
    for (size_t i = 0; i < steps; ++i) m_store.Redo();
    Log("Redid %zu change(s).", steps);
}

void App::DrawStartScreen(const ImVec2& origin, const ImVec2& size)
{
    const ImVec2 box{ std::min(560.0f, size.x - 40), std::min(380.0f, size.y - 40) };
    if (box.x < 200 || box.y < 150) return;
    ImGui::SetCursorScreenPos({ origin.x + (size.x - box.x) / 2, origin.y + (size.y - box.y) / 2 });
    if (ImGui::BeginChild("##start", box, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::TextUnformatted("WoW World Editor");
        ImGui::TextColored(kQuiet, "A project holds your edits to one client and one server.");
        ImGui::Spacing();
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        if (ImGui::Button("New project...  Ctrl+N", { half, 0 })) NewProject();
        ImGui::SameLine();
        if (ImGui::Button("Open project...  Ctrl+O", { half, 0 })) OpenProjectDialog();
        ImGui::SeparatorText("Recent");
        if (m_recent.empty()) ImGui::TextColored(kQuiet, "None yet.");
        std::optional<std::string> open;
        for (const std::string& dir : m_recent)
        {
            const bool exists = fs::exists(fs::path(dir) / "project.json");
            ImGui::BeginDisabled(!exists);
            if (ImGui::Selectable((fs::path(dir).filename().string() + "##" + dir).c_str())) open = dir;
            ImGui::EndDisabled();
            ImGui::SameLine(160);
            ImGui::TextColored(kQuiet, "%s%s", Fit(dir, ImGui::GetContentRegionAvail().x - 80).c_str(), exists ? "" : "  (missing)");
        }
        ImGui::Spacing();
        ImGui::TextColored(kQuiet, "Ctrl+P: every command   Ctrl+/: keyboard shortcuts");
        if (open) OpenProject(*open);
    }
    ImGui::EndChild();
}

void App::DrawShortcuts()
{
    if (!m_showShortcuts) return;
    ImGui::SetNextWindowSize({ 620, 640 }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard shortcuts", &m_showShortcuts)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter: a key or what it does", &m_shortcutFilter);
    // The commands that have a key (tools included, from the tool table), then gestures no command covers.
    struct Row { std::string keys, what; };
    std::vector<std::pair<const char*, std::vector<Row>>> sections;
    std::vector<Row> commands;
    for (const Command& c : m_commands)
        if (!c.shortcut.empty()) commands.push_back({ c.shortcut, c.name });
    sections.push_back({ "Commands", std::move(commands) });
    sections.push_back({ "Camera", { { "Right drag", "look around" }, { "W A S D / Q E", "move / down, up (while looking or with the viewport focused)" },
                                     { "Shift", "move faster" }, { "Wheel while right-dragging", "camera speed" }, { "Wheel", "move forward or back" },
                                     { "F", "focus the camera on the tile" },
                                     { "Top-down (Ctrl+T)", "right or middle drag pans, Shift+right drag turns, W A S D glide, wheel or Q E zoom (towards the cursor); going to something keeps top-down over it" } } });
    sections.push_back({ "Selecting", { { "Click", "select" }, { "Shift+click", "add to the selection" }, { "Ctrl+click", "remove from the selection" },
                                        { "Drag empty space", "box select" }, { "Esc", "stop placing, cancel, then clear the selection" } } });
    sections.push_back({ "Moving things (objects, spawns, triggers, POIs, path points)",
                         { { "1 / 2 / 3", "move / rotate / scale handles" }, { "X", "world or local axes" }, { "Ctrl while dragging", "snap" },
                           { "G", "drop to the ground" }, { "PgUp / PgDn", "up / down" }, { "+ / -", "scale" }, { "Del", "delete" },
                           { "Alt+click", "move the selection to the cursor" } } });
    sections.push_back({ "Brushes", { { "Ctrl+wheel", "brush radius" }, { "Alt+click (Paint)", "pick the texture under the cursor" },
                                      { "Ctrl (Holes)", "fill instead of cut" } } });
    sections.push_back({ "Copy and paste", { { "Arrows", "nudge the pinned paste a chunk" }, { "Alt+wheel", "fine height of the paste" },
                                             { "Enter / Esc", "commit / cancel the pinned paste" } } });
    sections.push_back({ "Versions", { { "[ / ]", "compare the selection with the previous / next version" }, { "Enter", "paste the version shown" },
                                       { "Del", "reject the difference under review" } } });
    if (ImGui::BeginChild("##rows"))
        for (const auto& [title, rows] : sections)
        {
            std::vector<const Row*> shown;
            for (const Row& r : rows)
                if (m_shortcutFilter.empty() || ContainsNoCase(r.keys, m_shortcutFilter.c_str()) || ContainsNoCase(r.what, m_shortcutFilter.c_str()))
                    shown.push_back(&r);
            if (shown.empty()) continue;
            ImGui::SeparatorText(title);
            if (ImGui::BeginTable(title, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, 170);
                for (const Row* r : shown)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(kAccent, "%s", r->keys.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", r->what.c_str());
                }
                ImGui::EndTable();
            }
        }
    ImGui::EndChild();
    ImGui::End();
}

void App::DrawLogPanel()
{
    if (!ImGui::Begin("Log")) { ImGui::End(); return; }
    if (ImGui::SmallButton("Clear")) m_log.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy"))
    {
        std::string all;
        for (const std::string& line : m_log) all += line + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##logfilter", "Filter", &m_logFilter);
    ImGui::Separator();
    if (ImGui::BeginChild("##log", { 0, 0 }, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
    {
        for (const std::string& line : m_log)
        {
            if (!m_logFilter.empty() && !ContainsNoCase(line, m_logFilter.c_str())) continue;
            const int level = LogLevel(line);
            if (level) ImGui::PushStyleColor(ImGuiCol_Text, level == 2 ? kBad : kWarn);
            ImGui::TextUnformatted(line.c_str());
            if (level) ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------- modals

void App::DrawNewProjectModal()
{
    if (m_newProjectOpen) { ImGui::OpenPopup("New project"); m_newProjectOpen = false; }
    ImGui::SetNextWindowSize({ 560, 0 }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_NoResize)) return;

    auto folderRow = [this](const char* label, char* buf, size_t size, const wchar_t* title) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190);
        ImGui::InputText(label, buf, size);
        ImGui::SameLine();
        ImGui::PushID(label);
        if (ImGui::Button("Browse..."))
            if (auto dir = PickFolder(m_hwnd, title)) strncpy_s(buf, size, dir->c_str(), _TRUNCATE);
        ImGui::PopID();
    };
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190);
    ImGui::InputText("Name", m_newName, sizeof m_newName);
    folderRow("Parent folder", m_newDir, sizeof m_newDir, L"Folder to create the project in");
    folderRow("Client folder", m_newClient, sizeof m_newClient, L"WXL client folder (contains Wow.exe and Data)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190);
    ImGui::InputText("Author", m_newAuthor, sizeof m_newAuthor);

    const fs::path projectDir = fs::path(m_newDir) / m_newName;
    const bool clientOk = m_newClient[0] && fs::exists(fs::path(m_newClient) / "Data");
    const bool ready = m_newName[0] && m_newDir[0] && clientOk;
    ImGui::Spacing();
    if (m_newName[0] && m_newDir[0]) ImGui::TextColored(kQuiet, "Creates %s", projectDir.string().c_str());
    if (m_newClient[0] && !clientOk) ImGui::TextColored(kWarn, "That folder has no Data folder.");
    ImGui::Separator();

    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Create", { 120, 0 }))
    {
        Project p;
        p.dir = projectDir;
        p.name = m_newName;
        p.clientDir = m_newClient;
        p.author = m_newAuthor;
        std::string error;
        if (p.Save(error) && OpenProject(p.dir.string())) ImGui::CloseCurrentPopup();
        else Log("Could not create project: %s", error.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DrawUnsavedModal()
{
    if (m_unsavedOpen) { ImGui::OpenPopup("Unsaved changes"); m_unsavedOpen = false; }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("The project has unsaved changes.");
    ImGui::Spacing();
    auto finish = [this] {
        ImGui::CloseCurrentPopup();
        if (auto then = std::move(m_afterUnsaved)) { m_afterUnsaved = nullptr; then(); }
    };
    if (ImGui::Button("Save", { 110, 0 })) { if (Save()) finish(); }
    ImGui::SameLine();
    if (ImGui::Button("Discard", { 110, 0 })) { m_store.Clear(); finish(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 110, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_afterUnsaved = nullptr; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void App::DrawPalette()
{
    if (m_paletteOpen) { ImGui::OpenPopup("##palette"); m_paletteOpen = false; }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({ vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + 60 }, ImGuiCond_Always, { 0.5f, 0 });
    ImGui::SetNextWindowSize({ 520, 0 });
    if (!ImGui::BeginPopup("##palette")) return;

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##query", "Type a command", m_paletteQuery, sizeof m_paletteQuery);

    std::vector<const Command*> matches;
    for (const Command& c : m_commands)
        if (ContainsNoCase(c.name, m_paletteQuery)) matches.push_back(&c);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_paletteSelected = std::min(m_paletteSelected + 1, int(matches.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_paletteSelected = std::max(m_paletteSelected - 1, 0);
    m_paletteSelected = std::clamp(m_paletteSelected, 0, std::max(0, int(matches.size()) - 1));

    const Command* run = nullptr;
    for (int i = 0; i < int(matches.size()); ++i)
    {
        const Command& c = *matches[size_t(i)];
        const bool enabled = c.enabled();
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Selectable(c.name.c_str(), i == m_paletteSelected)) run = &c;
        ImGui::EndDisabled();
        if (!c.shortcut.empty())
        {
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(c.shortcut.c_str()).x - 20);
            ImGui::TextColored(kQuiet, "%s", c.shortcut.c_str());
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !matches.empty()) run = matches[size_t(m_paletteSelected)];
    if (run && run->enabled())
    {
        ImGui::CloseCurrentPopup();
        run->run();
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
