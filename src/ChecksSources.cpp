// Command-line checks: Sources and output, and the file tools. See docs/development/checks.md.

#include "Checks.hpp"

namespace checks
{
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
        {
            // A project's installed patch is its own output: its base leaves it out, so an exported row does not come
            // back as the client's. A Data-like folder: the client's Map.dbc in patch.MPQ, the project's patch with
            // one more row in enUS.
            MpqChain client;
            client.Open(data);
            std::vector<uint8_t> dbc = client.Read(shared).value_or(std::vector<uint8_t>{});
            const size_t rows = ParseMapDbc(dbc).size();
            expect(dbc.size() > 20 && rows > 0, "the client's Map.dbc read");
            if (dbc.size() > 20)
            {
                put(t / "own" / "src-client" / "DBFilesClient" / "Map.dbc", std::string(dbc.begin(), dbc.end()));
                uint32_t count, size;
                memcpy(&count, &dbc[4], 4);
                memcpy(&size, &dbc[12], 4);
                std::vector<uint8_t> row(dbc.begin() + 20, dbc.begin() + 20 + size);   // the first row again, as map 60000
                const uint32_t id = 60000;
                memcpy(row.data(), &id, 4);
                dbc.insert(dbc.begin() + 20 + size_t(count) * size, row.begin(), row.end());
                ++count;
                memcpy(&dbc[4], &count, 4);
                put(t / "own" / "src-patch" / "DBFilesClient" / "Map.dbc", std::string(dbc.begin(), dbc.end()));
            }
            Project p;
            p.clientDir = (t / "own").string();
            p.patchName = "patch-enUS-X.MPQ";
            expect(WriteMpq(t / "own" / "Data" / "patch.MPQ", t / "own" / "src-client", error) &&
                       WriteMpq(p.PatchInstallPath(), t / "own" / "src-patch", error),
                   "a client archive and the project's installed patch built");
            auto hasRow = [&](const MpqChain& c) {
                const auto maps = ParseMapDbc(c.Read(shared).value_or(std::vector<uint8_t>{}));
                return std::any_of(maps.begin(), maps.end(), [](const MapEntry& m) { return m.id == 60000; });
            };
            std::string spelled = p.PatchInstallPath().string();   // another spelling of the same file
            for (char& ch : spelled) ch = ch == '\\' ? '/' : char(std::toupper((unsigned char)ch));
            MpqChain c;
            c.Open({ { MpqLayer::Kind::MpqFolder, p.clientDir } });
            expect(hasRow(c), "without the skip the patch's row is seen");
            c.Open({ { MpqLayer::Kind::MpqFolder, p.clientDir } }, { spelled });
            expect(!hasRow(c) && ParseMapDbc(c.Read(shared).value_or(std::vector<uint8_t>{})).size() == rows && c.Report()[0].archives == 1,
                   "skipping the installed patch (any case, any slash): the client's rows only");
            c.Open({ { MpqLayer::Kind::MpqFile, p.PatchInstallPath().string() } }, { p.PatchInstallPath() });
            expect(c.Report()[0].archives == 0 && !c.Read(shared), "the patch as a single-MPQ layer is skipped too");
            Ghosts ghosts;   // a compare source on the same client: its files, not the project's export
            std::vector<std::string> errors;
            ghosts.Reset(&c, "project", { { "same client", { { MpqLayer::Kind::MpqFolder, p.clientDir } } } }, errors, { spelled });
            expect(ghosts.Sources().size() == 2 && !hasRow(*ghosts.Sources()[1].mpq), "a compare source on the same client skips it too");
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

    int ExportCmd()
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
        mpq.Open(project->base.layers, { project->PatchInstallPath() });
        Ghosts ghosts;
        std::vector<std::string> errors;
        std::vector<std::pair<std::string, std::vector<MpqLayer>>> compare;
        for (const Project::Source& s : project->compare) compare.push_back({ s.name, s.layers });
        ghosts.Reset(&mpq, project->name, compare, errors, { project->PatchInstallPath() });
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

    int WhereCmd()
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

    int CdnCheck()
    {
        // `--cdn-check <install>*<product> <game path>`: in the window's mode a CASC file missing on disk misses at
        // once and arrives in the background; then it reads.
        char dir[1024] = {}, name[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dir, sizeof dir, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, __wargv[3], -1, name, sizeof name, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dir);
        SetCdnAsync(true);
        const auto t0 = std::chrono::steady_clock::now();
        auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        const uint64_t before = CdnArrivals();
        const bool first = mpq.Read(name).has_value();
        printf("first read: %s in %.0f ms\n", first ? "on disk" : "missed (queued)", ms());
        if (first) return 0;
        while (CdnArrivals() == before && ms() < 300000) Sleep(50);
        printf("arrived after %.0f ms\n", ms());
        const auto second = mpq.Read(name);
        printf("second read: %s, %zu bytes, %.0f ms total\n", second ? "ok" : "FAILED", second ? second->size() : 0, ms());
        return second ? 0 : 1;
    }

    int ExtractCmd()
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

    int FindCmd()
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
}
