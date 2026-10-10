// Command-line checks: Server tables and DBC adapters. See docs/development/checks.md.

#include "Checks.hpp"

namespace checks
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

    int DialogueCheck()
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

    int NpcCheck()
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

    int WeatherCheck()
    {
        // `--weather-check <AC server dir>`: game_weather through the editor's adapter on the real world database: read
        // Elwynn's row, change one chance, read it back from the database, undo, and confirm the row is as it was.
        std::string password, note, error;
        setvbuf(stdout, nullptr, _IONBF, 0);
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        try   // the adapter holds columns as text: a number read as one throws
        {
            ChangeStore store;
            TableRowsAdapter weather(store, "game_weather", "zone");
            store.Register(weather);
            weather.SetDb(&db);
            auto dbRow = [&]() {
                auto rows = db.QueryRows("SELECT * FROM game_weather WHERE zone = 12", error);
                return rows && !rows->empty() ? (*rows)[0] : nlohmann::json();
            };
            auto num = [](const nlohmann::json& r, const char* c) { return r.is_object() && r.contains(c) ? std::atoi(r.at(c).get<std::string>().c_str()) : -1; };
            const nlohmann::json original = dbRow();
            const auto rows = weather.Rows(12);
            printf("Elwynn (12): %zu row(s), spring rain %d%%\n", rows.size(), rows.empty() ? -1 : num(rows[0], "spring_rain_chance"));
            if (rows.empty()) { printf("no stock weather row for Elwynn\n"); return 1; }
            nlohmann::json after = rows[0];
            const int want = num(rows[0], "spring_rain_chance") == 50 ? 49 : 50;
            after["spring_rain_chance"] = std::to_string(want);
            Change c = weather.MakeChange(12, rows, { after }, "weather check");
            weather.Apply(c);
            store.Commit(c);
            const int written = num(dbRow(), "spring_rain_chance");
            store.Undo();
            const bool restored = dbRow() == original;
            printf("written %d (want %d), undo restores the row: %s%s\n", written, want, restored ? "yes" : "NO",
                   weather.LastError().empty() ? "" : (", error: " + weather.LastError()).c_str());
            return written == want && restored ? 0 : 1;
        }
        catch (const std::exception& e) { printf("exception: %s\n", e.what()); return 1; }
    }

    int SpawnCheck()
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

    int UnitCatalogCheck()
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

    int SqlCmd()
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

    int ServerCheck()
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

    int ServerDataCheck()
    {
        // `--serverdata-check <data dir> <AC server dir> <map directory> <map id> [x,y ...]`: the server-data job for one map,
        // installed into a scratch copy of nothing (never the server's own Data), then every file compared with the
        // server's: an unedited map must come out byte for byte as AzerothCore's full extraction made it.
        setvbuf(stdout, nullptr, _IONBF, 0);
        auto arg = [](int i) {
            char buf[1024] = {};
            WideCharToMultiByte(CP_ACP, 0, __wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
            return std::string(buf);
        };
        const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "wwe-sd";
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        ServerDataJob::Options o;
        o.tools = arg(3);
        o.serverData = scratch / "Data";
        o.work = std::filesystem::path("C:\\wwe-sd-work");   // short: map_extractor cuts paths at 127 characters
        o.exported = scratch / "export";
        o.backup = scratch / "backup";
        o.layers = { { MpqLayer::Kind::MpqFolder, arg(2) } };
        ServerMap m;
        m.directory = arg(4);
        m.id = uint32_t(std::stoul(arg(5)));
        for (int i = 6; i < __argc; ++i)
        {
            const std::string t = arg(i);
            m.mmapTiles.insert({ std::stoi(t.substr(0, t.find(','))), std::stoi(t.substr(t.find(',') + 1)) });
        }
        // WWE_RAISE=x,y: an "edited" tile in the export (chunk 0 raised 20 yd): its .map must change, the rest stay.
        std::string raised;
        if (const char* r = std::getenv("WWE_RAISE"))
        {
            const std::string t = r;
            const std::string x = t.substr(0, t.find(',')), y = t.substr(t.find(',') + 1);
            MpqChain chain;
            chain.Open(arg(2));
            const std::string name = "World\\Maps\\" + m.directory + "\\" + m.directory + "_" + x + "_" + y + ".adt";
            auto bytes = chain.Read(name);
            const auto adt = bytes ? ParseAdt(*bytes, false) : std::nullopt;
            if (!adt) { printf("no tile %s\n", name.c_str()); return 1; }
            for (size_t j = 0; j < 145; ++j)
            {
                float h;
                std::memcpy(&h, bytes->data() + adt->chunks[0].mcvtOffset + j * 4, 4);
                h += 20;
                std::memcpy(bytes->data() + adt->chunks[0].mcvtOffset + j * 4, &h, 4);
            }
            const std::filesystem::path dest = o.exported / "World" / "Maps" / m.directory / (m.directory + "_" + x + "_" + y + ".adt");
            std::filesystem::create_directories(dest.parent_path(), ec);
            std::ofstream(dest, std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
            char file[32];
            snprintf(file, sizeof file, "%03u%02d%02d.map", m.id, std::stoi(y), std::stoi(x));
            raised = file;
        }
        ServerDataJob job;
        const auto t0 = std::chrono::steady_clock::now();
        job.Start(o, { m });
        while (job.Running() || !job.TakeLog().empty())
        {
            for (const std::string& l : job.TakeLog()) printf("  %s\n", l.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        for (const std::string& l : job.TakeLog()) printf("  %s\n", l.c_str());
        printf("job %s in %.0f s\n", job.Succeeded() ? "succeeded" : "FAILED", std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count());
        if (!job.Succeeded()) return 1;
        // Compare with the server's own files. vmtiles: the extractor numbers model instances in one count over all it
        // reads, so a run over one map numbers them otherwise than a run over the whole client; compare the spawns
        // themselves (everything but the number and the tree node index that follows the numbering).
        auto spawns = [](const std::string& d) {
            std::multiset<std::string> out;
            size_t p = 8;
            uint32_t n = 0;
            if (d.size() < 12) return out;
            std::memcpy(&n, d.data() + p, 4);
            p += 4;
            for (uint32_t i = 0; i < n && p + 42 <= d.size(); ++i)
            {
                uint32_t flags = 0, nameLen = 0;
                std::memcpy(&flags, d.data() + p, 4);
                const size_t fixed = 4 + 2 + 4 + 28 + ((flags & 4) ? 24 : 0);   // flags, adtId, ID, pos, rot, scale [, bound]
                std::memcpy(&nameLen, d.data() + p + fixed, 4);
                std::string key = d.substr(p, 6) + d.substr(p + 10, fixed - 10) + d.substr(p + fixed + 4, nameLen);
                out.insert(std::move(key));
                p += fixed + 4 + nameLen + 4;   // + the node index
            }
            return out;
        };
        const std::filesystem::path server = std::filesystem::path(arg(3)) / "Data";
        int problems = 0;
        for (const char* sub : { "maps", "vmaps", "mmaps" })
        {
            size_t same = 0, differ = 0, missing = 0;
            for (const auto& e : std::filesystem::directory_iterator(o.serverData / sub, ec))
            {
                std::ifstream a(e.path(), std::ios::binary), b(server / sub / e.path().filename(), std::ios::binary);
                if (!b) { ++missing; continue; }
                const std::string da((std::istreambuf_iterator<char>(a)), {}), db((std::istreambuf_iterator<char>(b)), {});
                // A .map that differs in a byte or two inside its liquid heights: map_extractor itself is not repeatable
                // there (run it twice on the same tile and it can differ), so that is not the pipeline's doing.
                auto liquidNoise = [&]() {
                    if (e.path().extension() != ".map" || da.size() != db.size() || da.size() < 44) return false;
                    uint32_t lo = 0, ls = 0;
                    std::memcpy(&lo, da.data() + 28, 4);
                    std::memcpy(&ls, da.data() + 32, 4);
                    size_t count = 0;
                    for (size_t k = 0; k < da.size(); ++k)
                        if (da[k] != db[k]) { if (k < lo || k >= size_t(lo) + ls) return false; ++count; }
                    return count <= 4;
                };
                // The edited tile, its .map and its navmesh tile (same name): must have changed.
                if (!raised.empty() && e.path().stem() == std::filesystem::path(raised).stem())
                {
                    printf("    %s (the raised tile): %s\n", e.path().filename().string().c_str(), da != db ? "changed, as it should" : "UNCHANGED");
                    problems += da == db;
                    continue;
                }
                if (da == db) ++same;
                else if (e.path().extension() == ".vmtile" && spawns(da) == spawns(db)) ++same;   // renumbered only
                else if (liquidNoise()) { ++same; printf("    %s: liquid heights differ by a byte (map_extractor is not repeatable there)\n", e.path().filename().string().c_str()); }
                else
                {
                    size_t at = 0, count = 0;
                    while (at < std::min(da.size(), db.size()) && da[at] == db[at]) ++at;
                    for (size_t k = 0; k < std::min(da.size(), db.size()); ++k) count += da[k] != db[k];
                    if (differ < 3)
                        printf("    differs: %s\\%s (%zu vs %zu bytes, first at %zu, %zu bytes differ)\n", sub, e.path().filename().string().c_str(),
                               da.size(), db.size(), at, count);
                    ++differ;
                }
            }
            printf("%-5s: %zu identical to the server's, %zu differ, %zu not in the server's\n", sub, same, differ, missing);
            problems += int(differ);
        }
        std::filesystem::remove_all(scratch, ec);
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    int SoundCheck()
    {
        // `--sound-check <data dir>`: the sound tables on the client's files: sizes, Elwynn's ambience / music / intro
        // resolved to names and files that exist, and where emitters stand (they must land on their map's terrain).
        setvbuf(stdout, nullptr, _IONBF, 0);
        char dataDir[1024] = {};
        WideCharToMultiByte(CP_ACP, 0, __wargv[2], -1, dataDir, sizeof dataDir, nullptr, nullptr);
        MpqChain mpq;
        mpq.Open(dataDir);
        ChangeStore store;
        Sounds sounds(mpq, store);
        AreaAdapter areas(mpq, store);
        int problems = 0;
        for (DbcTable* t : sounds.Tables())
        {
            const auto rows = t->Rows();
            printf("%-20s %5zu rows, max id %u\n", t->Name().c_str(), rows.size(), rows.empty() ? 0u : rows.rbegin()->first);
            if (rows.empty()) ++problems;
        }
        auto show = [&](const char* what, uint32_t entry) {
            const auto s = sounds.Entry(entry);
            size_t found = 0;
            if (s)
                for (const std::string& f : s->files) found += mpq.Read(f).has_value();
            printf("  %-14s entry %5u %-30s %zu/%zu file(s) found%s%s\n", what, entry, s ? s->name.c_str() : "(none)", found, s ? s->files.size() : 0,
                   s && !s->files.empty() ? ", first " : "", s && !s->files.empty() ? s->files[0].c_str() : "");
            if (entry && (!s || (s->files.size() && !found))) ++problems;
        };
        const nlohmann::json elwynn = areas.Row(12);
        const uint32_t amb = elwynn.value("AmbienceID", 0u), mus = elwynn.value("ZoneMusic", 0u), intro = elwynn.value("IntroSound", 0u);
        printf("Elwynn Forest: ambience %u, music %u, intro %u\n", amb, mus, intro);
        const nlohmann::json& a = sounds.ambience.Row(amb);
        show("ambience day", a.value("AmbienceID[0]", 0u));
        show("ambience night", a.value("AmbienceID[1]", 0u));
        const nlohmann::json& m = sounds.music.Row(mus);
        printf("  music set \"%s\", silence %u-%u ms\n", m.value("SetName", std::string()).c_str(), m.value("SilenceIntervalMin[0]", 0u), m.value("SilenceIntervalMax[0]", 0u));
        show("music day", m.value("Sounds[0]", 0u));
        show("music night", m.value("Sounds[1]", 0u));
        show("intro", intro && !sounds.intro.Row(intro).is_null() ? sounds.intro.Row(intro).value("SoundID", 0u) : 0u);
        // Emitters: which table their sound id names, and their positions against the map's tiles.
        size_t inEntries = 0, onTile = 0, azeroth = 0;
        const auto wdt = mpq.Read("World\\Maps\\Azeroth\\Azeroth.wdt");
        const auto present = wdt ? WdtTiles(*wdt) : std::vector<bool>{};
        for (const auto& [id, row] : sounds.emitters.Rows())
        {
            const uint32_t s = row.value("SoundEntryAdvancedID", 0u);
            inEntries += !sounds.entries.Row(s).is_null();
            const SoundEmitter e = Sounds::FromRow(row);
            if (e.map != 0) continue;
            ++azeroth;
            const int tx = int(e.pos.x / kTileSize), ty = int(e.pos.z / kTileSize);
            if (tx >= 0 && ty >= 0 && tx < 64 && ty < 64 && present.size() == 4096 && present[size_t(ty * 64 + tx)]) ++onTile;
            if (azeroth <= 3) printf("  emitter %u \"%s\" editor %.0f %.0f %.0f (tile %d_%d)\n", id, e.name.c_str(), e.pos.x, e.pos.y, e.pos.z, tx, ty);
        }
        printf("emitters: %zu/%zu name a SoundEntries row; Azeroth %zu, %zu on a present tile\n", inEntries, sounds.emitters.Rows().size(), azeroth, onTile);
        // The player opens both kinds the sound files come in (opened and closed, not played: no noise).
        for (const char* path : { "Sound\\Ambience\\ZoneAmbience\\ForestNormalDay.wav", "Sound\\Music\\ZoneMusic\\Forest\\DayForest01.mp3" })
        {
            const auto bytes = mpq.Read(path);
            const std::filesystem::path temp = std::filesystem::temp_directory_path() / ("wwe-sound-check" + std::filesystem::path(path).extension().string());
            if (bytes) std::ofstream(temp, std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
            const std::wstring open = L"open \"" + temp.wstring() + L"\" type mpegvideo alias wwe_check";
            const MCIERROR err = bytes ? mciSendStringW(open.c_str(), nullptr, 0, nullptr) : MCIERROR(1);
            wchar_t length[64] = {};
            if (!err) mciSendStringW(L"status wwe_check length", length, 64, nullptr);
            mciSendStringW(L"close wwe_check", nullptr, 0, nullptr);
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            printf("player: %s %s (length %ls ms)\n", path, err ? "CANNOT OPEN" : "opens", length);
            if (err) ++problems;
        }
        if ((azeroth && onTile * 10 < azeroth * 9) || inEntries != sounds.emitters.Rows().size()) ++problems;
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }

    int LightCheck()
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

    int AreaCheck()
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

    /// `--race-server-check <AC server dir> [race] [donor race]`: the Races window's Server tab against the real world
    /// database, cleaning up after itself: copies the donor's (default Human) character-creation rows to the race (default
    /// 22, which must have none) for Warrior, Paladin and Shaman (a class the donor cannot be comes from another race),
    /// with Orcish as its language; sets one start; checks what the database holds; undoes everything and checks it is gone.
    int RaceServerCheck()
    {
        if (__argc < 3 || !__wargv) return 2;
        setvbuf(stdout, nullptr, _IONBF, 0);
        std::string password, note, error;
        const auto profile = ServerProfile::FromWorldserverConf(std::filesystem::path(__wargv[2]), password, note);
        if (!profile) { printf("%s\n", note.c_str()); return 1; }
        Db db;
        if (!db.Connect(profile->dbHost, profile->dbPort, profile->dbUser, password, profile->worldDb, error)) { printf("db: %s\n", error.c_str()); return 1; }
        const uint32_t race = __argc > 3 ? uint32_t(_wtoi(__wargv[3])) : 22, donor = __argc > 4 ? uint32_t(_wtoi(__wargv[4])) : 1;
        int problems = 0;
        auto expect = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); problems += !ok; };
        auto count = [&](const std::string& sql) {
            const auto rows = db.Query(sql, error);
            return rows && !rows->empty() ? std::stoul((*rows)[0][0]) : 0ul;
        };
        const std::string r = std::to_string(race), bit = std::to_string(RaceBit(race));
        auto inDb = [&] {
            return count("SELECT COUNT(*) FROM playercreateinfo WHERE race = " + r) + count("SELECT COUNT(*) FROM playercreateinfo_action WHERE race = " + r) +
                   count("SELECT COUNT(*) FROM player_race_stats WHERE Race = " + r) + count("SELECT COUNT(*) FROM playercreateinfo_skills WHERE raceMask = " + bit) +
                   count("SELECT COUNT(*) FROM playercreateinfo_item WHERE race = " + r) + count("SELECT COUNT(*) FROM playercreateinfo_spell_custom WHERE racemask = " + bit);
        };
        if (const unsigned long before = inDb()) { printf("race %u already has %lu row(s) in the database: pick another race\n", race, before); return 1; }

        ChangeStore store;
        TableRowsAdapter starts{ store, "playercreateinfo", "race", "class" }, actions{ store, "playercreateinfo_action", "race", "class" },
            items{ store, "playercreateinfo_item", "race", "class" }, stats{ store, "player_race_stats", "Race" },
            skills{ store, "playercreateinfo_skills", "raceMask", "skill" }, spells{ store, "playercreateinfo_spell_custom", "racemask", "Spell" };
        const RaceServerTables t{ starts, actions, items, stats, skills, spells };
        for (TableRowsAdapter* a : { &starts, &actions, &items, &stats, &skills, &spells })
        {
            store.Register(*a);
            a->SetDb(&db);
        }
        auto apply = [&](const std::vector<Change>& parts, const char* label) {
            for (const Change& c : parts)
                for (TableRowsAdapter* a : { &starts, &actions, &items, &stats, &skills, &spells })
                    if (c.domain == a->Domain()) a->Apply(c);
            store.Commit(parts, label);
        };
        const std::vector<uint32_t> classes{ 1, 2, 7 };
        std::vector<uint32_t> fallbacks;
        for (uint32_t id = 1; id < 12; ++id)
            if (id != donor) fallbacks.push_back(id);
        apply(CopyRaceServerRows(race, donor, classes, 1 /* Orcish */, fallbacks, t), "copy");

        expect(count("SELECT COUNT(*) FROM playercreateinfo WHERE race = " + r) == 3, "a start for each of the 3 classes, in the database");
        expect(count("SELECT COUNT(*) FROM playercreateinfo WHERE race = " + r + " AND class = 7") == 1, "Shaman's start from a race that has one");
        const unsigned long donorActions = count("SELECT COUNT(*) FROM playercreateinfo_action WHERE race = " + std::to_string(donor) + " AND class IN (1, 2)");
        const unsigned long shamanActions = count("SELECT COUNT(*) FROM playercreateinfo_action WHERE class = 7 AND race = (SELECT MIN(race) FROM playercreateinfo WHERE class = 7 AND race <> " + r + ")");
        expect(count("SELECT COUNT(*) FROM playercreateinfo_action WHERE race = " + r) == donorActions + shamanActions,
               std::to_string(donorActions + shamanActions) + " action bar buttons");
        expect(count("SELECT COUNT(*) FROM player_race_stats a JOIN player_race_stats b ON b.Race = " + std::to_string(donor) +
                     " WHERE a.Race = " + r + " AND a.Strength = b.Strength AND a.Spirit = b.Spirit") == 1, "the donor's base stats");
        expect(count("SELECT COUNT(*) FROM playercreateinfo_skills WHERE raceMask = " + bit + " AND skill = 109") == 1 &&
                   count("SELECT COUNT(*) FROM playercreateinfo_skills WHERE raceMask = " + bit + " AND skill = 98") == 0,
               "its own mask's language is Orcish (109), not the donor's Common");
        // One class's start moved; the other classes' kept.
        apply({ SetRaceStart(race, 1, 0, 12, -8949.95f, -132.49f, 83.53f, 0.5f, t) }, "start");
        expect(count("SELECT COUNT(*) FROM playercreateinfo WHERE race = " + r + " AND class = 1 AND zone = 12 AND ABS(position_x + 8949.95) < 0.01") == 1 &&
                   count("SELECT COUNT(*) FROM playercreateinfo WHERE race = " + r) == 3,
               "Warrior's start set, the others kept");
        store.Undo();
        store.Undo();
        expect(inDb() == 0, "undo leaves the database as it was");
        store.Redo();
        expect(count("SELECT COUNT(*) FROM playercreateinfo WHERE race = " + r) == 3, "redo writes them again");
        store.Undo();
        expect(inDb() == 0, "and undo takes them away again");
        printf("%d problem(s)\n", problems);
        return problems ? 1 : 0;
    }
}
