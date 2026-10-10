// Command-line checks: Models, looks, the catalog, assets and drawing. See docs/development/checks.md.

#include "Checks.hpp"

namespace checks
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
            for (uint32_t g = 0; g < groups; ++g) groupFiles.push_back(mpq.Read(WmoGroupFile(w.model, *root, g)).value_or(std::vector<uint8_t>{}));
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

    /// `--casc-to-mpq <install*product> <out dir> [map ...]`: a CASC client's maps (every one, or the named folders) as
    /// 3.3.5a files: merged ADTs, WDTs without FileDataIDs, WDLs as they are, and every model and texture they use
    /// (newer formats converted, as export does). Written under <out dir>\staging (a rerun skips what is there), then
    /// packed into <out dir>\<out dir name>.MPQ (format 4: the editor reads it, the 3.3.5a client does not).
    int CascToMpq()
    {
        if (__argc < 4 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto seconds = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };

        MpqChain mpq;
        if (!mpq.Open(arg(2))) { printf("Cannot open %s\n", arg(2).c_str()); return 1; }
        const fs::path out = fs::path(__wargv[3]), stage = out / "staging";
        std::set<std::string> only;
        for (int i = 4; i < __argc; ++i) only.insert(lower(arg(i)));

        // Maps: every World\Maps\<dir>\<dir>.wdt the build has.
        std::vector<std::string> maps;
        for (const MpqChain::Entry& e : mpq.List())
        {
            const std::string name = lower(e.name);
            if (name.rfind("world\\maps\\", 0) != 0 || !name.ends_with(".wdt")) continue;
            const std::string rest = e.name.substr(11, e.name.size() - 15);   // <dir>\<dir>
            const size_t slash = rest.find('\\');
            if (slash == std::string::npos || lower(rest.substr(0, slash)) != lower(rest.substr(slash + 1))) continue;
            if (only.empty() || only.count(lower(rest.substr(0, slash)))) maps.push_back(rest.substr(0, slash));
        }
        std::sort(maps.begin(), maps.end());
        printf("%zu map(s) (%.0f s to open)\n", maps.size(), seconds());

        auto write = [&](const std::string& game, const std::vector<uint8_t>& bytes) {
            std::string rel = game;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const fs::path p = stage / fs::path(rel);
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            return p;
        };
        auto staged = [&](const std::string& game) {
            std::string rel = game;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            return stage / fs::path(rel);
        };
        const FileIdName nameOf = [&](uint32_t id) { return mpq.NameOf(id); };

        std::vector<fs::path> files;   // tiles and WDTs: where the asset walk starts
        std::vector<std::string> notes;
        size_t tilesTotal = 0, tilesMissing = 0;
        for (size_t m = 0; m < maps.size(); ++m)
        {
            const std::string& dir = maps[m];
            const std::string base = "World\\Maps\\" + dir + "\\" + dir;
            const auto wdt = mpq.Read(base + ".wdt");
            if (!wdt) { notes.push_back(dir + ": WDT unreadable"); continue; }
            const std::vector<bool> listed = WdtTiles(*wdt);
            std::set<int> have;
            size_t missing = 0;
            for (int k = 0; k < int(listed.size()); ++k)
            {
                if (!listed[size_t(k)]) continue;
                const std::string adt = base + "_" + std::to_string(k % 64) + "_" + std::to_string(k / 64) + ".adt";
                std::error_code ec;
                fs::path p = staged(adt);
                if (!fs::is_regular_file(p, ec))
                {
                    const auto bytes = mpq.Read(adt);   // split tiles come back merged
                    if (!bytes) { ++missing; continue; }
                    p = write(adt, *bytes);
                }
                have.insert(k);
                files.push_back(p);
            }
            std::vector<std::string> wdtNotes;
            const auto converted = DowngradeWdt(*wdt, nameOf, [&](int x, int y) { return have.count(y * 64 + x) != 0; }, &wdtNotes);
            if (converted.empty()) { notes.push_back(dir + ": WDT has no MAIN"); continue; }
            files.push_back(write(base + ".wdt", converted));
            for (const auto& n : wdtNotes) notes.push_back(dir + ": " + n);
            if (const auto wdl = mpq.Read(base + ".wdl")) write(base + ".wdl", *wdl);
            tilesTotal += have.size();
            tilesMissing += missing;
            if (missing) notes.push_back(dir + ": " + std::to_string(missing) + " listed tile(s) unreadable");
            printf("[%zu/%zu] %-32s %4zu tile(s)%s  (%.0f s)\n", m + 1, maps.size(), dir.c_str(), have.size(),
                   missing ? (", " + std::to_string(missing) + " unreadable").c_str() : "", seconds());
        }

        printf("Models and textures...\n");
        const AssetReport report = CopyMissingAssets(mpq, files, stage, true);
        printf("%zu file(s) written, %zu converted, %zu missing  (%.0f s)\n", report.copied.size(), report.converted.size(),
               report.missing.size(), seconds());
        {
            std::ofstream r(out / "report.txt");
            r << "Maps " << maps.size() << ", tiles " << tilesTotal << " (" << tilesMissing << " unreadable)\n";
            for (const auto& n : notes) r << n << "\n";
            r << "\nMissing (" << report.missing.size() << "):\n";
            for (const auto& n : report.missing) r << n << "\n";
            r << "\nConversion notes (" << report.notes.size() << "):\n";
            for (const auto& n : report.notes) r << n << "\n";
        }

        const fs::path archive = out / (out.filename().string() + ".MPQ");
        printf("Packing %s...\n", archive.string().c_str());
        std::string error;
        size_t packed = 0;
        if (!WriteMpq(archive, stage, error, &packed, true)) { printf("%s\n", error.c_str()); return 1; }
        printf("Done: %zu map(s), %zu tile(s), %zu file(s) in %s  (%.0f s). Report: %s\n", maps.size(), tilesTotal, packed,
               archive.string().c_str(), seconds(), (out / "report.txt").string().c_str());
        return 0;
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
        // "<install>*<product>": a CASC storage, as for the base.
        const std::string other = arg(3);
        const size_t star = other.find('*');
        const MpqLayer otherLayer = star == std::string::npos ? MpqLayer{ MpqLayer::Kind::MpqFolder, other }
                                                              : MpqLayer{ MpqLayer::Kind::Casc, other.substr(0, star), true, false, "", "", other.substr(star + 1) };
        ghosts.Reset(&mpq, "base", { { "other", { otherLayer } } }, errors);
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
        printf("converted for 3.3.5a: %zu file(s)\n", report.converted.size());
        for (size_t i = 0; i < report.notes.size() && i < 20; ++i) printf("  ~ %s\n", report.notes[i].c_str());
        if (report.notes.size() > 20) printf("  ~ ... %zu more\n", report.notes.size() - 20);
        size_t stillNewer = 0;   // every converted file must now be in 3.3.5a shape
        for (const std::string& c : report.converted)
        {
            std::string rel = c;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            std::ifstream f(out / std::filesystem::path(rel), std::ios::binary);
            const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (IsNewerFormat(c, b)) { ++stillNewer; printf("  still newer: %s\n", c.c_str()); }
        }

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
                const bool optional = lower.ends_with("_s.blp") || lower.ends_with("01.skin") || lower.ends_with("02.skin") || lower.ends_with("03.skin") || lower.ends_with(".anim");
                if (!b) { if (!optional && std::find(report.missing.begin(), report.missing.end(), r) == report.missing.end()) { ++unresolved; printf("  unresolved: %s\n", r.c_str()); } continue; }
                ++checked;
                todo.push_back({ r, std::move(*b) });
            }
        }
        std::filesystem::remove_all(out, ec);
        printf("verification: %zu references resolve from the base client plus the export, %zu unresolved\n", checked, unresolved);
        return unresolved || stillNewer ? 1 : 0;
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
        // WWE_ROAD=1: a Barrens road curving through where the camera looks (Roads tool defaults), to see its look.
        RoadStore roads(store);
        if (std::getenv("WWE_ROAD") && pitch < 0)
        {
            const float reach = above / std::sin(-p);   // to the ground ahead
            const float cx = eye.x + XMVectorGetX(fwd) * reach, cz = eye.z + XMVectorGetZ(fwd) * reach;
            const float sx = std::cos(y), sz = -std::sin(y);   // across the view
            Road r;
            r.id = 1;
            r.map = arg(3);
            r.texture = "Tileset\\Barrens\\BarrensRoad01.blp";
            r.shoulderTexture = "Tileset\\Barrens\\BarrensBaseDirt.blp";
            for (int i = -3; i <= 3; ++i)   // an S across the view
            {
                const float a = i * 25.0f, bend = std::sin(i * 0.9f) * 18.0f;
                const float px = cx + sx * a + XMVectorGetX(fwd) * bend, pz = cz + sz * a + XMVectorGetZ(fwd) * bend;
                r.points.push_back({ { px, terrain.HeightAt(px, pz).value_or(0), pz } });
            }
            terrain.SetRoads(&roads);
            roads.onChanged = [&](const std::string& map, const RoadStore::Cells& cells) { terrain.RefreshCells(map, cells); };
            roads.Commit(nullptr, &r, "render road");
        }
        const UINT w = 1280, h = 720;
        // WWE_TOPDOWN=<height>: the editor's top-down view (flat, straight down from that high over the eye's ground).
        const float topDown = std::getenv("WWE_TOPDOWN") ? float(std::atof(std::getenv("WWE_TOPDOWN"))) : 0.0f;
        const float tall = 2.0f * topDown * std::tan(XMConvertToRadians(30.0f));
        const XMFLOAT3 ground{ eye.x, eye.y - above, eye.z }, high{ eye.x, eye.y - above + topDown, eye.z };
        const XMMATRIX viewProj = topDown > 0
            ? XMMatrixLookToRH(XMLoadFloat3(&high), XMVectorSet(0, -1, 0, 0), XMVectorSet(std::sin(XMConvertToRadians(yaw)), 0, std::cos(XMConvertToRadians(yaw)), 0)) *
                  XMMatrixOrthographicRH(tall * w / h, tall, -3000.0f, std::max(6000.0f, topDown + 3000.0f))
            : XMMatrixLookToRH(XMLoadFloat3(&eye), fwd, XMVectorSet(0, 1, 0, 0)) * XMMatrixPerspectiveFovRH(XMConvertToRadians(60.0f), float(w) / h, 1.0f, 6000.0f);

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
        options.eye = topDown > 0 ? ground : eye;
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
        models.Draw(viewProj, options.eye, settings);
        {
            const auto m0 = std::chrono::steady_clock::now();   // CPU side of a model frame (culling, bone posing, uploads)
            for (int i = 0; i < 5; ++i) models.Draw(viewProj, options.eye, settings);
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

    int MapPreviewCmd()
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

    int AnimCheck()
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

    /// `--race-check <data dir> [other data dir...]`: every race of each client, read in its own layout (1.12 or 3.3.5):
    /// identity, classes, the choices per sex, and the textures its CharSections name that the client does not have (stock
    /// 3.3.5 has some too; the client skips them). Fails when a client's races cannot be read, or a race characters can be
    /// (CharBaseInfo) has no character model for a sex, or one its client lacks.
    int RaceCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        int problems = 0;
        for (int i = 2; i < __argc; ++i)
        {
            const std::string dir = std::filesystem::path(__wargv[i]).string();
            MpqChain mpq;
            mpq.Open(dir);
            RaceCatalog races;
            std::string error;
            if (!races.Load(mpq, error)) { printf("%s: %s\n", dir.c_str(), error.c_str()); ++problems; continue; }
            printf("%s: %s layout, %zu races, %zu CharSections, %zu hair styles, %zu facial hair, %zu race/class pairs, %zu outfits\n",
                   dir.c_str(), RaceCatalog::LayoutName(races.Format()), races.Races().size(), races.Sections().size(), races.HairGeosets().size(),
                   races.FacialHairStyles().size(), races.BaseInfo().size(), races.Outfits().size());
            for (const RaceCatalog::Race& r : races.Races())
            {
                const RaceCatalog::Counts m = races.Count(r.id, 0), f = races.Count(r.id, 1);
                std::string modelProblem;
                for (uint32_t sex = 0; sex < 2; ++sex)
                {
                    const std::string model = races.Model(r.id, sex);
                    if (model.empty()) modelProblem += sex ? " no female model" : " no male model";
                    else if (!mpq.HasOwn(model)) modelProblem += " missing " + model;
                }
                size_t missing = 0;
                for (const std::string& file : races.Files(r.id))
                    if (!mpq.HasOwn(file)) ++missing;
                std::string classes;
                for (uint32_t c : races.Classes(r.id)) classes += (classes.empty() ? "" : ",") + std::to_string(c);
                const bool playable = !classes.empty();
                printf("  %3u %-22s %-18s %-3s %s  M %zu/%zu/%zu/%zu/%zu  F %zu/%zu/%zu/%zu/%zu  classes %s%s%s%s\n", r.id, r.name.c_str(),
                       r.fileString.c_str(), r.prefix.c_str(), r.alliance < 0 ? "?" : r.alliance ? "H" : "A", m.skins, m.faces, m.hairStyles,
                       m.hairColors, m.facialHair, f.skins, f.faces, f.hairStyles, f.hairColors, f.facialHair, classes.empty() ? "-" : classes.c_str(),
                       missing ? (", " + std::to_string(missing) + " texture(s) missing").c_str() : "", modelProblem.empty() ? "" : " |",
                       modelProblem.c_str());
                if (playable && !modelProblem.empty()) ++problems;
            }
        }
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--race-look <data dir> <race> <sex> <out.png>`: a race's character as the Races window previews it (DisplayLooks::
    /// CharacterLook with the first of each choice): its model must load, every texture it wears must be readable, and the
    /// composited body skin is saved to the PNG, and the head drawn from four sides (software device) to <out>.model.png.
    /// With `all` after the PNG, every skin colour of the race and sex is tried.
    int RaceLookCheck()
    {
        if (__argc < 6 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) { return std::filesystem::path(__wargv[i]).string(); };
        MpqChain mpq;
        mpq.Open(arg(2));
        const uint32_t race = uint32_t(_wtoi(__wargv[3])), sex = uint32_t(_wtoi(__wargv[4]));
        const bool all = __argc > 6 && arg(6) == "all";
        std::map<std::string, BlpImage> composed;
        DisplayLooks looks(mpq);
        looks.SetUpload([&](const std::string& name, const BlpImage& image) { composed[name] = image; });
        DisplayLooks::Choices c = looks.CharacterChoices(race, sex, 0, 0);
        if (c.skins.empty()) { printf("race %u sex %u has no skin colours\n", race, sex); return 1; }
        int problems = 0;
        std::optional<BlpImage> first;
        std::optional<DisplayLooks::SpawnModel> firstLook;
        for (size_t k = 0; k < (all ? c.skins.size() : 1); ++k)
        {
            const uint32_t skin = c.skins[k];
            const DisplayLooks::Choices cs = looks.CharacterChoices(race, sex, skin, c.hairStyles.empty() ? 0 : c.hairStyles.front());
            auto pick = [](const std::vector<uint32_t>& list) { return list.empty() ? 0u : list.front(); };
            const auto look = looks.CharacterLook(race, sex, skin, pick(cs.faces), pick(cs.hairStyles), pick(cs.hairColors), pick(cs.facialHair));
            if (!look) { printf("race %u sex %u: no character model\n", race, sex); return 1; }
            const auto mesh = LoadM2(M2Name(look->look.model), [&](const std::string& p) { return mpq.Read(p); });
            if (!mesh) { printf("model %s does not load\n", look->look.model.c_str()); ++problems; }
            size_t unreadable = 0;
            for (const auto& [type, name] : look->look.textures)
                if (!composed.count(name) && !mpq.HasOwn(name)) { printf("  skin %u: texture type %u %s is not in the client\n", skin, type, name.c_str()); ++unreadable; }
            const auto body = look->look.textures.find(1);
            const auto image = body == look->look.textures.end() ? composed.end() : composed.find(body->second);
            if (image == composed.end()) { printf("  skin %u: no composited body\n", skin); ++problems; }
            else if (!first) { first = image->second; firstLook = look; }
            printf("%s skin %u: %s, %zu batches, %zu texture(s) (%zu not in the client), %zu geosets\n", unreadable || !mesh ? "FAIL" : "ok  ", skin,
                   look->look.model.c_str(), mesh ? mesh->batches.size() : 0, look->look.textures.size(), unreadable, look->look.geosets.size());
            problems += int(unreadable);
        }
        if (first)
        {
            std::vector<uint8_t> bgra = first->mips[0];
            for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            {
                std::swap(bgra[i], bgra[i + 2]);
                bgra[i + 3] = 255;
            }
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (!SavePng(__wargv[5], first->width, first->height, std::move(bgra))) { printf("cannot write %s\n", arg(5).c_str()); ++problems; }
        }
        if (firstLook)
        {
            // The head from four sides, as the window's preview draws it (DrawParts), on a software device.
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
            Renderer renderer;
            ModelRenderer models;
            std::string error;
            if (!renderer.Init(device.Get(), context.Get(), error) || !models.Init(device.Get(), context.Get(), renderer, error)) { printf("%s\n", error.c_str()); return 1; }
            for (const auto& [name, image] : composed) renderer.CacheTexture(name, image);
            const auto info = models.Info(firstLook->look.model, mpq);
            if (!info) { printf("model does not draw\n"); return 1; }
            std::string has, shown;
            for (uint16_t g : info->geosets) has += " " + std::to_string(g);
            for (uint16_t g : firstLook->look.geosets) shown += " " + std::to_string(g);
            printf("submeshes in the model:%s\nsubmeshes shown:%s\n", has.c_str(), shown.c_str());
            for (const auto& [type, name] : firstLook->look.textures) printf("texture type %u: %s\n", type, name.c_str());
            if (const auto mesh = LoadM2(M2Name(firstLook->look.model), [&](const std::string& p) { return mpq.Read(p); }))
                for (const auto& b : mesh->batches)
                    if (b.geoset < 100)
                        printf("  batch: submesh %u, texture type %u%s%s, blend %d, weight %d, colour %d, %u indices, opacity %.2f\n", b.geoset,
                               b.textureType, b.texture.empty() ? "" : " ", b.texture.c_str(), int(b.blend), b.weight, b.color, b.indexCount,
                               info->skeleton ? BatchAlpha(*info->skeleton, b.weight, b.color, 0) : 1.0f);
            if (info->skeleton && !info->skeleton->weights.empty())
            {
                const auto& w = info->skeleton->weights[0];
                printf("transparency track 0: global sequence %d, %zu key(s)", w.globalSequence, w.values.size());
                for (size_t i = 0; i < w.values.size() && i < 8; ++i) printf(" %u:%.2f", w.times[i], w.values[i]);
                printf("; sequences %zu, duration %u\n", info->skeleton->sequences.size(), info->skeleton->duration);
            }
            if (const auto& body = composed.find(firstLook->look.textures[1]); body != composed.end())
            {
                size_t transparent = 0;
                const auto& px = body->second.mips[0];
                for (size_t i = 3; i < px.size(); i += 4) transparent += px[i] < 128;
                printf("composited body: %ux%u, %.0f%% of its texels alpha < 128\n", body->second.width, body->second.height,
                       100.0 * double(transparent) / double(std::max<size_t>(1, px.size() / 4)));
            }
            constexpr UINT kSide = 256;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = kSide;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> color, depth, staging;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            device->CreateTexture2D(&d, nullptr, &color);
            device->CreateRenderTargetView(color.Get(), nullptr, &rtv);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            device->CreateTexture2D(&d, nullptr, &staging);
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            d.Format = DXGI_FORMAT_D32_FLOAT;
            d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            device->CreateTexture2D(&d, nullptr, &depth);
            device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
            // Framed on the body's own vertices (submesh 0): some models' header box is far larger than the character.
            XMFLOAT3 lo = info->boundsMin, hi = info->boundsMax;
            if (const auto mesh = LoadM2(M2Name(firstLook->look.model), [&](const std::string& p) { return mpq.Read(p); }))
            {
                XMFLOAT3 a{ 1e9f, 1e9f, 1e9f }, b{ -1e9f, -1e9f, -1e9f };
                for (const auto& batch : mesh->batches)
                    if (batch.geoset == 0)
                        for (uint32_t i = batch.indexStart; i < batch.indexStart + batch.indexCount && i < mesh->indices.size(); ++i)
                        {
                            const XMFLOAT3& p = mesh->vertices[mesh->indices[i]].pos;
                            a = { std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z) };
                            b = { std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z) };
                        }
                if (a.x < b.x) { printf("body vertices: y %.2f .. %.2f (model box y %.2f .. %.2f)\n", a.y, b.y, lo.y, hi.y); lo = a; hi = b; }
            }
            const float height = hi.y - lo.y;
            const XMVECTOR head = XMVectorSet((lo.x + hi.x) * 0.5f, lo.y + height * 0.86f, (lo.z + hi.z) * 0.5f, 0);
            // Posed standing, as the window shows it.
            std::shared_ptr<const ModelSkeleton> pose;
            if (const auto& skel = info->skeleton)
                for (size_t i = 0; i < skel->sequences.size(); ++i)
                    if (skel->sequences[i].id == 0)
                    {
                        pose = LoadSkeleton(M2Name(firstLook->look.model), [&](const std::string& p) { return mpq.Read(p); }, int(i));
                        break;
                    }
            std::vector<ModelRenderer::Part> parts{ { firstLook->look, {}, pose.get(), 0 } };
            XMStoreFloat4x4(&parts[0].world, XMMatrixIdentity());
            std::vector<uint8_t> strip(size_t(kSide) * 4 * kSide * 4, 0);
            for (UINT view = 0; view < 4; ++view)
            {
                const float yaw = XM_PIDIV2 * float(view);
                const XMVECTOR eye = XMVectorAdd(head, XMVectorScale(XMVectorSet(std::cos(yaw), 0.1f, std::sin(yaw), 0), height * 0.4f));
                const XMMATRIX viewProj = XMMatrixLookAtRH(eye, head, XMVectorSet(0, 1, 0, 0)) *
                                          XMMatrixPerspectiveFovRH(XMConvertToRadians(40.0f), 1.0f, height * 0.01f, height * 10.0f);
                const float bg[4] = { 0.16f, 0.17f, 0.20f, 1 };
                const D3D11_VIEWPORT vp{ 0, 0, float(kSide), float(kSide), 0, 1 };
                context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
                context->RSSetViewports(1, &vp);
                context->ClearRenderTargetView(rtv.Get(), bg);
                context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
                models.DrawParts(parts, viewProj, mpq);
                context->CopyResource(staging.Get(), color.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                {
                    for (UINT y = 0; y < kSide; ++y)
                        for (UINT x = 0; x < kSide; ++x)
                        {
                            const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                            uint8_t* q = strip.data() + (size_t(y) * kSide * 4 + view * kSide + x) * 4;
                            q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 255;
                        }
                    context->Unmap(staging.Get(), 0);
                }
            }
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (!SavePng(std::wstring(__wargv[5]) + L".model.png", kSide * 4, kSide, std::move(strip))) ++problems;
        }
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    // A project as the window has it: the race packages and the display and model tables, in one change store.
    struct RaceProjectTables
    {
        ChangeStore store;
        RaceAdapter races;
        DbcTable displays, models;
        explicit RaceProjectTables(MpqChain& mpq) : displays(mpq, store, "CreatureDisplayInfo", CreatureDisplayInfoFields(), 16),
                                         models(mpq, store, "CreatureModelData", CreatureModelDataFields(), 28)
        {
            store.Register(races);
            store.Register(displays);
            store.Register(models);
        }
        void Apply(const std::vector<Change>& parts)
        {
            for (const Change& c : parts)
                if (c.domain == races.Domain()) races.Apply(c);
                else if (c.domain == displays.Domain()) displays.Apply(c);
                else models.Apply(c);
        }
        RaceCatalog Project(const RaceCatalog& client) const
        {
            RaceCatalog p = client;
            p.SetModelLookup([this](uint32_t display) -> std::string {
                const nlohmann::json* d = displays.Edited(display);
                const nlohmann::json& m = d && d->is_object() ? models.Row(d->value("ModelID", 0u)) : nlohmann::json();
                return m.is_object() ? m.value("ModelName", "") : std::string();
            });
            for (const auto& [id, package] : races.Packages()) p.Apply(id, package);
            return p;
        }
    };

    /// `--race-import-check <data dir> <source data dir> <source race> [target race]`: imports a race of another client
    /// into a project on the first, as the Races window does, and checks the result: every row carries the new race id,
    /// every id of its own is new and inside the project's ranges, its displays point at models with the source's model
    /// files, the project's races show the same choices the source has, and saving, reloading, undo, redo and removing
    /// all give back what they should.
    int RaceImportCheck()
    {
        if (__argc < 5 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        auto arg = [](int i) { return std::filesystem::path(__wargv[i]).string(); };
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };

        MpqChain base, source;
        base.Open(arg(2));
        source.Open(arg(3));
        const uint32_t sourceRace = uint32_t(_wtoi(__wargv[4]));
        RaceCatalog baseRaces, sourceRaces;
        std::string error;
        if (!baseRaces.Load(base, error) || !sourceRaces.Load(source, error)) { printf("%s\n", error.c_str()); return 1; }
        const Project project;   // the default id ranges
        const Project::IdRange raceRange = project.Range("race.id");
        uint32_t target = __argc > 5 ? uint32_t(_wtoi(__wargv[5])) : 0;
        for (uint32_t id = raceRange.first; !target && id <= raceRange.last; ++id)
            if (!baseRaces.Find(id)) target = id;
        const RaceCatalog::Race* from = sourceRaces.Find(sourceRace);
        if (!from || !target) { printf("no race %u in the source, or no free race id\n", sourceRace); return 1; }
        printf("importing %s (race %u, %s layout) as race %u\n", from->name.c_str(), sourceRace, RaceCatalog::LayoutName(sourceRaces.Format()), target);

        RaceProjectTables t(base);
        auto parts = ImportRaceChanges(sourceRaces, "source", sourceRace, target, baseRaces, t.races, t.displays, t.models, project, error);
        if (parts.empty()) { printf("import: %s\n", error.c_str()); return 1; }
        t.Apply(parts);
        t.store.Commit(parts, "import race");
        const nlohmann::json* p = t.races.Package(target);
        expect(p && p->at("ChrRaces").value("ID", 0u) == target, "the race package is race " + std::to_string(target));
        if (!p) return 1;

        // Every row carries the new race; every id of its own is new and in the project's range.
        const nlohmann::json sourcePackage = sourceRaces.Package(sourceRace);
        bool raceIds = true;
        for (const char* table : { "CharSections", "CharHairGeosets", "CharacterFacialHairStyles", "CharBaseInfo", "CharStartOutfit" })
        {
            for (const nlohmann::json& row : p->at(table)) raceIds = raceIds && row.value("RaceID", 0u) == target;
            expect(p->at(table).size() == sourcePackage.at(table).size(),
                   std::string(table) + ": " + std::to_string(p->at(table).size()) + " row(s), as the source has");
        }
        expect(raceIds, "every row is of race " + std::to_string(target));
        for (const auto& [table, range] : { std::pair{ "CharSections", "charsections.id" }, std::pair{ "CharHairGeosets", "charhairgeosets.id" },
                                            std::pair{ "CharStartOutfit", "charstartoutfit.id" } })
        {
            const Project::IdRange r = project.Range(range);
            const std::set<uint32_t> client = baseRaces.Ids(table);
            std::set<uint32_t> seen;
            bool ok = true;
            for (const nlohmann::json& row : p->at(table))
            {
                const uint32_t id = row.value("ID", 0u);
                ok = ok && id >= r.first && id <= r.last && !client.count(id) && seen.insert(id).second;
            }
            expect(ok, std::string(table) + " ids are new, once each, inside " + range);
        }
        // Displays: new, in range, each pointing at a model with the source's model file.
        for (uint32_t sex = 0; sex < 2; ++sex)
        {
            const uint32_t display = p->at("ChrRaces").value(sex ? "FemaleDisplayID" : "MaleDisplayID", 0u);
            const Project::IdRange r = project.Range("creaturedisplayinfo.id");
            const nlohmann::json* row = t.displays.Edited(display);
            const uint32_t model = row && row->is_object() ? row->value("ModelID", 0u) : 0;
            const nlohmann::json& modelRow = t.models.Row(model);
            const std::string modelName = modelRow.is_object() ? modelRow.value("ModelName", "") : std::string();
            expect(display >= r.first && display <= r.last && row, std::string(sex ? "female" : "male") + " display " + std::to_string(display) + " is the project's");
            expect(lower(M2Name(modelName)) == lower(sourceRaces.Model(sourceRace, sex)),
                   std::string(sex ? "female" : "male") + " model " + std::to_string(model) + " is " + modelName);
        }
        // The project's races show what the source shows.
        const RaceCatalog projected = t.Project(baseRaces);
        for (uint32_t sex = 0; sex < 2; ++sex)
        {
            const RaceCatalog::Counts a = sourceRaces.Count(sourceRace, sex), b = projected.Count(target, sex);
            expect(a.skins == b.skins && a.faces == b.faces && a.hairStyles == b.hairStyles && a.hairColors == b.hairColors && a.facialHair == b.facialHair,
                   std::string(sex ? "female" : "male") + " choices " + std::to_string(b.skins) + "/" + std::to_string(b.faces) + "/" +
                       std::to_string(b.hairStyles) + "/" + std::to_string(b.hairColors) + "/" + std::to_string(b.facialHair) + " as in the source");
        }
        expect(projected.Classes(target) == sourceRaces.Classes(sourceRace), "the same classes");

        // Saved and loaded again: the same package and rows.
        const fs::path dir = fs::temp_directory_path() / "wow-world-editor-raceimport";
        std::error_code ec;
        fs::remove_all(dir, ec);
        expect(t.store.Save(dir, error), "changes saved " + error);
        {
            RaceProjectTables again(base);
            expect(again.store.Load(dir, error), "changes loaded " + error);
            const nlohmann::json* q = again.races.Package(target);
            expect(q && *q == *p && again.displays.Count() == t.displays.Count() && again.models.Count() == t.models.Count(), "reloaded: the same package and rows");
        }
        fs::remove_all(dir, ec);

        // Undo, redo, remove, undo the removal.
        const nlohmann::json kept = *p;
        const size_t displays = t.displays.Count(), models = t.models.Count();
        t.store.Undo();
        expect(t.races.Packages().empty() && !t.displays.Count() && !t.models.Count(), "undo leaves nothing");
        t.store.Redo();
        expect(t.races.Package(target) && *t.races.Package(target) == kept && t.displays.Count() == displays && t.models.Count() == models, "redo brings it back");
        auto removal = RemoveRaceChanges(target, t.races, t.displays, t.models);
        t.Apply(removal);
        t.store.Commit(removal, "remove race");
        expect(t.races.Packages().empty() && !t.displays.Count() && !t.models.Count(), "remove takes the race and the rows its import added");
        t.store.Undo();
        expect(t.races.Package(target) && t.displays.Count() == displays, "undoing the removal brings it back");
        printf("%zu display(s) and %zu model(s) added, %zu CharSections rows\n", displays, models, kept.at("CharSections").size());

        // The import's choices: the other team, Orc's faction template, Warrior and Mage kept, Paladin added with Human's
        // starting outfits, every other class dropped.
        {
            RaceProjectTables o(base);
            RaceImportOptions options;
            options.alliance = from->alliance == 1 ? 0 : 1;
            options.faction = 2;
            options.classes = std::set<uint32_t>{ 1, 2, 8 };
            options.outfitDonor = 1;
            size_t sourcePaladin = 0;   // its own Paladin outfits, kept over the donor's
            const nlohmann::json sourceOutfits = sourceRaces.Package(sourceRace).at("CharStartOutfit");
            for (const nlohmann::json& row : sourceOutfits) sourcePaladin += row.value("ClassID", 0u) == 2;
            auto chosen = ImportRaceChanges(sourceRaces, "source", sourceRace, target, baseRaces, o.races, o.displays, o.models, project, error, options);
            o.Apply(chosen);
            o.store.Commit(chosen, "import race");
            const nlohmann::json* q = o.races.Package(target);
            expect(q && q->at("ChrRaces").value("Alliance", -1) == options.alliance && q->at("ChrRaces").value("FactionID", 0u) == 2u,
                   "team and faction template as chosen (2: " + baseRaces.FactionName(2) + ")");
            std::set<uint32_t> classes, outfitClasses;
            for (const nlohmann::json& row : q->at("CharBaseInfo")) classes.insert(row.value("ClassID", 0u));
            for (const nlohmann::json& row : q->at("CharStartOutfit")) outfitClasses.insert(row.value("ClassID", 0u));
            expect(classes == std::set<uint32_t>{ 1, 2, 8 }, "classes Warrior, Paladin, Mage");
            size_t humanPaladin = 0, paladin = 0;
            const nlohmann::json human = baseRaces.Package(1);
            for (const nlohmann::json& row : human.at("CharStartOutfit")) humanPaladin += row.value("ClassID", 0u) == 2;
            for (const nlohmann::json& row : q->at("CharStartOutfit")) paladin += row.value("ClassID", 0u) == 2;
            expect(std::includes(classes.begin(), classes.end(), outfitClasses.begin(), outfitClasses.end()) &&
                       paladin == (sourcePaladin ? sourcePaladin : humanPaladin),
                   "outfits only for its classes; Paladin's " + std::to_string(paladin) + (sourcePaladin ? " its own" : " from Human"));
            const std::set<uint32_t> client = baseRaces.Ids("CharStartOutfit");
            bool fresh = true;
            for (const nlohmann::json& row : q->at("CharStartOutfit")) fresh = fresh && !client.count(row.value("ID", 0u)) && row.value("RaceID", 0u) == target;
            expect(fresh, "the outfits copied from Human have new ids and the new race");
        }
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--race-edit-check <data dir> [race]`: the Races window's edits on a client race's package (default Human):
    /// removing the middle value of every choice of both sexes (the values after it move down with their textures, no
    /// gaps, and the rows tied to it go too: a skin colour's faces and underwear, a hair style's geosets), and giving it
    /// other classes (outfits of dropped classes go, a new class takes another race's with new ids).
    int RaceEditCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        MpqChain mpq;
        mpq.Open(std::filesystem::path(__wargv[2]).string());
        RaceCatalog races;
        std::string error;
        if (!races.Load(mpq, error)) { printf("%s\n", error.c_str()); return 1; }
        const uint32_t race = __argc > 3 ? uint32_t(_wtoi(__wargv[3])) : 1;
        const nlohmann::json original = races.Package(race);
        if (!original.is_object()) { printf("no race %u\n", race); return 1; }
        printf("race %u %s: %zu CharSections rows\n", race, original["ChrRaces"].value("Name_lang", "").c_str(), original["CharSections"].size());

        static const std::pair<RaceChoice, const char*> kChoices[] = { { RaceChoice::Skin, "skin colour" }, { RaceChoice::Face, "face" },
                                                                       { RaceChoice::HairStyle, "hair style" }, { RaceChoice::HairColor, "hair colour" },
                                                                       { RaceChoice::FacialHair, "facial hair" } };
        // The first texture of a value, as the Looks tab shows it.
        auto texture = [](const nlohmann::json& p, uint32_t sex, RaceChoice c, uint32_t value) {
            const uint32_t section = c == RaceChoice::Skin ? 0 : c == RaceChoice::Face ? 1 : c == RaceChoice::FacialHair ? 2 : 3;
            const char* column = c == RaceChoice::Skin || c == RaceChoice::HairColor ? "ColorIndex" : "VariationIndex";
            std::vector<std::string> out;
            for (const nlohmann::json& s : p["CharSections"])
                if (s.value("SexID", 0u) == sex && s.value("BaseSection", 0u) == section && s.value(column, 0u) == value)
                    out.push_back(s.value("TextureName[0]", std::string()) + "|" + s.value("TextureName[1]", std::string()));
            std::sort(out.begin(), out.end());
            return out;
        };
        auto contiguous = [](const std::vector<uint32_t>& v) {
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i] != i) return false;
            return true;
        };
        for (uint32_t sex = 0; sex < 2; ++sex)
            for (const auto& [choice, name] : kChoices)
            {
                nlohmann::json p = original;
                const std::vector<uint32_t> before = RaceChoiceValues(p, sex, choice);
                if (before.size() < 3) { printf("     %s %s: %zu value(s), skipped\n", sex ? "female" : "male", name, before.size()); continue; }
                const uint32_t middle = before[before.size() / 2];
                const auto next = texture(p, sex, choice, middle + 1);   // what the next value shows: it moves down into the gap
                const auto last = texture(p, sex, choice, before.back());
                const size_t rowsBefore = p["CharSections"].size();
                const bool removed = RemoveRaceChoice(p, sex, choice, middle);
                const std::vector<uint32_t> after = RaceChoiceValues(p, sex, choice);
                const bool wasContiguous = contiguous(before);
                bool ok = removed && after.size() == before.size() - 1 && (!wasContiguous || contiguous(after)) &&
                          texture(p, sex, choice, middle) == next && texture(p, sex, choice, before.back() - 1) == last;
                // Rows tied to the value went with it: no face or underwear of a removed skin colour is past the last one left.
                if (choice == RaceChoice::Skin)
                    for (const nlohmann::json& s : p["CharSections"])
                        ok = ok && !(s.value("SexID", 0u) == sex && s.value("ColorIndex", 0u) >= before.size() - 1 &&
                                     (s.value("BaseSection", 0u) == 1 || s.value("BaseSection", 0u) == 4));
                if (choice == RaceChoice::HairStyle)
                    for (const nlohmann::json& h : p["CharHairGeosets"]) ok = ok && !(h.value("SexID", 0u) == sex && h.value("VariationID", 0u) >= before.size() - 1);
                expect(ok, std::string(sex ? "female " : "male ") + name + " " + std::to_string(middle + 1) + " of " + std::to_string(before.size()) +
                               " removed: " + std::to_string(after.size()) + " left" + (wasContiguous ? ", no gap" : " (the client's had gaps)") +
                               ", " + std::to_string(rowsBefore - p["CharSections"].size()) + " CharSections row(s) went");
            }

        // Classes: keep two, add one it has no outfits of from another race.
        {
            nlohmann::json p = original;
            const std::vector<uint32_t> had = races.Classes(race);
            if (had.size() >= 2)
            {
                const uint32_t keepA = had[0], keepB = had[1];
                uint32_t added = 0, donorRace = 0;
                for (const RaceCatalog::Race& r : races.Races())   // a class this race lacks, and a race with outfits of it
                {
                    if (r.id == race || added) continue;
                    for (const RaceCatalog::Outfit& o : races.Outfits())
                        if (o.race == r.id && std::find(had.begin(), had.end(), uint32_t(o.cls)) == had.end()) { added = o.cls; donorRace = r.id; break; }
                }
                uint32_t nextId = 990000;
                const std::set<uint32_t> wanted{ keepA, keepB, added };
                const nlohmann::json donor = races.Package(donorRace);
                expect(SetRaceClasses(p, wanted, donor, [&] { return nextId++; }, error), "classes set " + error);
                std::set<uint32_t> classes, outfitClasses;
                bool ids = true;
                for (const nlohmann::json& b : p["CharBaseInfo"]) classes.insert(b.value("ClassID", 0u));
                for (const nlohmann::json& o : p["CharStartOutfit"])
                {
                    outfitClasses.insert(o.value("ClassID", 0u));
                    ids = ids && o.value("RaceID", 0u) == race && (o.value("ClassID", 0u) != added || o.value("ID", 0u) >= 990000);
                }
                expect(classes == wanted && outfitClasses == wanted && ids,
                       "classes " + std::to_string(keepA) + ", " + std::to_string(keepB) + " kept, " + std::to_string(added) + " added with race " +
                           std::to_string(donorRace) + "'s outfits under new ids");
            }
        }
        // The edited package as the project's races show it.
        {
            nlohmann::json p = original;
            RemoveRaceChoice(p, 0, RaceChoice::Skin, 0);
            RaceCatalog edited = races;
            edited.Apply(race, p);
            expect(edited.Count(race, 0).skins == races.Count(race, 0).skins - 1 && edited.Count(race, 1).skins == races.Count(race, 1).skins,
                   "the project's races count one male skin colour fewer, the female ones unchanged");
        }
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    /// `--race-export-check <data dir> <source data dir> <source race>`: imports a race as the Races window does (its files
    /// listed, clashing ones renamed), exports it into a temp folder as Export does, and opens the client with that folder
    /// over it: the race reads back with the same choices, its model loads, its character composites from files the
    /// client and the export hold, the other races are as they were, CharSections is sorted, the server gets the same tables.
    int RaceExportCheck()
    {
        if (__argc < 5 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        namespace fs = std::filesystem;
        auto arg = [](int i) { return fs::path(__wargv[i]).string(); };
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        auto lower = [](std::string x) { for (char& c : x) c = char(std::tolower((unsigned char)c)); return x; };

        MpqChain base, source;
        base.Open(arg(2));
        source.Open(arg(3));
        const uint32_t sourceRace = uint32_t(_wtoi(__wargv[4]));
        RaceCatalog baseRaces, sourceRaces;
        std::string error;
        if (!baseRaces.Load(base, error) || !sourceRaces.Load(source, error)) { printf("%s\n", error.c_str()); return 1; }
        const Project project;
        uint32_t target = 0;
        for (uint32_t id = project.Range("race.id").first; !target && id <= project.Range("race.id").last; ++id)
            if (!baseRaces.Find(id)) target = id;
        RaceProjectTables t(base);
        const auto t0 = std::chrono::steady_clock::now();
        auto parts = ImportRaceChanges(sourceRaces, "source", sourceRace, target, baseRaces, t.races, t.displays, t.models, project, error, {}, &source, &base);
        if (parts.empty()) { printf("import: %s\n", error.c_str()); return 1; }
        t.Apply(parts);
        t.store.Commit(parts, "import race");
        const nlohmann::json p = *t.races.Package(target);
        size_t renamed = 0;
        for (const nlohmann::json& f : p.at("files")) renamed += f.value("path", std::string()) != f.value("from", std::string());
        printf("import as race %u: %zu file(s) to copy, %zu renamed, %zu named but missing in the source, %.0f ms\n", target, p.at("files").size(), renamed,
               p.value("missingFiles", size_t(0)), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

        // Export as App::Export does: the display and model rows, then the race tables and files.
        const fs::path dir = fs::temp_directory_path() / "wow-world-editor-raceexport", client = dir / "client", server = dir / "server" / "dbc";
        std::error_code ec;
        fs::remove_all(dir, ec);
        expect(t.displays.Export({ client / "DBFilesClient", server }, dir / "dbc", error) && t.models.Export({ client / "DBFilesClient", server }, dir / "dbc", error),
               "display and model rows exported " + error);
        RaceCatalog projectRaces = baseRaces;
        for (const auto& [id, package] : t.races.Packages()) projectRaces.Apply(id, package);
        std::vector<std::string> notes;
        expect(ExportRaces(projectRaces, t.races.Packages(), [&](const std::string& c) { return c == "source" ? &source : nullptr; },
                           { client / "DBFilesClient", server }, client, notes, error, &base),
               "race tables and files exported " + error);
        for (const std::string& n : notes) printf("     note: %s\n", n.c_str());
        {
            // The creator: the client's script kept whole, the race added once, and an export of an export the same.
            const auto original = base.Read("Interface\\GlueXML\\CharacterCreate.lua");
            const auto script = ReadFileBytes(client / "Interface" / "GlueXML" / "CharacterCreate.lua");
            const std::string text = script ? std::string(script->begin(), script->end()) : std::string();
            std::string head = original ? std::string(original->begin(), original->end()) : std::string();
            if (const size_t cut = head.find("\n-- wow-world-editor"); cut != std::string::npos) head.resize(cut);
            while (!head.empty() && std::isspace((unsigned char)head.back())) head.pop_back();
            size_t playable = 0;
            for (const RaceCatalog::Race& race : projectRaces.Races()) playable += !(race.flags & 1);
            std::string upper = projectRaces.Find(target)->fileString;
            for (char& ch : upper) ch = char(std::toupper((unsigned char)ch));
            expect(script && text.compare(0, head.size(), head) == 0, "creator script starts with the client's own");
            expect(text.find("EDITOR_CREATOR_PLAYABLE = " + std::to_string(playable) + ";") != std::string::npos &&
                       text.find("file = \"" + upper + "\", donor = ") != std::string::npos && text.find(", scene = ") != std::string::npos,
                   "creator script lists race " + upper + " among " + std::to_string(playable) + " playable");
            auto has = [&](const std::string& f) { return base.HasInstalled(f); };
            expect(CreatorScript(text, projectRaces, t.races.Packages(), has) == CreatorScript(head, projectRaces, t.races.Packages(), has),
                   "an export over an earlier export's script adds the races once");
        }
        size_t present = 0;
        for (const nlohmann::json& f : p.at("files"))
        {
            std::string rel = f.value("path", std::string());
            std::replace(rel.begin(), rel.end(), '\\', '/');
            present += fs::is_regular_file(client / fs::path(rel), ec);
        }
        expect(present == p.at("files").size(), std::to_string(present) + " of " + std::to_string(p.at("files").size()) + " listed file(s) in the export");
        bool sameServer = true;
        for (const char* table : RaceCatalog::kTables)
        {
            const auto a = ReadFileBytes(client / "DBFilesClient" / (std::string(table) + ".dbc")), b = ReadFileBytes(server / (std::string(table) + ".dbc"));
            sameServer = sameServer && a && b && *a == *b;
        }
        expect(sameServer, "the server's dbc folder gets the same six tables");

        // The client with the export over it, as players will have it.
        MpqChain played;
        played.Open(std::vector<MpqLayer>{ { MpqLayer::Kind::MpqFolder, arg(2) }, { MpqLayer::Kind::Folder, client.string() } });
        RaceCatalog back;
        expect(back.Load(played, error), "the exported tables read " + error);
        {
            // Looks players may choose (3.3.5 flag 0x1) as many as the source offers players, whatever its layout.
            size_t player = 0, sourcePlayer = 0;
            for (const RaceCatalog::Section& s : back.Sections()) player += s.race == target && (s.flags & 1);
            for (const RaceCatalog::Section& s : sourceRaces.Sections()) sourcePlayer += s.race == sourceRace && (s.flags & 1);
            expect(player && player == sourcePlayer, std::to_string(player) + " looks players may choose, as in the source (" +
                                                         RaceCatalog::LayoutName(sourceRaces.Format()) + ")");
        }
        const RaceCatalog::Race* r = back.Find(target);
        expect(r && back.Races().size() == baseRaces.Races().size() + 1, "race " + std::to_string(target) + " is there, beside the client's " +
                                                                           std::to_string(baseRaces.Races().size()) + " races");
        for (uint32_t sex = 0; sex < 2; ++sex)
        {
            const RaceCatalog::Counts a = sourceRaces.Count(sourceRace, sex), b = back.Count(target, sex);
            expect(a.skins == b.skins && a.faces == b.faces && a.hairStyles == b.hairStyles && a.hairColors == b.hairColors && a.facialHair == b.facialHair,
                   std::string(sex ? "female" : "male") + " choices as in the source");
            const std::string model = back.Model(target, sex);
            const auto mesh = model.empty() ? std::nullopt : LoadM2(model, [&](const std::string& f) { return played.Read(f); });
            expect(mesh.has_value(), std::string(sex ? "female" : "male") + " model " + model + " loads" +
                                         (lower(model).rfind("character\\race", 0) == 0 ? " (renamed: the client has another file of its name)" : ""));
        }
        bool others = true;
        for (const RaceCatalog::Race& race : baseRaces.Races()) others = others && back.Package(race.id) == baseRaces.Package(race.id);
        expect(others, "every other race reads back as the client has it");
        {
            Dbc sections;
            sections.Load(ReadFileBytes(client / "DBFilesClient" / "CharSections.dbc").value_or(std::vector<uint8_t>{}));
            bool sorted = true;
            std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> last{};
            for (uint32_t row = 0; row < sections.Rows(); ++row)
            {
                const auto key = std::tuple(sections.U32(row, 1), sections.U32(row, 2), sections.U32(row, 3), sections.U32(row, 8), sections.U32(row, 9));
                sorted = sorted && !(key < last);
                last = key;
            }
            expect(sorted && sections.Rows() == back.Sections().size(), "CharSections sorted by race, sex, section, variation, colour (" + std::to_string(sections.Rows()) + " rows)");
        }
        // Its character as the creator shows it: every texture it wears is in the client or the export.
        {
            std::map<std::string, BlpImage> composed;
            DisplayLooks looks(played);
            looks.SetUpload([&](const std::string& name, const BlpImage& image) { composed[name] = image; });
            const DisplayLooks::Choices c = looks.CharacterChoices(target, 0, 0, 0);
            auto first = [](const std::vector<uint32_t>& v) { return v.empty() ? 0u : v.front(); };
            const auto look = looks.CharacterLook(target, 0, first(c.skins), first(c.faces), first(c.hairStyles), first(c.hairColors), first(c.facialHair));
            bool ok = look && look->look.textures.count(1) && composed.count(look->look.textures.at(1));
            for (const auto& [type, name] : look ? look->look.textures : std::map<uint32_t, std::string>{})
                ok = ok && (composed.count(name) || played.HasOwn(name));
            expect(ok, "its male character composites from the client and the export");
        }
        if (!_wgetenv(L"WWE_KEEP")) fs::remove_all(dir, ec);   // WWE_KEEP=1 keeps the export to look at
        else printf("kept %s\n", dir.string().c_str());
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }
}
