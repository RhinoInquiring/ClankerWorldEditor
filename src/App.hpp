#pragma once

#include "Areas.hpp"
#include "Blueprint.hpp"
#include "Catalog.hpp"
#include "Changes.hpp"
#include "Differences.hpp"
#include "Formats.hpp"
#include "Ghosts.hpp"
#include "Loader.hpp"
#include "ModelRenderer.hpp"
#include "Mpq.hpp"
#include "Project.hpp"
#include "Renderer.hpp"
#include "Server.hpp"
#include "Looks.hpp"
#include "Paths.hpp"
#include "Spawns.hpp"
#include "Tables.hpp"
#include "Terrain.hpp"
#include "Flights.hpp"
#include "Lights.hpp"
#include "Sounds.hpp"
#include "ServerData.hpp"
#include "Pois.hpp"
#include "Triggers.hpp"

#include <d3d11.h>
#include <chrono>
#include <imgui.h>
#include <DirectXMath.h>
#include <windows.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

/// Windows folder picker; nullopt when cancelled.
std::optional<std::string> PickFolder(HWND owner, const wchar_t* title);

/// The editor: owns the project, the change store, the adapters and every panel.
class App
{
public:
    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, bool firstRun);

    /// Builds the UI for one frame (between ImGui::NewFrame and ImGui::Render) and renders the viewport.
    void Frame(float dt);

    /// The window's close button asks; the editor decides after checking for unsaved changes.
    void RequestClose() { m_closeRequested = true; }
    bool WantsQuit() const { return m_quit; }

private:
    enum class Tool { Select, Sculpt, Copy, Holes, Objects, Paint, Creatures, Gameobjects, Zones, Triggers, Pois, Flights, Shade, Lights, Sound, Roads, Water };
    /// Tools come in groups (the toolbar's buttons); a group remembers the tool last used in it.
    enum class Group { Terrain, Objects, Units, Regions, Atmosphere };
    static constexpr int kGroups = 5;
    /// The group's function key: F1-F4, then F6 (F5 is Play test).
    static ImGuiKey GroupKey(int g) { return ImGuiKey(g < 4 ? ImGuiKey_F1 + g : ImGuiKey_F6 + (g - 4)); }
    static constexpr const char* kGroupNames[kGroups] = { "Terrain", "Objects", "Units", "Regions", "Atmosphere" };
    /// Every tool once: its group, name, hotkey and what it does. Hotkeys, the Tools panel, the command palette and
    /// the shortcuts window all read this list, so a new tool is added here only.
    struct ToolInfo { Tool tool; Group group; const char* name; const char* key; ImGuiKey imKey; const char* about; };
    static constexpr ToolInfo kTools[] = {
        { Tool::Select, Group::Terrain, "Select", "V", ImGuiKey_V, "pick chunks to copy, rotate or save" },
        { Tool::Sculpt, Group::Terrain, "Sculpt", "B", ImGuiKey_B, "raise, lower, flatten, smooth (1-4)" },
        { Tool::Paint, Group::Terrain, "Paint", "T", ImGuiKey_T, "ground textures" },
        { Tool::Shade, Group::Terrain, "Shade", "U", ImGuiKey_U, "vertex colours (maps with vertex shading)" },
        { Tool::Holes, Group::Terrain, "Holes", "H", ImGuiKey_H, "cut and fill terrain holes" },
        { Tool::Water, Group::Terrain, "Water", "", ImGuiKey_None, "paint, level and remove water, magma and slime" },
        { Tool::Roads, Group::Terrain, "Roads", "", ImGuiKey_None, "roads and paths along splines: paint and grade the ground" },
        { Tool::Copy, Group::Terrain, "Copy", "C", ImGuiKey_C, "copy, paste and blend terrain" },
        { Tool::Objects, Group::Objects, "Place and edit", "O", ImGuiKey_O, "doodads and WMOs" },
        { Tool::Creatures, Group::Units, "Creatures", "N", ImGuiKey_N, "creature spawns and their paths" },
        { Tool::Gameobjects, Group::Units, "Gameobjects", "I", ImGuiKey_I, "gameobject spawns" },
        { Tool::Zones, Group::Regions, "Zones", "Z", ImGuiKey_Z, "paint area ids, buildings, world maps" },
        { Tool::Triggers, Group::Regions, "Triggers", "K", ImGuiKey_K, "area triggers, teleports, entrances" },
        { Tool::Pois, Group::Regions, "POIs", "J", ImGuiKey_J, "map landmarks, gossip points, .tele bookmarks" },
        { Tool::Flights, Group::Regions, "Flights", "Y", ImGuiKey_Y, "flight masters' nodes and routes" },
        { Tool::Lights, Group::Atmosphere, "Lights", "L", ImGuiKey_L, "light volumes: sky, fog and sun colours by time of day" },
        { Tool::Sound, Group::Atmosphere, "Sound", "M", ImGuiKey_M, "zone ambience, music, intro and weather; sound emitters" },
    };
    static Group GroupOf(Tool t)
    {
        for (const ToolInfo& i : kTools)
            if (i.tool == t) return i.group;
        return Group::Terrain;
    }
    void SetGroup(Group g) { m_tool = m_groupTool[int(g)]; }
    Tool m_groupTool[kGroups] = { Tool::Sculpt, Tool::Objects, Tool::Creatures, Tool::Zones, Tool::Lights };

    struct Camera
    {
        DirectX::XMFLOAT3 pos{ 0, 0, 0 };
        float yaw = 0, pitch = -0.5f, speed = 80.0f;
        /// Top-down: looks straight down, yaw turns the map. pitch keeps the angle features set (FlyTo and the
        /// like), so a move they make can be followed to the spot they aimed at.
        bool topDown = false;
        DirectX::XMVECTOR Forward() const;
        DirectX::XMVECTOR Heading() const;   // level direction of yaw
        DirectX::XMMATRIX View() const;
    };

    struct Command
    {
        std::string name, shortcut;
        std::function<void()> run;
        std::function<bool()> enabled;
    };

    // panels
    void DrawMenuBar();
    void DrawToolbar();
    void DrawStatusBar();
    void BuildDefaultLayout(unsigned int dockspace);
    void DrawViewport(float dt);
    void DrawToolsPanel();
    /// The Tools panel's sections are tabs, so nothing needs scrolling: BeginSections opens the tab bar (one per tool,
    /// so each keeps its tab), Section ends the previous tab and starts the next one, true when that one is shown
    /// (`select` brings it forward), EndSections closes whatever is open. A section can be skipped, and a panel can
    /// return early: EndSections still closes the bar.
    void BeginSections(const std::string& id);
    bool Section(const char* label, bool select = false);
    void EndSections();
    bool m_sectionBar = false, m_sectionTab = false;
    bool m_placementShown = false;   // the Placement tab existed last frame (it comes forward when placing starts)
    std::set<uint32_t> m_spawnSelShown;   // the spawn selection last frame (a new one brings the Selected tab forward)
    bool m_pathTabShown = false;          // a path was being edited last frame (starting one brings the Path tab forward)
    bool m_vertexTabShown = false;        // Sculpt > Vertices was on last frame (switching to it brings its tab forward)
    uint32_t m_buildingTabUid = 0;        // the building picked last frame (picking another brings the Building tab forward)
    void DrawMapsPanel();
    /// Rebuilds the Maps panel picture when the selected map's loaded terrain changed (edits, undo, streaming).
    void RefreshMapPreview();
    void DrawInspector();
    /// Inspector pages for the selected spawn (every column of its row and of its template), object and area; false
    /// when there is nothing to show (the chunk page shows instead).
    bool InspectSpawn();
    bool InspectObject();
    bool InspectArea();
    std::string m_inspectKey;                            // spawn + project revision the rows below were read for
    nlohmann::json m_inspectRow, m_inspectTemplate;
    void DrawChangesPanel();
    void DrawProblemsPanel();
    void DrawLogPanel();
    void DrawNewProjectModal();
    void DrawUnsavedModal();
    void DrawPalette();
    void HandleShortcuts();

    // server link and checks (AppServer.cpp)
    void DrawSetupModal();
    void OpenProjectSettings();
    void DrawProjectSettingsModal();
    void DrawServerPanel();
    /// Connects the database of the project's server profile; logs the outcome.
    void ConnectServer();
    /// Runs a GM command over SOAP with the project's profile; logs and keeps the output for the Server panel.
    std::optional<std::string> RunServerCommand(const std::string& command);
    /// Export dry run into out/check plus server checks; fills m_problems.
    void RunChecks();
    void OpenSetup();

    // populate: creature and gameobject spawns (AppPopulate.cpp)
    uint32_t CurrentMapId() const;
    void UpdateSpawnView();
    /// The spawn (of the tool's kind) whose marker is under `mouse`.
    std::optional<uint32_t> SpawnAt(const ImVec2& mouse, const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj) const;
    void CommitSpawns(const std::vector<std::pair<std::optional<nlohmann::json>, std::optional<nlohmann::json>>>& rows, const std::string& label);
    void PlaceSpawn(const DirectX::XMFLOAT3& at);
    /// One change editing every selected spawn.
    void EditSpawns(const std::string& label, const std::function<void(Spawn&)>& fn);
    void DeleteSpawns();
    /// Moves the selection so its centre lands on `at` (editor axes), each spawn on the ground.
    void MoveSpawns(const DirectX::XMFLOAT3& at);
    /// Creatures / Gameobjects tool in the viewport: click or box select (Shift adds, Ctrl removes), place, Alt+click move.
    void SpawnsViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildSpawnOverlay(std::vector<LineVertex>& lines) const;
    void DrawSpawnLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj) const;
    void DrawSpawnsPanel(float width);
    /// Tools > On this map: every spawn of the open map by template; click one to go there and select it.
    void DrawSpawnList(float w);
    std::vector<Spawn> m_spawnList;                  // the open map's spawns of m_spawnKind
    std::string m_spawnListKey;                      // map + kind + store revision it was read for
    std::string m_spawnListFilter;
    /// "12 Hallow's End" from game_event (just the number when unknown).
    std::string EventName(int event);
    /// The game event filter (combo): which spawns the viewport shows.
    void DrawEventFilter(float width);
    bool SpawnTool() const { return m_tool == Tool::Creatures || m_tool == Tool::Gameobjects; }
    /// The adapter of the kind the spawn tool works on.
    SpawnAdapter& Spawns() { return m_spawnKind == SpawnKind::Creature ? m_creatures : m_gameobjects; }
    const SpawnAdapter& Spawns() const { return m_spawnKind == SpawnKind::Creature ? m_creatures : m_gameobjects; }
    /// Model tile of a kind's spawns (-3 creatures, -5 gameobjects; -4 is the armed template at the cursor).
    static int SpawnTileKey(SpawnKind kind) { return kind == SpawnKind::Creature ? -3 : -5; }
    void UpdateSpawnModels();
    /// The armed template as a spawn (for its look).
    Spawn ArmedSpawn() const;

    // zones: area ids painted on chunks, AreaTable rows (AppZones.cpp)
    void DrawZonesPanel(float width);
    /// The building section: the picked WMO's groups and the areas WMOAreaTable gives them.
    void DrawBuildingAreas(float width);
    // world map: zone pictures and exploration overlays rendered from the terrain (AppWorldMap.cpp)
    void DrawWorldMapSection(float width);
    /// Queues pictures for a world map: the base (faded, unexplored look) and an overlay per area.
    void QueueMapJob(uint32_t worldMap, bool base, std::vector<uint32_t> areas);
    /// Renders the queued job once its tiles and models are loaded: BLPs into the project's assets, overlay rows.
    void RunMapJob();
    /// Zones tool in the viewport: drag paints the active area, Alt+click picks the area under the cursor.
    void ZonesViewport();
    /// Area borders on the loaded terrain (each chunk edge that meets another area), in each area's colour.
    void BuildZoneOverlay(std::vector<LineVertex>& lines) const;
    /// "Name (id)" of an area, or just the id when AreaTable lacks it.
    std::string AreaLabel(uint32_t id) const;
    /// Whether any applied terrain change paints area ids (the server needs its .map files extracted again).
    bool AreasPainted() const;

    // regions: area triggers, teleports and instance entrances (AppTriggers.cpp)
    void DrawTriggersPanel(float width);
    /// Triggers tool in the viewport: click picks a trigger, Alt+click moves the selected one, and an armed pick
    /// (place, move, teleport target, corpse point) takes the next click on the ground.
    void TriggersViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildTriggerOverlay(std::vector<LineVertex>& lines) const;
    void DrawTriggerLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj) const;
    /// One undo step over a trigger's AreaTrigger.dbc row, its `areatrigger` row and its areatrigger_teleport row
    /// (nullopt removes them); parts that do not change are left out. Reloads teleports over SOAP when one changed.
    void CommitTrigger(uint32_t id, const std::optional<Trigger>& after, const std::optional<Teleport>& teleport, const std::string& label);
    /// Teleports by trigger id (database and project), re-read when the project changes.
    const std::map<uint32_t, Teleport>& TeleportsView() const;
    /// Triggers of the open map (cached).
    const std::vector<Trigger>& TriggersOnMap() const;
    /// The map's name for a Map.dbc id.
    std::string MapLabel(uint32_t id) const;
    /// Ids, teleports without a trigger or into a missing map, arrivals inside another teleport, instances without a way out.
    void CheckTriggers(std::vector<Problem>& problems) const;
    /// Camera above a point (server coordinates), opening its map first when another one is open.
    /// `ground`: the point has no height of its own; the camera goes above the ground there (once it streams in).
    void FlyTo(uint32_t map, float x, float y, float z, bool ground = false);

    // regions: points of interest: world map landmarks, gossip map flags, .tele bookmarks (AppPois.cpp)
    void DrawPoisPanel(float width);
    /// POIs tool in the viewport: click picks a point of the listed kind, Alt+click moves the selected one, an armed
    /// pick (place, move) takes the next click on the ground.
    void PoisViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildPoiOverlay(std::vector<LineVertex>& lines) const;
    void DrawPoiLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    /// One undo step setting a point's row (nullopt removes it). Table kinds need the database and reload over SOAP.
    void CommitPoi(PoiKind kind, uint32_t id, const std::optional<Poi>& after, const std::string& label);
    /// FlyTo the point (gossip points: on the open map; no height: above the ground).
    void FlyToPoi(const Poi& p);
    /// Landmarks of another version standing on `cells`: AreaPOI rows of map folder `mapDir` in `chain` (any client build),
    /// as TerrainClipboard::pois relative to grid cell (originX, originZ). Empty for the open map itself.
    nlohmann::json VersionPois(const MpqChain& chain, const std::string& mapDir, const std::set<std::pair<int, int>>& cells, int originX, int originZ) const;
    /// AreaPOI rows (applied changes, for the caller's undo step) for pasted landmarks (world positions) the open map lacks:
    /// one of the same name within 150 yards counts as there already.
    std::vector<Change> AddPastedPois(const nlohmann::json& pois, const std::string& label);
    /// Points of the listed kind on the open map (gossip points: all of them), cached.
    const std::vector<Poi>& PoisOnMap() const;
    /// Where a point stands, editor axes: its own height, or the ground's when it has none (gossip points); nullopt when
    /// that ground is not loaded.
    std::optional<DirectX::XMFLOAT3> PoiPoint(const Poi& p) const;
    /// An icon of Interface\Minimap\POIIcons as an ImGui image; false when the texture is missing.
    bool PoiIcon(uint32_t icon, float size);
    /// Project rows inside the ranges, tele names used twice, gossip points no gossip option shows.
    void CheckPois(std::vector<Problem>& problems);

    // populate: creature waypoint paths (AppPaths.cpp)
    CreaturePath PathOf() { return { m_creatures, m_waypoints, m_addons }; }
    void BeginPathEdit(uint32_t guid);
    void SavePathEdit();
    void CancelPathEdit();
    void DeletePathPoint();
    /// The selected point onto the terrain under it.
    void DropPathPoint();
    /// Solid spheres for the shown path's points and the walk preview ball.
    void BuildPathSolids(std::vector<LineVertex>& triangles) const;
    /// Centre of a point's sphere (it sits on the point).
    static DirectX::XMFLOAT3 PathPointCenter(const PathPoint& p);
    /// Path mode in the viewport: click the ground adds a point after the selected one, drag a point moves it.
    void PathViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildPathOverlay(std::vector<LineVertex>& lines) const;
    void DrawPathLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj) const;
    void DrawPathPanel(float width);
    /// The points drawn: the path being edited, else the selected creature's own path (read on selection).
    const std::vector<PathPoint>* ShownPath() const;
    /// Reads the selected creature's saved path when the selection or the project changes (every frame).
    void RefreshPathView();

    // actions
    void NewProject();
    void OpenProjectDialog();
    bool OpenProject(const std::string& dir);
    void CloseProject();
    /// `quiet`: autosave, logged only when it fails.
    bool Save(bool quiet = false);
    void Export(bool playTest);
    /// Exports, then packs out/client into the project's patch MPQ (out/<patchName>); with `install`, copies it into
    /// the client's Data folder too (the client must be closed: it holds its archives open).
    void BuildPatch(bool install);
    // Sources window: the project's base files and the compare sources, as layers (edited copy until Apply).
    bool m_showSources = false;
    std::vector<Project::Source> m_sourcesEdit;   // [0] = the base
    std::map<size_t, std::string> m_scanNotes;    // per edited source: what the last scan found and left out
    void DrawSources();
    void ApplySources();
    std::vector<std::pair<std::string, std::vector<MpqLayer>>> CompareSources() const;
    void Undo();
    void Redo();
    void GoToTile(const std::string& map, int x, int y);
    /// Opens a WMO-only map (a dungeon) with the camera over the middle of its WMO.
    void GoToWmoMap(const std::string& map);
    /// The floor under (x, z) at or below height fromY: the terrain or a loaded WMO, whichever is higher (a bridge, a
    /// building's floor, a dungeon). Falls back to the terrain height; null when there is neither.
    std::optional<float> GroundAt(float x, float z, float fromY) const;
    void FocusTile();
    void CopySelection();
    void PasteAtCursor();
    void RotateClipboard(int quarterTurns);
    void RotateSelectionInPlace();
    void RevertSelectionToClient();
    /// Height shift applied to the clipboard at this anchor (snap mode adds the ground offset).
    float PasteOffsetAt(int gx, int gz) const;
    /// Tilt for Follow slope (zero otherwise), written into the paste options.
    void PasteSlopeAt(int gx, int gz, PasteOptions& options) const;
    /// Placing: the ghost follows the cursor; once pinned the terrain shows the blended result.
    void UpdatePlacement();
    void ClearPlacementView();
    void CommitPlacement();
    void CancelPin();
    /// A version of a map in the Maps window: a base layer holding its WDT (the topmost is the one edited), or a compare
    /// source whose Map.dbc has the same id (its folder may differ). archive: the base archive holding the WDT, its layer
    /// read alone; -1 = the project's own files (Project) or a compare source resolved whole.
    struct MapVersion { size_t source = 0; int archive = -1; std::string directory, label; bool edited = false; };
    std::vector<std::vector<MapVersion>> m_mapVersions;   // per m_maps entry
    int m_mapVersion = 0;                                 // the selected map's version shown
    std::map<std::string, std::vector<size_t>> m_wdtArchives;   // map folder -> base archives holding its WDT (cleared when the sources change)
    const MapVersion* SelectedMapVersion() const;
    void SelectMap(size_t index, int version = 0);
    /// Runs `then` now, or after the user saves or discards unsaved changes.
    void GuardUnsaved(std::function<void()> then);

    void Log(const char* fmt, ...);
    void EnsureViewportTarget(UINT width, UINT height);
    void BuildOverlay(std::vector<LineVertex>& lines) const;
    /// Applies a click or box result: replace, add (Shift) or remove (Ctrl).
    void ApplySelection(const std::set<ChunkRef>& hits);
    /// One undoable edit of the selected objects: `fn` gets each object's position, rotation and scale
    /// (scale is null for WMOs, which cannot scale) as they were before the edit.
    void EditObjects(const std::string& label, const std::function<void(float* pos, float* rot, float* scale)>& fn);
    /// Live (uncommitted) version of the same; EndObjectEdit commits.
    void PreviewObjects(const std::function<void(float* pos, float* rot, float* scale)>& fn);
    void EndObjectEdit(const std::string& label);
    std::optional<DirectX::XMFLOAT3> ObjectPosition(const ObjectRef& ref) const;
    void DeleteSelectedObjects();
    void DrawObjectPanel();
    void DrawCatalog();
    void DrawVersions();
    /// Blueprints: save the selection as a reusable area, browse them in the catalog, paste them like a copy.
    void DrawBlueprints();
    void DrawSaveBlueprintModal();
    void OpenSaveBlueprint();
    void UseBlueprint(const Blueprint& b, bool inPlace);
    /// Top-down picture of an area, rendered offscreen with its textures and objects (RGBA, size x size).
    std::vector<uint8_t> RenderAreaThumbnail(const TerrainClipboard& clip, UINT size);
    /// One layer's terrain and objects seen from straight above (orthographic): world x0..x0+spanX left to right,
    /// z0..z0+spanZ top to bottom, `top` the highest ground; RGBA, width x height.
    std::vector<uint8_t> RenderOrtho(float x0, float z0, float spanX, float spanZ, float top, UINT width, UINT height, int layer);
    /// Shows one ghost layer alone, objects included (0 = the map again).
    void SetSolo(int layer);
    void RemoveGhostLayer(int layer);
    /// Pins the clipboard where it was copied from, with its original heights (for ghost copies).
    void PasteInPlace();
    /// The chunk as the viewport shows it: the solo ghost's chunk at the same spot while soloing, else the map's.
    const AdtChunk* ShownChunk(ChunkRef ref) const;
    /// Places the armed catalog model at the cursor; keeps it armed with Shift.
    void PlaceFromCatalog(bool keepArmed);
    // Move, rotate and scale (AppTransform.cpp): one set of handles, keys and panel controls for every tool whose
    // selection can be moved. A tool takes part by returning its selection from ActiveTransform; a future tool that
    // moves things adds a case there and gets the same handles, snapping, keys and undo for free.

    /// The active tool's selection as the shared move / rotate / scale controls see it.
    struct Transformable
    {
        bool rotate = true, yawOnly = false;              // rotation: none, about the vertical only, or free
        enum class Scale { None, Uniform, Axes } scale = Scale::None;
        DirectX::XMFLOAT4X4 frame{};                      // the handles: the item's own frame (one item) or the selection's centre
        std::string what;                                 // "3 creature(s)": for undo labels
        std::string limits;                               // why rotate / scale are limited ("creatures turn about the vertical only")
        std::function<void()> begin;                      // a change starts: remember the items as they are
        std::function<void(DirectX::FXMMATRIX delta)> preview;   // live: each item = as it was at begin, then `delta` (editor space)
        std::function<void(const std::string& label)> commit;    // the change ends: one undo step
        std::function<void()> cancel;                     // drop the preview
        std::function<void()> ground;                     // every item onto the ground, one undo step (null: not offered)
        std::function<void()> remove;                     // delete the selection (null: not offered)
    };
    /// The active tool's selection, if it has one that can be moved.
    std::optional<Transformable> ActiveTransform();
    /// Whether the tool moves things with the shared controls (1 / 2 / 3 pick the handle, not a sculpt brush).
    bool TransformTool() const { return m_tool == Tool::Objects || SpawnTool() || m_tool == Tool::Triggers || m_tool == Tool::Pois || m_tool == Tool::Flights || m_tool == Tool::Lights || m_tool == Tool::Sound || m_tool == Tool::Roads; }
    /// Move / rotate / scale handles on the selection; true while the mouse is over or dragging one.
    bool UpdateGizmo(const ImVec2& origin, const ImVec2& size);
    /// begin, preview(delta), commit in one go: keys, Alt+click and typed values.
    void ApplyTransform(const Transformable& t, DirectX::FXMMATRIX delta, const std::string& label);
    /// 1 / 2 / 3 handle, X world / local, PgUp / PgDn height, + / - scale, G ground, Del delete.
    void TransformKeys();
    /// Alt+click: the selection's handle point goes to `at`, the items keep their layout.
    void MoveSelectionTo(const DirectX::XMFLOAT3& at);
    /// Handle mode, world / local and snapping, plus what the selection allows; at the top of each tool's selection panel.
    void DrawTransformBar(float width);
    /// Viewport caption hint for the shared keys.
    std::string TransformHint() const;
    /// The objects' handle frame: the object's own (one object) or the selection's centre (several).
    DirectX::XMFLOAT4X4 GizmoFrame() const;
    std::optional<Transformable> ObjectTransform();
    std::optional<Transformable> SpawnTransform();
    std::optional<Transformable> TriggerTransform();
    std::optional<Transformable> PoiTransform();
    std::optional<Transformable> PathPointTransform();
    std::optional<Transformable> FlightTransform();
    std::optional<Transformable> LightTransform();
    std::optional<Transformable> SoundTransform();
    std::optional<Transformable> RoadTransform();

    // atmosphere: light volumes (Light.dbc) and their colour, fog and sky sets (AppLights.cpp)
    bool m_gameLight = false;          // viewport shows the game's light at the camera (View menu, Lights tool)
    int m_lightTime = 1440;            // preview time of day, half-minutes from midnight (noon)
    int m_lightSlot = 0;               // which LightParamsID slot the preview and the editors use (0 clear weather)
    uint32_t m_lightSel = 0;           // selected Light row (0: none)
    std::optional<uint32_t> m_lightHover;
    bool m_lightPlace = false;         // the next click on the ground adds a light there
    Lights::Draft m_lightDraft;        // rows being edited (sliders, colour pickers, the gizmo), committed when the edit ends
    std::string m_lightDraftLabel;
    uint64_t m_lightDraftRevision = 0;
    LightVolume m_lightStart;          // the selected light when a move began
    /// The map's lights for this frame (cached on the tables' versions and the draft).
    const std::vector<LightVolume>& LightsOnMap();
    std::vector<LightVolume> m_lightsOnMap;
    std::tuple<uint32_t, uint64_t, uint64_t> m_lightsOnMapKey{ ~0u, 0, 0 };
    /// The scene light at the camera, to the renderer (or off); cached while the camera, time and data stay put.
    void UpdateSceneLight();
    std::tuple<uint32_t, int, int, int, int, int, uint64_t> m_sceneLightKey{};
    void DrawLightsPanel(float width);
    void LightsViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildLightOverlay(std::vector<LineVertex>& lines) const;
    /// A row into the draft (live preview); LightDraftCommit writes the draft as one undo step.
    void LightDraftSet(DbcTable& table, uint32_t id, nlohmann::json row, const std::string& label);
    void LightDraftCommit();
    /// Adds a light at an editor position: its own colour set copied from the light there now.
    void AddLight(const DirectX::XMFLOAT3& at);
    void FlyToLight(const LightVolume& v);

    // server data: maps, vmaps and mmaps for the maps the project exports, with AzerothCore's tools (AppServer.cpp)
    ServerDataJob m_serverJob;
    bool m_showServerData = false;
    bool m_serverWholeMesh = false;              // rebuild every navmesh tile of the map, not only around the edits
    std::set<std::string> m_serverSkip;          // maps left out of the next build
    std::vector<std::string> m_serverJobLog;
    /// The maps the project exports, with the navmesh tiles to rebuild (edited tiles and their neighbours).
    std::vector<ServerMap> ServerMapsToBuild() const;
    void DrawServerDataWindow();
    void StartServerData();
    /// Puts back every server file a build replaced (the originals saved the first time).
    void RestoreServerData();

    // terrain: roads, editor-only splines that paint and grade the ground under them (AppRoads.cpp)
    uint32_t m_roadSel = 0;                       // selected road (0: none)
    std::optional<size_t> m_roadPoint;            // its selected point (none: the whole road)
    bool m_roadDraw = false;                      // clicks on the ground add points to the selected road (or start one)
    std::optional<std::pair<uint32_t, size_t>> m_roadHover;
    Road m_roadStart;                             // the road when a move began
    Road m_roadDefaults;                          // what a new road starts with (the last road's settings)
    std::optional<Road> m_roadPending;            // a preview waiting its turn (long roads: previews are spaced out)
    std::chrono::steady_clock::time_point m_roadPreviewAt{};
    float m_roadPreviewMs = 0;                    // how long the last preview took
    bool m_roadLines = true;                      // the Roads tool draws lines, points and handles (off: the road as it will look)
    void DrawRoadsPanel(float width);
    void RoadsViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildRoadOverlay(std::vector<LineVertex>& lines) const;
    /// Live edit of the selected road: shown at once, one undo step when `commit`.
    void EditRoad(const Road& edited, bool commit, const std::string& label);
    void BakeSelectedRoad();

    // atmosphere: zone sound (AreaTable ambience / music / intro) and sound emitters (SoundEmitters.dbc) (AppSound.cpp)
    bool m_soundZone = false;          // Zone sound tab edits the zone (true) or the area under the camera (false)
    uint32_t m_emitterSel = 0;
    std::optional<uint32_t> m_emitterHover;
    bool m_emitterPlace = false;       // the next click on the ground adds an emitter with m_emitterSound
    uint32_t m_emitterSound = 0;       // SoundEntries id for new emitters
    SoundEmitter m_emitterStart;       // the selected emitter when a move began
    SoundEmitter m_emitterEdit;        // ... and as the move shows it
    std::vector<std::pair<uint32_t, std::string>> m_soundNames;   // every SoundEntries row (id, name), read once for the pickers
    std::vector<SoundEmitter> EmittersOnMap() const;
    void DrawSoundPanel(float width);
    void SoundViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildSoundOverlay(std::vector<LineVertex>& lines) const;
    /// A searchable list of every SoundEntries row with a play button each; true when one was picked into `id`.
    bool SoundPicker(const char* label, uint32_t& id, float width);
    /// Plays a SoundEntries row (its first file), or logs why it cannot.
    void PlayEntry(uint32_t id);
    void CommitEmitter(uint32_t id, const std::optional<SoundEmitter>& after, const std::string& label);

    // regions: flight paths: taxi nodes, the paths between them and their points (AppFlights.cpp)
    void DrawFlightsPanel(float width);
    /// Flights tool in the viewport: click picks a node or a path point, a click on the ground with a point selected
    /// inserts one after it, an armed pick places a node.
    void FlightsViewport(const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj);
    void BuildFlightOverlay(std::vector<LineVertex>& lines) const;
    void BuildFlightSolids(std::vector<LineVertex>& triangles) const;
    void DrawFlightLabels(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, DirectX::FXMMATRIX viewProj) const;
    /// DBC rows set (null: removed) as one undo step; rows that do not change are left out.
    void CommitDbc(std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows, const std::string& label,
                   const char* note = "export, then restart the client and worldserver (taxi DBCs)");
    /// `count` ids of `table` free in the project's range `kind` (fewer when it runs out).
    std::vector<uint32_t> FreeDbcIds(const DbcTable& table, const std::string& kind, size_t count);
    /// Nodes on the open map and the paths with a point on it or an end at one of them.
    struct FlightView { std::vector<TaxiNode> nodes; std::vector<TaxiPath> paths; };
    const FlightView& FlightsOnMap() const;
    /// A flight master of the open map (creature with npcflag 0x2000) and the node it serves per team, as AzerothCore
    /// picks it: the nearest node on the map with a mount for that team ([0] Horde, [1] Alliance; 0 = none).
    struct FlightMaster { uint32_t guid = 0; std::string name; DirectX::XMFLOAT3 pos{}; uint32_t node[2] = {}; };
    /// Reads the open map's flight masters again when the map, connection or project changes.
    void RefreshFlightMasters();
    /// A node and every path end that sat on it (within 30 yards), moved by `delta` (editor space), as one undo step.
    void CommitNodeMove(const TaxiNode& before, const TaxiNode& after, const std::string& label);
    /// Paths from `from` to `to` (and back when `both`) with planned points.
    void CreateFlightPath(uint32_t from, uint32_t to, bool both);
    /// What a new flight must clear at a spot (server x, y): the ground (or the map's low-detail heights where tiles are not
    /// loaded) and the top of any loaded building, tree or other model there.
    std::optional<float> FlightFloor(float x, float y) const;
    /// How flights leave `node`, copied from its existing paths (their first ~200 yards, or the last of those arriving,
    /// reversed): the one heading most towards `other`; empty when no path touches the node.
    std::vector<TaxiPoint> FlightDeparture(const TaxiNode& node, const TaxiNode& other) const;
    /// Deletes a path and its points, or a node with every path from or to it.
    void DeleteFlightPath(uint32_t path);
    void DeleteFlightNode(uint32_t node);
    /// Inserts a point after point `after` of the selected path; removes point `index`.
    void InsertFlightPoint(size_t after, const DirectX::XMFLOAT3& at);
    void DeleteFlightPoint(size_t index);
    /// The selected path's points (empty when none).
    const std::vector<TaxiPoint>& FlightPoints() const;
    /// Ids, nodes no team can fly from or no flight master serves, paths that do not start and end on their nodes.
    void CheckFlights(std::vector<Problem>& problems);
    /// The zone and area of the chunk a spawn stands on.
    void SetSpawnArea(Spawn& s) const;
    /// Grid cell the clipboard's first chunk goes to so the copied area is centred on the hovered chunk.
    std::pair<int, int> PasteAnchor() const;
    /// Tiles the clipboard would touch at the current anchor, blend band included (kept loaded while placing).
    std::set<int> PasteTiles() const;

    HWND m_hwnd = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    MpqChain m_mpq;
    Renderer m_renderer;
    ChangeStore m_store;
    Lights m_lights{ m_mpq, m_store };   // atmosphere tables (AppLights.cpp)
    Sounds m_sounds{ m_mpq, m_store };   // sound tables (AppSound.cpp)
    RoadStore m_roads{ m_store };        // editor-only road splines (AppRoads.cpp)
    TerrainAdapter m_terrain{ m_mpq, m_renderer, m_store };
    ModelRenderer m_models;
    ModelRenderer::DrawSettings m_modelSettings;
    Loader m_loader;   // after m_mpq: destroyed (stopped) before the archives close
    std::optional<Project> m_project;

    std::vector<MapEntry> m_maps;
    int m_mapIndex = -1;
    std::vector<bool> m_mapTiles;
    std::optional<WmoPlacement> m_mapGlobal;   // the selected map is WMO-only: its WMO
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_mapPreview;   // the selected map from its WDL, coloured by height
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_mapPreviewTexture;
    std::vector<std::vector<int16_t>> m_mapWdl;   // the selected map's low-detail heights, loaded tiles patched with their current heights
    std::string m_mapPreviewDir;                  // the map m_mapWdl belongs to
    uint64_t m_mapPreviewKey = ~0ull;             // project revision + loaded tiles the picture was made from
    char m_mapFilter[64] = {};

    Camera m_camera;
    int m_loadRadius = 2;
    std::optional<int> m_focusTile;   // tile key whose height the camera snaps to once it streams in
    // Top-down view: height above the ground, and the camera as it was left last frame (a feature that moved it
    // since gets re-centred on the spot it aimed at, see FollowTopDown).
    float m_topYaw = DirectX::XM_PI;   // top-down turn: north (-z, the top of the map) up; Shift+right drag changes it
    float m_topHeight = 350.0f, m_topGoal = 350.0f;   // the zoom eases from height to goal
    DirectX::XMFLOAT2 m_topVel{};                      // key panning speed (across, along the heading), eased
    bool m_panning = false;                            // middle drag pans the top-down map
    std::optional<std::pair<DirectX::XMFLOAT3, float>> m_topSeen;
    void SetTopDown(bool on);
    void FollowTopDown();
    /// Puts the camera on an area (centre, size in yards): top-down straight over it and zoomed to fit, else from the south.
    void FrameArea(float cx, float cz, float span);
    DirectX::XMFLOAT3 CameraTarget() const;   // where the camera looks on the ground
    /// The viewport projection: perspective, or flat (orthographic) in top-down, as wide as the perspective view
    /// would be at the ground. Top-down reaches 3000 yd above the camera so peaks under it still draw.
    DirectX::XMMATRIX Projection(float aspect, float nearZ, float farZ) const;
    DirectX::XMFLOAT3 ViewEye() const;   // detail and model distances count from here (top-down: the ground below)
    std::optional<std::pair<float, float>> m_flyGround;   // FlyTo: editor x, z whose ground height the camera takes once loaded
    DrawOptions m_drawOptions;
    Tool m_tool = Tool::Sculpt;
    Brush m_brush;

    std::optional<TerrainHit> m_hover;
    std::set<ObjectRef> m_objSel;
    std::optional<ObjectRef> m_objHover, m_objPress;   // under the cursor; under it when the button went down
    enum class Gizmo { Move, Rotate, Scale };
    Gizmo m_gizmo = Gizmo::Move;
    bool m_gizmoLocal = false, m_gizmoSnap = false;    // Ctrl inverts snapping while dragging a handle
    float m_snapMove = 1.0f, m_snapRotate = 15.0f, m_snapScale = 0.1f;
    bool m_gizmoActive = false;                         // a handle is being dragged (one edit)
    std::function<void()> m_gizmoCancel;                // the dragged selection's cancel, should the tool change mid-drag
    std::map<uint32_t, Spawn> m_spawnStart, m_spawnPreview;   // spawns being moved: as they were, as they show now (by guid)
    Trigger m_triggerStart;                             // the trigger being moved, as it was
    Poi m_poiStart;                                     // the point being moved, as it was
    std::optional<DirectX::XMFLOAT3> m_poiStartPoint;   // where it stood (editor axes)
    PathPoint m_pathStart;                              // the path point being moved, as it was
    bool m_fieldEditing = false;                        // a panel position / facing field is previewing (commits on release)
    bool m_carryInside = true;                          // map doodads inside a moved building move with it
    std::set<ObjectRef> m_gizmoRiders;                  // those doodads, for the drag in progress
    DirectX::XMFLOAT4X4 m_gizmoMatrix{}, m_gizmoStart{};
    Tool m_lastTool = Tool::Sculpt;

    // Catalog: browse the client's models and textures; a model picked there is "armed" and placed by clicking.
    Catalog m_catalog;
    int m_catalogTab = 0;                 // Catalog::Kind of the tab (Count = all files)
    char m_catalogQuery[128] = {};
    std::string m_catalogFolder;          // lower-case folder prefix
    float m_thumbSize = 96;
    bool m_catalogNearby = false;         // only what the loaded tiles use
    std::map<std::string, int> m_usage;   // catalog path -> placements on loaded tiles
    size_t m_usageStamp = 0;
    struct Armed { std::string path; bool wmo = false; };
    std::optional<Armed> m_armed;
    bool m_placeRandomYaw = true;
    float m_placeYaw = 0, m_placeScale = 1, m_placeScaleJitter = 0;
    std::string m_activeTexture;          // for the Paint tool
    std::vector<std::string> m_recentTextures;   // newest first
    PaintBrush m_paint;
    PaintBrush m_shadeBrush;
    std::array<uint8_t, 3> m_shadeColor{ 0x5A, 0x5A, 0x6A };   // R, G, B; 0x7F = unchanged
    std::optional<int> m_catalogShowTab;  // a tab the catalog should bring forward
    void PickTexture(const std::string& path);
    /// The project's reader falls back to the attached sources (assets pasted from other clients).
    void UpdateFallbacks();
    std::string m_catalogKey;             // inputs m_catalogItems was filtered with

    // Ghost layers: other versions of the map (other clients, single patch archives).
    Ghosts m_ghosts;
    // CDN files fetched in the background (CASC sources): the count last seen, when misses were last retried, and
    // ghost tiles drawn with white stand-ins (layer id, tile key) to load again once their textures land.
    uint64_t m_cdnArrivalsSeen = 0;
    double m_cdnRefreshAt = 0;
    std::set<std::pair<int, int>> m_ghostTilesMissingTextures;
    int m_soloLayer = 0, m_copyLayer = 0;          // 0 = the map itself

    // Compare: the selected chunks shown as each other version in turn, pinned in place like a paste ([ and ] cycle,
    // Enter pastes, Esc stops). The selection stays editable meanwhile.
    struct CompareEntry
    {
        int layer = 0;            // ghost layer; 0 = the map itself
        bool own = false;         // layer made for the compare (hidden, removed when it ends)
        std::string label;
        AreaDiff diff;
        bool ready = false;       // every selected tile of this version is loaded (or known missing)
        size_t assetsOther = 0, assetsMissing = 0;   // referenced files only another client has / no source has
    };
    bool m_comparing = false;
    std::vector<CompareEntry> m_compare;
    size_t m_compareAt = 0;
    std::string m_compareKey, m_compareStatsKey;   // inputs the shown paste / the table were built from
    // Cycling stays quick: each version's copy and plan are kept (by what they were built from), and while versions
    // are flicked through the plan is a hard paste; the blend follows once cycling pauses (kCompareSettle seconds).
    std::map<std::string, TerrainClipboard> m_compareClips;   // by m_compareKey
    std::map<std::string, PastePlan> m_planCache;            // by plan key, compare clips only
    std::string m_compareClipTag;                            // m_compareKey of the clip in m_clipboard
    int m_compareClipVersion = -1;                           // m_clipVersion when it was put there
    double m_compareCycledAt = -1e9;                         // ImGui time of the last version switch
    static constexpr double kCompareSettle = 0.25;
    struct CompareTarget { size_t source = 0; std::string map, label; };   // one version: a map of a Ghosts source
    void StartCompare(const CompareTarget* only = nullptr);
    void StopCompare();
    void CycleCompare(int step);
    void UpdateCompare();
    void DrawCompare();
    void ShowCompareClip(const std::string& tag, const TerrainClipboard& clip);
    /// Enter in a compare: paste the shown version; a reviewed difference is marked pasted (its blend band too).
    void CommitCompare();
    /// Minimaps of the open map's edited tiles as they are now (only those whose edits changed since the last
    /// render), into <project>/minimaps/ for export.
    void RenderMinimaps();

    // Differences: another version of the map scanned against this one; each edited area is a catalog card to
    // review in place, then approve (paste) or reject.
    struct DiffCandidate { size_t source = 0; std::string map, label; };
    Differences m_diffs;
    DiffCandidate m_diffTarget;                        // the version being (or last) scanned
    std::string m_diffActive;                          // key of the area under review
    std::vector<std::pair<int, int>> m_diffPending;    // its chunks, selected once their tiles are loaded
    std::map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_diffThumbs;
    std::set<std::string> m_diffNoThumb;               // areas too large for a picture
    int m_diffSort = 0;
    bool m_diffShowRejected = false, m_diffShowNewTerrain = false;
    char m_diffQuery[64] = {};
    static constexpr int kDifferencesTab = -4;
    std::vector<DiffCandidate> DiffCandidates() const;
    void StartDifferences(const DiffCandidate& c);
    void UpdateDifferences();
    void DrawDifferences();
    void ReviewDifference(const Differences::Region& r);
    void RejectDifference();
    void RenderDifferenceThumb(const Differences::Region& r);
    // A reviewed area on tiles the map lacks: those tiles shown alone from the other version until Enter adds them.
    std::set<int> m_diffNewTiles;
    int m_diffNewLayer = 0;
    void AddDifferenceTiles();
    void EndNewTiles();

    std::vector<Blueprint> m_blueprints;
    std::map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_blueprintThumbs;   // by file
    bool m_saveBlueprintOpen = false;
    char m_blueprintName[96] = {}, m_blueprintNotes[512] = {};
    std::optional<size_t> m_blueprintToDelete;
    static constexpr int kBlueprintTab = -1, kCreatureTab = -2, kGameobjectTab = -3;
    static constexpr int kPortalTab = -5;
    /// Catalog > Portal effects (Triggers tool): Blizzard's instance portal models; a click puts one at the selected trigger.
    void DrawPortalCatalog();
    /// A map doodad of `model` at the selected trigger's own position (caves included), facing the camera; one undo step.
    void PlacePortalEffect(const std::string& model);
    /// The Creatures / Gameobjects tabs: every template of the world database with a picture of its model.
    void DrawUnitCatalog(SpawnKind kind);
    std::vector<SpawnAdapter::Template> m_unitTemplates[2];   // creature_template, gameobject_template (read on first view)
    bool m_unitTemplatesRead[2] = {};
    /// The template list of a kind, read from the world database on first use (empty without one).
    const std::vector<SpawnAdapter::Template>& UnitTemplates(SpawnKind kind);

    // NPC viewer (AppNpc.cpp): one creature template in a preview of its own, after wow.export's Creatures tab:
    // animations, skins, equipment, geosets, textures.
    struct NpcView
    {
        uint32_t entry = 0;
        std::string name;
        struct Model { uint32_t displayId = 0; float scale = 1, probability = 0; };
        std::vector<Model> models;                       // creature_template_model, by Idx
        struct Equip { uint32_t id = 0, items[3] = {}, displays[3] = {}, types[3] = {}; };
        std::vector<Equip> equips;                       // creature_equip_template sets
        int equip = 0;                                   // index into equips, -1 none
        uint32_t displayId = 0;                          // shown
        float displayScale = 1;
        std::vector<uint32_t> skins;                     // displays sharing the model
        std::optional<DisplayLooks::SpawnModel> look;
        std::optional<ModelRenderer::ModelInfo> info;
        std::vector<uint16_t> geosets;                   // shown submeshes
        std::set<uint32_t> hidden;                       // items hidden, by attachment id (99 = cape)
        int sequence = -1;                               // index into info->skeleton->sequences
        std::shared_ptr<const ModelSkeleton> pose;
        float timeMs = 0, speed = 1;
        bool paused = false, autoCamera = true;
        float yaw = 0.6f, pitch = 0.25f, distance = 6, center[3] = {};
        float background[3] = { 0.16f, 0.17f, 0.20f };
        bool humanoid = false;                           // a CreatureDisplayInfoExtra display (character model)
        bool focus = false;                              // bring the window forward next frame
        std::string filter, listedFor = "\x01";          // template list filter; the filter `listed` was built for
        std::vector<size_t> listed;                      // indices into UnitTemplates(Creature) passing the filter
        // Editing: the rows as edited, applied together as one undo step. The preview follows the edits.
        nlohmann::json editTemplate;                     // creature_template row (null: the entry has none)
        std::vector<nlohmann::json> editModels, editEquips;   // creature_template_model / creature_equip_template rows
        uint64_t editRevision = ~0ull;                   // change store revision the rows were read at
        bool dirty = false;                              // edited and not applied
        std::string columnFilter;
        struct Item { std::string name; uint32_t display = 0, type = 0; bool found = false; uint32_t quality = 1; };
        std::map<uint32_t, Item> items;                  // item_template rows read so far, by entry
        int pickSet = -1, pickSlot = 0;                  // the equipment slot the item picker fills
        std::string pickQuery;
        std::vector<std::pair<uint32_t, Item>> pickHits;
        uint32_t pendingOpen = 0;                        // asked to open this entry while edits were not applied
        bool pendingShow = false;                        // ... in the viewer window
        // Appearance: a project character display (CreatureDisplayInfo + CreatureDisplayInfoExtra rows) as edited.
        uint32_t appearanceId = 0;                       // the display editDisplay / editExtra are for (0 none)
        nlohmann::json editDisplay, editExtra;
        int pickArmor = -1;                              // the NPCItemDisplay slot the item picker fills (-1: an equipment set)
        bool pickLoot = false;                           // the item picker adds a loot row instead
        // Loot as edited, per kind (0 drops, 1 pickpocketing, 2 skinning): the rows of the loot id the template names.
        struct Loot { uint32_t id = ~0u; std::vector<nlohmann::json> rows; };
        Loot loot[3];
        int lootKind = 0;
        std::map<uint32_t, std::vector<std::vector<std::string>>> references;   // reference_loot_template rows read so far
        // Dialogue as edited: gossip menus by id (their gossip_menu, gossip_menu_option and conditions rows), the
        // npc_text rows they show, and the creature's creature_text lines.
        struct Dialogue
        {
            std::map<uint32_t, std::vector<nlohmann::json>> menus, options, conditions;
            std::map<uint32_t, nlohmann::json> texts;          // null: no such row
            uint32_t current = 0;                              // the menu shown
            std::vector<uint32_t> trail;                       // submenus followed from the root
            std::vector<nlohmann::json> barks;
            bool barksLoaded = false;
        } dialogue;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> color, depth;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        UINT width = 0, height = 0;
    } m_npc;
    bool m_showNpc = false;
    void DrawNpcViewer();
    /// Makes a creature template the one the NPC tabs edit (reads its rows); `show` also opens the viewer window.
    void OpenNpc(uint32_t entry, bool show = true);
    /// Every frame, viewer open or not: re-read after an undo or redo, and ask before dropping unapplied edits.
    void UpdateNpc();
    /// The Apply / Revert line and the editing tabs (View only in the viewer); `first` draws extra tabs before them.
    void DrawNpcEditor(bool view, const std::function<void()>& first = {});
    uint32_t m_npcSpawnGuid = 0;   // the creature spawn the Inspector last opened in the NPC tabs
    /// Reads the template's rows into the edit buffers (dropping edits) and refreshes the preview; `frame` re-aims the camera.
    void LoadNpc(uint32_t entry, bool frame);
    /// The preview's model and equipment lists from the edit buffers.
    void RebuildNpcLists();
    /// An item_template row (name, display, inventory type), read once.
    const NpcView::Item& NpcItem(uint32_t entry);
    /// Writes the edited rows (one undo step) and reloads the template on a running server.
    void ApplyNpc();
    /// A copy of the open template under the next free entry of the project's range, with its models and equipment.
    void DuplicateNpc();
    /// Deletes the open template, models and equipment (entries of the project's range only).
    void DeleteNpc();
    void DrawNpcViewTab();
    void DrawNpcTemplateTab();
    void DrawNpcGearTab();
    void DrawItemPicker();
    void DrawNpcAppearanceTab();
    void DrawNpcLootTab();
    /// The loot rows of one kind for the template's current loot id (read when the id changes).
    NpcView::Loot& NpcLoot(int kind);
    // Dialogue tab (AppDialogue.cpp).
    void DrawNpcDialogueTab();
    void DrawGossip();
    void DrawBarks();
    /// Conditions on one menu text (source 14) or option (15): rows of the menu's conditions, edited in place.
    void DrawConditions(uint32_t menu, int source, uint32_t entry);
    void LoadGossipMenu(uint32_t menu);
    /// A new gossip menu with one new text in the project's ranges (not written until Apply); 0 when out of ids.
    uint32_t NewGossipMenu(const std::string& text);
    uint32_t NewNpcText(const std::string& text);
    uint32_t NewDialogueId(TableRowsAdapter& table, const char* kind, const std::vector<uint32_t>& buffered);
    /// The dialogue's changed rows as change parts; `reloads` gets the tables a running server can reload.
    void DialogueChanges(std::vector<Change>& parts, const std::string& label, std::set<std::string>& reloads);
    /// A new editable character display in the project's ranges: a copy of `from` (its baked texture dropped, so the
    /// client composites the edits), or a new human when 0. It replaces `from` in the template's models (else is added).
    void NewNpcAppearance(uint32_t from);
    /// Rebuilds the shown display after an appearance edit, keeping the camera and the animation.
    void RefreshNpcDisplay();
    /// Shows one display of the open template: rebuilds its look, skins, geosets and animation list.
    void SetNpcDisplay(uint32_t displayId, float scale);
    /// Rebuilds the look after the equipment set changed (geosets and hidden items stay).
    void RefreshNpcLook();
    void SetNpcSequence(int sequence);
    void DrawNpcPreview(const ImVec2& size);
    uint32_t m_unitCategory[2] = { ~0u, ~0u };                // folder picked in the tree (~0 = everything)
    std::vector<Ghosts::Version> m_versions;        // of the tile under the camera
    std::string m_versionsKey;
    std::vector<const Catalog::Item*> m_catalogItems;
    std::set<ChunkRef> m_selection;
    bool m_looking = false;
    bool m_boxing = false;
    float m_boxStart[2] = {};
    TerrainClipboard m_clipboard;
    enum class PasteHeight { FollowGround, FollowSlope, LowestPoint, Absolute };
    std::optional<PasteHeight> m_heightBeforeInPlace;   // paste in place switches to Absolute; this puts it back
    PasteHeight m_pasteHeightMode = PasteHeight::FollowGround;
    float m_pasteOffset = 0;
    bool m_pasteHeights = true, m_pasteTextures = true, m_pasteHoles = true, m_pasteObjects = true, m_pasteWater = true;
    bool m_pasteMapWater = false;   // pasted water takes the map's liquid and level (PasteOptions::mapWater)
    bool m_holeCut = true;          // Holes tool: cut (true) or fill; Ctrl inverts while dragging
    bool m_impassMode = false;      // Holes tool: paints the chunks' impassable flag instead (cut = set)
    // Paint > Swap: the texture swapped out (for the active texture, or removed)
    std::string m_swapFrom;
    bool m_swapBrush = false, m_swapRemove = false;
    std::string SwapLabel() const;
    float m_vertexStep = 1.0f, m_vertexHeight = 0.0f;   // Sculpt > Vertices
    void EditVertices(TerrainAdapter::VertexOp op, float amount);
    WaterBrush m_water;             // Water tool; Ctrl swaps Add and Remove while dragging
    bool m_waterFromClick = true;   // Add: each stroke matches the water it starts on or near, else m_waterDepth above the ground
    static constexpr float kWaterMatchRadius = 80.0f;   // yards to look for water to match
    float m_waterDepth = 2.0f;
    void DrawWaterPanel(float w);
    float m_holeRadius = 1.0f;      // yards; small values hit only the cell under the cursor
    bool m_ghostPreview = true;
    int m_clipVersion = 0;                       // bumps when the clipboard changes (copy, rotate)
    bool m_placing = false;                      // Copy tool: clipboard follows the cursor
    std::optional<std::pair<int, int>> m_pin;    // pinned centre cell (grid), when pinned
    bool m_blend = true, m_blendAuto = true;
    float m_blendWidth = 30.0f;                  // yards per side when not automatic
    PastePlan m_plan;
    std::string m_planKey;                       // inputs m_plan was built from; empty = nothing shown

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_vpColor;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_vpRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_vpSrv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_vpDsv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_vpDepth, m_vpDepthStaging;   // depth + CPU copy for box-select occlusion
    UINT m_vpWidth = 0, m_vpHeight = 0;

    // server link
    std::vector<ServerProfile> m_profiles;
    Db m_db;
    std::optional<bool> m_soapOk;            // last SOAP call worked; unknown until one runs
    bool m_setupOpen = false;
    bool m_settingsOpen = false;                       // Project settings dialog (AppServer.cpp)
    std::string m_settingsName, m_settingsAuthor;
    std::map<std::string, Project::IdRange> m_settingsRanges;
    std::map<std::string, SpawnAdapter::RangeUse> m_settingsUse;
    ServerProfile m_setup;                   // the wizard's working copy
    std::string m_setupClient, m_setupDbPassword, m_setupSoapPassword;
    std::string m_setupDbResult, m_setupSoapResult;
    bool m_setupDbOk = false, m_setupSoapOk = false;
    std::string m_command;                   // Server panel: GM command being typed
    std::vector<std::pair<std::string, std::string>> m_commandLog;   // command, output (newest last)

    SpawnAdapter m_creatures{ m_store, SpawnKind::Creature }, m_gameobjects{ m_store, SpawnKind::GameObject };
    SpawnKind m_spawnKind = SpawnKind::Creature;         // the table of the Creatures / Gameobjects tool last used
    std::vector<Spawn> m_spawnView;                      // spawns of both kinds around the camera (database + project)
    std::set<uint32_t> m_spawnSel;                       // selected spawn guids (of m_spawnKind)
    std::optional<uint32_t> m_spawnHover;                // spawn under the cursor (marker or model)
    std::optional<uint32_t> m_spawnPress;                // spawn under the cursor when the button went down
    bool m_showSpawns[2] = { true, true };               // creatures, gameobjects: markers, names and models
    std::optional<int> m_spawnEvent;                     // game event filter: none = every spawn, 0 = no event running, N = only event N
    std::map<int, size_t> m_spawnEvents;                 // events of the spawns around the camera -> spawn count
    std::optional<std::map<int, std::string>> m_eventNames;   // game_event descriptions, loaded on first use
    std::optional<SpawnAdapter::Template> m_spawnArmed;  // template placed by clicking the ground
    std::string m_spawnQuery;
    std::vector<SpawnAdapter::Template> m_spawnResults;
    DisplayLooks m_looks{ m_mpq };                       // display ids -> models (client DBCs)
    AreaAdapter m_areas{ m_mpq, m_store };               // AreaTable.dbc: the client's rows plus the project's
    uint32_t m_activeArea = 0;                           // Zones tool: the area painted
    float m_areaRadius = 1.0f;                           // yards; small values paint only the chunk under the cursor
    std::string m_areaFilter;
    bool m_areasLoadedOnly = true;                       // Areas tab: what the loaded terrain uses, or every row of the map
    std::string m_newAreaName;
    uint32_t m_newAreaParent = 0, m_newAreaLevel = 1;
    uint32_t m_areaEditId = 0;                           // the row m_areaEdit was read from (re-read when it changes)
    uint64_t m_areaEditVersion = ~0ull;
    nlohmann::json m_areaEdit;                           // the active area's row being edited
    WmoAreaAdapter m_wmoAreas{ m_mpq, m_store };         // WMOAreaTable.dbc: areas inside buildings
    std::map<std::string, std::optional<WmoAreaKeys>> m_wmoKeys;   // WMO root name -> its WMOID and group ids (read on first use)
    WorldMapAreaAdapter m_worldMaps{ m_mpq, m_store };   // WorldMapArea.dbc: zone map pictures and their world rectangles
    WorldMapOverlayAdapter m_mapOverlays{ m_mpq, m_store };   // WorldMapOverlay.dbc: pieces revealed by exploring
    /// A world map picture waiting for its tiles: rendered (base and / or overlays) once they and their models are loaded.
    struct MapJob { uint32_t worldMap = 0; bool base = false; std::vector<uint32_t> areas; std::set<int> tiles; };
    std::optional<MapJob> m_mapJob;
    uint32_t m_spawnModelVersion[2] = { ~0u, ~0u };      // adapter version each kind's model tile was built from
    TableRowsAdapter m_waypoints{ m_store, "waypoint_data", "id", "point" }, m_addons{ m_store, "creature_addon", "guid" };
    AreaTriggerAdapter m_triggers{ m_mpq, m_store };      // AreaTrigger.dbc: what the client fires
    MapRowsAdapter m_mapRows{ m_mpq, m_store };           // Map.dbc: corpse entrance, new maps
    DbcTable m_mapDifficulty{ m_mpq, m_store, "MapDifficulty", { { "ID", 0, 'i' }, { "MapID", 1, 'i' }, { "Difficulty", 2, 'i' }, { "Message_lang", 3, 's' },
                                                                 { "RaidDuration", 20, 'i' }, { "MaxPlayers", 21, 'i' }, { "Difficultystring", 22, 's' } }, 23 };
    // File > New map: a Map.dbc row (and MapDifficulty / instance_template for instances), WDT, WDL and flat tiles.
    struct NewMapForm
    {
        std::string directory, name = "New map", texture;
        int kind = 0, maxPlayers = 5, x = 32, y = 32, size = 2;   // kind = Map.dbc InstanceType: 0 world, 1 dungeon, 2 raid
        uint32_t like = 0, area = 0;
        float height = 0;
        bool bigAlpha = true;
    };
    NewMapForm m_newMap;
    bool m_newMapOpen = false;
    std::string m_newMapError;
    uint64_t m_mapListVersion = ~0ull;
    void OpenNewMap();
    void DrawNewMapModal();
    bool CreateNewMap(std::string& error);
    void RefreshMapList();
    TableRowsAdapter m_triggerRows{ m_store, "areatrigger", "entry" }, m_teleports{ m_store, "areatrigger_teleport", "ID" },
                     m_instances{ m_store, "instance_template", "map" };
    uint32_t m_triggerSel = 0;                            // selected trigger id (0 = none)
    std::optional<uint32_t> m_triggerHover;
    enum class TriggerPick { None, Place, Move, Target, Corpse };
    TriggerPick m_triggerPick = TriggerPick::None;        // what the next click on the ground sets
    Trigger m_triggerShape{ 0, 0, 0, 0, 0, 5 };           // shape of new triggers
    bool m_triggerNewTeleport = true;
    uint32_t m_triggerEditId = 0;                         // the trigger m_triggerEdit was read from
    uint64_t m_triggerEditRevision = ~0ull;
    Trigger m_triggerEdit;
    std::optional<Teleport> m_teleportEdit;
    std::string m_triggerFilter;
    uint32_t m_entranceMap = ~0u;                         // Entrance tab: the map edited (~0 = the open map)
    mutable std::map<uint32_t, Teleport> m_teleportView;
    mutable uint64_t m_teleportViewRevision = ~0ull;
    mutable bool m_teleportViewDb = false;                // the view was read with the database connected
    mutable std::vector<Trigger> m_triggerView;           // triggers of the open map
    mutable std::pair<uint32_t, uint64_t> m_triggerViewKey{ ~0u, ~0ull };   // map + AreaTrigger version it was made for
    std::string m_entranceKey;                            // map + revision + connection m_entranceInstance was read for
    std::vector<nlohmann::json> m_entranceInstance;       // instance_template row of the Entrance tab's map
    AreaPoiAdapter m_areaPois{ m_mpq, m_store };          // AreaPOI.dbc: world map landmarks
    TableRowsAdapter m_gossipPois{ m_store, "points_of_interest", "ID" }, m_teles{ m_store, "game_tele", "id" };
    // NPC editor (AppNpc.cpp): a template, its models and its equipment sets.
    TableRowsAdapter m_npcTemplates{ m_store, "creature_template", "entry" }, m_npcModels{ m_store, "creature_template_model", "CreatureID", "Idx" },
                     m_npcEquips{ m_store, "creature_equip_template", "CreatureID", "ID" };
    // Loot (AppNpc.cpp Loot tab): what a creature drops, can be pickpocketed for and skinned for, by loot id.
    TableRowsAdapter m_lootDrops{ m_store, "creature_loot_template", "Entry", "Item" }, m_lootPickpocket{ m_store, "pickpocketing_loot_template", "Entry", "Item" },
                     m_lootSkinning{ m_store, "skinning_loot_template", "Entry", "Item" };
    // Dialogue: gossip menus, their texts and options, the conditions on those (source types 14 and 15 only), barks.
    TableRowsAdapter m_gossipMenus{ m_store, "gossip_menu", "MenuID", "TextID" }, m_gossipOptions{ m_store, "gossip_menu_option", "MenuID", "OptionID" },
                     m_npcTexts{ m_store, "npc_text", "ID" },
                     m_gossipConditions{ m_store, "conditions", "SourceGroup", "SourceEntry", "SourceTypeOrReferenceId IN (14, 15)", "gossip" },
                     m_creatureTexts{ m_store, "creature_text", "CreatureID", "GroupID" };
    /// Every world-table adapter: registered, connected, synced, exported and checked alike.
    TableRowsAdapter m_weather{ m_store, "game_weather", "zone" };   // weather chances per zone and season (Sound tool, Weather tab)
    nlohmann::json m_weatherEdit;      // the zone's row while a slider is dragged
    std::vector<TableRowsAdapter*> TableAdapters()
    {
        return { &m_waypoints, &m_addons, &m_triggerRows, &m_teleports, &m_instances, &m_gossipPois, &m_teles, &m_npcTemplates, &m_npcModels, &m_npcEquips,
                 &m_lootDrops, &m_lootPickpocket, &m_lootSkinning, &m_gossipMenus, &m_gossipOptions, &m_npcTexts, &m_gossipConditions, &m_creatureTexts,
                 &m_weather };
    }
    PoiKind m_poiKind = PoiKind::MapIcon;                 // POIs tool: the kind listed, placed and selected
    uint32_t m_poiSel = 0;                                // selected point id of m_poiKind (0 = none)
    std::optional<uint32_t> m_poiHover;
    bool m_poiShowSelected = false;                       // bring the Selected tab forward (a viewport pick; not a list click)
    enum class PoiPick { None, Place, Move };
    PoiPick m_poiPick = PoiPick::None;                    // what the next click on the ground does
    Poi m_poiNew;                                         // New tab: fields of the next point placed (per kind on switch)
    Poi m_poiEdit;                                        // Selected tab: the selected point being edited
    std::pair<int, uint32_t> m_poiEditKey{ -1, 0 };       // kind + id m_poiEdit was read from
    uint64_t m_poiEditRevision = ~0ull;
    std::string m_poiFilter;
    std::string m_poiUsesKey;                             // id + revision m_poiUses was read for
    std::vector<std::string> m_poiUses;                   // gossip options showing the selected gossip point
    mutable std::vector<Poi> m_poiView;
    mutable std::string m_poiViewKey;                     // kind, map, revision, AreaPOI version, connection
    TaxiNodesAdapter m_taxiNodes{ m_mpq, m_store };      // TaxiNodes.dbc: flight points
    TaxiPathAdapter m_taxiPaths{ m_mpq, m_store };       // TaxiPath.dbc: one way between two of them
    TaxiPathNodeAdapter m_taxiPoints{ m_mpq, m_store };  // TaxiPathNode.dbc: the points a path flies through
    // NPC appearances (AppNpc.cpp): character-model displays the project adds.
    DbcTable m_displayRows{ m_mpq, m_store, "CreatureDisplayInfo",
                            { { "ID", 0, 'i' }, { "ModelID", 1, 'i' }, { "SoundID", 2, 'i' }, { "ExtendedDisplayInfoID", 3, 'i' },
                              { "CreatureModelScale", 4, 'f' }, { "CreatureModelAlpha", 5, 'i' }, { "TextureVariation[0]", 6, 's' },
                              { "TextureVariation[1]", 7, 's' }, { "TextureVariation[2]", 8, 's' }, { "PortraitTextureName", 9, 's' },
                              { "SizeClass", 10, 'i' }, { "BloodID", 11, 'i' }, { "NPCSoundID", 12, 'i' }, { "ParticleColorID", 13, 'i' },
                              { "CreatureGeosetData", 14, 'i' }, { "ObjectEffectPackageID", 15, 'i' } },
                            16 };
    DbcTable m_extraRows{ m_mpq, m_store, "CreatureDisplayInfoExtra",
                          { { "ID", 0, 'i' }, { "DisplayRaceID", 1, 'i' }, { "DisplaySexID", 2, 'i' }, { "SkinID", 3, 'i' }, { "FaceID", 4, 'i' },
                            { "HairStyleID", 5, 'i' }, { "HairColorID", 6, 'i' }, { "FacialHairID", 7, 'i' }, { "NPCItemDisplay[0]", 8, 'i' },
                            { "NPCItemDisplay[1]", 9, 'i' }, { "NPCItemDisplay[2]", 10, 'i' }, { "NPCItemDisplay[3]", 11, 'i' },
                            { "NPCItemDisplay[4]", 12, 'i' }, { "NPCItemDisplay[5]", 13, 'i' }, { "NPCItemDisplay[6]", 14, 'i' },
                            { "NPCItemDisplay[7]", 15, 'i' }, { "NPCItemDisplay[8]", 16, 'i' }, { "NPCItemDisplay[9]", 17, 'i' },
                            { "NPCItemDisplay[10]", 18, 'i' }, { "Flags", 19, 'i' }, { "BakeName", 20, 's' } },
                          21 };
    /// Every client DBC the project edits: registered, reset, exported alike.
    std::vector<DbcTable*> DbcTables()
    {
        return { &m_areas, &m_wmoAreas, &m_worldMaps, &m_mapOverlays, &m_triggers, &m_mapRows, &m_mapDifficulty, &m_areaPois, &m_taxiNodes, &m_taxiPaths, &m_taxiPoints,
                 &m_displayRows, &m_extraRows, &m_lights.light, &m_lights.params, &m_lights.intBands, &m_lights.floatBands, &m_lights.skyboxes,
                 &m_sounds.entries, &m_sounds.ambience, &m_sounds.music, &m_sounds.intro, &m_sounds.emitters };
    }
    uint32_t m_flightNode = 0, m_flightPath = 0;         // selected node, or selected path (one at a time)
    std::optional<size_t> m_flightPoint;                 // selected point of the selected path (index)
    struct FlightHit { uint32_t node = 0, path = 0; size_t point = 0; };
    std::optional<FlightHit> m_flightHover;
    bool m_flightPlace = false;                          // the next click on the ground places a node
    bool m_flightShowSelected = false;                   // bring the Selected tab forward (a viewport pick)
    TaxiNode m_flightNew;                                // New tab: the next node's name and mounts
    uint32_t m_flightTo = 0;                             // Selected node: the node a new path goes to
    bool m_flightBoth = true;                            // ... and a path back
    int m_flightCost = 100;                              // ... costing this much copper
    float m_flightCruise = 40;                           // ... flying this high above the ground
    bool m_flightAll = true;                             // draw every path of the map, not only the selected one
    TaxiNode m_flightNodeStart;                          // being moved: as it was
    TaxiPoint m_flightPointStart;
    std::optional<TaxiNode> m_flightNodePreview;         // being moved: as it shows now
    std::optional<TaxiPoint> m_flightPointPreview;
    std::string m_flightFilter;
    std::vector<FlightMaster> m_flightMasters;
    std::string m_flightMastersKey;                      // map, connection and revision they were read for
    mutable FlightView m_flightView;
    mutable std::string m_flightViewKey;
    mutable std::vector<std::vector<int16_t>> m_flightWdl;   // the open map's WDL (FlightFloor where tiles are not loaded)
    mutable std::string m_flightWdlMap;
    float m_portalScale = 1;                              // Catalog > Portal effects: scale of the next one placed
    std::vector<std::string> m_portalModels;              // Catalog > Portal effects: the models offered
    size_t m_portalModelsKey = ~size_t(0);                // catalog doodad count they were found for
    uint32_t m_triggerTabId = 0;                          // the trigger selected last frame (another one brings its tab forward)
    struct PathEdit
    {
        uint32_t guid = 0;
        std::string name;
        std::vector<PathPoint> points;
        std::optional<size_t> sel, drag;
        bool dirty = false;
    };
    std::optional<PathEdit> m_path;                      // the creature path being edited
    std::optional<size_t> m_pathHover;                   // point under the cursor
    bool m_pathPreview = true;                           // a dot walking the path
    uint32_t m_pathViewGuid = 0;                         // creature whose saved path m_pathView holds
    uint64_t m_pathViewRevision = ~0ull;
    std::vector<PathPoint> m_pathView;

    std::vector<Problem> m_problems;
    bool m_problemsChecked = false;

    uint64_t m_problemsRevision = 0;                 // change store revision the problems were checked at
    std::vector<std::string> m_log;
    std::string m_logFilter;
    std::string m_toast;                             // the last error or warning, shown a while in the status bar
    double m_toastUntil = 0;
    bool m_toastError = false;
    std::vector<Command> m_commands;

    // Autosave: the change log is written a moment after each edit. Database edits are live at once, so the log that
    // can undo and revert them must not wait for Ctrl+S.
    uint64_t m_autosaveRevision = ~0ull;
    double m_autosaveAt = 0;
    void Autosave();
    // Recent projects (SettingsDir()/recent.txt, newest first) and the start screen listing them.
    std::vector<std::string> m_recent;
    void RememberProject(const std::string& dir);
    void DrawStartScreen(const ImVec2& origin, const ImVec2& size);
    // Help > Keyboard shortcuts: every command with a key plus the viewport's mouse and key gestures.
    bool m_showShortcuts = false;
    std::string m_shortcutFilter;
    void DrawShortcuts();
    // Changes panel: a filter, and per change: go to it, undo or redo up to it.
    std::string m_changeFilter;
    /// Flies the camera to where a change was made (a terrain tile, a spawn); false when it has no place. `go` false: only asks.
    bool GoToChange(const Change& change, bool go);
    void UndoTo(size_t doneCount);
    void RedoTo(size_t undoneCount);
    double m_speedShownUntil = 0;                    // camera speed shown in the viewport after the wheel changed it
    int m_historyPos = -1;                           // Server panel: command history position while pressing up / down
    bool m_paletteOpen = false;
    char m_paletteQuery[128] = {};
    int m_paletteSelected = 0;

    bool m_buildLayout = false;
    bool m_newProjectOpen = false;
    char m_newName[128] = {}, m_newDir[512] = {}, m_newClient[512] = {}, m_newAuthor[64] = {};
    std::function<void()> m_afterUnsaved;
    bool m_unsavedOpen = false;
    bool m_closeRequested = false, m_quit = false;
    float m_fps = 0;
};
