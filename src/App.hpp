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

#include <d3d11.h>
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
    enum class Tool { Select, Sculpt, Copy, Holes, Objects, Paint, Creatures, Gameobjects, Zones };
    /// Tools come in groups (the toolbar's buttons); a group remembers the tool last used in it.
    enum class Group { Terrain, Objects, Units, Regions };
    static constexpr const char* kGroupNames[4] = { "Terrain", "Objects", "Units", "Regions" };
    static Group GroupOf(Tool t)
    {
        return t == Tool::Objects ? Group::Objects : t == Tool::Creatures || t == Tool::Gameobjects ? Group::Units
             : t == Tool::Zones   ? Group::Regions : Group::Terrain;
    }
    void SetGroup(Group g) { m_tool = m_groupTool[int(g)]; }
    Tool m_groupTool[4] = { Tool::Sculpt, Tool::Objects, Tool::Creatures, Tool::Zones };

    struct Camera
    {
        DirectX::XMFLOAT3 pos{ 0, 0, 0 };
        float yaw = 0, pitch = -0.5f, speed = 80.0f;
        DirectX::XMVECTOR Forward() const;
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

    // populate: creature waypoint paths (AppPaths.cpp)
    CreaturePath PathOf() { return { m_creatures, m_waypoints, m_addons }; }
    void BeginPathEdit(uint32_t guid);
    void SavePathEdit();
    void CancelPathEdit();
    void DeletePathPoint();
    /// The selected point onto the terrain under it.
    void DropPathPoint();
    /// Translate handles on the selected path point; true while the cursor is on them (the viewport then ignores clicks).
    bool UpdatePathGizmo(const ImVec2& origin, const ImVec2& size);
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
    bool Save();
    void Export(bool playTest);
    /// Exports, then packs out/client into the project's patch MPQ (out/<patchName>); with `install`, copies it into
    /// the client's Data folder too (the client must be closed: it holds its archives open).
    void BuildPatch(bool install);
    void Undo();
    void Redo();
    void GoToTile(const std::string& map, int x, int y);
    void FocusTile();
    void CopySelection();
    void PasteAtCursor();
    void RotateClipboard(int quarterTurns);
    void RotateSelectionInPlace();
    /// Height shift applied to the clipboard at this anchor (snap mode adds the ground offset).
    float PasteOffsetAt(int gx, int gz) const;
    /// Tilt for Follow slope (zero otherwise), written into the paste options.
    void PasteSlopeAt(int gx, int gz, PasteOptions& options) const;
    /// Placing: the ghost follows the cursor; once pinned the terrain shows the blended result.
    void UpdatePlacement();
    void ClearPlacementView();
    void CommitPlacement();
    void CancelPin();
    void SelectMap(size_t index);
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
    /// Move / rotate / scale handles on the selected objects; true while the mouse is over or dragging one.
    bool UpdateGizmo(const ImVec2& origin, const ImVec2& size);
    bool UpdateObjectGizmo(const ImVec2& origin, const ImVec2& size);
    /// The handles' frame: the object's own (one object) or the selection's centre (several).
    DirectX::XMFLOAT4X4 GizmoFrame() const;
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
    TerrainAdapter m_terrain{ m_mpq, m_renderer, m_store };
    ModelRenderer m_models;
    ModelRenderer::DrawSettings m_modelSettings;
    Loader m_loader;   // after m_mpq: destroyed (stopped) before the archives close
    std::optional<Project> m_project;

    std::vector<MapEntry> m_maps;
    int m_mapIndex = -1;
    std::vector<bool> m_mapTiles;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_mapPreview;   // the selected map from its WDL, coloured by height
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_mapPreviewTexture;
    std::vector<std::vector<int16_t>> m_mapWdl;   // the selected map's low-detail heights, loaded tiles patched with their current heights
    std::string m_mapPreviewDir;                  // the map m_mapWdl belongs to
    uint64_t m_mapPreviewKey = ~0ull;             // project revision + loaded tiles the picture was made from
    char m_mapFilter[64] = {};

    Camera m_camera;
    int m_loadRadius = 2;
    std::optional<int> m_focusTile;   // tile key whose height the camera snaps to once it streams in
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
    bool m_gizmoActive = false;                         // a handle is being dragged (one object edit)
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
    std::optional<int> m_catalogShowTab;  // a tab the catalog should bring forward
    void PickTexture(const std::string& path);
    /// The project's reader falls back to the attached sources (assets pasted from other clients).
    void UpdateFallbacks();
    std::string m_catalogKey;             // inputs m_catalogItems was filtered with

    // Ghost layers: other versions of the map (other clients, single patch archives).
    Ghosts m_ghosts;
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
    /// The Creatures / Gameobjects tabs: every template of the world database with a picture of its model.
    void DrawUnitCatalog(SpawnKind kind);
    std::vector<SpawnAdapter::Template> m_unitTemplates[2];   // creature_template, gameobject_template (read on first view)
    bool m_unitTemplatesRead[2] = {};
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
    bool m_holeCut = true;          // Holes tool: cut (true) or fill; Ctrl inverts while dragging
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
    std::optional<SpawnAdapter::Template> m_spawnArmed;  // template placed by clicking the ground
    std::optional<float> m_spawnPending;                 // facing being dragged, committed on release
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

    std::vector<std::string> m_log;
    std::vector<Command> m_commands;
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
