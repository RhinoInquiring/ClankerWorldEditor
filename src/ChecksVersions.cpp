// Command-line checks: Versions: ghosts, comparing, differences, added tiles. See docs/development/checks.md.

#include "Checks.hpp"

namespace checks
{
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
        store.Register(terrain);
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
        // A paste in place leaves the tile's objects as the version has them (the map's others removed); undo puts them back.
        for (const auto& l : ghosts.Layers())
        {
            const AreaDiff before = CompareArea(terrain.Tiles(), l.tiles, whole);
            if (before.newDoodads.empty() && before.newWmos.empty() && before.gone.empty()) continue;
            TerrainClipboard clip = TerrainAdapter::CopyFrom(l.tiles, whole);
            SetAreaObjects(clip, before);
            auto change = terrain.ApplyPlan(terrain.PlanPaste(clip, clip.originX, clip.originZ, 0.0f, PasteOptions{}), "replace check");
            if (!change) { printf("  replace %s: nothing pasted\n", l.label.c_str()); ++problems; break; }
            store.Commit(std::move(*change));
            const AreaDiff after = CompareArea(terrain.Tiles(), l.tiles, whole);
            // Revert to the client: the tile is the unedited one again (same difference from the version as before the paste).
            size_t skipped = 0;
            auto revert = RevertToClient(terrain, mpq, whole, skipped);
            if (!revert) { printf("  revert: nothing reverted\n"); ++problems; break; }
            store.Commit(std::move(*revert));
            const AreaDiff reverted = CompareArea(terrain.Tiles(), l.tiles, whole);
            printf("  revert to client: %zu chunk(s) differ (were %zu), objects +%zu -%zu, %zu skipped\n", reverted.changed, before.changed,
                   reverted.newDoodads.size() + reverted.newWmos.size(), reverted.gone.size(), skipped);
            if (reverted.changed != before.changed || reverted.newDoodads.size() != before.newDoodads.size() || reverted.gone.size() != before.gone.size() ||
                std::fabs(reverted.maxHeight - before.maxHeight) > 0.01f)
                ++problems;
            store.Undo();
            store.Undo();
            const AreaDiff undone = CompareArea(terrain.Tiles(), l.tiles, whole);
            printf("  replace %s: before +%zu -%zu, after +%zu -%zu, undone +%zu -%zu\n", l.label.c_str(),
                   before.newDoodads.size() + before.newWmos.size(), before.gone.size(), after.newDoodads.size() + after.newWmos.size(),
                   after.gone.size(), undone.newDoodads.size() + undone.newWmos.size(), undone.gone.size());
            // ponytail: a building whose bounds only graze the tile can stay counted (its added copy is clipped to loaded tiles).
            if (after.gone.size() || after.newDoodads.size()) ++problems;
            if (undone.gone.size() != before.gone.size() || undone.newDoodads.size() != before.newDoodads.size()) ++problems;
            break;
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
        if (std::getenv("WWE_ALL"))
        {
            // Every tile only the other map has, one at a time: how many go in, and why the rest do not.
            std::map<std::string, std::vector<std::string>> why;
            size_t added = 0, tried = 0;
            for (int k = 0; k < 4096; ++k)
            {
                if (!theirs[size_t(k)] || ours[size_t(k)]) continue;
                ++tried;
                const std::string name = std::to_string(k % 64) + "_" + std::to_string(k / 64);
                const auto t = TerrainAdapter::ReadNewTile(mpq, other, k % 64, k / 64);
                if (!t) { why["not readable"].push_back(name); continue; }
                std::string err;
                if (auto c = terrain.AddTiles({ *t }, otherBig, err)) { ++added; store.Commit(std::move(*c)); continue; }
                const std::string reason = err.substr(err.find(':') == std::string::npos ? 0 : err.find(':') + 2);
                why[reason.substr(0, 90)].push_back(name);
            }
            printf("%zu tile(s) only %s has: %zu added\n", tried, other.c_str(), added);
            for (const auto& [reason, names] : why)
            {
                printf("  %zu refused: %s  e.g.", names.size(), reason.c_str());
                for (size_t i = 0; i < names.size() && i < 6; ++i) printf(" %s", names[i].c_str());
                printf("\n");
            }
            return added == tried ? 0 : 1;
        }
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
}
