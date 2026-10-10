// Command-line checks: Tiles, structure, streaming and timing. See docs/development/checks.md.

#include "Checks.hpp"

namespace checks
{
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

    /// `--stream-check <project dir> <map> <x> <y> [tiles] [nofallback]`: opens the project the way the window does (base
    /// layers, compare sources attached as fallbacks, CDN in the background), then walks `tiles` tiles east from (x, y)
    /// loading terrain (radius 2) and models like the viewport: time per step and read counters, to find what is slow.
    int StreamCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        std::string error;
        const auto project = Project::Load(arg(2), error);
        if (!project) { printf("%s\n", error.c_str()); return 1; }
        const std::string map = arg(3);
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5)), steps = __argc > 6 ? std::stoi(arg(6)) : 4;
        const bool fallbacks = !(__argc > 7 && arg(7) == "nofallback");
        auto now = [] { return std::chrono::steady_clock::now(); };
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

        auto t = now();
        MpqChain mpq;
        mpq.Open(project->base.layers, { project->PatchInstallPath() });
        mpq.SetMapsFromLowestLayer(true);
        mpq.SetOverlay(project->dir / "overlay");
        printf("base open: %.0f ms\n", ms(t, now()));
        for (size_t i = 0; i < mpq.Report().size() && i < project->base.layers.size(); ++i)
            printf("  layer %s: %.0f ms, %zu archive(s), %zu file(s) %s\n", project->base.layers[i].path.c_str(), mpq.Report()[i].ms,
                   mpq.Report()[i].archives, mpq.Report()[i].files, mpq.Report()[i].note.c_str());
        Ghosts ghosts;
        if (fallbacks)
        {
            t = now();
            std::vector<std::pair<std::string, std::vector<MpqLayer>>> compare;
            for (const Project::Source& s : project->compare) compare.push_back({ s.name, s.layers });
            std::vector<std::string> errors;
            ghosts.Reset(&mpq, project->name, compare, errors);
            std::vector<const MpqChain*> chains;
            for (size_t i = 1; i < ghosts.Sources().size(); ++i) chains.push_back(ghosts.Sources()[i].mpq);
            mpq.SetFallbacks(chains);
            printf("compare sources attached as fallbacks: %zu, %.0f ms\n", chains.size(), ms(t, now()));
        }
        SetCdnAsync(true);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
        Renderer renderer;
        ModelRenderer models;
        if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) { printf("%s\n", error.c_str()); return 1; }
        ChangeStore store;
        TerrainAdapter terrain(mpq, renderer, store);
        if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return 1; }
        MpqStats last = GetMpqStats();
        for (int step = 0; step < steps; ++step)
        {
            const float x = (tx + step + 0.5f) * kTileSize, z = (ty + 0.5f) * kTileSize;
            t = now();
            size_t tiles = 0;
            for (int i = 0; i < 40; ++i)
            {
                const auto loaded = terrain.Stream(x, z, 2, error, 1000.0f);
                tiles += loaded.size();
                if (loaded.empty() && i > 2) break;
            }
            const double terrainMs = ms(t, now());
            t = now();
            size_t added = 0;
            for (const auto& [key, tile] : terrain.Tiles())
                if (!models.HasTile(key)) { models.AddTile(key, tile.adt, mpq); ++added; }
            const double modelMs = ms(t, now());
            const MpqStats s = GetMpqStats();
            printf("step %d (tile %d_%d): %zu terrain tile(s) %.0f ms, models of %zu tile(s) %.0f ms | fallback reads %llu (hits %llu, %.0f ms), "
                   "CASC reads %llu, not on disk %llu, CDN queued %llu\n",
                   step, tx + step, ty, tiles, terrainMs, added, modelMs, s.fallbackReads - last.fallbackReads, s.fallbackHits - last.fallbackHits,
                   (s.fallbackMicros - last.fallbackMicros) / 1000.0, s.cascReads - last.cascReads, s.cascLocalMisses - last.cascLocalMisses,
                   s.cdnQueued - last.cdnQueued);
            last = s;
        }
        printf("first fallback names:\n");
        for (const auto& n : GetMpqStats().fallbackNames) printf("  %s\n", n.c_str());
        return 0;
    }

    /// `--loader-check <data dir> <map> <x> <y> [tiles] [yd/s]`: streams through the background loader as the window does,
    /// once with one worker and once with as many as the editor starts. First arrives at (x, y) with nothing loaded (a
    /// jump on the map) and waits until the 5 x 5 tiles around it and their models are in; then flies `tiles` (default 6)
    /// tiles east (default 2000 yd/s). Prints the arrival time and frame times on this thread (software device: slower
    /// than a GPU's); fails when the two runs end with different tiles or objects.
    int LoaderCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::string map = arg(3);
        const int tx = std::stoi(arg(4)), ty = std::stoi(arg(5)), span = __argc > 6 ? std::stoi(arg(6)) : 6;
        const float speed = __argc > 7 ? std::stof(arg(7)) : 2000.0f;
        MpqChain mpq;
        mpq.Open(arg(2));
        struct Result { std::set<int> tiles; std::multiset<std::tuple<bool, uint32_t, int, int, int>> objects; };
        auto run = [&](unsigned threads) -> std::optional<Result> {
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
                return std::nullopt;
            Renderer renderer;
            ModelRenderer models;
            std::string error;
            if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) return std::nullopt;
            ChangeStore store;
            TerrainAdapter terrain(mpq, renderer, store);
            Loader loader;
            loader.Start(&mpq, threads);
            renderer.SetLoader(&loader);
            models.SetLoader(&loader);
            terrain.SetLoader(&loader);
            if (!terrain.SetMap(map, error)) { printf("%s\n", error.c_str()); return std::nullopt; }
            std::vector<float> frames;
            // One paced frame as App::Frame streams: one shared budget, terrain first, then models. True once all is in.
            auto frame = [&](float x, float z) {
                const auto t0 = std::chrono::steady_clock::now();
                auto ms = [&] { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
                terrain.Stream(x, z, 2, error, 4.0f);
                for (int key : models.TileKeys())
                    if (!terrain.Tiles().count(key)) models.RemoveTile(key);
                bool modelsDone = true;
                for (const auto& [key, tile] : terrain.Tiles())
                    if (!models.HasTile(key))
                    {
                        if (ms() > 8.0f) { modelsDone = false; break; }
                        models.AddTile(key, tile.adt, mpq);
                    }
                frames.push_back(ms());
                std::this_thread::sleep_until(t0 + std::chrono::microseconds(16667));   // frame pacing: the workers run meanwhile
                return !loader.Pending() && modelsDone;
            };
            const float z = (ty + 0.5f) * kTileSize, endX = (tx + span + 0.5f) * kTileSize;
            const auto start = std::chrono::steady_clock::now();
            while (!frame((tx + 0.5f) * kTileSize, z))
                if (frames.size() > 20000) { printf("did not finish\n"); return std::nullopt; }
            const float arrival = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            for (float x = (tx + 0.5f) * kTileSize; !frame(x, z) || x < endX; x = std::min(endX, x + speed / 60))
                if (frames.size() > 20000) { printf("did not finish\n"); return std::nullopt; }
            loader.Stop();
            Result r;
            for (const auto& [key, tile] : terrain.Tiles()) r.tiles.insert(key);
            models.ForEachObject([&](bool wmo, uint32_t uid, const XMFLOAT3& c) { r.objects.insert({ wmo, uid, int(c.x), int(c.y), int(c.z) }); });
            std::vector<float> sorted = frames;
            std::sort(sorted.begin(), sorted.end());
            printf("%u worker(s): arrival loaded in %.2f s, %zu frames, p95 %.1f ms, worst %.1f ms, %zu tiles, %zu objects\n", threads, arrival,
                   frames.size(), sorted[sorted.size() * 95 / 100], sorted.back(), r.tiles.size(), r.objects.size());
            return r;
        };
        const unsigned many = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
        const auto one = run(1), all = run(many);
        if (!one || !all) return 1;
        const bool same = one->tiles == all->tiles && one->objects == all->objects;
        printf("same tiles and objects: %s\n", same ? "yes" : "NO");
        return same ? 0 : 1;
    }

    int GroundEffectsCmd()
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

    int ValidateRefsCmd()
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

    int ValidateCmd()
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

    int NormalsCheck()
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

    int PerfCheck()
    {
        // `--perf-check <data dir>`: what a long compare session costs: 60 blended 4 x 4 pastes on Elwynn, then the work
        // that runs with that history: the Maps panel's edited tiles (every frame), a tile load replaying the edits,
        // a fresh object id, and one more paste.
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
        if (!terrain.SetMap("Azeroth", error)) return 1;
        for (int ty = 47; ty <= 49; ++ty)
            for (int tx = 31; tx <= 33; ++tx) terrain.LoadNow(tx, ty, error);
        auto ms = [](auto t0) { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        std::set<ChunkRef> sel;
        for (int dz = 0; dz < 4; ++dz)
            for (int dx = 0; dx < 4; ++dx) sel.insert(*terrain.ChunkAtGrid(32 * 16 + 2 + dx, 48 * 16 + 2 + dz));
        const TerrainClipboard clip = terrain.Copy(sel);
        float lastPaste = 0;
        for (int i = 0; i < 61; ++i)
        {
            const int gx = 31 * 16 + 2 + (i % 8) * 5, gz = 47 * 16 + 2 + (i / 8) * 5;
            const auto t0 = std::chrono::steady_clock::now();
            const PastePlan plan = terrain.PlanPaste(clip, gx, gz, 0.0f, PasteOptions{});
            if (auto c = terrain.ApplyPlan(plan, "perf paste")) store.Commit(std::move(*c));
            lastPaste = ms(t0);
            if (i == 0) printf("first paste: %.1f ms\n", lastPaste);
        }
        printf("61st paste: %.1f ms (%zu changes)\n", lastPaste, store.Done().size());
        auto t0 = std::chrono::steady_clock::now();
        terrain.EditedTiles("Azeroth");
        printf("edited tiles, first time (each change's tiles worked out once): %.2f ms\n", ms(t0));
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) terrain.EditedTiles("Azeroth");
        printf("edited tiles after that (Maps panel, every frame): %.3f ms\n", ms(t0) / 100);
        t0 = std::chrono::steady_clock::now();
        LoadedTile tile = terrain.Tiles().at(TileKey(32, 48));
        TerrainAdapter::ReplayEdits(tile, "Azeroth", store.Done());
        printf("replay every edit on a tile: %.2f ms\n", ms(t0));
        t0 = std::chrono::steady_clock::now();
        std::vector<size_t> touching;   // as a tile load does it: only the changes touching the tile
        for (size_t i = 0; i < store.Done().size(); ++i)
            if (TerrainAdapter::TilesOf(store.Done()[i])["Azeroth"].count(TileKey(33, 49))) touching.push_back(i);
        LoadedTile corner = terrain.Tiles().at(TileKey(33, 49));
        TerrainAdapter::ReplayEdits(corner, "Azeroth", store.Done(), &touching);
        printf("tile load replay (a tile %zu of the changes touch, index built cold): %.2f ms\n", touching.size(), ms(t0));
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; ++i) TerrainAdapter::EditHashes(store.Done(), "Azeroth");
        printf("edit hashes (minimaps, differences): %.2f ms\n", ms(t0) / 10);
        return 0;
    }
}
