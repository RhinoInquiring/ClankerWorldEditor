// Command-line checks: Terrain edits: copy and paste, water, sculpt, paint, new maps, roads. See docs/development/checks.md.

#include "Checks.hpp"

namespace checks
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
        // Map's water: another liquid, raised 5 yd, pasted on a dry chunk near the map's water takes the map's liquid and level.
        {
            std::optional<std::pair<float, uint16_t>> match;
            int spot = -1;
            for (int ci = 0; ci < int(tile().adt.chunks.size()) && spot < 0; ++ci)
            {
                const AdtChunk& d = tile().adt.chunks[size_t(ci)];
                if (water(ci).empty() && (match = terrain.NearestWater(d.baseX + kChunkSize / 2, d.baseZ + kChunkSize / 2, 80.0f))) spot = ci;
            }
            TerrainClipboard odd = wetClip;
            for (auto& l : odd.chunks[0].liquids)
            {
                l["type"] = l["type"].get<int>() == 3 ? 1 : 3;   // magma, or water when it was magma
                for (auto& h : l["heights"]) h = h.get<float>() + 5.0f;
            }
            o.mapWater = true;
            const bool pasted = spot >= 0 && paste(odd, spot);
            o.mapWater = false;
            expect(pasted, "other liquid pasted with Map's water beside the map's water");
            bool same = pasted && !water(spot).empty();
            if (pasted)
                for (const auto& l : water(spot))
                {
                    same = same && l["type"].get<int>() == match->second;
                    for (const auto& h : l["heights"]) same = same && std::fabs(h.get<float>() - match->first) < 0.01f;
                }
            expect(same, "  it has the map's liquid and level");
            if (pasted) store.Undo();
        }

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

    /// `--water-tool-check <Data>`: the Water tool on Azeroth 32_48 (Northshire): add, slope, level and remove water,
    /// magma and ocean, undo and redo, then export and read the tile back.
    int WaterToolCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        const int tx = 32, ty = 48, key = TileKey(tx, ty);
        if (!terrain.SetMap("Azeroth", error) || !terrain.LoadNow(tx, ty, error)) { printf("%s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        const Adt& adt = terrain.Tiles().at(key).adt;
        auto state = [&](int ci) { return TerrainAdapter::LiquidState(adt, adt.chunks[size_t(ci)]); };

        // Liquids the client knows.
        const auto& types = terrain.LiquidTypes();
        auto first = [&](LiquidInfo::Kind kind) -> const LiquidInfo* {
            for (const LiquidInfo& l : types)
                if (l.kind == kind) return &l;
            return nullptr;
        };
        const LiquidInfo* lake = terrain.Liquid(5);
        const LiquidInfo* magma = first(LiquidInfo::Kind::Magma);
        const LiquidInfo* ocean = first(LiquidInfo::Kind::Ocean);
        check(types.size() > 10 && lake && magma && ocean, std::to_string(types.size()) + " liquid types; lake " + (lake ? lake->name : "?") + " format " +
              std::to_string(lake ? lake->format : 99) + ", magma " + (magma ? magma->name + " format " + std::to_string(magma->format) : "?") +
              ", ocean " + (ocean ? ocean->name + " format " + std::to_string(ocean->format) : "?"));
        if (!lake || !magma || !ocean) return 1;

        // A dry chunk with dry neighbours, away from the tile's edge.
        int dry = -1;
        for (int ci = 0; ci < 256 && dry < 0; ++ci)
        {
            const int cx = ci % 16, cz = ci / 16;
            if (cx < 2 || cx > 13 || cz < 2 || cz > 13) continue;
            bool ok = true;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) ok &= state((cz + dz) * 16 + cx + dx).empty();
            if (ok) dry = ci;
        }
        check(dry >= 0, "found a dry spot (chunk " + std::to_string(dry) + ")");
        if (dry < 0) return 1;
        const AdtChunk& dc = adt.chunks[size_t(dry)];
        const XMFLOAT3 mid{ dc.baseX + kChunkSize / 2, 0, dc.baseZ + kChunkSize / 2 };
        const float ground = *terrain.HeightAt(mid.x, mid.z);
        const size_t before = adt.liquids.size();

        auto stroke = [&](const WaterBrush& b, std::initializer_list<XMFLOAT3> points) {
            terrain.BeginWater(*points.begin());
            for (const XMFLOAT3& p : points) terrain.WaterStep(p, b);
            auto change = terrain.EndWater(b);
            if (change) { printf("     %s\n", change->label.c_str()); store.Commit(std::move(*change)); }
            return change.has_value();
        };

        // Add a lake 3 yd above the ground.
        WaterBrush b;
        b.type = lake->id;
        b.radius = 12;
        b.level = ground + 3;
        check(stroke(b, { mid }), "add lake water");
        const auto at = terrain.WaterAt(mid.x, mid.z);
        check(at && std::fabs(at->first - b.level) < 0.01f && at->second == lake->id, "the surface is at the brush level and of its type");
        const nlohmann::json added = state(dry);
        check(added.size() == 1 && added[0]["format"] == lake->format, "one instance, the liquid's format");
        {
            const std::string extra = added[0]["extra"];
            const auto bytes = Base64Decode(extra);
            const int deepest = bytes.empty() ? 0 : *std::max_element(bytes.begin(), bytes.end());
            check(deepest >= 20 && deepest <= 60, "depth bytes about 9 per yard (deepest " + std::to_string(deepest) + ")");
        }

        // A stroke starting on dry ground beside it finds it to match (Match nearby water).
        const auto nearby = terrain.NearestWater(mid.x + 16 + 12, mid.z, 80);
        check(nearby && std::fabs(nearby->first - b.level) < 0.01f && nearby->second == lake->id, "dry ground beside it matches its level and liquid");
        check(!terrain.NearestWater(mid.x + 300, mid.z, 80), "nothing to match far away");

        // Painting beside it at another level keeps the lake where it was.
        WaterBrush higher = b;
        higher.level = ground + 10;
        check(stroke(higher, { { mid.x + 14, 0, mid.z } }), "paint more water beside it, higher");
        check(std::fabs(terrain.WaterAt(mid.x, mid.z)->first - b.level) < 0.01f, "the first water kept its level");

        // Level: the surface moves; a slope tilts it.
        WaterBrush level = b;
        level.mode = WaterBrush::Mode::Level;
        level.level = ground + 5;
        check(stroke(level, { mid }), "level the water 2 yd higher");
        check(std::fabs(terrain.WaterAt(mid.x, mid.z)->first - level.level) < 0.01f, "the surface is at the new level");
        WaterBrush sloped = level;
        sloped.angle = 10;
        sloped.direction = 90;   // downhill towards +x
        check(stroke(sloped, { mid }), "slope it 10 degrees");
        const float w0 = terrain.WaterAt(mid.x - 6, mid.z)->first, w1 = terrain.WaterAt(mid.x + 6, mid.z)->first;
        check(w0 > w1 + 1.0f, "it falls towards +x (" + std::to_string(w0) + " -> " + std::to_string(w1) + ")");

        // Undo all four, redo them.
        const nlohmann::json last = state(dry);
        for (int i = 0; i < 4; ++i) store.Undo();
        check(state(dry).empty() && adt.liquids.size() == before, "undo removes all of it");
        for (int i = 0; i < 4; ++i) store.Redo();
        check(state(dry) == last, "redo brings it back exactly");

        // Remove the middle.
        WaterBrush dryOut = b;
        dryOut.mode = WaterBrush::Mode::Remove;
        dryOut.radius = 5;
        check(stroke(dryOut, { mid }), "remove water in the middle");
        check(!terrain.WaterAt(mid.x, mid.z) && terrain.WaterAt(mid.x - 10, mid.z), "the middle is dry, around it is wet");

        // Magma over part of the lake takes those cells; ocean in another chunk stays flat.
        WaterBrush lava = b;
        lava.type = magma->id;
        lava.radius = 6;
        lava.level = ground + 1;
        check(stroke(lava, { { mid.x - 10, 0, mid.z } }), "paint magma over the lake's edge");
        check(terrain.WaterAt(mid.x - 10, mid.z)->second == magma->id, "magma there now");
        size_t types2 = 0;
        for (const auto& l : state(dry)) types2 += l["type"] == lake->id;
        check(types2 == 1, "the lake keeps the rest");
        const AdtChunk& oc = adt.chunks[size_t(dry + 32)];
        WaterBrush sea = b;
        sea.type = ocean->id;
        sea.angle = 20;
        sea.radius = 8;
        sea.level = *terrain.HeightAt(oc.baseX + 16, oc.baseZ + 16) + 2;
        check(stroke(sea, { { oc.baseX + 16, 0, oc.baseZ + 16 } }), "paint ocean, sloped brush");
        const nlohmann::json seaState = TerrainAdapter::LiquidState(adt, oc);
        bool flat = !seaState.empty();
        for (const auto& l : seaState)
            for (float h : l["heights"].get<std::vector<float>>()) flat &= h == sea.level;
        check(flat, "ocean (depth only) stays flat");

        // Export and read back.
        const auto out = std::filesystem::temp_directory_path() / "wow-world-editor-watertoolcheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::vector<std::filesystem::path> files;
        terrain.Export(out, error, &files);
        std::ifstream f(out / "World" / "Maps" / "Azeroth" / "Azeroth_32_48.adt", std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(!bytes.empty() && ValidateAdt(bytes, false).empty(), "exported tile validates");
        if (const auto back = ParseAdt(bytes, false))
        {
            size_t same = 0;
            for (int ci = 0; ci < 256; ++ci) same += TerrainAdapter::LiquidState(*back, back->chunks[size_t(ci)]) == state(ci);
            check(same == 256, "every chunk's water reads back as edited (" + std::to_string(same) + "/256)");
        }
        else check(false, "exported tile parses");
        std::filesystem::remove_all(out, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--sculpt-check <Data>`: Sculpt options on Kalimdor 40_30: falloff shapes, a sloped flatten (ramp) lands on its
    /// plane, fill only leaves high ground alone, moved vertex selections open no cracks, undo restores.
    int SculptCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        if (!terrain.SetMap("Kalimdor", error) || !terrain.LoadNow(40, 30, error)) { printf("%s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };

        using F = Brush::Falloff;
        check(BrushFalloff(F::Flat, 0.1f) == 1 && BrushFalloff(F::Linear, 0.5f) == 0.5f && BrushFalloff(F::Sharp, 0.5f) == 0.25f &&
              BrushFalloff(F::Smooth, 0.5f) == 0.5f && BrushFalloff(F::Gaussian, 1) == 1 && BrushFalloff(F::Gaussian, 0) < 0.02f,
              "falloff shapes (flat, linear, sharp, smooth, gaussian)");

        const float cx = 40.5f * kTileSize, cz = 30.5f * kTileSize;
        const float ground = *terrain.HeightAt(cx, cz);
        int commits = 0;
        auto stroke = [&](const Brush& b, XMFLOAT3 at, int steps) {
            at.y = *terrain.HeightAt(at.x, at.z);
            terrain.BeginStroke({ at, *terrain.ChunkAtGrid(int(at.x / kChunkSize), int(at.z / kChunkSize)) });
            for (int i = 0; i < steps; ++i) terrain.StrokeStep(at, b, 0.5f);
            auto change = terrain.EndStroke(b);
            if (change) { store.Commit(std::move(*change)); ++commits; }
            return change.has_value();
        };

        // A ramp: flatten to a plane 20 degrees downhill towards +x through (cx, ground + 4).
        Brush ramp;
        ramp.mode = Brush::Mode::Flatten;
        ramp.radius = 30;
        ramp.strength = 100;
        ramp.falloff = F::Flat;
        ramp.fixedHeight = true;
        ramp.height = ground + 4;
        ramp.angle = 20;
        ramp.direction = 90;
        check(stroke(ramp, { cx, 0, cz }, 20), "sloped flatten stroke");
        float worst = 0;
        for (float dx : { -15.0f, -5.0f, 5.0f, 15.0f })
        {
            const float want = ground + 4 - std::tan(XMConvertToRadians(20.0f)) * dx;
            worst = std::max(worst, std::fabs(*terrain.HeightAt(cx + dx, cz) - want));
        }
        check(worst < 0.5f, "the ground lies on the ramp's plane (worst " + std::to_string(worst) + " yd)");

        // Fill only: a flat target 10 yd under the ramp's top changes nothing above it.
        const float before = *terrain.HeightAt(cx - 15, cz);
        Brush fill = ramp;
        fill.angle = 0;
        fill.height = ground;   // the ramp's high side is ~9.5 yd above this
        fill.only = Brush::Only::Raise;
        fill.radius = 8;
        stroke(fill, { cx - 15, 0, cz }, 20);
        check(std::fabs(*terrain.HeightAt(cx - 15, cz) - before) < 0.01f, "fill only leaves higher ground alone");

        // Vertices across a chunk corner: every copy moves, no cracks; undo restores.
        const float vx = 40 * kTileSize + 4 * kChunkSize, vz = 30 * kTileSize + 4 * kChunkSize;   // a chunk corner
        const float vBefore = *terrain.HeightAt(vx, vz);
        terrain.SelectVertices({ vx, 0, vz }, 9, true);
        const size_t picked = terrain.SelectedVertices();
        terrain.SelectVertices({ vx + 6, 0, vz }, 2, false);
        check(picked > 20 && terrain.SelectedVertices() < picked, "select " + std::to_string(picked) + " vertices, deselect some");
        auto move = terrain.EditSelectedVertices(TerrainAdapter::VertexOp::Move, 6);
        check(move.has_value(), "raise the selection 6 yd");
        if (move) { store.Commit(std::move(*move)); ++commits; }
        check(std::fabs(*terrain.HeightAt(vx, vz) - vBefore - 6) < 0.01f, "the corner is 6 yd higher");
        std::vector<Problem> cracks;
        terrain.FindCracks(cracks);
        check(cracks.empty(), "no cracks between chunks (" + std::to_string(cracks.size()) + ")");
        auto even = terrain.EditSelectedVertices(TerrainAdapter::VertexOp::Even, 0);
        if (even) { store.Commit(std::move(*even)); ++commits; }
        const auto positions = terrain.SelectedVertexPositions();
        float spread = 0;
        for (const XMFLOAT3& p : positions) spread = std::max(spread, std::fabs(p.y - positions.front().y));
        check(even.has_value() && spread < 0.01f, "even out sets them all to one height");
        for (int i = 0; i < commits; ++i) store.Undo();
        check(std::fabs(*terrain.HeightAt(vx, vz) - vBefore) < 0.01f && std::fabs(*terrain.HeightAt(cx, cz) - ground) < 0.01f, "undo restores the ground");
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--newmap-check <Data>`: File > New map without the window: a Map.dbc and MapDifficulty row, the WDT and WDL, 2 x 2
    /// flat tiles as one undo step; the tiles load, export writes everything, undo removes it all, redo and reopening
    /// (overlay rebuilt from the changes) bring it back.
    int NewMapCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wow-world-editor-newmapcheck";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "tiles", ec);
        MpqChain mpq;
        mpq.Open(dataDir);
        mpq.SetOverlay(dir / "overlay");
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        MapRowsAdapter maps(mpq, store);
        DbcTable difficulty(mpq, store, "MapDifficulty", { { "ID", 0, 'i' }, { "MapID", 1, 'i' }, { "Difficulty", 2, 'i' }, { "Message_lang", 3, 's' },
                                                           { "RaidDuration", 20, 'i' }, { "MaxPlayers", 21, 'i' }, { "Difficultystring", 22, 's' } }, 23);
        store.Register(terrain);
        store.Register(maps);
        store.Register(difficulty);
        terrain.SetProjectDir(dir);
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };

        const std::string folder = "WweCheckMap";
        const uint32_t id = maps.FreeId(800, 999), diffId = difficulty.FreeId(1000, 1099);
        check(id >= 800 && diffId >= 1000 && maps.Row(0)["Directory"] == "Azeroth", "free ids " + std::to_string(id) + " / " + std::to_string(diffId) + ", Azeroth's row reads");
        nlohmann::json row = maps.Row(36);   // like the Deadmines
        row["ID"] = id;
        row["Directory"] = folder;
        row["MapName_lang"] = "Check Map";
        row["InstanceType"] = 1u;
        row["MaxPlayers"] = 5u;
        std::vector<Change> parts;
        parts.push_back(maps.MakeChange(id, nullptr, row, "new map"));
        maps.Apply(parts.back());
        parts.push_back(difficulty.MakeChange(diffId, nullptr, { { "ID", diffId }, { "MapID", id }, { "MaxPlayers", 5u } }, "new map"));
        difficulty.Apply(parts.back());
        parts.push_back(terrain.CreateMap(folder, 0x4));
        check(terrain.SetMap(folder, error), "the new map opens from the overlay WDT");
        std::vector<TerrainAdapter::NewTile> tiles;
        for (int y = 31; y <= 32; ++y)
            for (int x = 31; x <= 32; ++x) tiles.push_back({ x, y, BlankAdt(x, y, 25.0f, "Tileset\\Elwynn\\ElwynnGrassBase.blp", 0), {} });
        auto added = terrain.AddTiles(tiles, true, error);
        check(added.has_value(), "four flat tiles added" + (error.empty() ? "" : ": " + error));
        if (!added) return 1;
        parts.push_back(std::move(*added));
        store.Commit(std::move(parts), "New map");
        check(terrain.LoadNow(32, 32, error) && terrain.HeightAt(32.5f * kTileSize, 32.5f * kTileSize) == 25.0f, "tile 32_32 loads, flat at 25 yd");
        const auto wdt = mpq.Read("World\\Maps\\" + folder + "\\" + folder + ".wdt");
        check(wdt && WdtBigAlpha(*wdt) && WdtHasTile(*wdt, 31, 31) && WdtHasTile(*wdt, 32, 32) && !WdtHasTile(*wdt, 33, 32), "its WDT lists exactly the four tiles");

        // Export: tiles, WDT, WDL, and the DBC rows.
        const auto out = dir / "out";
        std::vector<std::filesystem::path> files;
        terrain.Export(out / "client", error, &files);
        maps.Export({ out / "client" / "DBFilesClient" }, out / "dbc", error);
        difficulty.Export({ out / "client" / "DBFilesClient" }, out / "dbc", error);
        const auto mapDir = out / "client" / "World" / "Maps" / folder;
        check(std::filesystem::exists(mapDir / (folder + "_31_32.adt")) && std::filesystem::exists(mapDir / (folder + ".wdt")) &&
              std::filesystem::exists(mapDir / (folder + ".wdl")), "export writes the tiles, WDT and WDL (" + std::to_string(files.size()) + " files)");
        bool listed = false;
        if (std::ifstream f(out / "client" / "DBFilesClient" / "Map.dbc", std::ios::binary); f)
            for (const MapEntry& m : ParseMapDbc(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())))
                listed |= m.id == id && m.directory == folder && m.name == "Check Map";
        check(listed, "exported Map.dbc has the row");
        check(std::filesystem::exists(out / "client" / "DBFilesClient" / "MapDifficulty.dbc") && std::filesystem::exists(out / "dbc" / "Map.json"),
              "MapDifficulty.dbc and the mod-dbc-patch JSON written");

        store.Undo();
        check(!mpq.Read("World\\Maps\\" + folder + "\\" + folder + ".wdt") && maps.Row(id).is_null() && difficulty.Row(diffId).is_null() && terrain.Map().empty(),
              "undo removes the WDT, both rows, and closes the map");
        store.Redo();
        const auto again = mpq.Read("World\\Maps\\" + folder + "\\" + folder + ".wdt");
        check(again && WdtHasTile(*again, 32, 31) && maps.Row(id)["Directory"] == folder, "redo brings them back");
        std::filesystem::remove_all(dir / "overlay", ec);
        terrain.RebuildOverlay();
        const auto rebuilt = mpq.Read("World\\Maps\\" + folder + "\\" + folder + ".wdt");
        check(rebuilt && WdtHasTile(*rebuilt, 31, 32) && mpq.Read("World\\Maps\\" + folder + "\\" + folder + "_32_32.adt"), "reopening rebuilds the overlay");
        std::filesystem::remove_all(dir, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--swap-check <Data>`: Paint > Swap on Azeroth 32_48: a texture swapped for one the tile lacks, one merged into a
    /// texture its chunks already have (shares add up per texel), one removed; export validates; undo restores.
    int SwapCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        const int key = TileKey(32, 48);
        if (!terrain.SetMap("Azeroth", error) || !terrain.LoadNow(32, 48, error)) { printf("%s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        const LoadedTile& tile = terrain.Tiles().at(key);
        auto name = [&](const AdtChunk& c, uint32_t l) { return Catalog::Normalize(tile.adt.textures[c.textureIds[l]]); };
        auto layerOf = [&](const AdtChunk& c, const std::string& t) {
            for (uint32_t l = 0; l < c.layerCount; ++l)
                if (name(c, l) == Catalog::Normalize(t)) return int(l);
            return -1;
        };
        auto weight = [&](const AdtChunk& c, int layer, size_t texel) {
            const uint32_t n = c.layerCount;
            return LayerWeights(n > 1 ? c.alpha[texel * 4] / 255.0f : 0, n > 2 ? c.alpha[texel * 4 + 1] / 255.0f : 0, n > 3 ? c.alpha[texel * 4 + 2] / 255.0f : 0)[size_t(layer)];
        };
        std::vector<ChunkRef> all;
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) all.push_back({ key, int(ci) });
        std::vector<nlohmann::json> original;
        for (const AdtChunk& c : tile.adt.chunks) original.push_back(TerrainAdapter::LayerState(tile, c));
        int commits = 0;
        auto swap = [&](const std::string& from, const std::string& to) {
            auto change = terrain.SwapTexture(from, to, all, "swap check");
            if (change) { store.Commit(std::move(*change)); ++commits; }
            return change.has_value();
        };

        // A pair of textures sharing a chunk, for the merge; count uses of each texture.
        std::map<std::string, int> uses;
        std::string a, b;
        size_t pairChunk = 0;
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            const AdtChunk& c = tile.adt.chunks[ci];
            for (uint32_t l = 0; l < c.layerCount; ++l) ++uses[tile.adt.textures[c.textureIds[l]]];
            if (a.empty() && c.layerCount >= 3) { a = tile.adt.textures[c.textureIds[2]]; b = tile.adt.textures[c.textureIds[1]]; pairChunk = ci; }
        }
        check(!a.empty(), "a chunk with 3+ textures (" + std::to_string(pairChunk) + "): merge " + a + " into " + b);
        if (a.empty()) return 1;

        // Merge: a's share joins b's, texel by texel; one layer fewer.
        const AdtChunk& pc = tile.adt.chunks[pairChunk];
        const uint32_t layersBefore = pc.layerCount;
        std::vector<float> expected(4096);
        for (size_t i = 0; i < 4096; ++i) expected[i] = weight(pc, layerOf(pc, a), i) + weight(pc, layerOf(pc, b), i);
        check(swap(a, b), "merge swap");
        float worst = 0;
        for (size_t i = 0; i < 4096; ++i) worst = std::max(worst, std::fabs(weight(pc, layerOf(pc, b), i) - expected[i]));
        check(pc.layerCount == layersBefore - 1 && layerOf(pc, a) < 0 && worst < 0.03f, "one layer fewer, b's share = a + b (worst " + std::to_string(worst) + ")");

        // Plain swap: b becomes a texture the tile lacks; layer counts unchanged everywhere.
        const std::string dirt = "Tileset\\Barrens\\BarrensBaseDirt.blp";
        std::vector<uint32_t> counts;
        for (const AdtChunk& c : tile.adt.chunks) counts.push_back(c.layerCount);
        check(swap(b, dirt), "swap " + b + " for a new texture");
        bool same = true, gone = true;
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
        {
            same &= tile.adt.chunks[ci].layerCount == counts[ci];
            gone &= layerOf(tile.adt.chunks[ci], b) < 0;
        }
        check(same && gone && layerOf(pc, dirt) >= 0, "layer counts unchanged, the old texture gone, the new one in");

        // Remove: the most used texture goes wherever another texture can take its place.
        std::string most;
        for (const auto& [t, n] : uses)
            if (t != a && t != b && (most.empty() || n > uses[most])) most = t;
        check(swap(most, ""), "remove " + most);
        size_t kept = 0;
        for (const AdtChunk& c : tile.adt.chunks)
            if (layerOf(c, most) >= 0) { ++kept; if (c.layerCount != 1) { kept = 9999; break; } }
        check(kept != 9999, "left only where it was the chunk's only texture (" + std::to_string(kept) + " chunk(s))");

        const auto out = std::filesystem::temp_directory_path() / "wow-world-editor-swapcheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        terrain.Export(out, error);
        std::ifstream f(out / "World" / "Maps" / "Azeroth" / "Azeroth_32_48.adt", std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(!bytes.empty() && ValidateAdt(bytes, false).empty(), "exported tile validates");
        std::filesystem::remove_all(out, ec);

        for (int i = 0; i < commits; ++i) store.Undo();
        bool restored = true;
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) restored &= TerrainAdapter::LayerState(tile, tile.adt.chunks[ci]) == original[ci];
        check(restored, "undo restores every chunk's layers");
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--impass-check <Data>`: Holes > Impassable on Azeroth 32_48: the brush sets MCNK flag 0x2, export writes exactly
    /// those flags (every other header bit and chunk unchanged), clearing and undo restore.
    int ImpassCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) return 1;
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        store.Register(terrain);
        const int key = TileKey(32, 48);
        if (!terrain.SetMap("Azeroth", error) || !terrain.LoadNow(32, 48, error)) { printf("%s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        const LoadedTile& tile = terrain.Tiles().at(key);
        std::vector<uint32_t> before;
        for (const AdtChunk& c : tile.adt.chunks) before.push_back(c.flags);
        const XMFLOAT3 at{ 32.5f * kTileSize, 0, 48.5f * kTileSize };
        auto stroke = [&](bool set, float radius) {
            terrain.BeginImpass();
            terrain.ImpassStep(at, radius, set);
            auto change = terrain.EndImpass();
            if (change) { printf("     %s\n", change->label.c_str()); store.Commit(std::move(*change)); }
            return change.has_value();
        };
        check(stroke(true, 40), "mark chunks impassable");
        std::set<size_t> marked;
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci)
            if ((tile.adt.chunks[ci].flags & 0x2u) && !(before[ci] & 0x2u)) marked.insert(ci);
        check(marked.size() >= 4 && marked.size() < 30, std::to_string(marked.size()) + " chunks newly flagged");

        const auto out = std::filesystem::temp_directory_path() / "wow-world-editor-impasscheck";
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        terrain.Export(out, error);
        std::ifstream f(out / "World" / "Maps" / "Azeroth" / "Azeroth_32_48.adt", std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const auto back = bytes.empty() ? std::nullopt : ParseAdt(bytes, false);
        check(back && ValidateAdt(bytes, false).empty(), "exported tile validates");
        if (back)
        {
            size_t right = 0;
            for (size_t ci = 0; ci < back->chunks.size(); ++ci)
                right += back->chunks[ci].flags == (marked.count(ci) ? before[ci] | 0x2u : before[ci]);
            check(right == 256, "exported flags: the marked chunks gain 0x2, nothing else changes (" + std::to_string(right) + "/256)");
        }
        std::filesystem::remove_all(out, ec);

        check(stroke(false, 10), "Ctrl-stroke clears some");
        store.Undo();
        store.Undo();
        bool restored = true;
        for (size_t ci = 0; ci < tile.adt.chunks.size(); ++ci) restored &= tile.adt.chunks[ci].flags == before[ci];
        check(restored, "undo restores every chunk's flags");
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    int RoadCheck()
    {
        // `--road-check <data dir>`: a Barrens-style road across Kalimdor 40_30: shown over the ground while the tiles keep
        // it, exported into the ADT (validates, away from it untouched), gone on undo, and baked to the same result.
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        std::string error;
        if (!renderer.Init(device.Get(), context.Get(), error)) { printf("renderer: %s\n", error.c_str()); return 1; }
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        RoadStore roads(store);
        store.Register(terrain);
        store.Register(roads);
        terrain.SetRoads(&roads);
        roads.onChanged = [&](const std::string& map, const RoadStore::Cells& cells) { terrain.RefreshCells(map, cells); };
        if (!terrain.SetMap("Kalimdor", error) || !terrain.LoadNow(40, 30, error)) { printf("%s\n", error.c_str()); return 1; }
        int problems = 0;
        auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };

        // Across the tile through its middle, the points on the ground.
        const float z = 30.5f * kTileSize, x0 = 40 * kTileSize + 40, x1 = 41 * kTileSize - 40, xm = (x0 + x1) / 2;
        Road road;
        road.id = roads.NextId();
        road.map = "Kalimdor";
        road.width = 8;
        road.texture = "Tileset\\Barrens\\BarrensRoad01.blp";
        road.shoulderTexture = "Tileset\\Barrens\\BarrensBaseDirt.blp";
        for (float x : { x0, xm - 60, xm + 60, x1 }) road.points.push_back({ { x, *terrain.HeightAt(x, z + (x - xm) * 0.2f), z + (x - xm) * 0.2f } });
        const float plainMid = *terrain.HeightAt(xm, z);
        roads.Commit(nullptr, &road, "road check");
        check(*terrain.HeightAt(xm, z) == plainMid, "the tiles keep the ground under the road");
        check(std::fabs(*terrain.ShownHeightAt(xm, z) - plainMid) > 0.05f, "the road shows over it (" + std::to_string(*terrain.ShownHeightAt(xm, z) - plainMid) + " yd)");

        const std::filesystem::path out = std::filesystem::temp_directory_path() / "wow-world-editor-roadcheck";
        auto exportTile = [&]() -> std::optional<Adt> {
            std::error_code ec;
            std::filesystem::remove_all(out, ec);
            terrain.Export(out, error);
            std::ifstream f(out / "World" / "Maps" / "Kalimdor" / "Kalimdor_40_30.adt", std::ios::binary);
            if (!f) return std::nullopt;
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            check(ValidateAdt(bytes, false).empty(), "exported tile validates");
            return ParseAdt(bytes, false);
        };
        const auto original = ParseAdt(*mpq.Read("World\\Maps\\Kalimdor\\Kalimdor_40_30.adt"), false);
        const auto live = exportTile();
        check(live.has_value(), "export writes the tile");
        if (live && original)
        {
            const auto ref = terrain.ChunkAtGrid(int(std::floor(xm / kChunkSize)), int(std::floor(z / kChunkSize)));
            const AdtChunk& c = live->chunks[size_t(ref->chunk)];
            bool road01 = false;
            for (uint32_t l = 0; l < c.layerCount; ++l) road01 |= live->textures[c.textureIds[l]].find("BarrensRoad01") != std::string::npos;
            check(road01, "the chunk under the road has the road texture");
            check(c.heights != original->chunks[size_t(ref->chunk)].heights, "and its heights are graded");
            const AdtChunk& corner = live->chunks[0];   // the tile's corner, far from the road
            check(corner.heights == original->chunks[0].heights && corner.layerCount == original->chunks[0].layerCount, "a chunk away from the road is untouched");
        }
        store.Undo();
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        check(terrain.Export(out, error) == 0, "undo: nothing left to export");
        store.Redo();

        // Bake: the same ground as the live road, the road gone; undo brings both back.
        const Road saved = *roads.Saved(road.id);
        auto baked = terrain.BakeRoad(saved, "bake");
        Change remove = roads.MakeChange(&saved, nullptr, "bake");
        roads.Apply(remove);
        std::vector<Change> parts;
        if (baked) parts.push_back(std::move(*baked));
        parts.push_back(std::move(remove));
        store.Commit(std::move(parts), "bake");
        check(roads.OnMap("Kalimdor").empty(), "baked: no road left");
        const auto bakedTile = exportTile();
        if (bakedTile && live)
        {
            float worst = 0;
            for (size_t ci = 0; ci < live->chunks.size(); ++ci)
                for (size_t j = 0; j < 145; ++j) worst = std::max(worst, std::fabs(bakedTile->chunks[ci].heights[j] - live->chunks[ci].heights[j]));
            check(worst < 1e-3f, "baked heights match the live road's export (worst " + std::to_string(worst) + " yd)");
        }
        store.Undo();
        check(roads.OnMap("Kalimdor").size() == 1 && *terrain.HeightAt(xm, z) == plainMid, "undo bake: road back, ground as it was");

        // Speed on a long road: ~1000 yd over 3 x 3 tiles, 21 points. Moving one point redraws only the stretch around
        // it; a setting changes the whole road.
        for (int ty = 29; ty <= 31; ++ty)
            for (int tx = 39; tx <= 41; ++tx) terrain.LoadNow(tx, ty, error);
        Road longRoad = road;
        longRoad.id = roads.NextId();
        longRoad.points.clear();
        for (int i = 0; i <= 20; ++i)
        {
            const float px = 39.3f * kTileSize + i * 50.0f, pz = 30.5f * kTileSize + std::sin(i * 0.7f) * 60.0f;
            longRoad.points.push_back({ { px, terrain.HeightAt(px, pz).value_or(0), pz } });
        }
        roads.Commit(nullptr, &longRoad, "long road");
        size_t redrawn = 0;
        roads.onChanged = [&](const std::string& map, const RoadStore::Cells& cells) { redrawn = cells.size(); terrain.RefreshCells(map, cells); };
        auto timed = [&](const char* what, auto edit) {
            Road e = *roads.Saved(longRoad.id);
            edit(e);
            const auto t0 = std::chrono::steady_clock::now();
            roads.Preview(&e);
            const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
            printf("     %s: %zu chunks redrawn, %.1f ms\n", what, redrawn, ms);
            roads.Preview(nullptr);
            return ms;
        };
        timed("move one point", [](Road& e) { e.points[10].pos.z += 8; });
        timed("change the width", [](Road& e) { e.width += 1; });
        check(redrawn > 0, "long road redraws");
        std::filesystem::remove_all(out, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }
}
