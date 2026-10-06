// wow-world-editor: window, Direct3D 11 device and the frame loop. The editor itself is App.
//
// `wow-world-editor.exe --selftest` runs the parser checks and exits 0 on success.
// `wow-world-editor.exe --check <Data dir> <map> <x> <y>` parses one real tile and prints a summary.

#include "App.hpp"
#include "Assets.hpp"
#include "Blend.hpp"
#include "Catalog.hpp"
#include "Formats.hpp"
#include "Ghosts.hpp"
#include "Minimap.hpp"
#include "Loader.hpp"
#include "ModelRenderer.hpp"
#include "Models.hpp"
#include "Mpq.hpp"
#include "Project.hpp"
#include "Changes.hpp"
#include "Renderer.hpp"
#include "Server.hpp"
#include "Looks.hpp"
#include "Paths.hpp"
#include "Spawns.hpp"
#include "Tables.hpp"
#include "Terrain.hpp"
#include "Flights.hpp"
#include "Pois.hpp"
#include "Triggers.hpp"
#include "Lights.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <map>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    ComPtr<ID3D11Device> g_device;
    ComPtr<ID3D11DeviceContext> g_context;
    ComPtr<IDXGISwapChain> g_swapChain;
    ComPtr<ID3D11RenderTargetView> g_rtv;
    UINT g_resizeWidth = 0, g_resizeHeight = 0;
    App* g_app = nullptr;

    void CreateTarget()
    {
        ComPtr<ID3D11Texture2D> back;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
    }

    bool CreateDevice(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                                 &sd, &g_swapChain, &g_device, nullptr, &g_context)))
            return false;
        CreateTarget();
        return true;
    }

    LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
        switch (msg)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                g_resizeWidth = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // no ALT menu
            break;
        case WM_CLOSE:
            if (g_app) { g_app->RequestClose(); return 0; }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    /// `--check <data dir> <map> <x> <y>`: parse one tile and its textures without a window.
    int Check()
    {
        if (__argc < 6 || !__wargv) return 2;
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        const size_t archives = mpq.Open(arg(2));
        const std::string base = "World\\Maps\\" + arg(3) + "\\" + arg(3);
        const auto wdt = mpq.Read(base + ".wdt");
        const auto bytes = mpq.Read(base + "_" + arg(4) + "_" + arg(5) + ".adt");
        printf("archives %zu, wdt %s, adt %s\n", archives, wdt ? "found" : "MISSING", bytes ? "found" : "MISSING");
        if (!wdt || !bytes) return 1;
        const auto adt = ParseAdt(*bytes, WdtBigAlpha(*wdt));
        if (!adt) return 1;
        size_t blpOk = 0;
        for (const auto& t : adt->textures)
        {
            const auto tex = mpq.Read(t);
            const auto img = tex ? ParseBlp(*tex) : std::nullopt;
            if (img) ++blpOk;
            printf("  %-60s %s\n", t.c_str(), img ? (std::to_string(img->width) + "x" + std::to_string(img->height)).c_str() : "FAILED");
        }
        const auto maps = mpq.Read("DBFilesClient\\Map.dbc");
        printf("chunks %zu, textures %zu/%zu, doodads %zu, wmos %zu, maps in Map.dbc %zu\n", adt->chunks.size(), blpOk,
               adt->textures.size(), adt->doodads.size(), adt->wmos.size(), maps ? ParseMapDbc(*maps).size() : size_t(0));
        printf("liquids %zu (%zu from old MCLQ chunks)\n", adt->liquids.size(),
               size_t(std::count_if(adt->liquids.begin(), adt->liquids.end(), [](const AdtLiquid& l) { return l.fromMclq; })));
        for (const std::string& p : ValidateAdt(*bytes, WdtBigAlpha(*wdt))) printf("structure: %s\n", p.c_str());
        printf("vertex colours: wdt %s, chunks with MCCV %zu\n", WdtVertexColors(*wdt) ? "on" : "off",
               size_t(std::count_if(adt->chunks.begin(), adt->chunks.end(), [](const AdtChunk& c) { return !c.colors.empty(); })));
        return blpOk == adt->textures.size() ? 0 : 1;
    }

    /// `--rewrite-check <data dir> <map> <x> <y>`: give chunk 0 the layers of chunk 100, rewrite, re-parse and
    /// compare every chunk with the original. Exit 0 when only chunk 0's layers changed.
    int RewriteCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        mpq.Open(arg(2));
        const std::string base = "World\\Maps\\" + arg(3) + "\\" + arg(3);
        const auto wdt = mpq.Read(base + ".wdt");
        const auto bytes = mpq.Read(base + "_" + arg(4) + "_" + arg(5) + ".adt");
        if (!wdt || !bytes) { printf("missing files\n"); return 1; }
        const bool big = WdtBigAlpha(*wdt);
        const auto original = ParseAdt(*bytes, big);
        if (!original || original->chunks.size() < 101) { printf("parse failed\n"); return 1; }

        Adt edited = *original;
        AdtChunk& target = edited.chunks[0];
        const AdtChunk& source = edited.chunks[100];
        target.layerCount = source.layerCount;
        target.textureIds = source.textureIds;
        target.layerFlags = source.layerFlags;
        target.effectIds = source.effectIds;
        target.alpha = source.alpha;
        edited.textures.push_back("Tileset\\Test\\NewTexture.blp");

        const auto rewritten = RewriteAdt(*bytes, edited, { 0 }, big);
        const auto back = rewritten.empty() ? std::nullopt : ParseAdt(rewritten, big);
        if (!back) { printf("rewritten file does not parse\n"); return 1; }

        int problems = 0;
        for (const auto& p : ValidateAdt(rewritten, big)) { printf("structure: %s\n", p.c_str()); ++problems; }   // incl. the client's sub-chunk walk
        if (back->textures != edited.textures) { printf("texture list differs\n"); ++problems; }
        if (back->doodads.size() != original->doodads.size() || back->wmos.size() != original->wmos.size()) { printf("placements differ\n"); ++problems; }
        for (size_t i = 0; i < original->chunks.size(); ++i)
        {
            const AdtChunk& want = i == 0 ? target : original->chunks[i];
            const AdtChunk& got = back->chunks[i];
            if (got.heights != want.heights || got.layerCount != want.layerCount || got.textureIds != want.textureIds)
            { printf("chunk %zu: heights or layers differ\n", i); ++problems; continue; }
            int worst = 0;
            for (size_t p = 0; p < got.alpha.size(); ++p) worst = std::max(worst, std::abs(int(got.alpha[p]) - int(want.alpha[p])));
            if (worst > (big ? 0 : 8)) { printf("chunk %zu: alpha off by %d\n", i, worst); ++problems; }
        }
        printf("%s: %zu -> %zu bytes, big alpha %d, %d problem(s)\n", (arg(3) + "_" + arg(4) + "_" + arg(5)).c_str(), bytes->size(),
               rewritten.size(), big ? 1 : 0, problems);
        return problems ? 1 : 0;
    }
}

namespace
{
    /// `--shade-check <data dir>`: on Northrend (vertex shading on), shade a stroke, undo and redo it, export and
    /// read the tile back: the written MCCV matches the editor, other chunks keep Blizzard's bytes, the file validates.
    int ShadeCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
        { printf("no WARP device\n"); return 1; }
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) { printf("renderer: %s\n", error.c_str()); return 1; }
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        if (!terrain.SetMap("Northrend", error) || !terrain.LoadNow(30, 22, error)) { printf("%s\n", error.c_str()); return 1; }
        if (!terrain.VertexColors()) { printf("Northrend: vertex shading off?\n"); return 1; }
        const int key = TileKey(30, 22);
        const Adt original = terrain.Tiles().at(key).adt;

        const XMFLOAT3 centre{ (30 * 16 + 8) * kChunkSize, 0, (22 * 16 + 8) * kChunkSize };
        PaintBrush brush;
        brush.radius = 20;
        brush.pressure = 1;
        terrain.BeginShade();
        for (int i = 0; i < 30; ++i) terrain.ShadeStep(centre, brush, { 200, 40, 40 }, false, 0.05f);
        auto change = terrain.EndShade();
        if (!change) { printf("shade made no change\n"); return 1; }
        store.Commit(*change);
        const Adt shaded = terrain.Tiles().at(key).adt;
        int problems = 0;
        const auto at = terrain.ShadeAt(centre.x, centre.z);
        printf("%s; colour at the centre %d %d %d\n", change->label.c_str(), at ? (*at)[0] : -1, at ? (*at)[1] : -1, at ? (*at)[2] : -1);
        if (!at || (*at)[0] < 190 || (*at)[1] > 50) ++problems;
        size_t changed = 0;
        for (size_t ci = 0; ci < shaded.chunks.size(); ++ci) changed += shaded.chunks[ci].colors != original.chunks[ci].colors;
        if (changed != change->data.at("colors").size()) ++problems;

        store.Undo();
        size_t restored = 0;
        for (size_t ci = 0; ci < original.chunks.size(); ++ci) restored += terrain.Tiles().at(key).adt.chunks[ci].colors == original.chunks[ci].colors;
        store.Redo();
        printf("undo restored %zu/%zu chunks\n", restored, original.chunks.size());
        if (restored != original.chunks.size()) ++problems;

        // Copy the shaded 2x2 chunks and paste them blended 4 chunks away: the shading comes along, fading in at the edge.
        std::set<ChunkRef> sel;
        const int gx = 30 * 16 + 7, gz = 22 * 16 + 7;
        for (int dz = 0; dz < 2; ++dz)
            for (int dx = 0; dx < 2; ++dx) sel.insert(*terrain.ChunkAtGrid(gx + dx, gz + dz));
        const PastePlan plan = terrain.PlanPaste(terrain.Copy(sel), gx + 4, gz, 0.0f, PasteOptions{});
        auto pasteChange = terrain.ApplyPlan(plan, "paste check");
        if (!pasteChange || !pasteChange->data.contains("colors")) { printf("paste carried no shading\n"); return 1; }
        store.Commit(*pasteChange);
        const auto pastedAt = terrain.ShadeAt(centre.x + 4 * kChunkSize, centre.z);
        printf("paste: %zu chunk(s) reshaded, colour at the pasted centre %d %d %d\n", pasteChange->data.at("colors").size(),
               pastedAt ? (*pastedAt)[0] : -1, pastedAt ? (*pastedAt)[1] : -1, pastedAt ? (*pastedAt)[2] : -1);
        if (!pastedAt || (*pastedAt)[0] < 150 || (*pastedAt)[1] > 90) ++problems;

        const std::filesystem::path out = std::filesystem::temp_directory_path() / "wow-world-editor-shadecheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        const size_t files = terrain.Export(out, error);
        printf("export: %zu file(s)%s%s\n", files, error.empty() ? "" : ", ", error.c_str());
        std::ifstream f(out / "World" / "Maps" / "Northrend" / "Northrend_30_22.adt", std::ios::binary);
        const std::vector<uint8_t> written((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        const auto back = ParseAdt(written, true);
        size_t same = 0;
        if (back)
            for (size_t ci = 0; ci < back->chunks.size() && ci < shaded.chunks.size(); ++ci) same += back->chunks[ci].colors == terrain.Tiles().at(key).adt.chunks[ci].colors;
        const auto issues = ValidateAdt(written, true);
        printf("export: %zu bytes, MCCV matches the editor in %zu/%zu chunks, %zu validation problem(s)\n", written.size(), same,
               shaded.chunks.size(), issues.size());
        if (!back || same != shaded.chunks.size() || !issues.empty()) ++problems;
        std::filesystem::remove_all(out, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--plan-check <data dir>`: stream Northshire on a software device, copy 2x2 chunks, plan a blended
    /// paste 6 chunks away and check the result: finite heights, closed seams, at most four layers.
    int PlanCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
        { printf("no WARP device\n"); return 1; }
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) { printf("renderer: %s\n", error.c_str()); return 1; }
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap("Azeroth", error)) { printf("%s\n", error.c_str()); return 1; }
        for (int i = 0; i < 9; ++i) terrain.Stream(32.5f * kTileSize, 48.5f * kTileSize, 1, error);
        printf("tiles loaded %zu\n", terrain.Tiles().size());

        const int sx = 32 * 16 + 4, sz = 48 * 16 + 4;   // copy 2x2 chunks from Northshire
        std::set<ChunkRef> sel;
        for (int dz = 0; dz < 2; ++dz)
            for (int dx = 0; dx < 2; ++dx)
                if (auto r = terrain.ChunkAtGrid(sx + dx, sz + dz)) sel.insert(*r);
        const TerrainClipboard clip = terrain.Copy(sel);

        PasteOptions options;
        const auto t0 = std::chrono::steady_clock::now();
        const PastePlan plan = terrain.PlanPaste(clip, sx + 6, sz + 6, 0.0f, options);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

        int problems = 0;
        std::map<std::pair<int, int>, const PastePlan::Chunk*> byGrid;
        for (const auto& pc : plan.chunks)
        {
            byGrid[terrain.GridOf(pc.ref)] = &pc;
            for (float h : pc.heights)
                if (pc.setHeights && !std::isfinite(h)) { ++problems; break; }
            if (!pc.layers.is_null() && pc.layers.at("names").size() > 4) ++problems;
        }
        // Seams: a planned chunk's right edge must equal its right neighbour's left edge (absolute heights).
        float worstSeam = 0;
        auto absolute = [&](const std::pair<int, int>& g, size_t j) {
            const AdtChunk* c = terrain.Chunk(*terrain.ChunkAtGrid(g.first, g.second));
            auto it = byGrid.find(g);
            return c->baseY + (it != byGrid.end() && it->second->setHeights ? it->second->heights[j] : c->heights[j]);
        };
        for (const auto& [g, pc] : byGrid)
        {
            const std::pair<int, int> right{ g.first + 1, g.second }, below{ g.first, g.second + 1 };
            for (int k = 0; k <= 8; ++k)
            {
                if (terrain.ChunkAtGrid(right.first, right.second))
                    worstSeam = std::max(worstSeam, std::fabs(absolute(g, size_t(k * 17 + 8)) - absolute(right, size_t(k * 17))));
                if (terrain.ChunkAtGrid(below.first, below.second))
                    worstSeam = std::max(worstSeam, std::fabs(absolute(g, size_t(8 * 17 + k)) - absolute(below, size_t(k))));
            }
        }
        if (worstSeam > 0.01f) ++problems;

        // Spikes: no inner vertex may stand out from its cell's four corners much more than it did before.
        // Copy intact: every pasted vertex equals the copied height (offset 0 here).
        float worstSpike = 0, worstCopy = 0;
        for (const auto& pc : plan.chunks)
        {
            if (!pc.setHeights) continue;
            const AdtChunk& c = *terrain.Chunk(pc.ref);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 8; ++col)
                {
                    auto bump = [&](const std::array<float, 145>& h) {
                        const float corners = (h[size_t(row * 17 + col)] + h[size_t(row * 17 + col + 1)] + h[size_t((row + 1) * 17 + col)] +
                                               h[size_t((row + 1) * 17 + col + 1)]) / 4;
                        return std::fabs(h[size_t(row * 17 + 9 + col)] - corners);
                    };
                    worstSpike = std::max(worstSpike, bump(pc.heights) - bump(c.heights));
                }
            const auto [gx, gz] = terrain.GridOf(pc.ref);
            for (const auto& e : clip.chunks)
                if (gx == sx + 6 + e.dx && gz == sz + 6 + e.dz)
                    for (size_t j = 0; j < 145; ++j) worstCopy = std::max(worstCopy, std::fabs(c.baseY + pc.heights[j] - e.heights[j]));
        }
        if (worstSpike > 2.0f) ++problems;
        if (worstCopy > 0.001f) ++problems;
        printf("footprint %zu, planned chunks %zu, blend width %.1f yd, worst seam %.4f yd, worst spike %.2f yd, copy error %.4f yd, %.0f ms, %d problem(s)\n",
               plan.footprint.size(), plan.chunks.size(), plan.widthYards, worstSeam, worstSpike, worstCopy, ms, problems);

        // Objects: copy 4x4 chunks with their objects, rotate, paste, then the export must list them with fresh ids.
        std::set<ChunkRef> wide;
        for (int dz = 0; dz < 4; ++dz)
            for (int dx = 0; dx < 4; ++dx)
                if (auto r = terrain.ChunkAtGrid(sx + dx, sz + dz)) wide.insert(*r);
        for (const WmoPlacement& w : terrain.Tiles().at(TileKey(32, 48)).adt.wmos)   // plus the chunk under one small WMO
            if (w.extMax[0] - w.extMin[0] < 40)
            {
                if (auto r = terrain.ChunkAtGrid(int(w.pos[0] / kChunkSize), int(w.pos[2] / kChunkSize))) wide.insert(*r);
                printf("including %s\n", w.model.c_str());
                break;
            }
        TerrainClipboard objClip = terrain.Copy(wide);
        objClip.RotateClockwise();
        const PastePlan objPlan = terrain.PlanPaste(objClip, sx + 8, sz + 6, 0.0f, options);
        std::map<int, size_t> objectsBefore;
        for (const auto& [key, tile] : terrain.Tiles()) objectsBefore[key] = tile.adt.doodads.size() + tile.adt.wmos.size();
        std::map<std::pair<int, int>, nlohmann::json> texturesBefore;   // every chunk's layers before the paste
        for (const auto& [key, tile] : terrain.Tiles())
            for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) texturesBefore[{ key, int(ci) }] = TerrainAdapter::LayerState(tile, tile.adt.chunks[ci]);
        auto objChange = terrain.ApplyPlan(objPlan, "object check");
        if (!objChange) { printf("object paste made no change\n"); return 1; }
        store.Commit(*objChange);
        // Only the pasted chunks may change texture: the blend band around them changes height only.
        std::set<std::pair<int, int>> pastedCells;
        for (ChunkRef r : objPlan.footprint) pastedCells.insert({ r.tile, r.chunk });
        size_t textureLeaks = 0;
        for (const auto& [key, tile] : terrain.Tiles())
            for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
                if (!pastedCells.count({ key, int(ci) }) && TerrainAdapter::LayerState(tile, tile.adt.chunks[ci]) != texturesBefore[{ key, int(ci) }])
                    ++textureLeaks;
        printf("paste textures: %zu chunk(s) outside the pasted area changed texture in the editor\n", textureLeaks);
        if (textureLeaks) ++problems;
        const auto& added = objChange->data.at("objects");
        int objectProblems = objClip.doodads.empty() || added.size() != objPlan.doodads.size() + objPlan.wmos.size() ? 1 : 0;
        printf("objects: copied %zu doodads + %zu WMOs, planned %zu + %zu, added %zu\n", objClip.doodads.size(), objClip.wmos.size(),
               objPlan.doodads.size(), objPlan.wmos.size(), added.size());

        // Edit Blizzard's own objects: move and turn one doodad, delete another, move and turn one WMO.
        const LoadedTile& home = terrain.Tiles().at(TileKey(32, 48));
        const uint32_t movedUid = home.adt.doodads[0].uniqueId, deletedUid = home.adt.doodads[1].uniqueId, wmoUid = home.adt.wmos[0].uniqueId;
        const float wmoExtBefore = home.adt.wmos[0].extMax[0] - home.adt.wmos[0].extMin[0];
        terrain.BeginObjectEdit({ { false, movedUid }, { true, wmoUid } });
        terrain.PreviewObjectEdit([](DoodadPlacement& d) { d.pos[0] += 40; d.rot[1] += 30; },
                                  [](WmoPlacement& w) { w.pos[2] += 10; w.rot[1] += 90; });
        auto moveChange = terrain.EndObjectEdit("move check");
        auto deleteChange = terrain.DeleteObjects({ { false, deletedUid } });
        if (!moveChange || !deleteChange) { printf("object move/delete made no change\n"); return 1; }
        store.Commit(*moveChange);
        store.Commit(*deleteChange);
        const WmoPlacement turned = *terrain.FindWmo(wmoUid);
        printf("edits: %s, %s; WMO x extent %.1f -> %.1f yd after a quarter turn\n", moveChange->label.c_str(), deleteChange->label.c_str(),
               wmoExtBefore, turned.extMax[0] - turned.extMin[0]);
        if (terrain.FindDoodad(deletedUid)) ++objectProblems;
        // Holes: cut two cells in one chunk, export, read the written ADT back.
        const ChunkRef target = *terrain.ChunkAtGrid(sx, sz);
        const AdtChunk& before = *terrain.Chunk(target);
        const float cell = kChunkSize / 4;
        terrain.BeginHoles();
        terrain.HoleStep({ before.baseX + cell * 0.5f, 0, before.baseZ + cell * 0.5f }, 1.0f, true);   // bit 0
        terrain.HoleStep({ before.baseX + cell * 3.5f, 0, before.baseZ + cell * 2.5f }, 1.0f, true);   // row 2, col 3 -> bit 11
        const auto holeChange = terrain.EndHoles();
        if (!holeChange) { printf("no hole change\n"); return 1; }
        store.Commit(*holeChange);
        // Area ids: paint the same chunk with an id; export writes it into the MCNK header.
        terrain.BeginAreas();
        terrain.AreaStep({ before.baseX + kChunkSize / 2, 0, before.baseZ + kChunkSize / 2 }, 1.0f, 12345);
        const auto areaChange = terrain.EndAreas("area check");
        if (!areaChange || areaChange->data.at("areas").size() != 1) { printf("area paint: expected one chunk\n"); return 1; }
        store.Commit(*areaChange);
        const std::filesystem::path out = std::filesystem::temp_directory_path() / "wow-world-editor-plancheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        terrain.Export(out, error);
        const std::string tileName = "Azeroth_" + std::to_string(target.tile % 64) + "_" + std::to_string(target.tile / 64);
        std::ifstream f(out / "World" / "Maps" / "Azeroth" / (tileName + ".adt"), std::ios::binary);
        const std::vector<uint8_t> written((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        const auto back = ParseAdt(written, false);
        const uint16_t mask = back ? back->chunks[size_t(target.chunk)].holes : 0;
        const uint16_t expected = uint16_t(before.holes | 0x0001 | 0x0800);
        int holeProblems = mask == expected ? 0 : 1;
        if (back)
            for (size_t i = 0; i < back->chunks.size(); ++i)
                if (int(i) != target.chunk && back->chunks[i].holes != terrain.Tiles().at(target.tile).adt.chunks[i].holes) ++holeProblems;
        printf("holes: %s, exported mask 0x%04x (expected 0x%04x), %d problem(s)\n", holeChange->label.c_str(), mask, expected, holeProblems);
        int areaProblems = back && back->chunks[size_t(target.chunk)].areaId == 12345 ? 0 : 1;
        if (back)
            for (size_t i = 0; i < back->chunks.size(); ++i)
                if (int(i) != target.chunk && back->chunks[i].areaId != terrain.Tiles().at(target.tile).adt.chunks[i].areaId) ++areaProblems;
        printf("areas: %s, exported area %u, %d problem(s)\n", areaChange->label.c_str(), back ? back->chunks[size_t(target.chunk)].areaId : 0, areaProblems);
        holeProblems += areaProblems;

        // Every exported tile with object changes lists exactly the loaded tile's objects (ids, models, positions),
        // and its chunk references (MCRF) are in range and cover every object.
        std::set<int> objectTiles{ TileKey(32, 48) };
        for (const auto& e : added) objectTiles.insert(TileKey(e[0], e[1]));
        for (int key : objectTiles)
        {
            const std::string name = "Azeroth_" + std::to_string(key % 64) + "_" + std::to_string(key / 64);
            std::ifstream in(out / "World" / "Maps" / "Azeroth" / (name + ".adt"), std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            const auto tile = ParseAdt(bytes, false);
            if (!tile) { printf("%s: not written or unreadable\n", name.c_str()); ++objectProblems; continue; }
            const Adt& loaded = terrain.Tiles().at(key).adt;
            int bad = 0;
            // Textures of the chunks outside the paste must come out of the export exactly as the original file has them.
            if (const auto original = mpq.Read("World\\Maps\\Azeroth\\" + name + ".adt"))
                if (const auto orig = ParseAdt(*original, false))
                {
                    size_t leaks = 0;
                    for (size_t ci = 0; ci < orig->chunks.size() && ci < tile->chunks.size(); ++ci)
                    {
                        if (pastedCells.count({ key, int(ci) })) continue;
                        const AdtChunk& a = orig->chunks[ci];
                        const AdtChunk& b = tile->chunks[ci];
                        bool same = a.layerCount == b.layerCount && a.alpha == b.alpha;
                        for (uint32_t l = 0; same && l < a.layerCount; ++l)
                            same = a.textureIds[l] < orig->textures.size() && b.textureIds[l] < tile->textures.size() &&
                                   orig->textures[a.textureIds[l]] == tile->textures[b.textureIds[l]] && a.effectIds[l] == b.effectIds[l];
                        if (!same) ++leaks;
                    }
                    if (leaks) printf("  %s: %zu chunk(s) outside the paste changed texture in the export\n", name.c_str(), leaks);
                    bad += int(leaks);

                    // Normals: reshaped chunks carry normals of their new shape; chunks far from any edit keep Blizzard's.
                    size_t relit = 0, kept = 0, wrong = 0;
                    for (size_t ci = 0; ci < orig->chunks.size() && ci < tile->chunks.size(); ++ci)
                    {
                        const AdtChunk& a = orig->chunks[ci];
                        const AdtChunk& b = tile->chunks[ci];
                        if (!a.mcnrOffset || !b.mcnrOffset) continue;
                        const bool reshaped = a.heights != b.heights || a.baseY != b.baseY;
                        const bool sameBytes = std::equal(original->begin() + std::ptrdiff_t(a.mcnrOffset), original->begin() + std::ptrdiff_t(a.mcnrOffset + 435),
                                                          bytes.begin() + std::ptrdiff_t(b.mcnrOffset));
                        if (reshaped)
                        {
                            const auto n = ChunkNormals(*tile, ci);
                            for (size_t j = 0; j < 145; ++j)
                            {
                                // Vertices on the tile border are lit with the neighbouring tile too (--normals-check covers those).
                                const size_t row = j / 17, col = j % 17;
                                if (col < 9 && ((b.indexX == 0 && col == 0) || (b.indexX == 15 && col == 8) || (b.indexY == 0 && row == 0) ||
                                                (b.indexY == 15 && row == 8)))
                                    continue;
                                const float stored[3] = { int8_t(bytes[b.mcnrOffset + j * 3]) / 127.0f, int8_t(bytes[b.mcnrOffset + j * 3 + 1]) / 127.0f,
                                                          int8_t(bytes[b.mcnrOffset + j * 3 + 2]) / 127.0f };
                                if (std::fabs(stored[0] + n[j][2]) > 0.02f || std::fabs(stored[1] + n[j][0]) > 0.02f || std::fabs(stored[2] - n[j][1]) > 0.02f) { ++wrong; break; }
                            }
                            ++relit;
                        }
                        else if (sameBytes)
                            ++kept;
                    }
                    printf("  %s normals: %zu reshaped chunk(s) relit (%zu wrong), %zu untouched chunk(s) byte-identical\n", name.c_str(), relit, wrong, kept);
                    bad += int(wrong);
                }
            if (tile->doodads.size() != loaded.doodads.size() || tile->wmos.size() != loaded.wmos.size()) ++bad;
            for (size_t i = 0; i < std::min(tile->doodads.size(), loaded.doodads.size()); ++i)
            {
                const auto& x = tile->doodads[i];
                const auto& y = loaded.doodads[i];
                if (x.uniqueId != y.uniqueId || x.model != y.model || std::fabs(x.pos[0] - y.pos[0]) > 0.01f || std::fabs(x.rot[1] - y.rot[1]) > 0.01f) ++bad;
            }
            for (size_t i = 0; i < std::min(tile->wmos.size(), loaded.wmos.size()); ++i)
                if (tile->wmos[i].uniqueId != loaded.wmos[i].uniqueId || std::fabs(tile->wmos[i].extMin[0] - loaded.wmos[i].extMin[0]) > 0.01f) ++bad;
            std::vector<int> dSeen(tile->doodads.size()), wSeen(tile->wmos.size());
            for (const AdtChunk& c : tile->chunks)
            {
                for (uint32_t r : c.doodadRefs) { if (r < dSeen.size()) dSeen[r] = 1; else ++bad; }
                for (uint32_t r : c.wmoRefs) { if (r < wSeen.size()) wSeen[r] = 1; else ++bad; }
            }
            const size_t unreferenced = size_t(std::count(dSeen.begin(), dSeen.end(), 0) + std::count(wSeen.begin(), wSeen.end(), 0));
            objectProblems += bad + int(unreferenced);
            printf("%s: %zu doodads + %zu WMOs exported, %zu unreferenced, %d mismatch(es)\n", name.c_str(), tile->doodads.size(), tile->wmos.size(),
                   unreferenced, bad);
        }
        std::filesystem::remove_all(out, ec);
        printf("objects: %d problem(s)\n", objectProblems);

        // Rotate in place: a 3x3 block around an untouched doodad turns with its objects (same ids, no copies).
        int rotateProblems = 0;
        {
            const LoadedTile& t = terrain.Tiles().at(TileKey(32, 48));
            const DoodadPlacement probe = t.adt.doodads[t.adt.doodads.size() / 2];
            const int gx = int(probe.pos[0] / kChunkSize) - 1, gz = int(probe.pos[2] / kChunkSize) - 1;
            std::set<ChunkRef> block;
            for (int dz = 0; dz < 3; ++dz)
                for (int dx = 0; dx < 3; ++dx)
                    if (auto r = terrain.ChunkAtGrid(gx + dx, gz + dz)) block.insert(*r);
            const TerrainClipboard before = terrain.Copy(block);
            size_t countBefore = 0;
            for (const auto& [key, tile] : terrain.Tiles()) countBefore += tile.adt.doodads.size() + tile.adt.wmos.size();
            auto rotated = terrain.RotateInPlace(block);
            if (!rotated) { printf("rotate in place made no change\n"); return 1; }
            store.Commit(*rotated);
            size_t countAfter = 0;
            for (const auto& [key, tile] : terrain.Tiles()) countAfter += tile.adt.doodads.size() + tile.adt.wmos.size();
            if (countAfter != countBefore) ++rotateProblems;
            const float span = 3 * kChunkSize, ox = gx * kChunkSize, oz = gz * kChunkSize;
            float worst = 0;
            for (const DoodadPlacement& d : before.doodads)   // clipboard positions are relative to the block's corner
            {
                const auto now = terrain.FindDoodad(d.uniqueId);
                if (!now) { ++rotateProblems; continue; }
                worst = std::max({ worst, std::fabs(now->pos[0] - (ox + span - d.pos[2])), std::fabs(now->pos[2] - (oz + d.pos[0])),
                                   std::fabs(now->pos[1] - d.pos[1]) });
            }
            if (worst > 0.01f) ++rotateProblems;
            printf("rotate in place: %s, %zu objects before and %zu after, worst position error %.4f yd, %d problem(s)\n",
                   rotated->label.c_str(), countBefore, countAfter, worst, rotateProblems);
        }
        objectProblems += rotateProblems;

        // Paint: a texture this chunk lacks, from a chunk far away; full pressure for a second covers the brush centre.
        int paintProblems = 0;
        {
            const ChunkRef target = *terrain.ChunkAtGrid(32 * 16 + 12, 48 * 16 + 3);
            const LoadedTile& tile = terrain.Tiles().at(target.tile);
            const AdtChunk& chunk = *terrain.Chunk(target);
            auto has = [&](const AdtChunk& c, const std::string& name) {
                for (uint32_t l = 0; l < c.layerCount; ++l)
                    if (c.textureIds[l] < tile.adt.textures.size() && tile.adt.textures[c.textureIds[l]] == name) return true;
                return false;
            };
            std::string texture;
            for (const auto& [key, t] : terrain.Tiles())
                for (const AdtChunk& c : t.adt.chunks)
                    for (uint32_t l = 0; l < c.layerCount && texture.empty(); ++l)
                        if (c.textureIds[l] < t.adt.textures.size() && !has(chunk, t.adt.textures[c.textureIds[l]])) texture = t.adt.textures[c.textureIds[l]];
            const XMFLOAT3 centre{ chunk.baseX + kChunkSize / 2, 0, chunk.baseZ + kChunkSize / 2 };
            const uint32_t layersBefore = chunk.layerCount;
            PaintBrush brush;
            brush.radius = 10;
            brush.pressure = 1;
            terrain.BeginPaint();
            for (int i = 0; i < 10; ++i) terrain.PaintStep(centre, brush, texture, false, 0.1f);
            auto painted = terrain.EndPaint("paint check");
            const auto under = terrain.TextureAt(centre.x, centre.z);
            const AdtChunk& after = *terrain.Chunk(target);
            if (!painted || !under || *under != texture || after.layerCount > 4 || !has(after, texture)) ++paintProblems;
            // Far corner of the chunk (outside the brush) keeps what it showed.
            if (painted) store.Commit(*painted);
            terrain.BeginPaint();
            for (int i = 0; i < 20; ++i) terrain.PaintStep(centre, brush, texture, true, 0.1f);
            auto erased = terrain.EndPaint("erase check");
            const auto afterErase = terrain.TextureAt(centre.x, centre.z);
            if (!erased || (afterErase && *afterErase == texture)) ++paintProblems;
            printf("paint: %s on chunk with %u layer(s) -> %u layer(s), centre shows %s; erase -> centre shows %s; %d problem(s)\n",
                   texture.c_str(), layersBefore, after.layerCount, under ? under->c_str() : "-", afterErase ? afterErase->c_str() : "-", paintProblems);
        }
        objectProblems += paintProblems;

        // Furniture placed on the map inside Northshire Abbey moves with the building (same edit, same offset).
        for (const WmoPlacement& w : terrain.Tiles().at(TileKey(32, 48)).adt.wmos)
        {
            if (w.model.find("NSABBEY.WMO") == std::string::npos) continue;
            const auto riders = terrain.DoodadsInside({ { true, w.uniqueId } });
            std::set<ObjectRef> moving = riders;
            moving.insert({ true, w.uniqueId });
            const auto rider = riders.empty() ? std::nullopt : terrain.FindDoodad(riders.begin()->uid);
            terrain.BeginObjectEdit(moving);
            terrain.PreviewObjectEdit([](DoodadPlacement& d) { d.pos[0] += 10; }, [](WmoPlacement& p) { p.pos[0] += 10; });
            auto carry = terrain.EndObjectEdit("carry check");
            const auto moved = rider ? terrain.FindDoodad(rider->uniqueId) : std::nullopt;
            const bool ok = carry && rider && moved && std::fabs(moved->pos[0] - rider->pos[0] - 10) < 0.01f;
            printf("carry: %zu doodad(s) inside %s, %s\n", riders.size(), w.model.c_str(), ok ? "moved with it" : "FAILED");
            if (!ok) ++objectProblems;
            if (carry) { store.Commit(*carry); store.Undo(); }
            break;
        }
        return problems || holeProblems || objectProblems || plan.footprint.size() != 4 || plan.chunks.size() <= 4 ? 1 : 0;
    }
}

namespace
{
    /// `--model-check <data dir> <map> <x> <y>`: parse every M2 and WMO the tile places, and test the
    /// placement maths against the world bounding boxes Blizzard stored for each WMO placement.
    int ModelCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        mpq.Open(arg(2));
        const std::string base = "World\\Maps\\" + arg(3) + "\\" + arg(3);
        const auto wdt = mpq.Read(base + ".wdt");
        const auto bytes = mpq.Read(base + "_" + arg(4) + "_" + arg(5) + ".adt");
        const auto adt = wdt && bytes ? ParseAdt(*bytes, WdtBigAlpha(*wdt)) : std::nullopt;
        if (!adt) { printf("tile not found\n"); return 1; }

        std::set<std::string> m2Names, m2Failed;
        for (const auto& d : adt->doodads) m2Names.insert(M2Name(d.model));
        size_t triangles = 0;
        for (const auto& name : m2Names)
        {
            const auto mesh = LoadM2(name, [&mpq](const std::string& path) { return mpq.Read(path); });
            if (!mesh) m2Failed.insert(name);
            else triangles += mesh->indices.size() / 3;
        }
        printf("M2: %zu unique models, %zu failed, %zu triangles total\n", m2Names.size(), m2Failed.size(), triangles);
        for (const auto& name : m2Failed) printf("  failed: %s\n", name.c_str());

        float worst = 0;
        size_t wmoOk = 0;
        for (const auto& w : adt->wmos)
        {
            const auto root = mpq.Read(w.model);
            uint32_t groups = 0;
            float b[6] = {};
            if (!root || !WmoRootInfo(*root, groups, b)) { printf("  WMO root failed: %s\n", w.model.c_str()); continue; }
            std::vector<std::vector<uint8_t>> groupFiles;
            for (uint32_t g = 0; g < groups; ++g) groupFiles.push_back(mpq.Read(WmoGroupName(w.model, g)).value_or(std::vector<uint8_t>{}));
            const auto mesh = ParseWmo(*root, groupFiles);
            if (mesh) ++wmoOk;

            const XMMATRIX m = PlacementMatrix(w.pos, w.rot, 1.0f);
            XMFLOAT3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
            for (int i = 0; i < 8; ++i)
            {
                const XMFLOAT3 c = FromWowAxes(i & 1 ? b[3] : b[0], i & 2 ? b[4] : b[1], i & 4 ? b[5] : b[2]);
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&c), m));
                lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
                hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
            }
            const float err = std::max({ std::fabs(lo.x - w.extMin[0]), std::fabs(lo.y - w.extMin[1]), std::fabs(lo.z - w.extMin[2]),
                                         std::fabs(hi.x - w.extMax[0]), std::fabs(hi.y - w.extMax[1]), std::fabs(hi.z - w.extMax[2]) });
            worst = std::max(worst, err);
            printf("  %-60s %u groups, %s, box error %.2f yd\n", w.model.c_str(), groups, mesh ? "parsed" : "PARSE FAILED", err);
        }
        printf("WMO: %zu/%zu parsed, worst placement box error %.2f yd\n", wmoOk, adt->wmos.size(), worst);
        size_t texturesMissing = 0;
        for (const std::string& t : adt->textures)
            if (!mpq.Read(t)) { printf("  texture missing: %s\n", t.c_str()); ++texturesMissing; }
        printf("textures: %zu listed, %zu missing\n", adt->textures.size(), texturesMissing);
        return m2Failed.empty() && wmoOk == adt->wmos.size() && worst < 1.0f ? 0 : 1;
    }
}

namespace
{
    bool SavePng(const std::wstring& path, UINT width, UINT height, std::vector<uint8_t> bgra)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
               SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               format == GUID_WICPixelFormat32bppBGRA &&
               SUCCEEDED(frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data())) && SUCCEEDED(frame->Commit()) &&
               SUCCEEDED(encoder->Commit());
    }

    /// `--skin-check <data dir> <out.png> <display id> [...]`: for character displays with a baked texture, the client's
    /// bake beside the editor's composite of the same fields (BakeName left out), one row per display, 256 px each.
    /// Fails when a display gives no composite.
    int SkinCheck()
    {
        if (__argc < 5 || !__wargv) return 2;
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        mpq.Open(arg(2));
        Dbc displays, extras;
        displays.Load(mpq.Read("DBFilesClient\\CreatureDisplayInfo.dbc").value_or(std::vector<uint8_t>{}));
        extras.Load(mpq.Read("DBFilesClient\\CreatureDisplayInfoExtra.dbc").value_or(std::vector<uint8_t>{}));
        // The extra rows as the editor holds them, without their bake: DisplayLooks then composites.
        std::map<uint32_t, nlohmann::json> unbaked;
        std::map<std::string, BlpImage> composed;
        DisplayLooks looks(mpq);
        looks.SetRowOverride([&](int table, uint32_t id) -> const nlohmann::json* { return table == 1 && unbaked.count(id) ? &unbaked[id] : nullptr; });
        looks.SetUpload([&](const std::string& name, const BlpImage& image) { composed[name] = image; });
        constexpr uint32_t kCell = 256;
        const uint32_t rows = uint32_t(__argc - 4), width = kCell * 2;
        std::vector<uint8_t> bgra(size_t(width) * kCell * rows * 4, 40);
        auto blit = [&](const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, uint32_t col, uint32_t row) {
            for (uint32_t y = 0; y < kCell; ++y)
                for (uint32_t x = 0; x < kCell; ++x)
                {
                    const uint8_t* s = &rgba[(size_t(y * h / kCell) * w + x * w / kCell) * 4];
                    uint8_t* d = &bgra[((size_t(row) * kCell + y) * width + col * kCell + x) * 4];
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
                }
        };
        int bad = 0;
        for (int i = 4; i < __argc; ++i)
        {
            const uint32_t display = uint32_t(_wtoi(__wargv[i])), row = uint32_t(i - 4);
            const auto d = displays.Find(display);
            const auto e = d ? extras.Find(displays.U32(*d, 3)) : std::nullopt;
            if (!e) { printf("FAIL %u: not a character display\n", display); ++bad; continue; }
            const std::string bake = extras.Str(*e, 20);
            if (const auto file = bake.empty() ? std::nullopt : mpq.Read("Textures\\BakedNpcTextures\\" + bake))
                if (const auto image = ParseBlp(*file)) blit(BlpPixels(*image), image->width, image->height, 0, row);
            nlohmann::json j = { { "DisplayRaceID", extras.U32(*e, 1) }, { "DisplaySexID", extras.U32(*e, 2) }, { "SkinID", extras.U32(*e, 3) },
                                 { "FaceID", extras.U32(*e, 4) }, { "HairStyleID", extras.U32(*e, 5) }, { "HairColorID", extras.U32(*e, 6) },
                                 { "FacialHairID", extras.U32(*e, 7) }, { "BakeName", "" } };
            for (uint32_t k = 0; k < 11; ++k) j["NPCItemDisplay[" + std::to_string(k) + "]"] = extras.U32(*e, 8 + k);
            unbaked[extras.U32(*e, 0)] = j;
            Spawn s;
            s.displayId = display;
            const auto look = looks.SpawnLook(s);
            const auto body = look ? look->look.textures.find(1) : decltype(look->look.textures)::const_iterator{};
            const auto image = look && body != look->look.textures.end() ? composed.find(body->second) : composed.end();
            if (image == composed.end()) { printf("FAIL %u: no composite\n", display); ++bad; continue; }
            blit(image->second.mips[0], image->second.width, image->second.height, 1, row);
            // Mean colour difference between the two cells (0-255); a wrong region or layer shows as tens.
            double diff = 0;
            for (uint32_t y = 0; y < kCell; ++y)
                for (uint32_t x = 0; x < kCell; ++x)
                    for (int c = 0; c < 3; ++c)
                        diff += std::abs(int(bgra[((size_t(row) * kCell + y) * width + x) * 4 + c]) - int(bgra[((size_t(row) * kCell + y) * width + kCell + x) * 4 + c]));
            diff /= double(kCell) * kCell * 3;
            const bool ok = bake.empty() || diff < 12;
            bad += !ok;
            printf("%s %u: race %u sex %u, composite %ux%u, mean difference from the bake %.1f\n", ok ? "ok  " : "FAIL", display, extras.U32(*e, 1),
                   extras.U32(*e, 2), image->second.width, image->second.height, diff);
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!SavePng(__wargv[3], width, kCell * rows, bgra)) { printf("cannot write %s\n", arg(3).c_str()); return 1; }
        return bad ? 1 : 0;
    }

    /// `--appearance-check <data dir> [display id]`: a new appearance as the NPC editor makes it (a copy of a character
    /// display and its extra row in the 90000 ranges, bake dropped, hair changed), seen by DisplayLooks through the
    /// project rows, exported, and read back from the written DBCs: the client's rows kept, the new rows' fields intact.
    int AppearanceCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        char buf[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, buf, sizeof buf, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(buf);
        const uint32_t source = __argc >= 4 ? uint32_t(_wtoi(__wargv[3])) : 2072;   // Deputy Willem
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        ChangeStore store;
        DbcTable displays(mpq, store, "CreatureDisplayInfo",
                          { { "ID", 0, 'i' }, { "ModelID", 1, 'i' }, { "SoundID", 2, 'i' }, { "ExtendedDisplayInfoID", 3, 'i' }, { "CreatureModelScale", 4, 'f' },
                            { "CreatureModelAlpha", 5, 'i' }, { "TextureVariation[0]", 6, 's' }, { "TextureVariation[1]", 7, 's' },
                            { "TextureVariation[2]", 8, 's' }, { "PortraitTextureName", 9, 's' }, { "SizeClass", 10, 'i' }, { "BloodID", 11, 'i' },
                            { "NPCSoundID", 12, 'i' }, { "ParticleColorID", 13, 'i' }, { "CreatureGeosetData", 14, 'i' }, { "ObjectEffectPackageID", 15, 'i' } },
                          16);
        std::vector<DbcField> extraFields = { { "ID", 0, 'i' }, { "DisplayRaceID", 1, 'i' }, { "DisplaySexID", 2, 'i' }, { "SkinID", 3, 'i' }, { "FaceID", 4, 'i' },
                                              { "HairStyleID", 5, 'i' }, { "HairColorID", 6, 'i' }, { "FacialHairID", 7, 'i' } };
        static const char* const kItems[11] = { "NPCItemDisplay[0]", "NPCItemDisplay[1]", "NPCItemDisplay[2]", "NPCItemDisplay[3]", "NPCItemDisplay[4]", "NPCItemDisplay[5]",
                                                "NPCItemDisplay[6]", "NPCItemDisplay[7]", "NPCItemDisplay[8]", "NPCItemDisplay[9]", "NPCItemDisplay[10]" };
        for (uint32_t i = 0; i < 11; ++i) extraFields.push_back({ kItems[i], 8 + i, 'i' });
        extraFields.push_back({ "Flags", 19, 'i' });
        extraFields.push_back({ "BakeName", 20, 's' });
        DbcTable extras(mpq, store, "CreatureDisplayInfoExtra", extraFields, 21);
        store.Register(displays);
        store.Register(extras);

        nlohmann::json d = displays.Row(source);
        const uint32_t oldExtra = d.is_object() ? d.value("ExtendedDisplayInfoID", 0u) : 0;
        nlohmann::json e = oldExtra ? extras.Row(oldExtra) : nlohmann::json();
        if (!e.is_object()) { printf("display %u is not a character display\n", source); return 1; }
        const uint32_t displayId = displays.FreeId(90000, 90999), extraId = extras.FreeId(90000, 90999);
        check(displayId >= 90000 && extraId >= 90000, "free ids " + std::to_string(displayId) + " / " + std::to_string(extraId));
        d["ID"] = displayId;
        d["ExtendedDisplayInfoID"] = extraId;
        e["ID"] = extraId;
        e["BakeName"] = "";
        e["HairStyleID"] = e.value("HairStyleID", 0u) == 1 ? 2u : 1u;
        displays.Commit(displayId, d, "check");
        extras.Commit(extraId, e, "check");

        DisplayLooks looks(mpq);
        std::set<std::string> uploaded;
        looks.SetRowOverride([&](int table, uint32_t id) { return (table == 0 ? displays : extras).Edited(id); });
        looks.SetUpload([&](const std::string& name, const BlpImage&) { uploaded.insert(name); });
        Spawn s;
        s.displayId = displayId;
        const auto look = looks.SpawnLook(s);
        check(look && look->look.textures.count(1) && look->look.textures.at(1).starts_with("composite:") && uploaded.count(look->look.textures.at(1)),
              "the new display draws with a composited skin");

        const auto out = std::filesystem::temp_directory_path() / "wwe-appearance-check";
        std::filesystem::remove_all(out);
        std::string error;
        for (const DbcTable* t : { &displays, &extras })
            check(t->Export({ out / "dbc" }, out / "patch", error), "export " + t->Name() + " " + error);
        auto reread = [&](const char* name, Dbc& dbc) { std::ifstream f(out / "dbc" / name, std::ios::binary); return dbc.Load({ std::istreambuf_iterator<char>(f), {} }); };
        Dbc client, written, writtenExtra;
        client.Load(mpq.Read("DBFilesClient\\CreatureDisplayInfo.dbc").value_or(std::vector<uint8_t>{}));
        check(reread("CreatureDisplayInfo.dbc", written) && written.Rows() == client.Rows() + 1, "CreatureDisplayInfo.dbc: the client's rows plus one");
        const auto wd = written.Find(displayId);
        check(wd && written.U32(*wd, 1) == d.value("ModelID", 0u) && written.U32(*wd, 3) == extraId &&
                  std::abs(written.F32(*wd, 4) - d.value("CreatureModelScale", 0.0f)) < 1e-6f,
              "new display row: model, extra and scale");
        const auto cd = client.Find(source);
        check(cd && written.Find(source) && written.U32(*written.Find(source), 1) == client.U32(*cd, 1), "the copied display left as it was");
        check(reread("CreatureDisplayInfoExtra.dbc", writtenExtra), "CreatureDisplayInfoExtra.dbc written");
        const auto we = writtenExtra.Find(extraId);
        bool items = we.has_value();
        for (uint32_t i = 0; we && i < 11; ++i) items &= writtenExtra.U32(*we, 8 + i) == e.value(kItems[i], 0u);
        check(we && writtenExtra.U32(*we, 1) == e.value("DisplayRaceID", 0u) && writtenExtra.U32(*we, 5) == e.value("HairStyleID", 0u) &&
                  writtenExtra.Str(*we, 20).empty() && items,
              "new extra row: race, hair, every armour slot, no bake");
        check(std::filesystem::exists(out / "patch" / "CreatureDisplayInfo.json") && std::filesystem::exists(out / "patch" / "CreatureDisplayInfoExtra.json"),
              "mod-dbc-patch edit files written");
        std::filesystem::remove_all(out);
        printf("%s\n", problems ? "FAILED" : "all passed");
        return problems ? 1 : 0;
    }

    /// `--catalog-check <data dir> <out.png>`: build the catalog, time it, search it, and render thumbnails of the
    /// first matches for a few searches into one strip.
    int CatalogCheck()
    {
        if (__argc < 4 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        mpq.Open(arg(2));
        Catalog catalog;
        const auto t0 = std::chrono::steady_clock::now();
        catalog.Build(mpq);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        using K = Catalog::Kind;
        printf("catalog: %zu files, %zu doodads, %zu WMOs, %zu ground textures, %zu other textures, %zu other, %.0f ms\n",
               catalog.Tree(K::Count).count, catalog.Count(K::Doodad), catalog.Count(K::Wmo), catalog.Count(K::GroundTexture),
               catalog.Count(K::Texture), catalog.Count(K::Other), ms);
        printf("top folders of doodads:");
        for (const auto& [lower, f] : catalog.Tree(K::Doodad).children) printf(" %s(%zu)", f.name.c_str(), f.count);
        printf("\n");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        ModelRenderer models;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) { printf("%s\n", error.c_str()); return 1; }

        const std::pair<K, const char*> searches[] = { { K::Doodad, "elwynn tree" }, { K::Doodad, "lamp" }, { K::Wmo, "stormwind" },
                                                       { K::Wmo, "abbeygate" }, { K::GroundTexture, "elwynn grass" } };
        constexpr UINT kThumb = 128;
        std::vector<uint8_t> strip(size_t(kThumb) * 4 * kThumb * 4, 0);
        UINT slot = 0;
        int problems = 0;
        for (const auto& [kind, query] : searches)
        {
            const auto found = catalog.Filter(kind, "", query);
            printf("search %-14s -> %zu match(es)%s%s\n", query, found.size(), found.empty() ? "" : ", first ", found.empty() ? "" : found[0]->path.c_str());
            if (found.empty()) { ++problems; continue; }
            if (kind == K::GroundTexture) continue;   // textures are shown straight from their BLP
            ID3D11ShaderResourceView* srv = models.Thumbnail(found[0]->path, kind == K::Wmo, mpq, true);
            if (!srv) { printf("  thumbnail failed\n"); ++problems; continue; }
            ComPtr<ID3D11Resource> res;
            srv->GetResource(&res);
            D3D11_TEXTURE2D_DESC d{};
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            device->CreateTexture2D(&d, nullptr, &staging);
            context->CopyResource(staging.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
            {
                for (UINT y = 0; y < kThumb; ++y)
                    for (UINT x = 0; x < kThumb; ++x)
                    {
                        const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                        uint8_t* q = strip.data() + (size_t(y) * kThumb * 4 + slot * kThumb + x) * 4;
                        q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 255;
                    }
                context->Unmap(staging.Get(), 0);
            }
            ++slot;
        }
        // WMO coverage: how many Stormwind roots load at all.
        size_t wmoOk = 0, wmoAll = 0;
        for (const Catalog::Item* item : catalog.Filter(K::Wmo, "", "stormwind"))
        {
            ++wmoAll;
            if (models.Thumbnail(item->path, true, mpq, true)) ++wmoOk;
            else
            {
                uint32_t groups = 0;
                float b[6];
                const auto root = mpq.Read(item->path);
                printf("  failed: %s (root %s, %u groups)\n", item->path.c_str(), root ? "read" : "MISSING", root && WmoRootInfo(*root, groups, b) ? groups : 0u);
            }
        }
        printf("stormwind WMOs: %zu of %zu load\n", wmoOk, wmoAll);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool saved = SavePng(__wargv[3], kThumb * 4, kThumb, strip);
        printf("thumbnails: %u rendered, png %s, %d problem(s)\n", slot, saved ? "saved" : "FAILED", problems);
        return problems ? 1 : 0;
    }

    /// `--ghost-check <data dir> <other client dir> <map> <x> <y>`: list the tile's versions in both, load ghost
    /// layers around it, and copy from a ghost then paste in place: heights must come out exactly as the ghost has them.
    /// `--blueprint-check <data dir> <map> <x> <y> <out.png>`: keep a 4x4-chunk area of the tile as a blueprint, load it back,
    /// and render its thumbnail the way the editor does (its own layer, straight down, orthographic).
    int BlueprintCheck()
    {
        if (__argc < 7 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        ModelRenderer models;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) return 1;
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(arg(3), error)) { printf("%s\n", error.c_str()); return 1; }
        terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 0, error);
        std::set<ChunkRef> sel;
        for (int dz = 0; dz < 4; ++dz)
            for (int dx = 0; dx < 4; ++dx)
                if (auto r = terrain.ChunkAtGrid(tx * 16 + 6 + dx, ty * 16 + 6 + dz)) sel.insert(*r);
        Blueprint b;
        b.name = "check";
        b.clip = terrain.Copy(sel);
        const auto dir = std::filesystem::temp_directory_path() / "wow-world-editor-blueprint-check";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (!b.Save(dir, error)) { printf("%s\n", error.c_str()); return 1; }
        const auto back = Blueprint::Load(b.file, error);
        std::filesystem::remove_all(dir, ec);
        if (!back) { printf("%s\n", error.c_str()); return 1; }
        printf("blueprint: %zu chunks, %zu doodads, %zu WMOs saved and loaded back\n", back->clip.chunks.size(), back->clip.doodads.size(), back->clip.wmos.size());

        constexpr int kLayer = 9999, kKey = -9;
        constexpr UINT kSize = 256;
        const Adt adt = back->clip.ToAdt();
        renderer.LoadTile(kKey, adt, mpq, kLayer);
        models.AddTile(kKey, adt, mpq, kLayer);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kSize;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> color, depth, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        device->CreateTexture2D(&d, nullptr, &color);
        device->CreateRenderTargetView(color.Get(), nullptr, &rtv);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&d, nullptr, &staging);
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.Format = DXGI_FORMAT_D32_FLOAT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        device->CreateTexture2D(&d, nullptr, &depth);
        device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
        const TerrainClipboard& clip = back->clip;
        float top = -1e9f;
        for (const auto& e : clip.chunks) top = std::max(top, *std::max_element(e.heights.begin(), e.heights.end()));
        const float spanX = clip.Width() * kChunkSize, spanZ = clip.Depth() * kChunkSize, span = std::max(spanX, spanZ);
        const float cx = clip.originX * kChunkSize + spanX / 2, cz = clip.originZ * kChunkSize + spanZ / 2;
        const XMFLOAT3 eye{ cx, top + 1000.0f, cz };
        const XMMATRIX viewProj = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMVectorSet(cx, top, cz, 1), XMVectorSet(0, 0, -1, 0)) *
                                  XMMatrixOrthographicRH(span, span, 1.0f, 4000.0f);
        const float background[4] = { 0.16f, 0.17f, 0.20f, 1 };
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        const D3D11_VIEWPORT vp{ 0, 0, float(kSize), float(kSize), 0, 1 };
        context->RSSetViewports(1, &vp);
        context->ClearRenderTargetView(rtv.Get(), background);
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        DrawOptions options;
        options.showObjects = false;
        options.solo = kLayer;
        options.lod = false;
        renderer.Draw(viewProj, options);
        ModelRenderer::DrawSettings ms;
        ms.layer = kLayer;
        ms.distance = 100000.0f;
        models.Draw(viewProj, eye, ms);
        context->CopyResource(staging.Get(), color.Get());
        std::vector<uint8_t> bgra(size_t(kSize) * kSize * 4);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        {
            for (UINT y = 0; y < kSize; ++y)
                for (UINT x = 0; x < kSize; ++x)
                {
                    const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                    uint8_t* q = bgra.data() + (size_t(y) * kSize + x) * 4;
                    q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 255;
                }
            context->Unmap(staging.Get(), 0);
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool saved = SavePng(__wargv[6], kSize, kSize, bgra);
        printf("thumbnail %s\n", saved ? "saved" : "FAILED");
        return saved && clip.chunks.size() == 16 ? 0 : 1;
    }

    /// `--asset-check <base data dir> <other client dir> <map> <x> <y>`: paste a whole tile from the other client into
    /// the base one, export, and copy what the base client lacks; every reference of the result must then resolve.
    int AssetCheck()
    {
        if (__argc < 7 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string map = arg(4);
        const int tx = std::stoi(arg(5)), ty = std::stoi(arg(6));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return 1; }
        for (int i = 0; i < 9; ++i) terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1, error);
        Ghosts ghosts;
        std::vector<std::string> errors;
        ghosts.Reset(&mpq, "base", { { "other", { { MpqLayer::Kind::MpqFolder, arg(3) } } } }, errors);
        if (!errors.empty()) { printf("%s\n", errors[0].c_str()); return 1; }
        mpq.SetFallbacks({ ghosts.Sources()[1].mpq });
        const int id = ghosts.AddLayer(1, -1, "other", __argc > 7 ? arg(7) : std::string()).id;   // optional 7th: another map of that client
        for (int i = 0; i < 12; ++i) ghosts.Stream(map, (tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1);
        std::set<std::pair<int, int>> whole;
        for (int cz = 0; cz < 16; ++cz)
            for (int cx = 0; cx < 16; ++cx) whole.insert({ tx * 16 + cx, ty * 16 + cz });
        const TerrainClipboard clip = TerrainAdapter::CopyFrom(ghosts.Find(id)->tiles, whole);
        PasteOptions o;
        o.blend = false;
        auto change = terrain.ApplyPlan(terrain.PlanPaste(clip, clip.originX, clip.originZ, 0.0f, o), "asset check");
        if (!change) { printf("paste made no change\n"); return 1; }
        store.Commit(*change);
        // A hard paste is welded to the ground around it: no edge may stay open.
        std::vector<Problem> cracks;
        terrain.FindCracks(cracks);
        printf("cracks after a hard paste: %zu tile(s)\n", cracks.size());
        for (const Problem& p : cracks) printf("  %s %d_%d: %s\n", p.map.c_str(), p.tx, p.ty, p.message.c_str());
        const auto out = std::filesystem::temp_directory_path() / "wow-world-editor-assetcheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::vector<std::filesystem::path> tiles;
        terrain.Export(out, error, &tiles);
        const auto t0 = std::chrono::steady_clock::now();
        const AssetReport report = CopyMissingAssets(mpq, tiles, out);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("pasted %zu chunks, %zu doodads, %zu WMOs from the other client; exported %zu tile(s)\n", clip.chunks.size(), clip.doodads.size(),
               clip.wmos.size(), tiles.size());
        printf("closure: %zu file(s) copied, %zu missing everywhere, %.0f ms\n", report.copied.size(), report.missing.size(), ms);
        for (size_t i = 0; i < report.copied.size() && i < 12; ++i) printf("  + %s\n", report.copied[i].c_str());
        for (const auto& m : report.missing) printf("  ! %s\n", m.c_str());

        // Without fallbacks, base client + exported files must resolve every reference reachable from the tiles.
        mpq.SetFallbacks({});
        auto resolve = [&](const std::string& name) -> std::optional<std::vector<uint8_t>> {
            if (auto b = mpq.Read(name)) return b;
            std::string rel = name;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            std::ifstream f(out / std::filesystem::path(rel), std::ios::binary);
            if (!f) return std::nullopt;
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        std::set<std::string> seen;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> todo;
        for (const auto& t : tiles)
        {
            std::ifstream f(t, std::ios::binary);
            todo.push_back({ t.filename().string(), std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) });
        }
        size_t unresolved = 0, checked = 0;
        while (!todo.empty())
        {
            auto [name, bytes] = std::move(todo.back());
            todo.pop_back();
            for (const std::string& r : AssetReferences(mpq, name, bytes))
            {
                if (!seen.insert(Catalog::Normalize(r)).second) continue;
                auto b = resolve(r);
                const std::string lower = Catalog::Normalize(r);
                const bool optional = lower.ends_with("_s.blp") || lower.ends_with("01.skin") || lower.ends_with("02.skin") || lower.ends_with("03.skin");
                if (!b) { if (!optional && std::find(report.missing.begin(), report.missing.end(), r) == report.missing.end()) { ++unresolved; printf("  unresolved: %s\n", r.c_str()); } continue; }
                ++checked;
                todo.push_back({ r, std::move(*b) });
            }
        }
        std::filesystem::remove_all(out, ec);
        printf("verification: %zu references resolve from the base client plus the export, %zu unresolved\n", checked, unresolved);
        return unresolved ? 1 : 0;
    }

    /// `--compare-check <data dir> <map> <x> <y>`: chunk edges of the map meet the way CompareArea reads them, the map
    /// compared with itself is identical, and every <map>_* copy's version of a 4x4-chunk area and of the whole tile is listed.
    int CompareCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string map = arg(3);
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return 1; }
        for (int i = 0; i < 9; ++i) terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1, error);
        const LoadedTile& tile = terrain.Tiles().at(TileKey(tx, ty));
        int problems = 0;

        // Neighbouring chunks share their edge vertices: x neighbours columns 8/0, z neighbours rows 8/0.
        float worstX = 0, worstZ = 0;
        for (int r = 0; r < 16; ++r)
            for (int c = 0; c < 16; ++c)
            {
                const int16_t a = tile.byGrid[size_t(r * 16 + c)];
                if (a < 0) continue;
                const AdtChunk& A = tile.adt.chunks[size_t(a)];
                const int16_t right = c < 15 ? tile.byGrid[size_t(r * 16 + c + 1)] : -1, down = r < 15 ? tile.byGrid[size_t((r + 1) * 16 + c)] : -1;
                for (int k = 0; k <= 8; ++k)
                {
                    if (right >= 0)
                    {
                        const AdtChunk& B = tile.adt.chunks[size_t(right)];
                        worstX = std::max(worstX, std::fabs(A.baseY + A.heights[size_t(k * 17 + 8)] - B.baseY - B.heights[size_t(k * 17)]));
                    }
                    if (down >= 0)
                    {
                        const AdtChunk& B = tile.adt.chunks[size_t(down)];
                        worstZ = std::max(worstZ, std::fabs(A.baseY + A.heights[size_t(8 * 17 + k)] - B.baseY - B.heights[size_t(k)]));
                    }
                }
            }
        printf("edge layout: x neighbours differ by up to %.4f yd, z neighbours by %.4f yd\n", worstX, worstZ);
        if (worstX > 0.01f || worstZ > 0.01f) ++problems;

        std::set<std::pair<int, int>> area, whole;
        for (int dz = 0; dz < 4; ++dz)
            for (int dx = 0; dx < 4; ++dx) area.insert({ tx * 16 + 6 + dx, ty * 16 + 6 + dz });
        for (int dz = 0; dz < 16; ++dz)
            for (int dx = 0; dx < 16; ++dx) whole.insert({ tx * 16 + dx, ty * 16 + dz });
        const AreaDiff self = CompareArea(terrain.Tiles(), terrain.Tiles(), whole);
        printf("map vs itself: %zu cells, same %s\n", self.cells, self.Same() ? "yes" : "NO");
        if (!self.Same()) ++problems;

        Ghosts ghosts;
        std::vector<std::string> errors;
        ghosts.Reset(&mpq, "WXL", {}, errors);
        const std::string base = map.substr(0, map.find('_'));
        if (const auto dbc = mpq.Read("DBFilesClient\\Map.dbc"))
            for (const MapEntry& m : ParseMapDbc(*dbc))
                if (m.directory != map && m.directory.rfind(base, 0) == 0)
                    ghosts.AddLayer(0, -1, m.directory, m.directory).only = { TileKey(tx, ty) };
        for (int i = 0; i < 4 * int(ghosts.Layers().size()) + 4; ++i) ghosts.Stream(map, 0, 0, 0);   // camera far away: only `only` loads
        for (const auto& l : ghosts.Layers())
        {
            if (l.tiles.empty()) { printf("  %-22s no tile %d_%d\n", l.label.c_str(), tx, ty); continue; }
            for (const auto* cells : { &area, &whole })
            {
                const AreaDiff d = CompareArea(terrain.Tiles(), l.tiles, *cells);
                printf("  %-22s %-6s %3zu/%3zu chunks differ, height %.2f/%.1f yd, edge %.2f/%.1f yd, objects +%zu -%zu%s\n", l.label.c_str(),
                       cells == &area ? "4x4" : "tile", d.changed, d.cells, d.meanHeight, d.maxHeight, d.meanEdge, d.maxEdge,
                       d.newDoodads.size() + d.newWmos.size(), d.goneDoodads + d.goneWmos, d.Same() ? "  (identical)" : "");
                for (const auto& o : d.newDoodads)
                    if (!cells->count({ int(std::floor(o.pos[0] / kChunkSize)), int(std::floor(o.pos[2] / kChunkSize)) })) ++problems;
                if (cells == &whole && d.water)   // which water fields differ, first such chunk
                {
                    const LoadedTile& a = terrain.Tiles().at(TileKey(tx, ty));
                    const LoadedTile& b = l.tiles.at(TileKey(tx, ty));
                    for (size_t cell = 0; cell < 256; ++cell)
                    {
                        const int16_t i = a.byGrid[cell], j = b.byGrid[cell];
                        if (i < 0 || j < 0) continue;
                        const auto wa = TerrainAdapter::LiquidState(a.adt, a.adt.chunks[size_t(i)]), wb = TerrainAdapter::LiquidState(b.adt, b.adt.chunks[size_t(j)]);
                        if (wa == wb) continue;
                        std::string fields;
                        if (wa.size() != wb.size()) fields = "instance count " + std::to_string(wa.size()) + " vs " + std::to_string(wb.size());
                        else
                            for (size_t k = 0; k < wa.size(); ++k)
                                for (const auto& [name, value] : wa[k].items())
                                    if (wb[k].value(name, nlohmann::json()) != value) fields += " " + name;
                        printf("      water: cell %zu differs in%s\n", cell, fields.c_str());
                        break;
                    }
                }
            }
        }
        // What one step of cycling costs, per stage (the selection as a 4x4 area and as the whole tile).
        for (const auto& l : ghosts.Layers())
        {
            if (l.tiles.empty()) continue;
            for (const auto* cells : { &area, &whole })
            {
                using Clock = std::chrono::steady_clock;
                auto ms = [](Clock::time_point a) { return std::chrono::duration<float, std::milli>(Clock::now() - a).count(); };
                auto t = Clock::now();
                const AreaDiff d = CompareArea(terrain.Tiles(), l.tiles, *cells);
                const float diffMs = ms(t);
                t = Clock::now();
                const TerrainClipboard clip = TerrainAdapter::CopyFrom(l.tiles, *cells);
                const float copyMs = ms(t);
                PasteOptions o;
                t = Clock::now();
                const PastePlan plan = terrain.PlanPaste(clip, clip.originX, clip.originZ, 0.0f, o);
                const float planMs = ms(t);
                o.blend = false;   // what cycling shows until it settles
                t = Clock::now();
                const PastePlan hard = terrain.PlanPaste(clip, clip.originX, clip.originZ, 0.0f, o);
                const float hardMs = ms(t);
                t = Clock::now();
                terrain.PreviewPlan(&plan);
                terrain.PreviewPlan(nullptr);
                const float previewMs = ms(t);
                printf("  timing %-18s %-4s diff %6.1f ms, copy %6.1f ms, blended plan %7.1f ms (%zu chunks), hard plan %6.1f ms, preview+restore %6.1f ms\n",
                       l.label.c_str(), cells == &area ? "4x4" : "tile", diffMs, copyMs, planMs, plan.chunks.size(), hardMs, previewMs);
                (void)hard;
                (void)d;
            }
            break;   // one version is enough to see the costs
        }
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--diff-check <data dir> <base map> <other map>`: scans the other map against the base on the worker (progress
    /// printed), groups the areas, rejects the largest, scans again (every tile must come from the saved results and the
    /// rejected area must stay rejected), and checks one area's chunks against CompareArea.
    int DiffCheck()
    {
        if (__argc < 5 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        mpq.Open(arg(2));
        const std::string base = arg(3), other = arg(4);
        const auto file = std::filesystem::temp_directory_path() / "wow-world-editor-diffcheck.json";
        std::error_code ec;
        std::filesystem::remove(file, ec);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-62s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        auto scan = [&](Differences& d) {
            d.Start(mpq, base, mpq, other, other, {}, file);
            size_t shown = 0;
            while (d.GetProgress().running)
            {
                d.Update();
                const auto p = d.GetProgress();
                if (p.done >= shown + p.total / 10 || p.done == p.total)
                {
                    printf("    %4zu / %zu tiles (%zu cached), %.1f s, %zu areas so far\n", p.done, p.total, p.cached, p.seconds, d.Regions().size());
                    shown = p.done;
                }
                Sleep(50);
            }
            d.Cancel();   // joins, takes the rest, saves
            return d.GetProgress();
        };

        Differences first;
        printf("first scan %s vs %s:\n", base.c_str(), other.c_str());
        const auto p1 = scan(first);
        size_t cells = 0, biggest = 0;
        const Differences::Region* big = nullptr;
        for (const auto& r : first.Regions())
        {
            cells += r.cells.size();
            if (r.cells.size() > biggest) { biggest = r.cells.size(); big = &r; }
        }
        printf("  %zu tiles in %.1f s: %zu area(s), %zu chunk(s); largest %zu chunks\n", p1.total, p1.seconds, first.Regions().size(), cells, biggest);
        expect(p1.done == p1.total && p1.total > 0, "every tile scanned");
        expect(!first.Regions().empty(), "areas found");
        std::map<size_t, size_t> sizes;
        for (const auto& r : first.Regions()) ++sizes[r.cells.size() < 4 ? 1 : r.cells.size() < 16 ? 4 : r.cells.size() < 64 ? 16 : 64];
        for (const auto& [from, n] : sizes) printf("    %zu area(s) of %zu+ chunks\n", n, from);
        if (!big) return 1;
        printf("  largest: cells %d..%d x %d..%d, kinds 0x%02x, height up to %.1f yd\n", big->x0, big->x1, big->z0, big->z1, big->kinds, big->maxHeight);
        {
            // Which kinds its chunks carry, counted, and one chunk's texture lists side by side.
            std::map<int, size_t> kinds;
            auto tileOf = [&](const std::string& map, int key) -> std::optional<LoadedTile> {
                const auto wdt = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
                if (auto bytes = mpq.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt"))
                    if (auto adt = ParseAdt(*bytes, wdt && WdtBigAlpha(*wdt))) return LoadedTile::Make(key % 64, key / 64, {}, std::move(*adt));
                return std::nullopt;
            };
            const auto [gx, gz] = big->cells[big->cells.size() / 2];
            const int key = TileKey(gx / 16, gz / 16);
            const auto a = tileOf(base, key), b = tileOf(other, key);
            if (a && b)
            {
                std::map<int, LoadedTile> ma, mb;
                ma.emplace(key, *a);
                mb.emplace(key, *b);
                std::set<std::pair<int, int>> all;
                for (int c = 0; c < 256; ++c) all.insert({ (key % 64) * 16 + c % 16, (key / 64) * 16 + c / 16 });
                for (const CellDiff& c : CompareCells(ma, mb, all))
                    for (int bit = 0; bit < 6; ++bit)
                        if (c.kinds & (1 << bit)) ++kinds[bit];
                for (const auto& [bit, n] : kinds) printf("    kind bit %d on %zu chunk(s) of tile %d_%d\n", bit, n, key % 64, key / 64);
                const int16_t ia = a->byGrid[size_t((gz % 16) * 16 + gx % 16)], ib = b->byGrid[size_t((gz % 16) * 16 + gx % 16)];
                if (ia >= 0 && ib >= 0)
                    for (uint32_t l = 0; l < 4; ++l)
                    {
                        const AdtChunk& ca = a->adt.chunks[size_t(ia)];
                        const AdtChunk& cb = b->adt.chunks[size_t(ib)];
                        printf("    layer %u: %-55s | %s\n", l, l < ca.layerCount ? a->adt.textures[ca.textureIds[l]].c_str() : "-",
                               l < cb.layerCount ? b->adt.textures[cb.textureIds[l]].c_str() : "-");
                    }
            }
        }
        const std::string bigKey = big->key;
        const auto bigCells = big->cells;
        first.SetStatus(bigCells, Differences::Status::Rejected);
        expect(first.Find(bigKey) && first.Find(bigKey)->status == Differences::Status::Rejected, "largest area rejected");

        // The same area through CompareArea (the in-place compare's numbers): it must see changes on those chunks too.
        {
            std::map<int, LoadedTile> a, b;
            for (int key : first.Find(bigKey)->Tiles())
            {
                auto read = [&](const std::string& map, std::map<int, LoadedTile>& into) {
                    const auto wdt = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
                    if (auto bytes = mpq.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt"))
                        if (auto adt = ParseAdt(*bytes, wdt && WdtBigAlpha(*wdt))) into.emplace(key, LoadedTile::Make(key % 64, key / 64, {}, std::move(*adt)));
                };
                read(base, a);
                read(other, b);
            }
            const AreaDiff d = CompareArea(a, b, std::set<std::pair<int, int>>(bigCells.begin(), bigCells.end()));
            printf("  largest area by CompareArea: %zu/%zu chunks differ, +%zu objects\n", d.changed, d.cells, d.newDoodads.size() + d.newWmos.size());
            if (first.Find(bigKey)->kinds & CellDiff::NewTerrain)   // CompareArea only sees chunks both have
                expect(d.cells < bigCells.size(), "new terrain: chunks the map lacks are not in CompareArea");
            else
                expect(d.changed + d.newDoodads.size() + d.newWmos.size() > 0, "CompareArea agrees the area differs");
        }

        Differences second;
        printf("second scan (saved results):\n");
        const auto p2 = scan(second);
        printf("  %.1f s, %zu of %zu tiles from the saved results\n", p2.seconds, p2.cached, p2.total);
        expect(p2.cached == p2.total, "every tile reused");
        expect(second.Regions().size() == first.Regions().size(), "same areas");
        const Differences::Region* again = second.Find(bigKey);
        expect(again && again->status == Differences::Status::Rejected, "rejection kept");
        first.Clear();    // forgotten, so neither saves its file again on the way out
        second.Clear();
        std::filesystem::remove(file, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--diff-objects <data dir> <base map> <other map> <zone id>`: for every difference area in a zone (AreaTable
    /// parent chain), each building (WMO) of the other version whose extent reaches the area: whether the area carries
    /// it (origin on one of its chunks), whether the map already has it, and whether its model is in the client.
    int DiffObjects()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        MpqChain mpq;
        mpq.Open(arg(2));
        const std::string base = arg(3), other = arg(4);
        const uint32_t zone = uint32_t(std::stoul(arg(5)));
        Dbc areas;
        areas.Load(mpq.Read("DBFilesClient\\AreaTable.dbc").value_or(std::vector<uint8_t>{}));
        auto zoneOf = [&](uint32_t id) {
            for (int i = 0; i < 16 && id; ++i)
            {
                const auto row = areas.Find(id);
                if (!row) return id;
                const uint32_t parent = areas.U32(*row, 2);
                if (!parent) return id;
                id = parent;
            }
            return id;
        };
        auto name = [&](uint32_t id) { const auto row = areas.Find(id); return row ? areas.Str(*row, 11) : std::to_string(id); };
        const auto file = std::filesystem::temp_directory_path() / "wow-world-editor-diffobjects.json";
        std::error_code ec;
        std::filesystem::remove(file, ec);
        Differences d;
        d.Start(mpq, base, mpq, other, other, {}, file);
        while (d.GetProgress().running) { d.Update(); Sleep(50); }
        d.Cancel();
        const std::vector<Differences::Region> regions = d.Regions();
        d.Clear();   // forgotten, so it does not save its file again on the way out
        std::filesystem::remove(file, ec);
        auto read = [&](const std::string& map, int key) -> std::optional<Adt> {
            const auto wdt = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
            const auto bytes = mpq.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt");
            return bytes ? ParseAdt(*bytes, wdt && WdtBigAlpha(*wdt)) : std::nullopt;
        };
        auto lower = [](std::string s) { for (char& c : s) c = c == '/' ? '\\' : char(std::tolower((unsigned char)c)); return s; };
        size_t shown = 0;
        for (const auto& r : regions)
        {
            if (zoneOf(r.area) != zone) continue;
            const std::set<std::pair<int, int>> cells(r.cells.begin(), r.cells.end());
            printf("area %s: %s, %zu chunks, cells %d..%d x %d..%d, kinds 0x%02x, +%zu objects\n", r.key.c_str(), name(r.area).c_str(), r.cells.size(),
                   r.x0, r.x1, r.z0, r.z1, r.kinds, r.newObjects);
            // Every WMO either version lists on the area's tiles (and the tiles around: an extent can reach in from there).
            std::set<int> keys;
            for (int key : r.Tiles())
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) keys.insert(TileKey(std::clamp(key % 64 + dx, 0, 63), std::clamp(key / 64 + dy, 0, 63)));
            std::map<uint32_t, WmoPlacement> theirs;
            std::set<std::string> ours;
            std::map<int, LoadedTile> baseTiles, otherTiles;   // what the in-place compare sees (CompareArea)
            for (int key : keys)
            {
                if (auto a = read(base, key)) baseTiles.emplace(key, LoadedTile::Make(key % 64, key / 64, {}, std::move(*a)));
                if (auto a = read(other, key)) otherTiles.emplace(key, LoadedTile::Make(key % 64, key / 64, {}, std::move(*a)));
            }
            {
                const AreaDiff carried = CompareArea(baseTiles, otherTiles, cells);
                printf("  compare carries %zu building(s):", carried.newWmos.size());
                for (const auto& w : carried.newWmos) printf(" %s", w.model.substr(w.model.find_last_of("\\/") + 1).c_str());
                printf("\n");
            }
            for (int key : keys)
            {
                if (const auto a = read(other, key))
                    for (const auto& w : a->wmos) theirs.emplace(w.uniqueId, w);
                if (const auto a = read(base, key))
                    for (const auto& w : a->wmos)
                        ours.insert(lower(w.model) + "|" + std::to_string(std::lround(w.pos[0])) + "|" + std::to_string(std::lround(w.pos[2])));
            }
            for (const auto& [uid, w] : theirs)
            {
                // Does its extent (x/z box) overlap any of the area's chunks?
                bool reaches = false;
                for (const auto& [gx, gz] : cells)
                    reaches = reaches || (w.extMax[0] >= gx * kChunkSize && w.extMin[0] < (gx + 1) * kChunkSize && w.extMax[2] >= gz * kChunkSize &&
                                          w.extMin[2] < (gz + 1) * kChunkSize);
                if (!reaches) continue;
                const std::pair<int, int> origin{ int(std::floor(w.pos[0] / kChunkSize)), int(std::floor(w.pos[2] / kChunkSize)) };
                const bool carried = cells.count(origin) != 0;
                const bool onMap = ours.count(lower(w.model) + "|" + std::to_string(std::lround(w.pos[0])) + "|" + std::to_string(std::lround(w.pos[2]))) != 0;
                const bool inClient = mpq.HasOwn(w.model);
                printf("  wmo %-62s origin cell %d,%d %s | %s | %s\n", w.model.c_str(), origin.first, origin.second,
                       carried ? "origin IN the area" : "origin OUTSIDE the area", onMap ? "map has it" : "map LACKS it",
                       inClient ? "model in client" : "model MISSING");
            }
            ++shown;
        }
        printf("%zu area(s) in zone %u (%s)\n", shown, zone, name(zone).c_str());
        return 0;
    }

    /// Packs `folder` into `<scratch>/Data/<name>`, opens that Data folder as a client would, and compares every file
    /// read back with the one on disk. Returns how many differ (or are missing); prints a line.
    int PackAndVerify(const std::filesystem::path& folder, const std::filesystem::path& scratch, const std::string& name)
    {
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        const auto archive = scratch / "Data" / name;
        std::string error;
        size_t files = 0;
        if (!WriteMpq(archive, folder, error, &files)) { printf("  pack: %s\n", error.c_str()); return 1; }
        int bad = 0;
        {
            MpqChain mpq;
            mpq.Open((scratch / "Data").string());
            for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec))
            {
                if (!entry.is_regular_file()) continue;
                std::ifstream f(entry.path(), std::ios::binary);
                const std::vector<uint8_t> disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                const auto packed = mpq.Read(std::filesystem::relative(entry.path(), folder, ec).string());
                bad += !packed || *packed != disk;
            }
            printf("  packed %zu file(s) into %s (%.1f MB from %.1f MB), read back: %d differ\n", files, name.c_str(),
                   double(std::filesystem::file_size(archive, ec)) / 1048576.0, [&] {
                       uintmax_t total = 0;
                       for (const auto& e : std::filesystem::recursive_directory_iterator(folder, ec)) total += e.is_regular_file() ? e.file_size() : 0;
                       return double(total) / 1048576.0;
                   }(), bad);
        }
        std::filesystem::remove_all(scratch, ec);
        return bad;
    }

    /// `--sources-check <data dir>`: layers over a real client: an unpacked folder and a single MPQ both overriding a
    /// client file, files only one layer has, reordering and disabling, "players have it", notes for bad layers, the
    /// version list seeing an unpacked folder, and project.json (new layers and an old project's sources).
    int SourcesCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string data = arg(2);
        const fs::path t = fs::temp_directory_path() / "wow-world-editor-sourcescheck";
        std::error_code ec;
        fs::remove_all(t, ec);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-66s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        auto put = [](const fs::path& p, const std::string& text) {
            fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << text;
        };
        auto text = [](const std::optional<std::vector<uint8_t>>& b) { return b ? std::string(b->begin(), b->end()) : std::string("<none>"); };
        const std::string shared = "DBFilesClient\\Map.dbc";
        // An unpacked folder (with a tile, to show up as a version) and a single MPQ, both with their own Map.dbc.
        put(t / "loose" / "DBFilesClient" / "Map.dbc", "from the unpacked folder");
        put(t / "loose" / "Textures" / "Wwe" / "only_loose.blp", "loose only");
        {
            MpqChain client;
            client.Open(data);
            const auto adt = client.Read("World\\Maps\\Azeroth\\Azeroth_32_48.adt");
            fs::create_directories(t / "loose" / "World" / "Maps" / "Azeroth");
            if (adt) std::ofstream(t / "loose" / "World" / "Maps" / "Azeroth" / "Azeroth_32_48.adt", std::ios::binary)
                         .write(reinterpret_cast<const char*>(adt->data()), std::streamsize(adt->size() - 4));   // different bytes: its own version
        }
        put(t / "mpqsrc" / "DBFilesClient" / "Map.dbc", "from the single MPQ");
        put(t / "mpqsrc" / "Textures" / "Wwe" / "only_mpq.blp", "mpq only");
        std::string error;
        expect(WriteMpq(t / "single.MPQ", t / "mpqsrc", error), "a single MPQ built for the test");
        fs::create_directories(t / "empty");

        const MpqLayer client{ MpqLayer::Kind::MpqFolder, data }, single{ MpqLayer::Kind::MpqFile, (t / "single.MPQ").string() };
        MpqLayer loose{ MpqLayer::Kind::Folder, (t / "loose").string() };
        loose.installed = false;   // new art: the patch carries it
        {
            MpqChain c;
            c.Open({ client, single, loose });
            expect(text(c.Read(shared)) == "from the unpacked folder", "top layer wins: the unpacked folder over the MPQ and the client");
            expect(text(c.Read("textures/wwe/ONLY_MPQ.blp")) == "mpq only", "a file only the single MPQ has (any case, any slash)");
            expect(text(c.Read("Textures\\Wwe\\only_loose.blp")) == "loose only", "a file only the unpacked folder has");
            expect(c.Read("World\\Maps\\Azeroth\\Azeroth.wdt").has_value(), "client files still read through the layers");
            expect(!c.HasInstalled("Textures\\Wwe\\only_loose.blp") && c.HasInstalled("Textures\\Wwe\\only_mpq.blp") && c.HasInstalled(shared),
                   "players-have-it: off for the unpacked folder, on for the rest");
            size_t listed = 0;
            for (const auto& e : c.List())
            {
                const std::string n = Catalog::Normalize(e.name);
                listed += n.find("only_loose") != std::string::npos || n.find("only_mpq") != std::string::npos;
            }
            expect(listed == 2, "the catalog lists both layers' own files");
            expect(c.Report().size() == 3 && c.Report()[2].files == 3 && c.Report()[2].note.empty() && c.Report()[1].archives == 1,
                   "layer report: 3 unpacked files, 1 single archive, no notes");
        }
        {
            MpqChain c;
            c.Open({ client, loose, single });
            expect(text(c.Read(shared)) == "from the single MPQ", "reordered: now the single MPQ wins");
            MpqLayer off = single;
            off.enabled = false;
            c.Open({ client, loose, off });
            expect(text(c.Read(shared)) == "from the unpacked folder" && !c.Read("Textures\\Wwe\\only_mpq.blp"), "disabled: the single MPQ gives nothing");
            c.Open({ client, { MpqLayer::Kind::Folder, (t / "empty").string() }, { MpqLayer::Kind::MpqFile, (t / "nope.MPQ").string() },
                     { MpqLayer::Kind::Folder, (t / "loose" / "Textures").string() } });
            printf("    notes: \"%s\" / \"%s\" / \"%s\"\n", c.Report()[1].note.c_str(), c.Report()[2].note.c_str(), c.Report()[3].note.c_str());
            expect(!c.Report()[1].note.empty() && !c.Report()[2].note.empty() && !c.Report()[3].note.empty(),
                   "notes for an empty folder, a missing MPQ, a folder one level too deep");
        }
        {
            // The version list (patch history) shows the unpacked folder's copy of a tile.
            MpqChain base;
            base.Open(data);
            Ghosts ghosts;
            std::vector<std::string> errors;
            ghosts.Reset(&base, "client", { { "layered", { client, loose } } }, errors);
            const auto versions = ghosts.Versions("Azeroth", 32, 48, nullptr);
            const bool seen = std::any_of(versions.begin(), versions.end(), [](const Ghosts::Version& v) { return v.label.find("loose/") != std::string::npos; });
            expect(seen, "the version list includes the unpacked folder's tile");
        }
        {
            // project.json: layers round-trip; an old project's client and sources become base and compare sources.
            Project p;
            p.dir = t / "project";
            p.clientDir = "C:/client";
            p.base = { "Mine", { client, single, loose } };
            p.compare = { { "Turtle", { { MpqLayer::Kind::MpqFolder, "C:/turtle" } } } };
            expect(p.Save(error), "project saved");
            const auto back = Project::Load(p.dir, error);
            expect(back && back->base.layers.size() == 3 && back->base.layers[2].kind == MpqLayer::Kind::Folder && !back->base.layers[2].installed &&
                       back->base.layers[1].kind == MpqLayer::Kind::MpqFile && back->compare.size() == 1 && back->compare[0].name == "Turtle",
                   "layers and compare sources come back as saved");
            put(t / "old" / "project.json", R"({"name":"old","clientDir":"C:/wxl","sources":[{"name":"Epoch","dataDir":"D:/epoch/Data"}]})");
            const auto old = Project::Load(t / "old", error);
            expect(old && old->base.layers.size() == 1 && old->base.layers[0].path == "C:/wxl" && old->compare.size() == 1 &&
                       old->compare[0].layers[0].path == "D:/epoch/Data",
                   "an old project: its client becomes the base, its sources compare sources");
        }
        fs::remove_all(t, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--scan-check`: a mixed folder (MPQs at any depth, a locale folder, two unpacked mods, one shipping its own MPQ,
    /// a client-like folder with Interface beside Data, stray files) scanned into layers: kinds, order, game paths read
    /// through them, strays reported; then a rescan after a mod is added and one removed keeps the user's settings.
    int ScanCheck()
    {
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        const fs::path t = fs::temp_directory_path() / "wow-world-editor-scancheck", root = t / "root";
        std::error_code ec;
        fs::remove_all(t, ec);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-66s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        auto put = [](const fs::path& p, const std::string& text) {
            fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << text;
        };
        auto mpq = [&](const fs::path& at, const std::string& file, const std::string& text) {
            put(t / "src" / at.filename().string() / file, text);
            std::string error;
            fs::create_directories(at.parent_path());
            WriteMpq(at, t / "src" / at.filename().string(), error);
        };
        mpq(root / "Client" / "Data" / "patch-A.MPQ", "Textures\\shared.blp", "client patch-A");
        mpq(root / "Client" / "Data" / "enUS" / "patch-enUS-A.MPQ", "Textures\\shared.blp", "client patch-enUS-A");
        put(root / "Client" / "Interface" / "AddOns" / "Foo" / "Foo.lua", "-- an addon: not a game tree, the client folder holds Data");
        put(root / "Client" / "Wow.exe", "not really");
        mpq(root / "Deep" / "a" / "b" / "patch-3.MPQ", "Textures\\deep.blp", "deep patch-3");
        put(root / "Mods" / "ModOne" / "World" / "Maps" / "Azeroth" / "one.txt", "mod one");
        put(root / "Mods" / "ModTwo" / "DBFilesClient" / "two.dbc", "mod two");
        put(root / "Mods" / "ModTwo" / "Textures" / "shared.blp", "mod two shared");
        mpq(root / "Mods" / "ModTwo" / "patch-M.MPQ", "Textures\\twompq.blp", "mod two's own archive");
        put(root / "Mods" / "readme.txt", "stray");
        put(root / "notes.txt", "stray");

        LayerScan scan = ScanForLayers(root);
        std::vector<std::string> order;
        for (const MpqLayer& l : scan.layers)
            order.push_back(std::string(l.kind == MpqLayer::Kind::Folder ? "dir:" : "mpq:") + fs::path(l.path).filename().string());
        std::string shown;
        for (const auto& o : order) shown += o + " ";
        printf("  layers, lowest first: %s\n  strays: %zu\n", shown.c_str(), scan.strayCount);
        const std::vector<std::string> want = { "mpq:patch-3.MPQ", "mpq:patch-A.MPQ", "mpq:patch-enUS-A.MPQ", "mpq:patch-M.MPQ", "dir:ModOne", "dir:ModTwo" };
        expect(order == want, "archives in client order, then unpacked trees; the client folder not a tree");
        for (const auto& f : scan.strays) printf("    stray: %s\n", f.c_str());
        expect(scan.strayCount == 4, "four files left out and reported (readme, notes, Wow.exe, the client's addon)");
        expect(std::all_of(scan.layers.begin(), scan.layers.end(), [&](const MpqLayer& l) { return l.from == root.string(); }), "every layer knows the scanned folder");
        {
            MpqChain c;
            c.Open(scan.layers);
            auto text = [&](const std::string& n) { const auto b = c.Read(n); return b ? std::string(b->begin(), b->end()) : std::string("<none>"); };
            expect(text("World\\Maps\\Azeroth\\one.txt") == "mod one" && text("DBFilesClient\\two.dbc") == "mod two", "unpacked mods read by their game paths");
            expect(text("Textures\\shared.blp") == "mod two shared", "an unpacked mod overrides the archives below it");
            expect(text("Textures\\twompq.blp") == "mod two's own archive" && text("Textures\\deep.blp") == "deep patch-3", "archives at any depth read");
            expect(!c.Read("patch-M.MPQ"), "an unpacked tree does not list the archives inside it as files");
        }
        // Rescan: ModOne disabled by the user and moved to the top; ModThree appears; ModTwo goes.
        std::vector<MpqLayer> mine = scan.layers;
        MpqLayer one = mine[4];
        one.enabled = false;
        mine.erase(mine.begin() + 4);
        mine.push_back(one);
        put(root / "Mods" / "ModThree" / "Textures" / "three.blp", "mod three");
        fs::remove_all(root / "Mods" / "ModTwo", ec);
        const std::vector<MpqLayer> again = RescanLayers(mine, root.string());
        std::string after;
        for (const MpqLayer& l : again) after += fs::path(l.path).filename().string() + (l.enabled ? " " : "(off) ");
        printf("  after rescan: %s\n", after.c_str());
        const auto at = [&](const std::string& name) {
            for (size_t i = 0; i < again.size(); ++i)
                if (fs::path(again[i].path).filename().string() == name) return int(i);
            return -1;
        };
        expect(at("ModTwo") < 0 && at("patch-M.MPQ") < 0, "the removed mod and its archive are gone");
        expect(at("ModOne") >= 0 && !again[size_t(at("ModOne"))].enabled && at("ModThree") == int(again.size()) - 1 && at("ModOne") == at("ModThree") - 1,
               "ModOne kept where the user put it, still off; ModThree added on top");
        fs::remove_all(t, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--mpq-check <folder> [keep.MPQ]`: packs a folder as the project's patch is packed and reads every file back;
    /// with a second path, also writes the pack there (to open with other tools).
    int MpqCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        const int bad = PackAndVerify(__wargv[2], std::filesystem::temp_directory_path() / "wow-world-editor-mpqcheck", "patch-enUS-Z.MPQ");
        if (__argc > 3)
        {
            std::string error;
            if (!WriteMpq(__wargv[3], __wargv[2], error)) printf("  keep: %s\n", error.c_str());
        }
        printf("%d problem(s)\n", bad);
        return bad ? 1 : 0;
    }

    /// `--tiles-check <data dir> <base map> <other map>`: in a scratch project, adds two tiles only the other map has
    /// (neighbours when it can), then checks they stream in exactly as the other map has them (heights, alpha, water),
    /// object ids, undo/redo through the overlay, a rebuilt overlay, and the export (ADT plus the WDT listing it).
    int TilesCheck()
    {
        if (__argc < 5 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string base = arg(3), other = arg(4);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        const auto project = std::filesystem::temp_directory_path() / "wow-world-editor-tilescheck";
        std::error_code ec;
        std::filesystem::remove_all(project, ec);
        MpqChain mpq;
        mpq.Open(arg(2));
        mpq.SetOverlay(project / "overlay");
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        terrain.SetProjectDir(project);
        if (!terrain.SetMap(base, error)) { printf("%s\n", error.c_str()); return 1; }
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-64s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        auto wdtOf = [&](const std::string& map) { return mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt"); };
        auto adtPath = [](const std::string& map, int key) {
            return "World\\Maps\\" + map + "\\" + map + "_" + std::to_string(key % 64) + "_" + std::to_string(key / 64) + ".adt";
        };
        const auto otherWdt = wdtOf(other);
        if (!otherWdt) return 1;
        const bool otherBig = WdtBigAlpha(*otherWdt), baseBig = WdtBigAlpha(*wdtOf(base));
        const std::vector<bool> theirs = WdtTiles(*otherWdt), ours = terrain.Present();
        // Two tiles only the other map has, side by side when there are such; one with objects first (ids to check).
        std::vector<int> pick;
        auto hasObjects = [&](int k) {
            const auto bytes = mpq.Read(adtPath(other, k));
            const auto adt = bytes ? ParseAdt(*bytes, otherBig) : std::nullopt;
            return adt && !(adt->doodads.empty() && adt->wmos.empty());
        };
        for (int pass = 0; pass < 2 && pick.empty(); ++pass)
            for (int k = 0; k < 4096 && pick.empty(); ++k)
                if (theirs[size_t(k)] && !ours[size_t(k)] && (pass == 1 || hasObjects(k)))
                {
                    pick.push_back(k);
                    if (k % 64 < 63 && theirs[size_t(k + 1)] && !ours[size_t(k + 1)]) pick.push_back(k + 1);
                }
        if (pick.empty()) { printf("%s has no tile %s lacks\n", other.c_str(), base.c_str()); return 1; }
        printf("adding %zu tile(s) of %s to %s (alpha %s -> %s):", pick.size(), other.c_str(), base.c_str(), otherBig ? "8-bit" : "4-bit", baseBig ? "8-bit" : "4-bit");
        for (int k : pick) printf(" %d_%d", k % 64, k / 64);
        printf("\n");
        std::vector<TerrainAdapter::NewTile> tiles;
        size_t withMinimap = 0;
        for (int k : pick)
            if (auto t = TerrainAdapter::ReadNewTile(mpq, other, k % 64, k / 64))
            {
                withMinimap += !t->minimap.empty();
                tiles.push_back(std::move(*t));
            }
        printf("  %zu of them with a minimap image in %s\n", withMinimap, other.c_str());
        auto change = terrain.AddTiles(tiles, otherBig, error);
        if (!error.empty()) printf("  add: %s\n", error.c_str());
        expect(change.has_value() && change->data.at("tiles").size() == pick.size(), "every tile added");
        if (!change) return 1;
        store.Commit(std::move(*change));

        auto loaded = [&]() {
            for (int i = 0; i < 30; ++i)
                for (int k : pick) terrain.Stream((k % 64 + 0.5f) * kTileSize, (k / 64 + 0.5f) * kTileSize, 0, error);
            return std::all_of(pick.begin(), pick.end(), [&](int k) { return terrain.Tiles().count(k) != 0; });
        };
        auto present = [&]() { return std::all_of(pick.begin(), pick.end(), [&](int k) { return terrain.Present()[size_t(k)] && WdtTiles(*wdtOf(base))[size_t(k)]; }); };
        expect(present(), "the map lists them (editor and overlay WDT)");
        auto wdlHas = [&](int k) {
            const auto wdl = mpq.Read("World\\Maps\\" + base + "\\" + base + ".wdl");
            return wdl && !ParseWdl(*wdl)[size_t(k)].empty();
        };
        expect(std::all_of(pick.begin(), pick.end(), wdlHas), "the overlay WDL has their far heights");
        {
            // The far heights as the editor writes them, against Blizzard's own WDL, on 20 stock tiles with real relief.
            const auto wdlBytes = *mpq.Read("World\\Maps\\" + base + "\\" + base + ".wdl");
            const auto stockWdl = ParseWdl(wdlBytes);
            int worst = 0, tested = 0, relief = 0;
            for (int k = 0; k < 4096 && tested < 20; ++k)
            {
                if (!ours[size_t(k)] || stockWdl[size_t(k)].empty()) continue;
                const auto [lo, hi] = std::minmax_element(stockWdl[size_t(k)].begin(), stockWdl[size_t(k)].end());
                if (*hi - *lo < 30) continue;   // flat sea: proves nothing
                const auto adt = ParseAdt(*mpq.Read(adtPath(base, k)), baseBig);
                if (!adt) continue;
                const auto mine = ParseWdl(WdlSetTile(wdlBytes, k % 64, k / 64, &*adt));
                for (size_t i = 0; i < 289; ++i) worst = std::max(worst, std::abs(int(mine[size_t(k)][i]) - int(stockWdl[size_t(k)][i])));
                relief = std::max(relief, *hi - *lo);
                ++tested;
            }
            printf("  WDL from %d stock tiles (up to %d yd of relief) vs Blizzard's: corners within %d yd\n", tested, relief, worst);
            expect(tested > 0 && worst <= 2, "far heights match Blizzard's WDL (rounding aside)");
        }
        expect(loaded(), "they stream in");
        // As the other map has them: heights, alpha maps, water; objects with fresh (or kept) ids.
        float worstHeight = 0;
        int worstAlpha = 0;
        bool sameWater = true;
        size_t objects = 0, fresh = 0;
        for (int k : pick)
        {
            const LoadedTile& mine = terrain.Tiles().at(k);
            const auto theirsAdt = ParseAdt(*mpq.Read(adtPath(other, k)), otherBig);
            for (size_t ci = 0; ci < mine.adt.chunks.size() && ci < theirsAdt->chunks.size(); ++ci)
            {
                const AdtChunk& a = mine.adt.chunks[ci];
                const AdtChunk& b = theirsAdt->chunks[ci];
                for (size_t v = 0; v < 145; ++v) worstHeight = std::max(worstHeight, std::fabs(a.baseY + a.heights[v] - b.baseY - b.heights[v]));
                for (size_t t = 0; t < a.alpha.size() && t < b.alpha.size(); ++t) worstAlpha = std::max(worstAlpha, std::abs(int(a.alpha[t]) - int(b.alpha[t])));
                sameWater = sameWater && TerrainAdapter::LiquidState(mine.adt, a) == TerrainAdapter::LiquidState(*theirsAdt, b);
            }
            objects += mine.adt.doodads.size() + mine.adt.wmos.size();
            for (const auto& d : mine.adt.doodads) fresh += d.uniqueId >= 200'000'000u;
            for (const auto& w : mine.adt.wmos) fresh += w.uniqueId >= 200'000'000u;
        }
        printf("  heights within %.4f yd, alpha within %d/255, %zu objects (%zu with fresh ids)\n", worstHeight, worstAlpha, objects, fresh);
        expect(worstHeight < 0.001f, "heights as the other map has them");
        expect(worstAlpha <= (otherBig == baseBig ? 0 : 17), "alpha maps as the other map has them (4-bit rounding aside)");
        expect(sameWater, "water as the other map has it");
        expect(objects == 0 || fresh > 0, "objects got fresh unique ids");

        store.Undo();
        expect(!std::any_of(pick.begin(), pick.end(), [&](int k) { return terrain.Present()[size_t(k)] || terrain.Tiles().count(k); }),
               "undo: gone from the map");
        // (the base archives may still hold a stray file for the tile: only the WDT decides whether the client loads it)
        expect(!std::any_of(pick.begin(), pick.end(), [&](int k) { return WdtTiles(*wdtOf(base))[size_t(k)] || std::filesystem::exists(mpq.OverlayPath(adtPath(base, k))); }),
               "undo: gone from the overlay and its WDT");
        expect(!std::any_of(pick.begin(), pick.end(), wdlHas), "undo: gone from the WDL");
        store.Redo();
        expect(present() && loaded(), "redo: back");

        std::filesystem::remove_all(project / "overlay", ec);   // as if the session ended unsaved; reopening rebuilds it
        terrain.RebuildOverlay();
        expect(std::all_of(pick.begin(), pick.end(), [&](int k) { return mpq.HasOwn(adtPath(base, k)); }) && present(), "overlay rebuilt from the changes");

        const auto out = project / "out";
        // A minimap the editor rendered of the first tile (App::RenderMinimaps writes these) must win over the other map's.
        const std::vector<uint8_t> rendered = WriteBlp(4, 4, std::vector<uint8_t>(64, 200).data());
        const std::string firstName = base + "_" + std::to_string(pick[0] % 64) + "_" + std::to_string(pick[0] / 64);
        std::filesystem::create_directories(project / "minimaps", ec);
        std::ofstream(project / "minimaps" / (firstName + ".blp"), std::ios::binary)
            .write(reinterpret_cast<const char*>(rendered.data()), std::streamsize(rendered.size()));
        std::vector<std::filesystem::path> files;
        std::vector<Problem> exportProblems;
        terrain.Export(out, error, &files, &exportProblems);
        for (const Problem& p : exportProblems) printf("  export: %s\n", p.message.c_str());
        size_t sound = 0;
        for (int k : pick)
        {
            std::ifstream f(out / "World" / "Maps" / base / (base + "_" + std::to_string(k % 64) + "_" + std::to_string(k / 64) + ".adt"), std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            sound += !bytes.empty() && ValidateAdt(bytes, baseBig).empty();
        }
        expect(sound == pick.size(), "exported tiles pass the structure check");
        std::vector<uint8_t> wdt;
        if (std::ifstream w(out / "World" / "Maps" / base / (base + ".wdt"), std::ios::binary); w)   // closed before the folder goes
            wdt.assign(std::istreambuf_iterator<char>(w), std::istreambuf_iterator<char>());
        expect(!wdt.empty() && std::all_of(pick.begin(), pick.end(), [&](int k) { return WdtTiles(wdt)[size_t(k)]; }), "exported WDT lists them");
        {
            std::vector<uint8_t> wdl, trs;
            if (std::ifstream f(out / "World" / "Maps" / base / (base + ".wdl"), std::ios::binary); f) wdl.assign(std::istreambuf_iterator<char>(f), {});
            if (std::ifstream f(out / "textures" / "Minimap" / "md5translate.trs", std::ios::binary); f) trs.assign(std::istreambuf_iterator<char>(f), {});
            expect(!wdl.empty() && std::all_of(pick.begin(), pick.end(), [&](int k) { return !ParseWdl(wdl)[size_t(k)].empty(); }), "exported WDL has them");
            size_t named = 0;
            for (int k : pick)
                if (const auto file = TrsLookup(trs, base, k % 64, k / 64))
                    named += std::filesystem::exists(out / "textures" / "Minimap" / *file);
            printf("  exported minimaps: %zu of %zu tile(s) named in md5translate.trs with their image\n", named, pick.size());
            expect(named >= withMinimap, "every minimap the other map had is exported and indexed");
            std::vector<uint8_t> first;
            if (const auto file = TrsLookup(trs, base, pick[0] % 64, pick[0] / 64))
                if (std::ifstream f(out / "textures" / "Minimap" / *file, std::ios::binary); f) first.assign(std::istreambuf_iterator<char>(f), {});
            expect(first == rendered, "an editor-rendered minimap wins over the other map's");
        }
        // The export as the project's patch MPQ: every file must read back exactly.
        expect(PackAndVerify(out, project / "mpqcheck", "patch-enUS-Z.MPQ") == 0, "export packed into a patch MPQ reads back exactly");
        std::vector<Problem> cracks;
        terrain.FindCracks(cracks);
        printf("  crack report: %zu tile(s) with open edges (seams with the map's own tiles are expected where the versions differ)\n", cracks.size());
        std::filesystem::remove_all(project, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--water-check <data dir> <map> <x> <y>`: on a tile with both wet and dry chunks, paste a dry chunk over a wet one
    /// (its water must go) and the wet one over a dry one (its water must come), undo and redo, turn the water four
    /// quarters (must come back the same), then export and read the tile back: MH2O as pasted, every other chunk's water kept.
    int WaterCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string map = arg(3);
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return 1; }
        for (int i = 0; i < 9; ++i) terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1, error);
        const int key = TileKey(tx, ty);
        auto tile = [&]() -> const LoadedTile& { return terrain.Tiles().at(key); };
        auto water = [&](int ci) { return TerrainAdapter::LiquidState(tile().adt, tile().adt.chunks[size_t(ci)]); };
        int wet = -1, dry = -1;
        for (int ci = 0; ci < int(tile().adt.chunks.size()); ++ci)
            (water(ci).empty() ? dry : wet) = ci;
        const size_t mclq = size_t(std::count_if(tile().adt.liquids.begin(), tile().adt.liquids.end(), [](const AdtLiquid& l) { return l.fromMclq; }));
        printf("%s_%d_%d: %zu liquid instance(s) (%zu old MCLQ); wet chunk %d, dry chunk %d\n", map.c_str(), tx, ty, tile().adt.liquids.size(), mclq,
               wet, dry);
        if (wet < 0 || dry < 0) { printf("need a tile with both wet and dry chunks\n"); return 1; }
        const nlohmann::json wetBefore = water(wet);
        const size_t total = tile().adt.liquids.size(), wetCount = wetBefore.size();
        auto cell = [&](int ci) { return terrain.GridOf({ key, ci }); };
        const TerrainClipboard dryClip = terrain.Copy({ { key, dry } }), wetClip = terrain.Copy({ { key, wet } });
        int problems = 0;
        auto expect = [&](bool ok, const char* what) { printf("  %-60s %s\n", what, ok ? "ok" : "FAILED"); problems += !ok; };

        // Rotation is lossless: four quarter turns give the same water.
        nlohmann::json turned = wetBefore;
        for (int i = 0; i < 4; ++i) turned = TerrainAdapter::RotateLiquidState(turned);
        expect(turned == wetBefore, "four quarter turns of the wet chunk's water");

        PasteOptions o;
        o.blend = false;
        o.objects = false;
        auto paste = [&](const TerrainClipboard& clip, int target) {
            const auto [gx, gz] = cell(target);
            auto change = terrain.ApplyPlan(terrain.PlanPaste(clip, gx, gz, 0.0f, o), "water check");
            if (change) store.Commit(std::move(*change));
            return change.has_value();
        };
        expect(paste(dryClip, wet), "dry chunk pasted over the wet one");
        expect(water(wet).empty(), "  its water is gone");
        store.Undo();
        expect(water(wet) == wetBefore, "  undo brings it back");
        store.Redo();
        expect(water(wet).empty(), "  redo removes it again");
        expect(paste(wetClip, dry), "wet chunk pasted over the dry one");
        expect(water(dry) == wetBefore, "  the dry chunk now has exactly that water");
        expect(tile().adt.liquids.size() == total, "  instance count unchanged (moved, not lost)");

        // Export and read back: the client's view of the tile.
        const auto out = std::filesystem::temp_directory_path() / "wow-world-editor-watercheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::vector<std::filesystem::path> files;
        std::vector<Problem> exportProblems;
        terrain.Export(out, error, &files, &exportProblems);
        for (const Problem& p : exportProblems) printf("  export: %s\n", p.message.c_str());
        const std::string name = map + "_" + std::to_string(tx) + "_" + std::to_string(ty) + ".adt";
        const auto file = std::find_if(files.begin(), files.end(), [&](const auto& p) { return p.filename().string() == name; });
        expect(file != files.end(), "exported the tile (structure check passed)");
        if (file != files.end())
        {
            std::ifstream f(*file, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const auto wdt = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
            const auto back = ParseAdt(bytes, wdt && WdtBigAlpha(*wdt));
            expect(back.has_value(), "exported tile parses");
            if (back)
            {
                expect(TerrainAdapter::LiquidState(*back, back->chunks[size_t(wet)]).empty(), "  exported: wet chunk is dry");
                expect(TerrainAdapter::LiquidState(*back, back->chunks[size_t(dry)]) == wetBefore, "  exported: dry chunk has the pasted water");
                size_t kept = 0;
                for (size_t ci = 0; ci < back->chunks.size(); ++ci)
                    if (int(ci) != wet && int(ci) != dry)
                    {
                        const bool same = TerrainAdapter::LiquidState(*back, back->chunks[ci]) == water(int(ci));
                        if (!same && kept + 1 == ci)   // the first difference, shown short
                            printf("  chunk %zu differs:\n    editor   %.300s\n    exported %.300s\n", ci, water(int(ci)).dump().c_str(),
                                   TerrainAdapter::LiquidState(*back, back->chunks[ci]).dump().c_str());
                        kept += same;
                    }
                expect(kept == back->chunks.size() - 2, "  exported: every other chunk's water unchanged");
                printf("  exported %zu instance(s) (was %zu, moved %zu)\n", back->liquids.size(), total, wetCount);
            }
        }
        std::filesystem::remove_all(out, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    int GhostCheck()
    {
        if (__argc < 7 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string map = arg(4);
        const int tx = std::stoi(arg(5)), ty = std::stoi(arg(6));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return 1; }
        for (int i = 0; i < 9; ++i) terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1, error);

        Ghosts ghosts;
        std::vector<std::string> errors;
        ghosts.Reset(&mpq, "WXL", { { "Other", { { MpqLayer::Kind::MpqFolder, arg(3) } } } }, errors);
        for (const auto& e : errors) printf("source error: %s\n", e.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        const auto versions = ghosts.Versions(map, tx, ty, &terrain.Tiles().at(TileKey(tx, ty)));
        printf("%s_%d_%d: %zu version(s) in %.0f ms\n", map.c_str(), tx, ty, versions.size(),
               std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
        for (const auto& v : versions)
            printf("  %-55s %s height diff %6.2f yd, %4zu objects, %zu identical cop(ies) elsewhere\n", v.label.c_str(), v.inUse ? "IN USE" : "      ",
                   v.heightDiff.value_or(-1.0f), v.doodads + v.wmos, v.alsoIn.size());
        if (versions.empty()) return 1;

        // A ghost of the version the map itself uses, and one of the other client as a whole.
        const Ghosts::Version* inUse = nullptr;
        for (const auto& v : versions)
            if (v.inUse) inUse = &v;
        if (!inUse) { printf("no version marked in use\n"); return 1; }
        const int same = ghosts.AddLayer(inUse->source, inUse->archive, inUse->label).id;
        const int other = ghosts.AddLayer(1, -1, "Other (whole client)").id;
        for (int i = 0; i < 40; ++i)
        {
            const auto r = ghosts.Stream(map, (tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1);
            for (const auto& [id, key] : r.loaded)
                renderer.LoadTile(Ghosts::Key(id, key), ghosts.Find(id)->tiles.at(key).adt, ghosts.Chain(ghosts.Find(id)->source), id);
        }
        printf("layers: same-version %zu tiles (%zu missing), other client %zu tiles (%zu missing)\n", ghosts.Find(same)->tiles.size(),
               ghosts.Find(same)->missing.size(), ghosts.Find(other)->tiles.size(), ghosts.Find(other)->missing.size());

        // Copy 3x3 chunks from each ghost and plan an in-place paste (absolute heights, no blend).
        int problems = 0;
        std::set<std::pair<int, int>> cells;
        for (int dz = 0; dz < 3; ++dz)
            for (int dx = 0; dx < 3; ++dx) cells.insert({ tx * 16 + 6 + dx, ty * 16 + 6 + dz });
        for (int id : { same, other })
        {
            const TerrainClipboard clip = TerrainAdapter::CopyFrom(ghosts.Find(id)->tiles, cells);
            PasteOptions o;
            o.blend = false;
            const PastePlan plan = terrain.PlanPaste(clip, clip.originX, clip.originZ, 0.0f, o);
            float worstVsGhost = 0, worstVsMap = 0;
            for (const auto& pc : plan.chunks)
            {
                const AdtChunk& c = *terrain.Chunk(pc.ref);
                const auto [gx, gz] = terrain.GridOf(pc.ref);
                for (const auto& e : clip.chunks)
                    if (gx == clip.originX + e.dx && gz == clip.originZ + e.dz)
                        for (size_t j = 0; j < 145; ++j)
                        {
                            worstVsGhost = std::max(worstVsGhost, std::fabs(c.baseY + pc.heights[j] - e.heights[j]));
                            worstVsMap = std::max(worstVsMap, std::fabs(pc.heights[j] - c.heights[j]));
                        }
            }
            printf("in-place paste from %s: %zu chunks, %zu objects, matches the ghost within %.4f yd, differs from the map by up to %.2f yd\n",
                   ghosts.Find(id)->label.c_str(), plan.chunks.size(), plan.doodads.size() + plan.wmos.size(), worstVsGhost, worstVsMap);
            if (plan.footprint.size() != 9 || worstVsGhost > 0.001f) ++problems;   // plus welded neighbours where heights differ
            if (id == same && worstVsMap > 0.001f) ++problems;   // the version in use is the map itself
        }
        // Other maps as ghosts of this one: every Map.dbc folder whose name starts with this map's.
        if (const auto dbc = mpq.Read("DBFilesClient\\Map.dbc"))
            for (const MapEntry& m : ParseMapDbc(*dbc))
            {
                if (m.directory == map || m.directory.rfind(map, 0) != 0) continue;
                const int id = ghosts.AddLayer(0, -1, m.directory, m.directory).id;
                for (int i = 0; i < 12; ++i) ghosts.Stream(map, (tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1);
                const Ghosts::Layer& l = *ghosts.Find(id);
                std::string diff = "-";
                if (const auto it = l.tiles.find(TileKey(tx, ty)); it != l.tiles.end())
                {
                    double sum = 0;
                    size_t n = 0;
                    const LoadedTile& mainTile = terrain.Tiles().at(TileKey(tx, ty));
                    for (size_t cell = 0; cell < 256; ++cell)
                    {
                        const int16_t a = mainTile.byGrid[cell], b = it->second.byGrid[cell];
                        if (a < 0 || b < 0) continue;
                        for (size_t k = 0; k < 145; ++k)
                            sum += std::fabs(mainTile.adt.chunks[size_t(a)].baseY + mainTile.adt.chunks[size_t(a)].heights[k] -
                                             it->second.adt.chunks[size_t(b)].baseY - it->second.adt.chunks[size_t(b)].heights[k]);
                        n += 145;
                    }
                    char buf[64];
                    snprintf(buf, sizeof buf, "%.2f yd", n ? sum / n : 0.0);
                    diff = buf;
                }
                printf("map ghost %-24s (id %4u): %zu tiles loaded, %zu missing, centre tile height diff %s\n", m.directory.c_str(), m.id,
                       l.tiles.size(), l.missing.size(), diff.c_str());
                // Copying from that ghost carries its own objects, and an in-place paste plans to add them all.
                std::set<std::pair<int, int>> whole;
                for (int cz = 0; cz < 16; ++cz)
                    for (int cx = 0; cx < 16; ++cx) whole.insert({ tx * 16 + cx, ty * 16 + cz });
                const TerrainClipboard clip = TerrainAdapter::CopyFrom(l.tiles, whole);
                PasteOptions po;
                po.blend = false;
                const PastePlan plan = terrain.PlanPaste(clip, clip.originX, clip.originZ, 0.0f, po);
                printf("    copy of the whole tile: %zu chunks, %zu doodads + %zu WMOs copied, %zu + %zu planned for the paste\n", clip.chunks.size(),
                       clip.doodads.size(), clip.wmos.size(), plan.doodads.size(), plan.wmos.size());
                if (clip.chunks.size() != 256 || plan.doodads.size() != clip.doodads.size() || plan.wmos.size() != clip.wmos.size()) ++problems;
            }
        printf("ghost check: %d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--render <data dir> <map> <x> <y> <out.png> [yaw] [pitch] [height above ground] [fx] [fz]`:
    /// degrees for yaw/pitch, yards above the ground under the camera, (fx, fz) = camera spot within the tile (0..1).
    /// load the tile and its neighbours on a software device and save what the editor camera would show.
    /// `--minimap-check <data dir> <map> <x> <y> [out.png]`: renders the tile straight down as the editor's minimaps
    /// are, decodes the client's own minimap of it, and scores every rotation / mirror of the render against it: the
    /// layout MinimapFromTopDown uses is printed with them. The score only hints (lighting and textures differ, so it can
    /// even come out negative for the right layout); the PNGs settle it: <out> is the editor's, <out>.client.png the
    /// client's. Checked by eye 2026-10-05: no turn, no mirror (Northshire, Elwynn lakes, a Kalimdor coast).
    int MinimapCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string map = arg(3);
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        ModelRenderer models;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) { printf("%s\n", error.c_str()); return 1; }
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return 1; }
        for (int i = 0; i < 4; ++i) terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 0, error);
        const auto it = terrain.Tiles().find(TileKey(tx, ty));
        if (it == terrain.Tiles().end()) { printf("tile not loaded\n"); return 1; }
        models.AddTile(it->first, it->second.adt, mpq);
        DrawOptions look;
        ModelRenderer::DrawSettings lookModels;
        MinimapLook(look, lookModels);
        const std::vector<uint8_t> mine = MinimapFromTopDown(
            RenderTopDown(device.Get(), context.Get(), renderer, models, look, lookModels, tx * kTileSize, ty * kTileSize,
                          kTileSize, kTileSize, it->second.maxHeight + 50.0f, kMinimapSize, kMinimapSize, true),
            kMinimapSize);

        const auto trs = mpq.Read("textures\\Minimap\\md5translate.trs");
        const auto file = trs ? TrsLookup(*trs, map, tx, ty) : std::nullopt;
        const auto blp = file ? mpq.Read("textures\\Minimap\\" + *file) : std::nullopt;
        const auto image = blp ? ParseBlp(*blp) : std::nullopt;
        if (!image || image->width != kMinimapSize || image->height != kMinimapSize) { printf("no %ux%u client minimap for the tile\n", kMinimapSize, kMinimapSize); return 1; }
        const std::vector<uint8_t> theirs = BlpPixels(*image);

        // Large-scale structure (16 x 16 block means of luminance: mountains, valleys, water) of the render, turned,
        // against the client's picture; texel detail and lighting differ too much between them to compare directly.
        const int n = 16, block = int(kMinimapSize) / n;
        auto blocks = [&](const std::vector<uint8_t>& p) {
            std::vector<double> b(size_t(n * n), 0.0);
            for (int y = 0; y < int(kMinimapSize); ++y)
                for (int x = 0; x < int(kMinimapSize); ++x)
                {
                    const size_t i = size_t(y) * kMinimapSize + size_t(x);
                    b[size_t((y / block) * n + x / block)] += 0.3 * p[i * 4] + 0.59 * p[i * 4 + 1] + 0.11 * p[i * 4 + 2];
                }
            return b;
        };
        const std::vector<double> bm = blocks(mine), bt = blocks(theirs);
        auto score = [&](int turn, bool mirror) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    int u = mirror ? n - 1 - x : x, v = y;
                    for (int t = 0; t < turn; ++t) { const int w = u; u = n - 1 - v; v = w; }
                    const double a = bm[size_t(v * n + u)], b = bt[size_t(y * n + x)];
                    sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
                }
            const double N = double(n) * n, cov = sab / N - sa / N * sb / N;
            const double va = saa / N - (sa / N) * (sa / N), vb = sbb / N - (sb / N) * (sb / N);
            return va > 0 && vb > 0 ? cov / std::sqrt(va * vb) : 0.0;
        };
        double best = -2, identity = score(0, false);
        int bestTurn = 0;
        bool bestMirror = false;
        for (int mirror = 0; mirror < 2; ++mirror)
            for (int turn = 0; turn < 4; ++turn)
            {
                const double s = score(turn, mirror != 0);
                printf("  turn %3d deg%s: correlation %.3f\n", turn * 90, mirror ? ", mirrored" : "          ", s);
                if (s > best) { best = s; bestTurn = turn; bestMirror = mirror != 0; }
            }
        printf("best: turn %d deg%s (%.3f); the editor's layout scores %.3f\n", bestTurn * 90, bestMirror ? " mirrored" : "", best, identity);
        if (__argc > 6)
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC writes the PNGs
            std::vector<uint8_t> bgra = mine;
            for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
            SavePng(__wargv[6], kMinimapSize, kMinimapSize, std::move(bgra));
            std::vector<uint8_t> client = theirs;   // the client's own beside it: <out>.client.png
            for (size_t i = 0; i < client.size(); i += 4) std::swap(client[i], client[i + 2]);
            SavePng(std::wstring(__wargv[6]) + L".client.png", kMinimapSize, kMinimapSize, std::move(client));
        }
        return mine.empty() ? 1 : 0;
    }

    int RenderCheck()
    {
        if (__argc < 7 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5));
        const float yaw = __argc > 7 ? std::stof(arg(7)) : 0.0f, pitch = __argc > 8 ? std::stof(arg(8)) : -25.0f;
        const float above = __argc > 9 ? std::stof(arg(9)) : 120.0f;
        const float fx = __argc > 10 ? std::stof(arg(10)) : 0.5f, fz = __argc > 11 ? std::stof(arg(11)) : -0.1f;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, std::getenv("WWE_HW") ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        ModelRenderer models;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) { printf("%s\n", error.c_str()); return 1; }
        MpqChain mpq;
        mpq.Open(arg(2));
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(arg(3), error)) { printf("%s\n", error.c_str()); return 1; }
        const int radius = std::getenv("WWE_RADIUS") ? std::atoi(std::getenv("WWE_RADIUS")) : 1;   // more tiles: see the LOD
        // WWE_LOADER=1: stream through the background loader as the editor does (worker-thread ADT, texture and mesh
        // preparation), to reproduce what the window shows.
        Loader loader;
        if (std::getenv("WWE_LOADER"))
        {
            loader.Start(&mpq);
            renderer.SetLoader(&loader);
            models.SetLoader(&loader);
            terrain.SetLoader(&loader);
            const size_t want = size_t((2 * radius + 1) * (2 * radius + 1));
            for (int i = 0; i < 2000 && (terrain.Tiles().size() < want || loader.Pending()); ++i)
            {
                terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, radius, error);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else
            for (int i = 0; i < (2 * radius + 1) * (2 * radius + 1); ++i)
                terrain.Stream((tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, radius, error);
        // Optional 12th argument: another map shown over this one as ghost layer 1 (default tint and opacity).
        Ghosts ghosts;
        if (__argc > 12)
        {
            std::vector<std::string> errors;
            ghosts.Reset(&mpq, "client", {}, errors);
            const Ghosts::Layer& layer = ghosts.AddLayer(0, -1, arg(12), arg(12));
            renderer.SetLayerStyle(layer.id, layer.tint, true);
            for (int i = 0; i < 12; ++i)
                for (const auto& [id, key] : ghosts.Stream(arg(3), (tx + 0.5f) * kTileSize, (ty + 0.5f) * kTileSize, 1).loaded)
                    renderer.LoadTile(Ghosts::Key(id, key), ghosts.Find(id)->tiles.at(key).adt, mpq, id);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& [key, tile] : terrain.Tiles()) models.AddTile(key, tile.adt, mpq);
        // A WMO-only map: its one WMO, as the editor shows it; the camera then works inside its box.
        const std::optional<WmoPlacement>& global = terrain.GlobalWmo();
        if (global)
        {
            Adt adt;
            adt.wmos.push_back(*global);
            models.AddTile(kGlobalWmoKey, adt, mpq);
            printf("WMO-only map: %s at (%.1f, %.1f, %.1f) rot (%.1f, %.1f, %.1f) doodad set %u\n", global->model.c_str(), global->pos[0], global->pos[1],
                   global->pos[2], global->rot[0], global->rot[1], global->rot[2], global->doodadSet);
        }
        if (const char* server = std::getenv("WWE_SERVER"))   // creature and gameobject spawns from that server's world database
        {
            std::string password, note;
            const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(server), password, note);
            Db db;
            if (!profile || !db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error))
                printf("spawns: %s%s\n", note.c_str(), error.c_str());
            else
            {
                uint32_t mapId = 0;
                for (const auto& m : ParseMapDbc(mpq.Read("DBFilesClient\\Map.dbc").value_or(std::vector<uint8_t>{})))
                    if (_stricmp(m.directory.c_str(), arg(3).c_str()) == 0) mapId = m.id;
                float minX, minY, maxX, maxY, unused;
                EditorToServer({ (tx + radius + 1) * kTileSize, 0, (ty + radius + 1) * kTileSize }, minX, minY, unused);
                EditorToServer({ (tx - radius) * kTileSize, 0, (ty - radius) * kTileSize }, maxX, maxY, unused);
                if (global) minX = minY = -20000, maxX = maxY = 20000;   // the whole instance
                ChangeStore spawnStore;
                DisplayLooks looks(mpq);
                for (const SpawnKind kind : { SpawnKind::Creature, SpawnKind::GameObject })
                {
                    SpawnAdapter spawns(spawnStore, kind);
                    spawns.SetDb(&db);
                    size_t total = 0, shown = 0;
                    for (const Spawn& s : spawns.Around(mapId, minX, minY, maxX, maxY))
                    {
                        ++total;
                        if (const auto model = looks.SpawnLook(s))
                        {
                            const XMFLOAT3 p = ServerToEditor(s.x, s.y, s.z);
                            AddSpawnModel(models, -3, *model, p, s.orientation, s.guid, mpq);
                            ++shown;
                            if (std::getenv("WWE_LIST") && std::hypot(p.x - (tx + fx) * kTileSize, p.z - (ty + fz) * kTileSize) < 60)
                            {
                                printf("  %s %u %s display %u scale %.2f %s", spawns.Table(), s.guid, s.name.c_str(), s.displayId, model->scale, model->look.model.c_str());
                                for (const auto& [type, tex] : model->look.textures) printf(" [%u]%s", type, tex.c_str());
                                if (!model->look.geosets.empty()) printf(" geosets");
                                for (uint16_t g : model->look.geosets) printf(" %u", g);
                                for (const auto& item : model->items) printf(" +%u:%s", item.attachment, item.look.model.c_str());
                                printf("\n");
                            }
                        }
                    }
                    printf("%s spawns: %zu, with a model %zu\n", spawns.Table(), total, shown);
                    if (global && kind == SpawnKind::Creature)
                    {
                        // Inside an instance every creature should stand on the WMO: a floor within a few yards below it.
                        ModelRenderer::DrawSettings all;
                        all.distance = 1e9f;
                        size_t floored = 0;
                        XMFLOAT3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
                        for (const Spawn& s : spawns.Around(mapId, minX, minY, maxX, maxY))
                        {
                            const XMFLOAT3 p = ServerToEditor(s.x, s.y, s.z);
                            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
                            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
                            if (const auto hit = models.Pick(XMVectorSet(p.x, p.y + 2, p.z, 0), XMVectorSet(0, -1, 0, 0), 6.0f, all); hit && hit->wmo) ++floored;
                        }
                        XMFLOAT3 c[8];
                        if (models.Corners(true, global->uniqueId, c))
                        {
                            XMFLOAT3 wl = c[0], wh = c[0];
                            for (const auto& k : c) wl = { std::min(wl.x, k.x), std::min(wl.y, k.y), std::min(wl.z, k.z) }, wh = { std::max(wh.x, k.x), std::max(wh.y, k.y), std::max(wh.z, k.z) };
                            printf("WMO box (%.0f..%.0f, %.0f..%.0f, %.0f..%.0f), creature box (%.0f..%.0f, %.0f..%.0f, %.0f..%.0f)\n", wl.x, wh.x, wl.y, wh.y,
                                   wl.z, wh.z, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);
                        }
                        printf("%s creatures on a WMO floor: %zu of %zu\n", floored * 10 >= total * 9 ? "ok  " : "FAIL", floored, total);
                    }
                }
            }
        }
        const float loadMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

        const auto centre = terrain.Tiles().find(TileKey(tx, ty));
        if (centre == terrain.Tiles().end() && !global) { printf("tile not loaded\n"); return 1; }
        // WMO-only: (fx, fz) are fractions across the WMO's box (x, z), `above` yards over its middle height.
        const float ex = global ? global->extMin[0] + fx * (global->extMax[0] - global->extMin[0]) : (tx + fx) * kTileSize;
        const float ez = global ? global->extMin[2] + fz * (global->extMax[2] - global->extMin[2]) : (ty + fz) * kTileSize;
        const XMFLOAT3 eye{ ex, global ? (global->extMin[1] + global->extMax[1]) / 2 + above : terrain.HeightAt(ex, ez).value_or(centre->second.maxHeight) + above, ez };
        const float y = XMConvertToRadians(yaw), p = XMConvertToRadians(pitch);
        const XMVECTOR fwd = XMVectorSet(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y), 0);
        const UINT w = 1280, h = 720;
        const XMMATRIX viewProj = XMMatrixLookToRH(XMLoadFloat3(&eye), fwd, XMVectorSet(0, 1, 0, 0)) *
                                  XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), float(w) / h, 1.0f, 6000.0f);

        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> color, depth, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        device->CreateTexture2D(&d, nullptr, &color);
        device->CreateRenderTargetView(color.Get(), nullptr, &rtv);
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        device->CreateTexture2D(&d, nullptr, &depth);
        device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&d, nullptr, &staging);

        const float sky[4] = { 0.36f, 0.52f, 0.72f, 1 };
        const D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->RSSetViewports(1, &vp);
        context->ClearRenderTargetView(rtv.Get(), sky);
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        if (const char* time = std::getenv("WWE_LIGHT"))   // the game's light at the eye, at this time (half-minutes), as View > Game lighting
        {
            ChangeStore lightStore;
            Lights lights(mpq, lightStore);
            uint32_t mapId = 0;
            for (const auto& m : ParseMapDbc(mpq.Read("DBFilesClient\\Map.dbc").value_or(std::vector<uint8_t>{})))
                if (_stricmp(m.directory.c_str(), arg(3).c_str()) == 0) mapId = m.id;
            const LightState s = lights.At(mapId, eye, std::atoi(time));
            SceneLight l;
            l.on = true;
            l.diffuse = s.colors[0];
            l.ambient = s.colors[1];
            l.fog = s.colors[7];
            l.fogEnd = s.floats[0] > 1 ? s.floats[0] : 1000;
            l.fogStart = l.fogEnd * std::clamp(s.floats[1], 0.0f, 0.99f);
            for (size_t i = 0; i < 5; ++i) l.sky[i] = s.colors[2 + i];
            renderer.SetSceneLight(l);
            renderer.DrawSky(viewProj);
            printf("game light at %s: fog %.0f-%.0f yd\n", time, l.fogStart, l.fogEnd);
        }
        if (const auto wdl = mpq.Read("World\\Maps\\" + arg(3) + "\\" + arg(3) + ".wdl"))   // the far map, as the editor draws it
        {
            renderer.LoadFar(ParseWdl(*wdl));
            renderer.DrawFar(XMMatrixLookToRH(XMLoadFloat3(&eye), fwd, XMVectorSet(0, 1, 0, 0)) *
                             XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), float(w) / h, 100.0f, 40000.0f));
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        }
        DrawOptions options;
        options.showObjects = false;
        options.eye = eye;
        const int solo = std::getenv("WWE_SOLO") && __argc > 12 ? 1 : 0;   // show the ghost map alone, as Solo does
        options.solo = solo;
        options.lod = std::getenv("WWE_NO_LOD") == nullptr;   // compare renders with and without
        for (int i = 0; i < 150; ++i) renderer.Draw(viewProj, options);   // warm-up: bakes the far tiles (2 a frame), as a few seconds in the editor would
        {
            // Terrain draw time, waited for on the device: the average of 5 draws after the first.
            D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
            ComPtr<ID3D11Query> done;
            device->CreateQuery(&qd, &done);
            context->End(done.Get());   // let the warm-up finish first
            while (context->GetData(done.Get(), nullptr, 0, 0) == S_FALSE) {}
            const auto d0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 5; ++i) renderer.Draw(viewProj, options);
            context->End(done.Get());
            while (context->GetData(done.Get(), nullptr, 0, 0) == S_FALSE) {}
            printf("terrain draw: %.1f ms (lod %s)\n", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - d0).count() / 5,
                   options.lod ? "on" : "off");
        }
        ModelRenderer::DrawSettings settings;
        settings.distance = 1200.0f;
        models.Draw(viewProj, eye, settings);
        {
            const auto m0 = std::chrono::steady_clock::now();   // CPU side of a model frame (culling, bone posing, uploads)
            for (int i = 0; i < 5; ++i) models.Draw(viewProj, eye, settings);
            printf("model draw (CPU): %.2f ms\n", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m0).count() / 5);
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            const auto hit = models.Pick(XMLoadFloat3(&eye), XMVector3Normalize(fwd), 6000.0f, settings);
            printf("centre pick: %s uid %u at %.1f yd, %.1f ms\n", hit ? (hit->wmo ? "WMO" : "M2") : "nothing", hit ? hit->uid : 0u, hit ? hit->distance : 0.0f,
                   std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        renderer.DrawWater(viewProj, solo);
        context->CopyResource(staging.Get(), color.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 1;
        std::vector<uint8_t> pixels(size_t(w) * h * 4);
        for (UINT row = 0; row < h; ++row)
            memcpy(pixels.data() + size_t(row) * w * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch, w * 4);
        context->Unmap(staging.Get(), 0);
        size_t lowAlpha = 0;   // pixels the window would have let the background through before the viewport drew opaque
        for (size_t i = 3; i < pixels.size(); i += 4) { lowAlpha += pixels[i] < 128; pixels[i] = 255; }
        printf("scene alpha below 50%%: %.1f%% of pixels\n", 100.0 * lowAlpha / (pixels.size() / 4));

        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool saved = SavePng(__wargv[6], w, h, std::move(pixels));
        printf("tiles %zu, models %zu, objects %zu, drawn %zu, model load %.0f ms, png %s\n", terrain.Tiles().size(), models.ModelCount(),
               models.InstanceCount(), models.DrawnLastFrame(), loadMs, saved ? "saved" : "FAILED");
        return saved ? 0 : 1;
    }
}

namespace
{
    /// `--triggers-check <data dir>`: AreaTrigger.dbc and Map.dbc read through their adapters (layout, shapes), then a
    /// trigger moved and turned, one added and a corpse entrance moved, exported and read back: only those fields differ,
    /// and the mod-dbc-patch files list one add and the modifies.
    int PoisCheck()
    {
        // `--poi-check <Data>`: AreaPOI.dbc read, what its client rows hold, and an edit + add round trip through export.
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        char buf[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, buf, sizeof buf, nullptr, nullptr);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-66s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        MpqChain chain;
        chain.Open(std::string(buf));
        ChangeStore store;
        AreaPoiAdapter pois(chain, store);
        store.Register(pois);

        const auto rows = pois.Rows();
        std::map<uint32_t, size_t> icons, importance, flags, maps;
        size_t mixed = 0, states = 0, links = 0, named = 0;
        for (const auto& [id, row] : rows)
        {
            const Poi p = Poi::FromDbcRow(row);
            ++icons[p.icon];
            ++importance[p.importance];
            ++flags[p.flags];
            ++maps[p.map];
            states += p.worldState != 0;
            links += row.value("WorldMapLink", 0u) != 0;
            named += !p.name.empty();
            for (int i = 1; i < 9; ++i) mixed += row.value("Icon[" + std::to_string(i) + "]", 0u) != p.icon ? 1 : 0;
        }
        auto dump = [](const char* what, const std::map<uint32_t, size_t>& m) {
            printf("  %s:", what);
            for (const auto& [k, n] : m) printf(" %u:%zu", k, n);
            printf("\n");
        };
        printf("  %zu rows, ids %u-%u, %zu named, %zu with a world state, %zu with a map link, %zu Icon[1-8] slots differ from Icon[0]\n", rows.size(),
               rows.empty() ? 0 : rows.begin()->first, rows.empty() ? 0 : rows.rbegin()->first, named, states, links, mixed);
        dump("Icon[0]", icons);
        dump("Importance", importance);
        dump("Flags", flags);
        dump("maps", maps);
        int shown = 0;
        for (const auto& [id, row] : rows)
            if (!row.value("WorldStateID", 0u) && shown++ < 8)
            {
                printf("  #%u %s pos %.1f %.1f %.1f area %u ws %u imp %u flags %u icons", id, row.value("Name_lang", std::string()).c_str(), row.value("Pos[0]", 0.0f), row.value("Pos[1]", 0.0f), row.value("Pos[2]", 0.0f), row.value("AreaID", 0u), row.value("WorldStateID", 0u),
                       row.value("Importance", 0u), row.value("Flags", 0u));
                for (int i = 0; i < 9; ++i) printf(" %u", row.value("Icon[" + std::to_string(i) + "]", 0u));
                printf("\n");
            }
        for (const auto& [id, row] : rows)
            if (row.value("WorldStateID", 0u))
            {
                printf("  e.g. #%u %s ws %u icons", id, row.value("Name_lang", std::string()).c_str(), row.value("WorldStateID", 0u));
                for (int i = 0; i < 9; ++i) printf(" %u", row.value("Icon[" + std::to_string(i) + "]", 0u));
                printf("\n");
                break;
            }
        expect(rows.size() > 50 && named * 10 >= rows.size() * 9, "AreaPOI.dbc read, rows named");

        const nlohmann::json first = rows.begin()->second;
        Poi moved = Poi::FromDbcRow(first);
        moved.x += 10;
        moved.name = "Moved landmark";
        pois.Commit(moved.id, moved.ToDbcRow(first), "move");
        Poi added = moved;
        added.id = 60000;
        added.name = "New landmark";
        added.description = "A test";
        pois.Commit(added.id, added.ToDbcRow(pois.NewRow()), "add");
        expect(Poi::FromDbcRow(pois.Row(60000)).description == "A test" && pois.Row(60000)["Name_lang_flags"] == first["Name_lang_flags"],
               "added row: fields and the client's string flags");

        const fs::path t = fs::temp_directory_path() / "wow-world-editor-poicheck";
        std::error_code ec;
        fs::remove_all(t, ec);
        std::string error;
        expect(pois.Export({ t / "dbc" }, t / "patch", error), "exported " + error);
        Dbc a, b;
        std::ifstream f(t / "dbc" / "AreaPOI.dbc", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
        if (a.Load(*chain.Read("DBFilesClient\\AreaPOI.dbc")) && b.Load(bytes))
        {
            size_t differ = 0;
            for (uint32_t r = 0; r < a.Rows(); ++r)
                if (const auto rb = b.Find(a.U32(r, 0)))
                    for (uint32_t c = 0; c < 54; ++c)
                        if (a.U32(r, c) != b.U32(*rb, c) && !(a.U32(r, 0) == moved.id && (c == 12 || c == 18)) && ++differ < 5) printf("  differs: #%u field %u: %u -> %u\n", a.U32(r, 0), c, a.U32(r, c), b.U32(*rb, c));
            const auto row = b.Find(60000);
            expect(b.Rows() == a.Rows() + 1 && differ == 0 && row && b.Str(*row, 35) == "A test" && b.Str(*b.Find(moved.id), 18) == "Moved landmark",
                   "AreaPOI.dbc: one row added, only x and name of the moved one differ");
        }
        else expect(false, "AreaPOI.dbc read back");
        fs::remove_all(t, ec);

        // `--poi-check <Data> <AC server dir>`: game_tele and points_of_interest rows written, read back, undone.
        if (__argc >= 4)
        {
            std::string password, note;
            const auto profile = ServerProfile::FromWorldserverConf(fs::path(__wargv[3]), password, note);
            Db db;
            if (!profile || !db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error))
            {
                printf("db: %s%s\n", note.c_str(), error.c_str());
                return 1;
            }
            TableRowsAdapter teles(store, "game_tele", "id"), gossip(store, "points_of_interest", "ID");
            store.Register(teles);
            store.Register(gossip);
            teles.SetDb(&db);
            gossip.SetDb(&db);
            for (const PoiKind kind : { PoiKind::Tele, PoiKind::Gossip })
            {
                TableRowsAdapter& table = kind == PoiKind::Tele ? teles : gossip;
                const std::string name = kind == PoiKind::Tele ? "game_tele" : "points_of_interest";
                const auto taken = db.Query("SELECT COUNT(*) FROM " + name + " WHERE " + (kind == PoiKind::Tele ? "id" : "ID") + " = 60999", error);
                if (!taken || (*taken)[0][0] != "0") { expect(false, name + " id 60999 is free for the test"); continue; }
                Poi p;
                p.kind = kind;
                p.id = 60999;
                p.map = 0;
                p.x = -8913.5f; p.y = -136.25f; p.z = 82.5f; p.o = 1.5f;
                p.name = "WweCheck";
                p.icon = 7; p.flags = 99;
                const nlohmann::json row = kind == PoiKind::Tele ? p.ToTeleRow() : p.ToGossipRow();
                Change c = table.MakeChange(p.id, {}, { row }, "add");
                table.Apply(c);
                store.Commit(std::move(c));
                const auto back = table.Rows(p.id);
                table.SetDb(nullptr);   // Rows from the database, not the project
                table.SetDb(&db);
                const auto rows = db.QueryRows("SELECT * FROM " + name + " WHERE " + (kind == PoiKind::Tele ? "id" : "ID") + " = 60999", error);
                const bool same = rows && rows->size() == 1 &&
                                  (kind == PoiKind::Tele ? Poi::FromTeleRow((*rows)[0]).ToTeleRow() == row : Poi::FromGossipRow((*rows)[0]).ToGossipRow() == row);
                expect(back.size() == 1 && same && table.LastError().empty(), name + ": added row reads back the same " + table.LastError());
                store.Undo();
                const auto gone = db.Query("SELECT COUNT(*) FROM " + name + " WHERE " + (kind == PoiKind::Tele ? "id" : "ID") + " = 60999", error);
                expect(gone && (*gone)[0][0] == "0", name + ": undo removes it");
            }
        }
        printf("poi check: %d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    int TaxiCheck()
    {
        // `--taxi-check <Data>`: the taxi DBCs read, what Blizzard's rows hold (ids, mounts), a planned path, and a node +
        // path + points added and moved, exported, read back.
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        char buf[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, buf, sizeof buf, nullptr, nullptr);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-66s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        MpqChain chain;
        chain.Open(std::string(buf));
        ChangeStore store;
        TaxiNodesAdapter nodes(chain, store);
        TaxiPathAdapter paths(chain, store);
        TaxiPathNodeAdapter points(chain, store);
        store.Register(nodes);
        store.Register(paths);
        store.Register(points);

        const auto all = nodes.All();
        std::map<std::pair<uint32_t, uint32_t>, size_t> mounts;
        uint32_t maxNode = 0, free = 0;
        std::set<uint32_t> used;
        for (const TaxiNode& n : all) { used.insert(n.id); maxNode = std::max(maxNode, n.id); ++mounts[{ n.mount[0], n.mount[1] }]; }
        for (uint32_t i = 1; i <= kTaxiMaxNode; ++i) free += !used.count(i);
        printf("  %zu nodes, max id %u, %u free ids <= %u\n  mounts (horde, alliance):", all.size(), maxNode, free, kTaxiMaxNode);
        for (const auto& [m, count] : mounts) if (count > 4) printf(" %u/%u x%zu", m.first, m.second, count);
        printf("\n");
        for (const TaxiNode& n : all)
            if (n.id == 2 || n.id == 23) printf("  node %u %s map %u (%.1f %.1f %.1f) mounts %u/%u\n", n.id, n.name.c_str(), n.map, n.x, n.y, n.z, n.mount[0], n.mount[1]);
        const auto allPaths = paths.All();
        const auto& byPath = points.ByPath();
        uint32_t maxPath = 0, maxPoint = 0;
        size_t pointCount = 0, maxLen = 0;
        for (const TaxiPath& p : allPaths) maxPath = std::max(maxPath, p.id);
        for (const auto& [path, list] : byPath) { pointCount += list.size(); maxLen = std::max(maxLen, list.size()); for (const TaxiPoint& p : list) maxPoint = std::max(maxPoint, p.id); }
        printf("  %zu paths (max id %u), %zu points (max id %u), longest path %zu points\n", allPaths.size(), maxPath, pointCount, maxPoint, maxLen);
        expect(all.size() > 100 && allPaths.size() > 500 && pointCount > 5000, "taxi DBCs read");
        size_t starts = 0, ends = 0, checked = 0;   // do Blizzard's paths start and end on their nodes?
        std::map<uint32_t, TaxiNode> byId;
        for (const TaxiNode& n : all) byId[n.id] = n;
        for (const TaxiPath& p : allPaths)
            if (const auto it = byPath.find(p.id); it != byPath.end() && byId.count(p.from) && byId.count(p.to) && it->second.size() > 1)
            {
                ++checked;
                const TaxiPoint &a = it->second.front(), &b = it->second.back();
                starts += std::hypot(a.x - byId[p.from].x, a.y - byId[p.from].y) < 30;
                ends += std::hypot(b.x - byId[p.to].x, b.y - byId[p.to].y) < 30;
            }
        printf("  of %zu paths: %zu start within 30 yd of their node, %zu end within 30 yd of theirs\n", checked, starts, ends);
        if (getenv("WWE_TAXI_SHAPES"))   // how Blizzard's flights leave a node: horizontal distance / height above it, per point
            for (const TaxiPath& p : allPaths)
                if (p.from == 2 || p.from == 23)
                    if (const auto it = byPath.find(p.id); it != byPath.end())
                    {
                        printf("  path %u from %u:", p.id, p.from);
                        for (size_t i = 0; i < std::min<size_t>(10, it->second.size()); ++i)
                            printf(" %.0f/%.0f", std::hypot(it->second[i].x - byId[p.from].x, it->second[i].y - byId[p.from].y), it->second[i].z - byId[p.from].z);
                        printf("\n");
                    }

        // Planned paths: as few points as will do, never under what they must clear (synthetic ground, 40 yd clearance).
        const TaxiNode a{ 1, 0, 0, 0, 0 };
        auto flat = [](float, float) { return std::optional<float>(0.0f); };
        // Every leg, sampled, over max(floor + 5 (less while climbing off an end), min(floor + 40, the climb off either end)).
        auto clears = [](const std::vector<TaxiPoint>& route, const std::function<std::optional<float>(float, float)>& floor) {
            const float total = std::hypot(route.back().x - route.front().x, route.back().y - route.front().y);
            for (size_t i = 0; i + 1 < route.size(); ++i)
                for (int k = 0; k <= 40; ++k)
                {
                    const float t = k / 40.0f, x = route[i].x + (route[i + 1].x - route[i].x) * t, z = route[i].z + (route[i + 1].z - route[i].z) * t;
                    const float d = x - route.front().x, under = *floor(x, 0);
                    if (d < 1 || d > total - 1) continue;   // the nodes themselves stand where they stand
                    const float climb = std::min(route.front().z + 0.5f * d, route.back().z + 0.5f * (total - d));
                    if (const float want = std::max(under + std::min(5.0f, 0.5f * std::min(d, total - d)), std::min(under + 40, climb)); z < want - 0.01f)
                    {
                        if (getenv("WWE_TAXI_SHAPES")) printf("    under at x %.1f: %.2f < %.2f (leg %zu of %zu)\n", x, z, want, i, route.size());
                        return false;
                    }
                }
            return true;
        };
        {
            const TaxiNode b{ 2, 0, 300, 0, 0 };
            const auto plan = PlanTaxiPoints(a, b, 40, flat);
            expect(plan.size() == 4 && plan.front().z == 0 && plan.back().x == 300 && plan.back().index == 3 && clears(plan, flat),
                   "flat ground: climb, one level leg, descend (" + std::to_string(plan.size()) + " points)");
            // High nodes over a valley: nothing to clear, one straight leg.
            const TaxiNode high{ 1, 0, 0, 0, 100 }, high2{ 2, 0, 500, 0, 100 };
            expect(PlanTaxiPoints(high, high2, 40, flat).size() == 2, "nothing in the way: a single straight leg");
        }
        {
            // A 300 yd tall building between x 140 and 160: cleared on every leg, still few points.
            auto floor = [](float x, float) { return std::optional<float>(x >= 140 && x <= 160 ? 300.0f : 0.0f); };
            const TaxiNode distant{ 2, 0, 600, 0, 0 };
            const auto over = PlanTaxiPoints(a, distant, 40, floor);
            expect(clears(over, floor) && over.size() <= 5, "a building in the way is cleared (" + std::to_string(over.size()) + " points)");
            // A rounded hill 150 yd high: cleared with a handful of points, not one per sample.
            auto hill = [](float x, float) { return std::optional<float>(150.0f * std::exp(-(x - 300) * (x - 300) / (2 * 80.0f * 80.0f))); };
            const auto across = PlanTaxiPoints(a, distant, 40, hill);
            expect(clears(across, hill) && across.size() <= 6, "a rounded hill is cleared with few points (" + std::to_string(across.size()) + ")");
            // Copied takeoff and landing are kept, less their straight-through points; the middle joins them.
            std::vector<TaxiPoint> takeoff(4), landing(2);
            takeoff[1].x = 30; takeoff[1].y = 20; takeoff[1].z = 10;   // a bend: kept
            takeoff[2].x = 65; takeoff[2].y = 10; takeoff[2].z = 30;   // halfway along a straight leg: dropped
            takeoff[3].x = 100; takeoff[3].z = 50;
            landing[0].x = 520; landing[0].z = 60; landing[1].x = 600;
            const auto joined = PlanTaxiPoints(a, distant, 40, floor, takeoff, landing);
            expect(joined[1].y == 20 && joined[2].x == 100 && joined[2].z == 50 && joined[joined.size() - 2].x == 520 && joined.back().x == 600 &&
                       std::none_of(joined.begin(), joined.end(), [](const TaxiPoint& p) { return p.x == 65; }) && joined.back().index == joined.size() - 1,
                   "copied takeoff and landing kept (straight-through points dropped), middle between them");
            // A real departure: Blizzard's points up to the first one 200 yd out.
            if (const auto it = byPath.find(6); it != byPath.end())
            {
                const auto dep = TaxiDeparture(it->second, 200);
                const float last = std::hypot(dep.back().x - dep.front().x, dep.back().y - dep.front().y);
                const float before = std::hypot(dep[dep.size() - 2].x - dep.front().x, dep[dep.size() - 2].y - dep.front().y);
                expect(dep.size() >= 3 && last >= 200 && before < 200, "departure of path 6 (Stormwind): " + std::to_string(dep.size()) + " points to 200 yd out");
            }
        }

        TaxiNode added{ 448, 0, -9000, 400, 60, "Editor Test Field" };
        added.mount[1] = all.front().mount[1];
        nodes.Commit(added.id, added.ToRow(nodes.NewRow()), "add node");
        TaxiNode moved = all.front();
        moved.x += 3;
        nodes.Commit(moved.id, moved.ToRow(nodes.Row(moved.id)), "move node");
        const TaxiPath newPath{ 9000, added.id, moved.id, 150 };
        paths.Commit(newPath.id, newPath.ToRow(nlohmann::json::object()), "add path");
        uint32_t pid = 90000;
        for (TaxiPoint p : PlanTaxiPoints(added, moved, 40, [](float, float) { return std::nullopt; }))
        {
            p.id = pid++;
            p.path = newPath.id;
            points.Commit(p.id, p.ToRow(nlohmann::json::object()), "add point");
        }
        expect(points.ByPath().at(newPath.id).size() >= 2, "added path's points read back in order");
        const fs::path t = fs::temp_directory_path() / "wow-world-editor-taxicheck";
        std::error_code ec;
        fs::remove_all(t, ec);
        std::string error;
        expect(nodes.Export({ t / "dbc" }, t / "patch", error) && paths.Export({ t / "dbc" }, t / "patch", error) && points.Export({ t / "dbc" }, t / "patch", error),
               "exported " + error);
        auto readBack = [&](const std::string& name) {
            Dbc d;
            std::ifstream f(t / "dbc" / (name + ".dbc"), std::ios::binary);
            d.Load(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}));
            return d;
        };
        const Dbc n = readBack("TaxiNodes"), p = readBack("TaxiPath"), q = readBack("TaxiPathNode");
        expect(n.Find(448) && n.Str(*n.Find(448), 5) == "Editor Test Field" && std::fabs(n.F32(*n.Find(moved.id), 2) - moved.x) < 1e-3f, "TaxiNodes.dbc: added node named, moved one moved");
        expect(p.Find(9000) && p.U32(*p.Find(9000), 1) == 448 && p.U32(*p.Find(9000), 3) == 150, "TaxiPath.dbc: added path from the new node, its cost");
        expect(q.Find(90000) && q.U32(*q.Find(90000), 1) == 9000 && q.Rows() == uint32_t(pointCount + (pid - 90000)), "TaxiPathNode.dbc: added points, every old one kept");
        fs::remove_all(t, ec);
        printf("taxi check: %d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    int PoisRead()
    {
        // `--poi-read <Data> <map folder>`: the landmarks another client (any build) has on a map, as copies pick them up.
        if (__argc < 4 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char buf[1024] = {}, dir[256] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, buf, sizeof buf, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[3], -1, dir, sizeof dir, nullptr, nullptr);
        MpqChain chain;
        chain.Open(std::string(buf));
        const auto mapDbc = chain.Read("DBFilesClient\\Map.dbc");
        const auto poiDbc = chain.Read("DBFilesClient\\AreaPOI.dbc");
        Dbc d;
        printf("AreaPOI.dbc: %s, %u fields\n", poiDbc ? "found" : "missing", poiDbc && d.Load(*poiDbc) ? d.Fields() : 0);
        std::optional<uint32_t> map;
        if (mapDbc)
            for (const MapEntry& m : ParseMapDbc(*mapDbc))
                if (_stricmp(m.directory.c_str(), dir) == 0) map = m.id;
        if (!map) { printf("no map %s in Map.dbc\n", dir); return 1; }
        const auto pois = ReadAreaPois(chain, *map);
        printf("map %s = %u: %zu landmark(s)\n", dir, *map, pois.size());
        for (size_t i = 0; i < pois.size(); i += std::max<size_t>(1, pois.size() / 12))
        {
            const Poi& p = pois[i];
            printf("  #%u %-28s %9.1f %9.1f %7.1f icon %u imp %u flags %u area %u ws %u  %s\n", p.id, p.name.c_str(), p.x, p.y, p.z, p.icon,
                   p.importance, p.flags, p.area, p.worldState, p.description.c_str());
        }
        return pois.empty() ? 1 : 0;
    }

    int TriggersCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        char buf[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, buf, sizeof buf, nullptr, nullptr);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("  %-66s %s\n", what.c_str(), ok ? "ok" : "FAILED"); problems += !ok; };
        MpqChain chain;
        chain.Open(std::string(buf));
        ChangeStore store;
        AreaTriggerAdapter triggers(chain, store);
        MapRowsAdapter maps(chain, store);
        store.Register(triggers);
        store.Register(maps);

        const auto rows = triggers.Rows();
        size_t shaped = 0;
        for (const auto& [id, row] : rows)
        {
            const Trigger t = Trigger::FromRow(row);
            shaped += t.Sphere() || (t.length > 0 && t.width > 0 && t.height > 0);
        }
        expect(rows.size() > 1000, "AreaTrigger.dbc read: " + std::to_string(rows.size()) + " triggers");
        expect(shaped * 100 >= rows.size() * 99, std::to_string(shaped) + " have a sphere or box size");
        expect(triggers.OnMap(0).size() > 50 && triggers.OnMap(1).size() > 50, "Eastern Kingdoms and Kalimdor have triggers");
        const auto mapRows = maps.Rows();
        uint32_t corpseMap = 0;
        for (const auto& [id, row] : mapRows)
            if (row.value("CorpseMapID", 0x80000000u) < 0x80000000u && row.value("InstanceType", 0u) == 1) { corpseMap = id; break; }
        expect(mapRows.size() > 100 && corpseMap, "Map.dbc read: " + std::to_string(mapRows.size()) + " maps, dungeon " + std::to_string(corpseMap) + " has a corpse entrance");

        const nlohmann::json firstRow = rows.begin()->second;
        Trigger moved = Trigger::FromRow(firstRow);
        moved.x += 10;
        moved.yaw = 1.0f;
        triggers.Commit(moved.id, moved.ToDbcRow(firstRow), "move");
        const Trigger added{ 60000, 0, 1, 2, 3, 0, 10, 4, 6, 0.5f };
        triggers.Commit(added.id, added.ToDbcRow(), "add");
        nlohmann::json corpse = maps.Row(corpseMap);
        corpse["Corpse[0]"] = corpse["Corpse[0]"].get<float>() + 5;
        maps.Commit(corpseMap, corpse, "corpse");

        const fs::path t = fs::temp_directory_path() / "wow-world-editor-triggerscheck";
        std::error_code ec;
        fs::remove_all(t, ec);
        std::string error;
        expect(triggers.Export({ t / "dbc" }, t / "patch", error) && maps.Export({ t / "dbc" }, t / "patch", error), "exported " + error);
        auto readBack = [&](const std::string& name, const std::set<std::pair<uint32_t, uint32_t>>& changed, uint32_t fields, int extra) {
            Dbc a, b;
            std::ifstream f(t / "dbc" / (name + ".dbc"), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
            if (!a.Load(*chain.Read("DBFilesClient\\" + name + ".dbc")) || !b.Load(bytes)) return false;
            if (b.Rows() != a.Rows() + extra) return false;
            size_t differ = 0;
            for (uint32_t r = 0; r < a.Rows(); ++r)
            {
                const auto rb = b.Find(a.U32(r, 0));
                if (!rb) return false;
                for (uint32_t c = 0; c < fields; ++c)
                    if (a.U32(r, c) != b.U32(*rb, c) && !changed.count({ a.U32(r, 0), c })) ++differ;
            }
            return differ == 0;
        };
        expect(readBack("AreaTrigger", { { moved.id, 2 }, { moved.id, 9 } }, 10, 1), "AreaTrigger.dbc: one row added, only x and yaw of the moved one differ");
        expect(readBack("Map", { { corpseMap, 60 } }, 66, 0), "Map.dbc: only the corpse x of one map differs");
        {
            Dbc b;
            std::ifstream f(t / "dbc" / "AreaTrigger.dbc", std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
            const auto r = b.Load(bytes) ? b.Find(60000) : std::nullopt;
            expect(r && Trigger::FromRow(triggers.Row(60000)).length == 10 && b.F32(*r, 6) == 10 && b.F32(*r, 9) == 0.5f && b.U32(*r, 1) == 0,
                   "the added trigger reads back");
        }
        std::ifstream pj(t / "patch" / "AreaTrigger.json");
        const nlohmann::json patch = nlohmann::json::parse(pj, nullptr, false);
        expect(!patch.is_discarded() && patch["add"].size() == 1 && patch["modify"].size() == 1 && patch["modify"][0].size() == 3,
               "mod-dbc-patch: 1 add, 1 modify of ID + 2 fields");
        fs::remove_all(t, ec);
        printf(problems ? "%d problem(s)\n" : "triggers check passed\n", problems);
        return problems ? 1 : 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int)
{
    if (cmdLine && wcsstr(cmdLine, L"--model-check")) return ModelCheck();
    if (cmdLine && wcsstr(cmdLine, L"--render")) return RenderCheck();
    if (cmdLine && wcsstr(cmdLine, L"--catalog-check")) return CatalogCheck();
    if (cmdLine && wcsstr(cmdLine, L"--skin-check")) return SkinCheck();
    if (cmdLine && wcsstr(cmdLine, L"--appearance-check")) return AppearanceCheck();
    if (cmdLine && wcsstr(cmdLine, L"--ghost-check")) return GhostCheck();
    if (cmdLine && wcsstr(cmdLine, L"--compare-check")) return CompareCheck();
    if (cmdLine && wcsstr(cmdLine, L"--water-check")) return WaterCheck();
    if (cmdLine && wcsstr(cmdLine, L"--diff-check")) return DiffCheck();
    if (cmdLine && wcsstr(cmdLine, L"--tiles-check")) return TilesCheck();
    if (cmdLine && wcsstr(cmdLine, L"--minimap-check")) return MinimapCheck();
    if (cmdLine && wcsstr(cmdLine, L"--mpq-check")) return MpqCheck();
    if (cmdLine && wcsstr(cmdLine, L"--sources-check")) return SourcesCheck();
    if (cmdLine && wcsstr(cmdLine, L"--scan-check")) return ScanCheck();
    if (cmdLine && wcsstr(cmdLine, L"--triggers-check")) return TriggersCheck();
    if (cmdLine && wcsstr(cmdLine, L"--poi-check")) return PoisCheck();
    if (cmdLine && wcsstr(cmdLine, L"--poi-read")) return PoisRead();
    if (cmdLine && wcsstr(cmdLine, L"--taxi-check")) return TaxiCheck();
    if (cmdLine && wcsstr(cmdLine, L"--diff-objects")) return DiffObjects();
    if (cmdLine && wcsstr(cmdLine, L"--blueprint-check")) return BlueprintCheck();
    if (cmdLine && wcsstr(cmdLine, L"--asset-check")) return AssetCheck();
    if (cmdLine && wcsstr(cmdLine, L"--export") && __argc >= 3)
    {
        // `--export <project dir>`: what Ctrl+E does, without the window: tiles plus files from other clients into out/client.
        std::string error;
        const auto project = Project::Load(std::filesystem::path(__wargv[2]), error);
        if (!project) { printf("%s\n", error.c_str()); return 1; }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(project->base.layers);
        Ghosts ghosts;
        std::vector<std::string> errors;
        std::vector<std::pair<std::string, std::vector<MpqLayer>>> compare;
        for (const Project::Source& s : project->compare) compare.push_back({ s.name, s.layers });
        ghosts.Reset(&mpq, project->name, compare, errors);
        std::vector<const MpqChain*> fallbacks;
        for (size_t i = 1; i < ghosts.Sources().size(); ++i) fallbacks.push_back(ghosts.Sources()[i].mpq);
        mpq.SetFallbacks(fallbacks);
        ChangeStore store;
        if (!store.Load(project->ChangesDir(), error)) { printf("%s\n", error.c_str()); return 1; }
        TerrainAdapter terrain(mpq, renderer, store);
        std::vector<std::filesystem::path> tiles;
        std::vector<Problem> problems;
        const size_t written = terrain.Export(project->ClientOutDir(), error, &tiles, &problems);
        terrain.FindCracks(problems);
        if (!error.empty()) { printf("export failed: %s\n", error.c_str()); return 1; }
        for (const Problem& p : problems)
            printf("%s %s %d_%d: %s\n", p.severity == Problem::Severity::Error ? "error" : "warning", p.map.c_str(), p.tx, p.ty, p.message.c_str());
        const AssetReport assets = CopyMissingAssets(mpq, tiles, project->ClientOutDir());
        printf("exported %zu tile(s) to %s; %zu ground effect(s) dropped; %zu file(s) from other clients; %zu missing\n", written,
               project->ClientOutDir().string().c_str(), terrain.TakeDroppedEffects(), assets.copied.size(), assets.missing.size());
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--map-preview") && __argc >= 5)
    {
        // `--map-preview <Data> <map> <out.png>`: the Maps panel picture of a map (16 px per tile) with its tile grid.
        MpqChain mpq;
        mpq.Open(std::filesystem::path(__wargv[2]).string());
        const std::string map = std::filesystem::path(__wargv[3]).string();
        const auto wdl = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdl");
        if (!wdl) { printf("no WDL for %s\n", map.c_str()); return 1; }
        const auto tiles = ParseWdl(*wdl);
        constexpr int px = 16, side = 64 * px;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> image = MapPreview(tiles, px);
        printf("preview built in %.1f ms\n", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
        size_t land = 0;
        for (int i = 0; i < side * side; ++i)
        {
            uint8_t* p = &image[size_t(i) * 4];
            const int x = i % side, y = i / side;
            if (p[3] && (x % px == 0 || y % px == 0)) { p[0] = 190; p[1] = 140; p[2] = 100; }   // grid, as the panel draws it
            if (!p[3]) { p[0] = p[1] = p[2] = 244; p[3] = 255; }                                 // no terrain: light background
            else land += p[1] > p[2] && p[1] > 100;
            std::swap(p[0], p[2]);   // RGBA -> BGRA
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool saved = SavePng(__wargv[4], side, side, std::move(image));
        printf("%s: %zu tiles, %zu land pixels, png %s\n", map.c_str(), size_t(std::count_if(tiles.begin(), tiles.end(), [](const auto& t) { return !t.empty(); })),
               land, saved ? "saved" : "FAILED");
        return saved ? 0 : 1;
    }
    if (cmdLine && wcsstr(cmdLine, L"--where") && __argc >= 5)
    {
        // `--where <Data> <map> <text>`: every doodad / WMO placement on the map whose model path contains <text>
        // (case-insensitive), as tile, placement position and the --render arguments that look at it.
        MpqChain mpq;
        mpq.Open(std::filesystem::path(__wargv[2]).string());
        const std::string map = std::filesystem::path(__wargv[3]).string();
        std::string want = std::filesystem::path(__wargv[4]).string();
        for (char& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
        const auto wdt = mpq.Read("World\\Maps\\" + map + "\\" + map + ".wdt");
        if (!wdt) { printf("no WDT for %s\n", map.c_str()); return 1; }
        const bool big = WdtBigAlpha(*wdt);
        const auto present = WdtTiles(*wdt);
        size_t found = 0;
        for (int i = 0; i < 4096 && found < 2000; ++i)
        {
            if (!present[size_t(i)]) continue;
            const int tx = i % 64, ty = i / 64;
            const auto bytes = mpq.Read("World\\Maps\\" + map + "\\" + map + "_" + std::to_string(tx) + "_" + std::to_string(ty) + ".adt");
            const auto adt = bytes ? ParseAdt(*bytes, big) : std::nullopt;
            if (!adt) continue;
            auto report = [&](const std::string& model, const float* pos) {
                std::string lower = model;
                for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (lower.find(want) == std::string::npos) return;
                printf("%s %d_%d at %.1f %.1f %.1f  (--render ... %s %d %d out.png 0 -15 10 %.3f %.3f)\n", model.c_str(), tx, ty, pos[0], pos[1], pos[2],
                       map.c_str(), tx, ty, pos[0] / kTileSize - tx, (pos[2] + 30) / kTileSize - ty);
                ++found;
            };
            for (const auto& d : adt->doodads) report(d.model, d.pos);
            for (const auto& w : adt->wmos) report(w.model, w.pos);
        }
        printf("%zu placement(s)\n", found);
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--anim-check") && __argc >= 4)
    {
        // `--anim-check <Data> <model.m2> [more models]`: bones, the Stand animation, and how far skinned vertices
        // move between 0 and 0.25 / 0.5 s (0 = still). Fails when a model with keys never moves or flies apart.
        MpqChain mpq;
        mpq.Open(std::filesystem::path(__wargv[2]).string());
        int bad = 0;
        for (int i = 3; i < __argc; ++i)
        {
            const std::string name = std::filesystem::path(__wargv[i]).string();
            const auto mesh = LoadM2(M2Name(name), [&mpq](const std::string& path) { return mpq.Read(path); });
            if (!mesh) { printf("%s: cannot load\n", name.c_str()); ++bad; continue; }
            if (!mesh->skeleton) { printf("%s: no skeleton\n", name.c_str()); continue; }
            const ModelSkeleton& s = *mesh->skeleton;
            auto skinned = [&](uint32_t t) {
                std::vector<XMFLOAT4X4> palette;
                PoseBones(s, t, palette);
                std::vector<XMFLOAT3> out;
                for (const ModelVertex& v : mesh->vertices)
                {
                    XMVECTOR p = XMVectorZero();
                    for (int k = 0; k < 4; ++k)
                        if (v.weights[k]) p += XMVector3TransformCoord(XMLoadFloat3(&v.pos), XMLoadFloat4x4(&palette[v.bones[k]])) * (v.weights[k] / 255.0f);
                    XMFLOAT3 f;
                    XMStoreFloat3(&f, p);
                    out.push_back(f);
                }
                return out;
            };
            const auto a = skinned(0), b = skinned(250), c = skinned(500);
            float move = 0, fromBind = 0;
            for (size_t k = 0; k < a.size(); ++k)
            {
                move = std::max({ move, std::hypot(b[k].x - a[k].x, b[k].y - a[k].y, b[k].z - a[k].z), std::hypot(c[k].x - a[k].x, c[k].y - a[k].y, c[k].z - a[k].z) });
                const XMFLOAT3& p = mesh->vertices[k].pos;
                fromBind = std::max(fromBind, std::hypot(a[k].x - p.x, a[k].y - p.y, a[k].z - p.z));
            }
            const float size = std::hypot(mesh->boundsMax.x - mesh->boundsMin.x, mesh->boundsMax.y - mesh->boundsMin.y, mesh->boundsMax.z - mesh->boundsMin.z);
            bool boneKeys = false;   // texture-only animation (waterfalls) moves no vertex
            for (const auto& bone : s.bones) boneKeys |= bone.translation.times.size() > 1 || bone.rotation.times.size() > 1 || bone.scale.times.size() > 1;
            const bool ok = (!boneKeys || move > 0.0001f) && fromBind < size;
            printf("%s %s: %zu bones, anim %u ms, keys %s, moves %.3f, off bind pose %.3f (model %.2f), %zu attachments\n", ok ? "ok  " : "FAIL",
                   name.c_str(), s.bones.size(), s.duration, s.animated ? "yes" : "no", move, fromBind, size, s.attachments.size());
            // Texture animation: each batch's UV shift and opacity at 0 and 1 s.
            for (size_t k = 0; k < mesh->batches.size(); ++k)
            {
                const auto& b = mesh->batches[k];
                if (b.uvAnim < 0 && b.weight < 0 && b.color < 0) continue;
                const XMFLOAT4 u0 = UvTransform(s, b.uvAnim, 0), u1 = UvTransform(s, b.uvAnim, 1000);
                printf("     batch %zu (blend %d, %s): uv shift (%.3f, %.3f) -> (%.3f, %.3f) in 1 s, opacity %.2f -> %.2f\n", k, int(b.blend), b.texture.c_str(), u0.z, u0.w,
                       u1.z, u1.w, BatchAlpha(s, b.weight, b.color, 0), BatchAlpha(s, b.weight, b.color, 1000));
            }
            bad += !ok;
            // Every other animation (the NPC viewer's list): each loads with the same bones, poses inside the model,
            // and Walk (id 4) moves the model differently from Stand.
            {
                const FileReader read = [&mpq](const std::string& path) { return mpq.Read(path); };
                size_t external = 0, failed = 0;
                float walkDiff = -1;
                for (size_t q = 0; q < s.sequences.size(); ++q)
                {
                    const auto other = LoadSkeleton(M2Name(name), read, int(q));
                    external += !(s.sequences[q].flags & 0x20);
                    if (!other || other->bones.size() != s.bones.size()) { ++failed; continue; }
                    std::vector<XMFLOAT4X4> pa, pb;
                    PoseBones(s, 250, pa);
                    PoseBones(*other, 250, pb);
                    float diff = 0, reach = 0;
                    for (const ModelVertex& v : mesh->vertices)
                    {
                        const XMVECTOR p = XMLoadFloat3(&v.pos);
                        const XMVECTOR x = XMVector3TransformCoord(p, XMLoadFloat4x4(&pa[v.bones[0]])), y = XMVector3TransformCoord(p, XMLoadFloat4x4(&pb[v.bones[0]]));
                        diff = std::max(diff, XMVectorGetX(XMVector3Length(x - y)));
                        reach = std::max(reach, XMVectorGetX(XMVector3Length(y - p)));
                    }
                    if (reach > size * 4) ++failed;   // flew apart
                    if (s.sequences[q].id == 4 && walkDiff < 0) walkDiff = diff;
                }
                const bool seqOk = failed == 0 && (walkDiff < 0 || walkDiff > 0.0001f);
                printf("     %s %zu animations (%zu in .anim files), %zu failed, Walk vs Stand %.3f\n", seqOk ? "ok  " : "FAIL", s.sequences.size(), external,
                       failed, walkDiff);
                bad += !seqOk;
            }
            if (std::getenv("WWE_GEOSETS"))
            {
                std::set<std::pair<uint16_t, uint32_t>> sets;
                for (const auto& b : mesh->batches) sets.insert({ b.geoset, b.textureType });
                printf("     geosets (id:texture type):");
                for (const auto& [g, t] : sets) printf(" %u:%u", g, t);
                printf("\n");
            }
        }
        return bad ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--dialogue-check") && __argc >= 3)
    {
        // `--dialogue-check <AC server dir>`: the Dialogue tab's tables against the real world database, cleaning up
        // after itself: a new menu with a text, an option opening a submenu, conditions on both, and a bark, as one batch;
        // a condition of another kind under the same id must survive every write; then undo and confirm nothing is left.
        std::string password, note, error;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        auto count = [&](const std::string& sql) {
            const auto r = db.Query("SELECT COUNT(*) FROM " + sql, error);
            return r && !r->empty() ? std::stoi((*r)[0][0]) : -1;
        };
        ChangeStore store;
        TableRowsAdapter menus(store, "gossip_menu", "MenuID", "TextID"), options(store, "gossip_menu_option", "MenuID", "OptionID"), texts(store, "npc_text", "ID"),
            conditions(store, "conditions", "SourceGroup", "SourceEntry", "SourceTypeOrReferenceId IN (14, 15)", "gossip"),
            barks(store, "creature_text", "CreatureID", "GroupID");
        for (TableRowsAdapter* t : { &menus, &options, &texts, &conditions, &barks }) { store.Register(*t); t->SetDb(&db); }
        const uint32_t menu = menus.NextKey(9000000, 9099999).value_or(0), sub = menu + 1, text = texts.NextKey(9000000, 9099999).value_or(0), text2 = text + 1;
        const uint32_t creature = 9099999;   // no such template: creature_text has no foreign key
        if (!menu || !text) { printf("ranges full\n"); return 1; }
        const std::string m = std::to_string(menu), s = std::to_string(sub), t1 = std::to_string(text), t2 = std::to_string(text2);
        // A condition of another source type under the same SourceGroup: the gossip adapter must never touch it.
        db.Query("INSERT INTO conditions (SourceTypeOrReferenceId, SourceGroup, SourceEntry, SourceId, ElseGroup, ConditionTypeOrReference, ConditionTarget, "
                 "ConditionValue1, ConditionValue2, ConditionValue3, NegativeCondition, ErrorType, ErrorTextId, ScriptName, Comment) VALUES (1, " + m +
                 ", 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, '', 'dialogue-check bystander')", error);
        auto textRow = [&](const std::string& id, const std::string& body) {
            nlohmann::json row = nlohmann::json::object();
            if (const auto cols = db.Query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'npc_text'", error))
                for (const auto& c : *cols) row[c[0]] = c[0].rfind("text", 0) == 0 ? "" : "0";
            row["ID"] = id;
            row["text0_0"] = body;
            row["Probability0"] = "1";
            return row;
        };
        auto condition = [&](int source, const std::string& entry) {
            return nlohmann::json{ { "SourceTypeOrReferenceId", std::to_string(source) }, { "SourceGroup", m }, { "SourceEntry", entry }, { "SourceId", "0" },
                                   { "ElseGroup", "0" }, { "ConditionTypeOrReference", "9" }, { "ConditionTarget", "0" }, { "ConditionValue1", "783" },
                                   { "ConditionValue2", "0" }, { "ConditionValue3", "0" }, { "NegativeCondition", "0" }, { "ErrorType", "0" }, { "ErrorTextId", "0" },
                                   { "ScriptName", "" }, { "Comment", "dialogue-check" } };
        };
        std::vector<Change> parts;
        auto put = [&](TableRowsAdapter& table, uint32_t key, const std::vector<nlohmann::json>& rows) {
            Change c = table.MakeChange(key, table.Rows(key), rows, "check");
            table.Apply(c);
            if (!table.LastError().empty()) printf("     %s: %s\n", table.Table().c_str(), table.LastError().c_str());
            parts.push_back(std::move(c));
        };
        put(texts, text, { textRow(t1, "Greetings, $N.") });
        put(texts, text2, { textRow(t2, "Back again?") });
        put(menus, menu, { { { "MenuID", m }, { "TextID", t1 } } });
        put(menus, sub, { { { "MenuID", s }, { "TextID", t2 } } });
        put(options, menu, { { { "MenuID", m }, { "OptionID", "0" }, { "OptionIcon", "0" }, { "OptionText", "Tell me more." }, { "OptionBroadcastTextID", "0" },
                               { "OptionType", "1" }, { "OptionNpcFlag", "1" }, { "ActionMenuID", s }, { "ActionPoiID", "0" }, { "BoxCoded", "0" }, { "BoxMoney", "0" },
                               { "BoxText", "" }, { "BoxBroadcastTextID", "0" }, { "VerifiedBuild", "0" } } });
        put(conditions, menu, { condition(14, t1), condition(15, "0") });
        put(barks, creature, { { { "CreatureID", std::to_string(creature) }, { "GroupID", "0" }, { "ID", "0" }, { "Text", "For the Alliance!" }, { "Type", "14" },
                                 { "Language", "0" }, { "Probability", "100" }, { "Emote", "0" }, { "Duration", "0" }, { "Sound", "0" }, { "BroadcastTextId", "0" },
                                 { "TextRange", "0" }, { "comment", "dialogue-check" } } });
        store.Commit(std::move(parts), "dialogue");
        check(count("npc_text WHERE ID IN (" + t1 + ", " + t2 + ")") == 2, "texts written");
        check(count("gossip_menu WHERE MenuID IN (" + m + ", " + s + ")") == 2, "menu and submenu written");
        check(count("gossip_menu_option WHERE MenuID = " + m + " AND ActionMenuID = " + s) == 1, "option opening the submenu written");
        check(count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId IN (14, 15)") == 2, "conditions on the text and the option written");
        check(count("creature_text WHERE CreatureID = " + std::to_string(creature)) == 1, "bark written");
        check(count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId = 1") == 1, "the other kind of condition under the same id untouched");
        // Edit: drop the option's condition only.
        {
            std::vector<Change> edit;
            Change c = conditions.MakeChange(menu, conditions.Rows(menu), { condition(14, t1) }, "edit");
            conditions.Apply(c);
            edit.push_back(std::move(c));
            store.Commit(std::move(edit), "edit");
        }
        check(count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId = 15") == 0 &&
                  count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId = 14") == 1, "edit: the option's condition removed, the text's kept");
        store.Undo();
        check(count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId IN (14, 15)") == 2, "undo edit: both conditions back");
        store.Undo();
        check(count("npc_text WHERE ID IN (" + t1 + ", " + t2 + ")") == 0 && count("gossip_menu WHERE MenuID IN (" + m + ", " + s + ")") == 0 &&
                  count("gossip_menu_option WHERE MenuID = " + m) == 0 && count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId IN (14, 15)") == 0 &&
                  count("creature_text WHERE CreatureID = " + std::to_string(creature)) == 0,
              "undo: every row gone");
        check(count("conditions WHERE SourceGroup = " + m + " AND SourceTypeOrReferenceId = 1") == 1, "the other kind of condition still untouched");
        db.Query("DELETE FROM conditions WHERE SourceTypeOrReferenceId = 1 AND SourceGroup = " + m + " AND Comment = 'dialogue-check bystander'", error);
        printf("%s\n", problems ? "FAILED" : "all passed");
        return problems ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--npc-check") && __argc >= 3)
    {
        // `--npc-check <AC server dir> [entry]`: the NPC editor's tables against the real world database, cleaning up
        // after itself: copy a template with its models and equipment to a new entry (one batch), edit it, add an
        // equipment set, undo each step and confirm the database is as it was and the entry is never handed out again.
        std::string password, note, error;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); problems += !ok; };
        ChangeStore store;
        TableRowsAdapter templates(store, "creature_template", "entry"), models(store, "creature_template_model", "CreatureID", "Idx"),
            equips(store, "creature_equip_template", "CreatureID", "ID"), loot(store, "creature_loot_template", "Entry", "Item");
        TableRowsAdapter* tables[] = { &templates, &models, &equips, &loot };
        for (TableRowsAdapter* t : tables) { store.Register(*t); t->SetDb(&db); }
        const uint32_t source = __argc >= 4 ? uint32_t(_wtoi(__wargv[3])) : 823;   // Deputy Willem: a model and a weapon set
        auto count = [&](const char* table, const char* key, uint32_t id) {
            const auto r = db.Query(std::string("SELECT COUNT(*) FROM ") + table + " WHERE `" + key + "` = " + std::to_string(id), error);
            return r && !r->empty() ? std::stoi((*r)[0][0]) : -1;
        };
        const auto src = templates.Rows(source);
        if (src.empty()) { printf("no creature_template %u\n", source); return 1; }
        const auto id = templates.NextKey(9000000, 9099999);
        if (!id) { printf("range 9000000-9099999 is full\n"); return 1; }
        printf("copying %u (%s) to %u\n", source, src[0]["name"].get<std::string>().c_str(), *id);
        const std::string entry = std::to_string(*id);
        auto retarget = [&](std::vector<nlohmann::json> rows, const char* key) { for (auto& r : rows) r[key] = entry; return rows; };
        std::vector<Change> parts;
        const std::pair<TableRowsAdapter*, std::vector<nlohmann::json>> copies[] = {
            { &templates, retarget(src, "entry") }, { &models, retarget(models.Rows(source), "CreatureID") }, { &equips, retarget(equips.Rows(source), "CreatureID") },
            { &loot, retarget(loot.Rows(uint32_t(std::stoul(src[0].value("lootid", std::string("0"))))), "Entry") } };   // its own loot, references included
        for (const auto& [t, rows] : copies)
            if (!rows.empty())
            {
                Change c = t->MakeChange(*id, {}, rows, "copy");
                t->Apply(c);
                parts.push_back(std::move(c));
            }
        store.Commit(std::move(parts), "copy");
        check(count("creature_template", "entry", *id) == 1, "copy: template row written");
        check(count("creature_template_model", "CreatureID", *id) == count("creature_template_model", "CreatureID", source), "copy: every model row written");
        check(count("creature_equip_template", "CreatureID", *id) == count("creature_equip_template", "CreatureID", source), "copy: every equipment set written");
        const uint32_t sourceLoot = uint32_t(std::stoul(src[0].value("lootid", std::string("0"))));
        check(sourceLoot == 0 || count("creature_loot_template", "Entry", *id) == count("creature_loot_template", "Entry", sourceLoot),
              "copy: every loot row written (items and references)");
        for (TableRowsAdapter* t : tables) check(t->LastError().empty(), (t->Table() + ": no write error " + t->LastError()).c_str());

        // Edit the name and add an equipment set in one batch, like Apply.
        auto edited = templates.Rows(*id);
        edited[0]["name"] = "npc-check edit";
        auto sets = equips.Rows(*id);
        sets.push_back({ { "CreatureID", entry }, { "ID", "9" }, { "ItemID1", "1899" }, { "ItemID2", "0" }, { "ItemID3", "0" }, { "VerifiedBuild", "0" } });
        std::vector<Change> edit;
        for (auto [t, after] : { std::pair{ &templates, edited }, std::pair{ &equips, sets } })
        {
            Change c = t->MakeChange(*id, t->Rows(*id), after, "edit");
            t->Apply(c);
            edit.push_back(std::move(c));
        }
        store.Commit(std::move(edit), "edit");
        const auto name = db.Query("SELECT name FROM creature_template WHERE entry = " + entry, error);
        check(name && !name->empty() && (*name)[0][0] == "npc-check edit", "edit: name written");
        check(count("creature_equip_template", "CreatureID", *id) == count("creature_equip_template", "CreatureID", source) + 1, "edit: set added");
        store.Undo();
        const auto back = db.Query("SELECT name FROM creature_template WHERE entry = " + entry, error);
        check(back && !back->empty() && (*back)[0][0] == src[0]["name"].get<std::string>(), "undo edit: name restored");
        check(count("creature_equip_template", "CreatureID", *id) == count("creature_equip_template", "CreatureID", source), "undo edit: set removed");
        store.Undo();
        check(count("creature_template", "entry", *id) == 0 && count("creature_template_model", "CreatureID", *id) == 0 &&
                  count("creature_equip_template", "CreatureID", *id) == 0 && count("creature_loot_template", "Entry", *id) == 0, "undo copy: every row gone");
        const auto next = templates.NextKey(9000000, 9099999);
        check(next && *next > *id, "the undone entry is not handed out again (redo can bring it back)");
        printf("%s\n", problems ? "FAILED" : "all passed");
        return problems ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--spawn-check") && __argc >= 3)
    {
        // `--spawn-check <AC server dir>`: the creature and gameobject adapters against the real world database, cleaning up after
        // itself: place, move, undo, redo, save + reload + sync, export, edit an existing row and undo it (every
        // column must come back), then undo everything and confirm the database is as it was.
        std::string password, note, error;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); problems += !ok; };
        for (const SpawnKind kind : { SpawnKind::Creature, SpawnKind::GameObject })
        {
        const std::string table = kind == SpawnKind::Creature ? "creature" : "gameobject";
        printf("-- %s\n", table.c_str());
        auto row = [&](uint32_t guid) -> std::optional<nlohmann::json> {
            auto rows = db.QueryRows("SELECT * FROM " + table + " WHERE guid = " + std::to_string(guid), error);
            return rows && !rows->empty() ? std::optional((*rows)[0]) : std::nullopt;
        };
        const auto before = db.Query("SELECT COUNT(*), COALESCE(SUM(guid), 0) FROM " + table, error);

        ChangeStore store;
        SpawnAdapter spawns(store, kind);
        store.Register(spawns);
        spawns.SetDb(&db);
        {
            // Game event membership: Around reads game_event_<table>; InWorld follows the core (negative = gone during).
            Spawn e;
            e.events = { 12, -7 };
            check(e.InWorld(12) && !e.InWorld(0) && !e.InWorld(7) && !e.InWorld(3), "InWorld: only during event 12");
            e.events = { -7 };
            check(e.InWorld(0) && e.InWorld(3) && !e.InWorld(7), "InWorld: gone during event 7 only");
            if (const auto ev = db.QueryRows("SELECT c.guid, c.map, c.position_x AS x, c.position_y AS y, g.eventEntry FROM game_event_" + table +
                                             " g JOIN " + table + " c ON c.guid = g.guid LIMIT 1", error); ev && !ev->empty())
            {
                const auto& r = (*ev)[0];
                const float x = std::stof(r["x"].get<std::string>()), y = std::stof(r["y"].get<std::string>());
                const uint32_t guid = std::stoul(r["guid"].get<std::string>());
                bool found = false;
                for (const Spawn& s : spawns.Around(std::stoul(r["map"].get<std::string>()), x - 1, y - 1, x + 1, y + 1))
                    if (s.guid == guid)
                        found = std::find(s.events.begin(), s.events.end(), std::stoi(r["eventEntry"].get<std::string>())) != s.events.end();
                check(found, "Around: an event spawn carries its event");
            }
            // The whole-map list (Tools > On this map): every spawn of a dungeon map, as many as the database has.
            for (const uint32_t map : { 34u, 389u })   // the Stockade, Ragefire Chasm
                if (const auto n = db.Query("SELECT COUNT(*) FROM " + table + " WHERE map = " + std::to_string(map), error); n && !n->empty())
                    check(spawns.OnMap(map).size() == size_t(std::stoul((*n)[0][0])),
                          ("OnMap " + std::to_string(map) + ": " + std::to_string(spawns.OnMap(map).size()) + " of " + (*n)[0][0]).c_str());
        }
        auto commit = [&](const std::optional<nlohmann::json>& b, const std::optional<nlohmann::json>& a, const char* label) {
            Change c = spawns.MakeChange(b, a, label);
            spawns.Apply(c);
            store.Commit(std::move(c));
        };
        const char* search = kind == SpawnKind::Creature ? "Guard" : "Mailbox";
        const auto templates = spawns.Search(search, error);
        if (templates.empty()) { printf("no %s_template matching %s: %s\n", table.c_str(), search, error.c_str()); return 1; }
        Spawn s;
        s.kind = kind;
        s.guid = *spawns.NextGuid(9000000, 9099999);
        s.entry = templates[0].entry;
        s.map = 0;
        s.x = -8913.0f; s.y = -136.0f; s.z = 82.0f;   // Northshire Abbey
        commit(std::nullopt, s.ToRow(), "place");
        check(row(s.guid) && (*row(s.guid))["id"] == std::to_string(s.entry), "placed spawn is in the database");
        check(!spawns.NextGuid(s.guid, s.guid) && spawns.NextGuid(s.guid, s.guid + 1) == s.guid + 1, "id range: full range refuses, next id above the used one");
        {
            const auto use = spawns.Use(s.guid, s.guid);
            check(use.mine == 1 && use.others == 0 && use.highest >= s.guid, "id range use: the placed row is the project's own");
        }
        auto placed = *row(s.guid);
        Spawn moved = Spawn::FromRow(placed, kind);
        moved.x += 5;
        commit(placed, moved.ToRow(placed), "move");
        check(std::fabs(std::stof((*row(s.guid))["position_x"].get<std::string>()) - (s.x + 5)) < 0.01f, "moved spawn has its new position");
        store.Undo();
        check(std::fabs(std::stof((*row(s.guid))["position_x"].get<std::string>()) - s.x) < 0.01f, "undo move: old position");
        store.Undo();
        check(!row(s.guid), "undo place: row gone");
        store.Redo();
        store.Redo();
        check(row(s.guid) && std::fabs(std::stof((*row(s.guid))["position_x"].get<std::string>()) - (s.x + 5)) < 0.01f, "redo both: placed and moved");
        if (kind == SpawnKind::GameObject)
        {
            // Turning a gameobject writes a yaw quaternion; undo puts the old one back.
            const auto r0 = *row(s.guid);
            Spawn t = Spawn::FromRow(r0, kind);
            t.orientation = 1.0f;
            commit(r0, t.ToRow(r0), "turn");
            const auto r1 = *row(s.guid);
            check(std::fabs(std::stof(r1["rotation2"].get<std::string>()) - std::sin(0.5f)) < 1e-4f &&
                  std::fabs(std::stof(r1["rotation3"].get<std::string>()) - std::cos(0.5f)) < 1e-4f && r1["state"] == "1",
                  "turned gameobject: rotation2/3 = sin/cos(o/2), state ready");
            store.Undo();
            check(*row(s.guid) == r0, "undo turn: row as before");
            store.Redo();
        }

        {
            // A group: two more spawns, moved together in one change, then that one undo puts both back.
            Spawn a = s, b = s;
            a.guid = *spawns.NextGuid(9000000, 9099999);
            commit(std::nullopt, a.ToRow(), "place a");
            b.guid = *spawns.NextGuid(9000000, 9099999);
            commit(std::nullopt, b.ToRow(), "place b");
            const auto ra = *row(a.guid), rb = *row(b.guid);
            Spawn ma = Spawn::FromRow(ra, kind), mb = Spawn::FromRow(rb, kind);
            ma.y += 3;
            mb.y += 3;
            Change group = spawns.MakeChange({ { ra, ma.ToRow(ra) }, { rb, mb.ToRow(rb) } }, "move both");
            spawns.Apply(group);
            store.Commit(std::move(group));
            auto y = [&](uint32_t guid) { return std::stof((*row(guid))["position_y"].get<std::string>()); };
            check(std::fabs(y(a.guid) - (s.y + 3)) < 0.01f && std::fabs(y(b.guid) - (s.y + 3)) < 0.01f, "group move: both spawns moved by one change");
            store.Undo();
            check(*row(a.guid) == ra && *row(b.guid) == rb, "undo group move: both back");
            store.Redo();
        }

        TableRowsAdapter waypoints(store, "waypoint_data", "id", "point"), addons(store, "creature_addon", "guid");
        if (kind == SpawnKind::Creature)
        {
            // A waypoint path on the placed creature: three tables in one undo step, then removed and restored.
            store.Register(waypoints);
            store.Register(addons);
            waypoints.SetDb(&db);
            addons.SetDb(&db);
            const CreaturePath path{ spawns, waypoints, addons };
            const Spawn at = Spawn::FromRow(*row(s.guid), kind);
            std::vector<PathPoint> points(3);
            points[0].x = at.x + 5; points[0].y = at.y;     points[0].z = at.z;
            points[1].x = at.x + 5; points[1].y = at.y + 5; points[1].z = at.z; points[1].delay = 1500; points[1].moveType = 1;
            points[2].x = at.x;     points[2].y = at.y + 5; points[2].z = at.z;
            const uint32_t id = path.PathId(s.guid);
            auto count = [&](const std::string& sql) { auto r = db.Query(sql, error); return r && !r->empty() ? std::stoul((*r)[0][0]) : 0ul; };
            const std::string wpCount = "SELECT COUNT(*) FROM waypoint_data WHERE id = " + std::to_string(id);
            const std::string addonPath = "SELECT COALESCE(MAX(path_id), 0) FROM creature_addon WHERE guid = " + std::to_string(s.guid);
            auto movement = [&] { return (*row(s.guid))["MovementType"].get<std::string>(); };
            const std::string movementBefore = movement();
            auto parts = path.Save(s.guid, points, error);
            check(parts.size() == 3, "path save: creature + creature_addon + waypoint_data parts");
            store.Commit(std::move(parts), "path");
            check(count(wpCount) == 3 && count(addonPath) == id && movement() == "2", "path in the database: 3 points, path_id, MovementType 2");
            const auto loaded = path.Load(s.guid);
            check(loaded.size() == 3 && loaded[1].delay == 1500 && loaded[1].moveType == 1 && std::fabs(loaded[2].y - points[2].y) < 0.01f,
                  "path loads back with delay and move type");
            store.Undo();
            check(count(wpCount) == 0 && count(addonPath) == 0 && movement() == movementBefore, "undo path: one step clears all three tables");
            store.Redo();
            store.Commit(path.Save(s.guid, {}, error), "remove path");
            check(count(wpCount) == 0 && count(addonPath) == 0 && movement() == "0", "remove path: no points, path_id 0, standing");
            store.Undo();
            check(count(wpCount) == 3 && count(addonPath) == id && movement() == "2", "undo remove: path back");
        }

        const auto dir = std::filesystem::temp_directory_path() / "wow-world-editor-spawncheck";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        check(store.Save(dir / "changes", error), "changes saved");
        {
            ChangeStore again;
            SpawnAdapter reload(again, kind);
            again.Register(reload);
            check(again.Load(dir / "changes", error) && again.Done().size() == store.Done().size(), "changes reload");
            reload.SetDb(&db);
            check(reload.Sync(error), "sync after reload (idempotent)");
            check(row(s.guid) && std::fabs(std::stof((*row(s.guid))["position_x"].get<std::string>()) - (s.x + 5)) < 0.01f, "row unchanged by sync");
            check(reload.ExportSql(dir / "server", error), "export SQL written");
            std::ifstream sql(dir / "server" / (table + "_spawns.sql")), revert(dir / "server" / (table + "_spawns_revert.sql"));
            const std::string a((std::istreambuf_iterator<char>(sql)), {}), r((std::istreambuf_iterator<char>(revert)), {});
            check(a.find(std::to_string(s.guid)) != std::string::npos && r.find("DELETE FROM " + table + " WHERE guid = " + std::to_string(s.guid)) != std::string::npos,
                  "export: insert + revert delete for the new guid");
        }

        // An existing spawn: move it, then undo; every column must be exactly as before.
        const auto existing = db.Query("SELECT MIN(guid) FROM " + table + " WHERE map = 0 AND ScriptName <> '' ", error);
        const auto any = db.Query("SELECT MIN(guid) FROM " + table + " WHERE map = 0", error);
        const uint32_t guid = uint32_t(std::stoul(existing && !existing->empty() && !(*existing)[0][0].empty() ? (*existing)[0][0] : (*any)[0][0]));
        const auto original = *row(guid);
        Spawn e = Spawn::FromRow(original, kind);
        e.x += 1;
        commit(original, e.ToRow(original), "move existing");
        auto after = *row(guid);
        bool othersKept = true;
        for (const auto& [col, val] : original.items())
            if (col != "position_x" && after[col] != val) { othersKept = false; printf("     column %s changed: %s -> %s\n", col.c_str(), val.dump().c_str(), after[col].dump().c_str()); }
        check(othersKept, "editing an existing spawn keeps its other columns");
        store.Undo();
        check(*row(guid) == original, "undo restores the existing row exactly");

        while (store.CanUndo()) store.Undo();
        const auto final = db.Query("SELECT COUNT(*), COALESCE(SUM(guid), 0) FROM " + table, error);
        check(before && final && (*before)[0] == (*final)[0], "database back to its starting state");
        std::filesystem::remove_all(dir, ec);
        }
        printf("spawn check: %d problem(s)\n", problems);
        return problems ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--unit-catalog-check") && __argc >= 4)
    {
        // `--unit-catalog-check <AC server dir> <data dir>`: reads every creature and gameobject template the way the
        // catalog does, times it, and counts how many of the first 500 of each have a model in the client.
        setvbuf(stdout, nullptr, _IONBF, 0);
        std::string password, note, error;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        MpqChain mpq;
        mpq.Open(std::filesystem::path(__wargv[3]).string());
        DisplayLooks looks(mpq);
        ChangeStore store;
        int problems = 0;
        for (const SpawnKind kind : { SpawnKind::Creature, SpawnKind::GameObject })
        {
            SpawnAdapter adapter(store, kind);
            adapter.SetDb(&db);
            const auto start = std::chrono::steady_clock::now();
            const auto all = adapter.All(error);
            const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::map<uint32_t, size_t> categories;
            size_t modelled = 0, tried = 0;
            for (const auto& t : all)
            {
                ++categories[t.category];
                if (tried >= 500) continue;
                ++tried;
                Spawn s;
                s.kind = kind;
                s.displayId = t.displayId;
                s.size = t.size;
                modelled += looks.SpawnLook(s).has_value();
            }
            printf("%s: %zu templates in %.0f ms, %zu categories, %zu of the first %zu have a model%s\n", kind == SpawnKind::Creature ? "creatures" : "gameobjects",
                   all.size(), ms, categories.size(), modelled, tried, error.empty() ? "" : ("  error: " + error).c_str());
            problems += all.empty() || modelled == 0;
        }
        return problems ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--sql") && __argc >= 4)
    {
        // `--sql <AC server dir> "<query>"`: runs one query on the world database from worldserver.conf, prints rows.
        std::string password, note, error;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        const auto rows = db.Query(std::filesystem::path(__wargv[3]).string(), error);
        if (!rows) { printf("query: %s\n", error.c_str()); return 1; }
        for (const auto& row : *rows)
        {
            for (size_t i = 0; i < row.size(); ++i) printf("%s%s", i ? "\t" : "", row[i].c_str());
            printf("\n");
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--server-check") && __argc >= 3)
    {
        // `--server-check <AC server dir> [soap account] [soap password]`: database + SOAP link from worldserver.conf.
        std::string password, note;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        if (!note.empty()) printf("note: %s\n", note.c_str());
        printf("db %s:%d user %s, world %s, characters %s; soap %s:%d\n", profile->dbHost.c_str(), profile->dbPort, profile->dbUser.c_str(),
               profile->worldDb.c_str(), profile->characterDb.c_str(), profile->soapHost.c_str(), profile->soapPort);
        Db db;
        std::string error;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        const auto rows = db.Query("SELECT core_version, (SELECT COUNT(*) FROM creature_template) FROM version", error);
        if (!rows || rows->empty()) { printf("db query: %s\n", error.c_str()); return 1; }
        printf("db ok: %s, %s creature templates\n", (*rows)[0][0].c_str(), (*rows)[0][1].c_str());
        auto narrow = [](int i) { return i < __argc ? std::filesystem::path(__wargv[i]).string() : std::string(); };
        const auto out = SoapCommand(profile->soapHost, profile->soapPort, narrow(3), narrow(4), "server info", error);
        printf("soap: %s\n", out ?out->c_str() : error.c_str());
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--extract") && __argc >= 5)
    {
        // `--extract <data dir> <game path> <out file>`: one file as the client resolves it.
        char dir[1024] = {}, name[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dir, sizeof dir, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[3], -1, name, sizeof name, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dir);
        const auto bytes = mpq.Read(name);
        if (!bytes) { printf("not found: %s\n", name); return 1; }
        std::ofstream(__wargv[4], std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--ground-effects") && __argc >= 4)
    {
        // `--ground-effects <data dir> <adt files...>`: every MCLY ground effect id the tiles use must be a row of the
        // client's GroundEffectTexture.dbc, and its doodads rows of GroundEffectDoodad.dbc.
        char dir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dir, sizeof dir, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dir);
        auto ids = [&](const char* name, std::map<uint32_t, std::vector<uint32_t>>& rows) {
            const auto dbc = mpq.Read(std::string("DBFilesClient\\") + name);
            if (!dbc || dbc->size() < 20 || memcmp(dbc->data(), "WDBC", 4) != 0) return false;
            uint32_t records = 0, fields = 0, recordSize = 0;
            memcpy(&records, dbc->data() + 4, 4);
            memcpy(&fields, dbc->data() + 8, 4);
            memcpy(&recordSize, dbc->data() + 12, 4);
            for (uint32_t r = 0; r < records; ++r)
            {
                std::vector<uint32_t> v(fields);
                memcpy(v.data(), dbc->data() + 20 + size_t(r) * recordSize, size_t(fields) * 4);
                rows[v[0]] = v;
            }
            return true;
        };
        std::map<uint32_t, std::vector<uint32_t>> textures, doodads;
        if (!ids("GroundEffectTexture.dbc", textures) || !ids("GroundEffectDoodad.dbc", doodads)) { printf("cannot read the DBCs\n"); return 1; }
        printf("GroundEffectTexture: %zu rows (max id %u), GroundEffectDoodad: %zu rows\n", textures.size(), textures.rbegin()->first, doodads.size());
        int bad = 0;
        for (int i = 3; i < __argc; ++i)
        {
            std::ifstream f(__wargv[i], std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const auto adt = ParseAdt(bytes, false);
            if (!adt) continue;
            std::map<uint32_t, size_t> used;
            for (const auto& c : adt->chunks)
                for (uint32_t l = 0; l < c.layerCount; ++l) ++used[c.effectIds[l]];
            size_t missing = 0, missingDoodads = 0;
            for (const auto& [id, n] : used)
            {
                if (id == 0) continue;
                auto it = textures.find(id);
                if (it == textures.end()) { ++missing; printf("  effect %u used %zu time(s): NOT in GroundEffectTexture.dbc\n", id, n); continue; }
                for (int k = 1; k <= 4; ++k)
                    if (it->second[size_t(k)] && !doodads.count(it->second[size_t(k)])) { ++missingDoodads; printf("  effect %u doodad %u: NOT in GroundEffectDoodad.dbc\n", id, it->second[size_t(k)]); }
            }
            printf("%ls: %zu effect ids used, %zu missing, %zu missing doodads\n", __wargv[i], used.size(), missing, missingDoodads);
            bad += missing + missingDoodads != 0;
        }
        return bad ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--validate-refs") && __argc >= 4)
    {
        // `--validate-refs <data dir> <adt files...>`: every model and texture the tiles use exists in the client and is
        // a 3.3.5 format (M2 version 264 with its skin, WMO version 17, BLP2).
        char dir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dir, sizeof dir, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dir);
        int bad = 0;
        for (int i = 3; i < __argc; ++i)
        {
            std::ifstream f(__wargv[i], std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const auto adt = ParseAdt(bytes, false);
            if (!adt) { printf("%ls: unreadable\n", __wargv[i]); ++bad; continue; }
            std::set<std::string> m2s, wmoNames;
            for (const auto& d : adt->doodads) m2s.insert(M2Name(d.model));
            for (const auto& w : adt->wmos) wmoNames.insert(w.model);
            size_t problems = 0;
            auto report = [&](const std::string& what) { if (problems++ < 15) printf("  %s\n", what.c_str()); };
            for (const auto& name : m2s)
            {
                const auto m2 = mpq.Read(name);
                uint32_t magic = 0, version = 0;
                if (m2 && m2->size() >= 8) { memcpy(&magic, m2->data(), 4); memcpy(&version, m2->data() + 4, 4); }
                if (!m2) report("missing M2 " + name);
                else if (magic != 0x3032444D || version != 264) report("M2 not 3.3.5 (magic " + std::to_string(magic) + ", version " + std::to_string(version) + "): " + name);
                else if (!mpq.Read(M2SkinName(name))) report("missing skin for " + name);
            }
            for (const auto& name : wmoNames)
            {
                const auto root = mpq.Read(name);
                uint32_t version = 0;
                if (root && root->size() >= 12) memcpy(&version, root->data() + 8, 4);
                if (!root) report("missing WMO " + name);
                else if (version != 17) report("WMO version " + std::to_string(version) + ": " + name);
            }
            for (const auto& t : adt->textures)
            {
                const auto blp = mpq.Read(t);
                if (!blp) report("missing texture " + t);
                else if (blp->size() < 4 || memcmp(blp->data(), "BLP2", 4) != 0) report("texture not BLP2: " + t);
            }
            printf("%ls: %zu M2s, %zu WMOs, %zu textures, %zu problem(s)\n", __wargv[i], m2s.size(), wmoNames.size(), adt->textures.size(), problems);
            bad += problems != 0;
        }
        return bad ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--validate"))
    {
        // `--validate <adt files...>`: structural check of ADTs as the client reads them (4-bit alpha assumed).
        int bad = 0;
        for (int i = 2; i < __argc; ++i)
        {
            std::ifstream f(__wargv[i], std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string summary;
            const auto problems = ValidateAdt(bytes, false, &summary);
            printf("%ls: %s, %zu problem(s)\n", __wargv[i], summary.c_str(), problems.size());
            for (const auto& p : problems) printf("  %s\n", p.c_str());
            bad += !problems.empty();
        }
        return bad ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--normals-check") && __argc >= 6)
    {
        // `--normals-check <data dir> <map> <x> <y>`: which axis order and signs make our normals match Blizzard's MCNR.
        char dir[1024] = {}, map[256] = {}, xs[16] = {}, ys[16] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dir, sizeof dir, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[3], -1, map, sizeof map, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[4], -1, xs, sizeof xs, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[5], -1, ys, sizeof ys, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dir);
        const std::string path = std::string("World\\Maps\\") + map + "\\" + map + "_" + xs + "_" + ys + ".adt";
        const auto bytes = mpq.Read(path);
        const auto adt = bytes ? ParseAdt(*bytes, false) : std::nullopt;
        if (!adt) { printf("cannot read %s\n", path.c_str()); return 1; }
        const int perms[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
        double best = -2;
        int bestPerm = 0, bestSigns = 0;
        for (int p = 0; p < 6; ++p)
            for (int sg = 0; sg < 8; ++sg)
            {
                double sum = 0;
                size_t count = 0;
                for (size_t ci = 0; ci < adt->chunks.size(); ++ci)
                {
                    const AdtChunk& c = adt->chunks[ci];
                    if (!c.mcnrOffset) continue;
                    const auto n = ChunkNormals(*adt, ci);
                    for (size_t j = 0; j < 145; ++j)
                    {
                        double dot = 0;
                        for (int k = 0; k < 3; ++k)
                        {
                            const double stored = int8_t((*bytes)[c.mcnrOffset + j * 3 + size_t(k)]) / 127.0;
                            dot += stored * ((sg >> k) & 1 ? -1.0 : 1.0) * n[j][size_t(perms[p][k])];
                        }
                        sum += dot;
                        ++count;
                    }
                }
                if (count && sum / count > best) { best = sum / count; bestPerm = p; bestSigns = sg; }
            }
        printf("MCNR byte k = sign * editor normal[perm[k]]: perm %d %d %d, signs %c %c %c, mean dot %.4f (%.1f deg)\n", perms[bestPerm][0], perms[bestPerm][1],
               perms[bestPerm][2], bestSigns & 1 ? '-' : '+', bestSigns & 2 ? '-' : '+', bestSigns & 4 ? '-' : '+', best, std::acos(std::min(best, 1.0)) * 57.2958);

        // Vertices on the tile border: without the neighbouring tiles (one-sided slope) and with them (export now).
        const int tx = std::atoi(xs), ty = std::atoi(ys);
        std::map<int, std::optional<Adt>> neighbours;
        auto beyond = [&](int gx, int gz) -> std::optional<float> {
            const int vx = tx * 128 + gx, vz = ty * 128 + gz;
            if (vx < 0 || vz < 0) return std::nullopt;
            auto [it, added] = neighbours.try_emplace(TileKey(vx / 128, vz / 128));
            if (added)
                if (auto b = mpq.Read(std::string("World\\Maps\\") + map + "\\" + map + "_" + std::to_string(vx / 128) + "_" + std::to_string(vz / 128) + ".adt"))
                    it->second = ParseAdt(*b, false);
            if (!it->second) return std::nullopt;
            for (const AdtChunk& c : it->second->chunks)
                if (int(c.indexX) == (vx % 128) / 8 && int(c.indexY) == (vz % 128) / 8)
                    return c.baseY + c.heights[size_t(((vz % 128) % 8) * 17 + (vx % 128) % 8)];
            return std::nullopt;
        };
        for (int withNeighbours = 0; withNeighbours < 2; ++withNeighbours)
        {
            double sum = 0;
            size_t count = 0;
            for (size_t ci = 0; ci < adt->chunks.size(); ++ci)
            {
                const AdtChunk& c = adt->chunks[ci];
                if (!c.mcnrOffset) continue;
                const auto n = withNeighbours ? ChunkNormals(*adt, ci, beyond) : ChunkNormals(*adt, ci);
                for (int row = 0; row <= 8; ++row)
                    for (int col = 0; col <= 8; ++col)
                    {
                        const bool border = (c.indexX == 0 && col == 0) || (c.indexX == 15 && col == 8) || (c.indexY == 0 && row == 0) || (c.indexY == 15 && row == 8);
                        if (!border) continue;
                        const size_t j = size_t(row * 17 + col);
                        const double v[3] = { -n[j][2], -n[j][0], n[j][1] };   // the MCNR order export writes
                        double dot = 0;
                        for (int k = 0; k < 3; ++k) dot += int8_t((*bytes)[c.mcnrOffset + j * 3 + size_t(k)]) / 127.0 * v[k];
                        sum += dot;
                        ++count;
                    }
            }
            printf("tile border vertices (%zu), %s: %.1f deg from Blizzard's\n", count, withNeighbours ? "with the neighbouring tiles" : "this tile only",
                   std::acos(std::min(sum / std::max<size_t>(count, 1), 1.0)) * 57.2958);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--find") && __argc >= 4)
    {
        // `--find <data dir> <word>`: every listed file whose path contains the word, with its archive.
        char dir[1024] = {}, word[256] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dir, sizeof dir, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[3], -1, word, sizeof word, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dir);
        Catalog catalog;
        catalog.Build(mpq);
        for (const Catalog::Item* item : catalog.Filter(Catalog::Kind::Count, "", word)) printf("%s  [%s]\n", item->path.c_str(), mpq.Names()[item->archive].c_str());
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--stream-check") && __argc >= 6)
    {
        // `--stream-check <data dir> <map> <x> <y> [other client]`: fly 6 tiles east at 400 yd/s (60 fps steps), load radius 2, on a
        // software device; per frame, time spent streaming terrain and models: without the loader (all on this
        // thread, as before) and with it. Prints frame times; exit 0 when the loader's worst frame is under 50 ms.
        auto arg = [](int i) { return std::filesystem::path(__wargv[i]).string(); };
        const std::string map = arg(3);
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5));
        auto fly = [&](bool background) {
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            Renderer renderer;
            ModelRenderer models;
            std::string error;
            renderer.Init(device.Get(), context.Get(), error);
            models.Init(device.Get(), context.Get(), renderer, error);
            MpqChain mpq;
            mpq.Open(arg(2));
            ChangeStore store;
            TerrainAdapter terrain(mpq, renderer, store);
            Loader loader;
            if (background)
            {
                loader.Start(&mpq);
                renderer.SetLoader(&loader);
                models.SetLoader(&loader);
                terrain.SetLoader(&loader);
            }
            terrain.SetMap(map, error);
            // Optional ghost layer: the other client's version of the same map, streamed like the editor does.
            Ghosts ghosts;
            if (__argc > 6)
            {
                std::vector<std::string> errors;
                ghosts.Reset(&mpq, "client", { { "other", { { MpqLayer::Kind::MpqFolder, arg(6) } } } }, errors);
                ghosts.AddLayer(1, -1, "other");
                if (background) ghosts.StartWorker();
            }
            size_t ghostTiles = 0;
            float uploadMs = 0;
            std::vector<float> frames;
            const float dt = 1.0f / 60, speed = 400;
            float x = (tx + 0.5f) * kTileSize;
            const float z = (ty + 0.5f) * kTileSize, endX = x + 6 * kTileSize;
            size_t tiles = 0;
            for (; x < endX || (background && loader.Pending()); x = std::min(endX, x + speed * dt))
            {
                const auto t0 = std::chrono::steady_clock::now();
                auto ms = [&] { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
                tiles += terrain.Stream(x, z, 2, error, 4.0f).size();
                if (!ghosts.Layers().empty())
                {
                    const auto streamed = ghosts.Stream(map, x, z, 2, !background || ms() < 4.0f ? 1 : 0);   // as App: one shared budget
                    for (const auto& [name, image] : ghosts.TakeImages()) renderer.CacheTexture(name, image);
                    for (const auto& [id, key] : streamed.unloaded) renderer.UnloadTile(Ghosts::Key(id, key));
                    for (const auto& [id, key] : streamed.loaded)
                    {
                        const auto u0 = std::chrono::steady_clock::now();
                        renderer.LoadTile(Ghosts::Key(id, key), ghosts.Find(id)->tiles.at(key).adt, ghosts.Chain(1), id);
                        uploadMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - u0).count();
                    }
                    ghostTiles += streamed.loaded.size();
                }
                for (int key : models.TileKeys())
                    if (!terrain.Tiles().count(key)) models.RemoveTile(key);
                for (const auto& [key, tile] : terrain.Tiles())
                    if (!models.HasTile(key))
                    {
                        models.AddTile(key, tile.adt, mpq);
                        if (!background || ms() > 8.0f) break;
                    }
                frames.push_back(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
                if (frames.size() > 20000) break;
                std::this_thread::sleep_until(t0 + std::chrono::microseconds(16667));   // real frame pacing: the loader works meanwhile
                if (x >= endX && !background) break;
            }
            loader.Stop();
            ghosts.StopWorker();
            std::vector<float> sorted = frames;
            std::sort(sorted.begin(), sorted.end());
            const float avg = std::accumulate(frames.begin(), frames.end(), 0.0f) / float(std::max<size_t>(1, frames.size()));
            const size_t over = size_t(std::count_if(frames.begin(), frames.end(), [](float f) { return f > 16.7f; }));
            if (ghostTiles) printf("  ghost tile upload: %.1f ms each\n", uploadMs / float(ghostTiles));
            printf("%-12s %zu frames, %zu tiles + %zu ghost: avg %.2f ms, p95 %.1f ms, worst %.1f ms, %zu frame(s) over 16.7 ms\n",
                   background ? "loader:" : "same thread:", frames.size(), tiles, ghostTiles, avg, sorted[sorted.size() * 95 / 100], sorted.back(), over);
            return sorted.back();
        };
        fly(false);
        return fly(true) < 50.0f ? 0 : 1;
    }
    if (cmdLine && wcsstr(cmdLine, L"--shade-check")) return ShadeCheck();
    if (cmdLine && wcsstr(cmdLine, L"--light-check") && __argc >= 3)
    {
        // `--light-check <data dir>`: the light tables on the client's files: sizes, Goldshire at noon and midnight
        // (the sky must be blue by day and dark by night), a params copy, an edited key read back.
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        Lights lights(mpq, store);
        int problems = 0;
        for (DbcTable* t : lights.Tables())
        {
            const auto rows = t->Rows();
            printf("%-15s %5zu rows, max id %u\n", t->Name().c_str(), rows.size(), rows.empty() ? 0u : rows.rbegin()->first);
            if (rows.empty()) ++problems;
        }
        const auto azeroth = lights.OnMap(0);
        printf("Azeroth: %zu lights, default %s\n", azeroth.size(), !azeroth.empty() && azeroth.front().Global() ? "yes" : "no");
        const XMFLOAT3 goldshire{ 32.6f * kTileSize, 60.0f, 48.6f * kTileSize };
        for (int time : { 1440, 0 })
        {
            const LightState s = lights.At(0, goldshire, time);
            printf("Goldshire %s: sky top %.2f %.2f %.2f, fog %.2f %.2f %.2f, fog end %.0f yd start %.2f, ambient %.2f %.2f %.2f, skybox %u\n",
                   time ? "noon" : "midnight", s.colors[2].x, s.colors[2].y, s.colors[2].z, s.colors[7].x, s.colors[7].y, s.colors[7].z,
                   s.floats[0], s.floats[1], s.colors[1].x, s.colors[1].y, s.colors[1].z, s.skybox);
            const float bright = s.colors[2].x + s.colors[2].y + s.colors[2].z;
            if (time && (s.colors[2].z <= s.colors[2].x || bright < 0.3f)) ++problems;   // noon sky: blue (deep: 0, 30, 74 in Elwynn)
            if (!time && bright > 1.2f) ++problems;                                     // midnight: dark
            if (s.floats[0] < 50 || s.floats[0] > 5000) ++problems;
        }
        // A params copy keeps every band; the copy evaluates like the original.
        const uint32_t from = azeroth.empty() ? 1 : azeroth.front().params[0];
        const uint32_t to = lights.FreeParamsId(3000, 3999);
        const auto rows = lights.CopyParams(from, to);
        Lights::Draft draft;
        for (const auto& [table, id, row] : rows) draft[{ table->Name(), id }] = row;
        const LightState a = lights.Params(from, 1440), b = lights.Params(to, 1440, &draft);
        printf("copy params %u -> %u: %zu rows, same noon fog colour %s\n", from, to, rows.size(),
               a.colors[7].x == b.colors[7].x && a.colors[7].z == b.colors[7].z ? "yes" : "NO");
        if (!to || rows.size() != 25 || a.colors[7].x != b.colors[7].x) ++problems;
        // Set the copy's fog colour to pure red at noon only: noon reads red, keys stay sorted.
        nlohmann::json band = draft.at({ "LightIntBand", to * 18 - 17 + 7 });
        auto keys = Lights::Keys(band);
        keys.push_back({ 1440, Lights::Raw({ 1, 0, 0 }) });
        std::erase_if(keys, [&](const auto& k) { return k.first == 1440 && k.second != Lights::Raw({ 1, 0, 0 }); });
        draft[{ "LightIntBand", to * 18 - 17 + 7 }] = Lights::SetKeys(band, keys);
        const LightState red = lights.Params(to, 1440, &draft);
        printf("edited key: noon fog %.2f %.2f %.2f\n", red.colors[7].x, red.colors[7].y, red.colors[7].z);
        if (red.colors[7].x < 0.99f || red.colors[7].y > 0.01f) ++problems;
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--plan-check"))
    {
        try { return PlanCheck(); }
        catch (const std::exception& e) { printf("exception: %s\n", e.what()); return 1; }
    }
    if (cmdLine && wcsstr(cmdLine, L"--area-check") && __argc >= 3)
    {
        // `--area-check <data dir>`: the AreaTable adapter on the client's real DBC: add a sub-area, rename an existing
        // area, export, read the DBC back (every other row unchanged), check the mod-dbc-patch file, undo both.
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        AreaAdapter areas(mpq, store);
        store.Register(areas);
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        Dbc client;
        const auto clientBytes = mpq.Read("DBFilesClient\\AreaTable.dbc");
        if (!clientBytes || !client.Load(*clientBytes)) { printf("no AreaTable.dbc\n"); return 1; }
        uint32_t maxId = 0, maxBit = 0, inRange = 0;
        for (uint32_t r = 0; r < client.Rows(); ++r)
        {
            maxId = std::max(maxId, client.U32(r, 0));
            maxBit = std::max(maxBit, client.U32(r, 3));
            inRange += client.U32(r, 0) >= 20000 && client.U32(r, 0) <= 20999;
        }
        printf("client: %u rows, max id %u, max AreaBit %u, %u row(s) in 20000-20999, %zu areas on Azeroth\n", client.Rows(), maxId, maxBit, inRange,
               areas.OnMap(0).size());
        const auto goldshire = areas.Find(87);
        check(goldshire && goldshire->name == "Goldshire" && areas.ZoneOf(87) == 12, "Goldshire (87) is in Elwynn Forest (12)");

        const uint32_t id = areas.FreeId(20000, 20999);
        const nlohmann::json row = areas.NewRow(id, 0, 12, "Editor Test Glade", 10);
        const uint32_t bit = row.value("AreaBit", 0u);
        check(id && bit && bit < 4096, "new area " + std::to_string(id) + " gets AreaBit " + std::to_string(bit));
        areas.Commit(id, row, "add");
        nlohmann::json renamed = areas.Row(87);
        renamed["AreaName_lang"] = "Goldshire Edited";
        areas.Commit(87, renamed, "rename");
        check(areas.Count() == 2 && areas.Find(id)->name == "Editor Test Glade" && areas.ZoneOf(id) == 12, "two project rows, the new one in Elwynn");

        const std::filesystem::path out = std::filesystem::temp_directory_path() / "wow-world-editor-areacheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::string error;
        check(areas.Export({ out / "client", out / "server" }, out / "dbc", error), "export " + error);
        auto readFile = [](const std::filesystem::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        Dbc back;
        check(back.Load(readFile(out / "client" / "AreaTable.dbc")) && back.Rows() == client.Rows() + 1, "written DBC has one row more");
        check(readFile(out / "client" / "AreaTable.dbc") == readFile(out / "server" / "AreaTable.dbc"), "client and server DBC identical");
        const auto n = back.Find(id), g = back.Find(87);
        check(n && back.Str(*n, 11) == "Editor Test Glade" && back.U32(*n, 2) == 12 && back.U32(*n, 3) == bit && back.U32(*n, 10) == 10,
              "new row: name, parent, AreaBit, level");
        check(g && back.Str(*g, 11) == "Goldshire Edited", "renamed row");
        size_t differ = 0;
        for (uint32_t r = 0; r < client.Rows(); ++r)
        {
            const uint32_t rid = client.U32(r, 0);
            const auto b = back.Find(rid);
            if (!b) { ++differ; continue; }
            for (uint32_t f = 0; f < 36; ++f)
            {
                if (rid == 87 && f == 11) continue;
                const bool isString = f >= 11 && f <= 26;
                if (isString ? client.Str(r, f) != back.Str(*b, f) : client.U32(r, f) != back.U32(*b, f)) { ++differ; break; }
            }
        }
        check(differ == 0, "every other row unchanged (" + std::to_string(differ) + " differ)");
        std::ifstream jf(out / "dbc" / "AreaTable.json");
        const nlohmann::json patch = nlohmann::json::parse(jf, nullptr, false);
        jf.close();
        check(patch.is_object() && patch["add"].size() == 1 && patch["add"][0]["ID"] == id && patch["modify"].size() == 1 &&
                  patch["modify"][0].size() == 2 && patch["modify"][0]["AreaName_lang"] == "Goldshire Edited",
              "mod-dbc-patch file: one add, one modify of the name only");
        printf("%s\n", patch.dump().substr(0, 400).c_str());

        store.Undo();
        store.Undo();
        check(areas.Count() == 0 && !areas.Find(id) && areas.Find(87)->name == "Goldshire", "undo restores the client's rows");
        check(areas.Export({ out / "client", out / "server" }, out / "dbc", error) && !std::filesystem::exists(out / "client" / "AreaTable.dbc"),
              "export with no rows removes the old files");

        // WMOAreaTable: the WMOs of Northshire (Azeroth 32_48); Blizzard's own rows must name groups the files have.
        printf("-- WMOAreaTable\n");
        WmoAreaAdapter wmoAreas(mpq, store);
        store.Register(wmoAreas);
        const auto rows = wmoAreas.Rows();
        uint32_t maxWmoRow = rows.empty() ? 0 : rows.rbegin()->first, wmoInRange = 0;
        for (const auto& [rid, r] : rows) wmoInRange += rid >= 700000 && rid <= 709999;
        printf("client: %zu rows, max id %u, %u row(s) in 700000-709999\n", rows.size(), maxWmoRow, wmoInRange);
        const auto tileBytes = mpq.Read("World\\Maps\\Azeroth\\Azeroth_32_48.adt");
        const auto tile = tileBytes ? ParseAdt(*tileBytes, false) : std::nullopt;
        check(tile && !tile->wmos.empty(), "Azeroth_32_48 has WMOs");
        size_t matched = 0, unknown = 0;
        std::optional<std::pair<WmoAreaKeys, uint32_t>> sample;   // keys + name set of a placement with rows
        for (const WmoPlacement& p : tile ? tile->wmos : std::vector<WmoPlacement>{})
        {
            const auto keys = ReadWmoAreaKeys(p.model, [&](const std::string& n) { return mpq.Read(n); });
            if (!keys) { printf("  cannot read %s\n", p.model.c_str()); ++unknown; continue; }
            std::set<uint32_t> groups;
            for (const auto& g : keys->groups) groups.insert(g.first);
            const auto own = wmoAreas.For(keys->wmoId, p.nameSet);
            size_t hits = 0;
            for (const auto& [group, row] : own) hits += groups.count(group);
            printf("  %s: WMOID %u, name set %u, %zu group(s), %zu row(s), %zu matching a group\n", p.model.c_str(), keys->wmoId, p.nameSet,
                   keys->groups.size(), own.size(), hits);
            matched += hits;
            if (!own.empty() && !sample) sample = { *keys, p.nameSet };
        }
        check(unknown == 0 && matched > 0, "Blizzard's rows name group ids read from the WMO files");
        if (sample)
        {
            const WmoAreaKeys& keys = sample->first;
            const uint32_t nameSet = 120;   // unused: every group gets a new row
            std::vector<Change> parts;
            for (const auto& [group, name] : keys.groups)
            {
                const auto own = wmoAreas.For(keys.wmoId, nameSet);
                const uint32_t rid = own.count(group) ? own.at(group).first : wmoAreas.FreeId(700000, 709999);
                nlohmann::json after = own.count(group) ? own.at(group).second : wmoAreas.NewRow(rid, keys.wmoId, nameSet, group, 0);
                after["AreaTableID"] = 87;
                Change c = wmoAreas.MakeChange(rid, wmoAreas.Row(rid), after, "wmo check");
                wmoAreas.Apply(c);
                parts.push_back(std::move(c));
            }
            store.Commit(std::move(parts), "wmo check");
            const auto own = wmoAreas.For(keys.wmoId, nameSet);
            size_t toGoldshire = 0;
            for (const auto& [group, row] : own) toGoldshire += row.second.value("AreaTableID", 0u) == 87;
            check(toGoldshire >= keys.groups.size(), "every group of the sample WMO points at Goldshire");
            check(wmoAreas.Export({ out / "client" }, out / "dbc", error), "WMOAreaTable export " + error);
            Dbc wback;
            const auto firstRow = own.begin()->second.first;
            check(wback.Load(readFile(out / "client" / "WMOAreaTable.dbc")) && wback.Rows() == rows.size() + keys.groups.size() && wback.Find(firstRow) &&
                      wback.U32(*wback.Find(firstRow), 10) == 87,
                  "WMOAreaTable written: row count and AreaTableID");
            store.Undo();
            check(wmoAreas.Count() == 0, "undo removes every WMO row change (one batch)");
        }
        // World map: Blizzard's Elwynn map and Goldshire overlay, read through the editor's mapping (editor x = zero - LocLeft
        // side, z = zero - LocTop side), must put Goldshire's chunks inside the overlay's pixel rectangle.
        printf("-- WorldMapArea / WorldMapOverlay\n");
        WorldMapAreaAdapter worldMaps(mpq, store);
        WorldMapOverlayAdapter overlays(mpq, store);
        const auto maps = worldMaps.Rows(), pieces = overlays.Rows();
        auto countIn = [](const auto& rows, uint32_t a, uint32_t b) { size_t n = 0; for (const auto& [rid, r] : rows) n += rid >= a && rid <= b; return n; };
        printf("client: %zu WorldMapArea rows (max id %u, %zu in 9000-9099), %zu overlays (max id %u, %zu in 90000-90999)\n", maps.size(),
               maps.empty() ? 0 : maps.rbegin()->first, countIn(maps, 9000, 9099), pieces.size(), pieces.empty() ? 0 : pieces.rbegin()->first,
               countIn(pieces, 90000, 90999));
        const auto elwynn = worldMaps.ForZone(0, 12);
        check(elwynn.has_value(), "Elwynn Forest has a world map");
        if (elwynn)
        {
            const nlohmann::json& m = worldMaps.Row(*elwynn);
            const float x0 = kZeroPoint - m.value("LocLeft", 0.0f), z0 = kZeroPoint - m.value("LocTop", 0.0f);
            const float sx = m.value("LocLeft", 0.0f) - m.value("LocRight", 0.0f), sz = m.value("LocTop", 0.0f) - m.value("LocBottom", 0.0f);
            nlohmann::json gold;
            for (const auto& [oid, o] : overlays.For(*elwynn))
                if (o.value("AreaID[0]", 0u) == 87) gold = o;
            check(gold.is_object(), "Elwynn has a Goldshire overlay (" + gold.value("TextureName", std::string("-")) + ")");
            size_t chunks = 0, inside = 0;
            for (int ty = int(z0 / kTileSize); ty <= int((z0 + sz) / kTileSize); ++ty)
                for (int tx = int(x0 / kTileSize); tx <= int((x0 + sx) / kTileSize); ++tx)
                {
                    const auto bytes = mpq.Read("World\\Maps\\Azeroth\\Azeroth_" + std::to_string(tx) + "_" + std::to_string(ty) + ".adt");
                    const auto adt = bytes ? ParseAdt(*bytes, false) : std::nullopt;
                    for (const AdtChunk& c : adt ? adt->chunks : std::vector<AdtChunk>{})
                    {
                        if (c.areaId != 87) continue;
                        ++chunks;
                        const float px = (c.baseX + kChunkSize / 2 - x0) / sx * 1002, py = (c.baseZ + kChunkSize / 2 - z0) / sz * 668;
                        inside += gold.is_object() && px >= gold.value("OffsetX", 0) - 4 && px <= gold.value("OffsetX", 0) + gold.value("TextureWidth", 0) + 4 &&
                                  py >= gold.value("OffsetY", 0) - 4 && py <= gold.value("OffsetY", 0) + gold.value("TextureHeight", 0) + 4;
                    }
                }
            check(chunks > 0 && inside * 10 >= chunks * 9, "Goldshire chunks land in its overlay: " + std::to_string(inside) + " of " + std::to_string(chunks));
        }
        std::filesystem::remove_all(out, ec);
        printf("%s\n", problems ? "AREA CHECK FAILED" : "area check passed");
        return problems ? 1 : 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--rewrite-check")) return RewriteCheck();
    if (cmdLine && wcsstr(cmdLine, L"--selftest")) return FormatsSelfTest() && ChangesSelfTest() && TerrainSelfTest() && BlendSelfTest() && CatalogSelfTest() && BlueprintSelfTest() && TriggersSelfTest() && TransformSelfTest() ? 0 : 1;
    if (cmdLine && wcsstr(cmdLine, L"--check")) return Check();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"WowWorldEditor" };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"WoW World Editor", WS_OVERLAPPEDWINDOW, 80, 60, 1700, 980, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(hwnd))
    {
        MessageBoxW(hwnd, L"Could not create a Direct3D 11 device.", L"WoW World Editor", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    // The panel layout lives with the other per-user settings; a layout left next to the exe by older builds moves there.
    static const std::string iniPath = (SettingsDir() / "imgui.ini").string();
    if (std::error_code ec; !std::filesystem::exists(iniPath) && std::filesystem::exists("imgui.ini")) std::filesystem::copy_file("imgui.ini", iniPath, ec);
    const bool firstRun = !std::filesystem::exists(iniPath);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = iniPath.c_str();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    App app;
    if (!app.Init(hwnd, g_device.Get(), g_context.Get(), firstRun)) return 1;
    g_app = &app;

    auto last = std::chrono::steady_clock::now();
    while (!app.WantsQuit())
    {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            quit |= msg.message == WM_QUIT;
        }
        if (quit) break;

        if (g_resizeWidth && g_resizeHeight)
        {
            g_rtv.Reset();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateTarget();
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.Frame(dt);
        ImGui::Render();

        const float clear[4] = { 0.08f, 0.09f, 0.10f, 1 };
        g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    g_app = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
