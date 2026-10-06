#include "Pois.hpp"

#include "Formats.hpp"
#include "Mpq.hpp"

#include <cstdio>
#include <string>

namespace
{

    std::string Text(float v)
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%.9g", v);
        return buf;
    }
    std::string Str(const nlohmann::json& row, const char* column)
    {
        const auto it = row.find(column);
        return it == row.end() || it->is_null() ? std::string() : it->get<std::string>();
    }
    float F(const nlohmann::json& row, const char* column) { return std::strtof(Str(row, column).c_str(), nullptr); }
    uint32_t U(const nlohmann::json& row, const char* column) { return uint32_t(std::strtoul(Str(row, column).c_str(), nullptr, 10)); }
}

Poi Poi::FromDbcRow(const nlohmann::json& row)
{
    Poi p;
    p.kind = PoiKind::MapIcon;
    p.id = row.value("ID", 0u);
    p.importance = row.value("Importance", 0u);
    p.icon = row.value("Icon[0]", 0u);
    p.x = row.value("Pos[0]", 0.0f);
    p.y = row.value("Pos[1]", 0.0f);
    p.z = row.value("Pos[2]", 0.0f);
    p.map = row.value("ContinentID", 0u);
    p.flags = row.value("Flags", 0u);
    p.area = row.value("AreaID", 0u);
    p.name = row.value("Name_lang", std::string());
    p.description = row.value("Description_lang", std::string());
    p.worldState = row.value("WorldStateID", 0u);
    return p;
}

nlohmann::json Poi::ToDbcRow(nlohmann::json base) const
{
    base["ID"] = id;
    base["Importance"] = importance;
    base["Icon[0]"] = icon;   // [1-8]: state icons of destructible buildings (Wintergrasp, Strand), kept
    base["Pos[0]"] = x;
    base["Pos[1]"] = y;
    base["Pos[2]"] = z;
    base["ContinentID"] = map;
    base["Flags"] = flags;
    base["AreaID"] = area;
    base["Name_lang"] = name;
    base["Description_lang"] = description;
    base["WorldStateID"] = worldState;
    return base;
}

Poi Poi::FromGossipRow(const nlohmann::json& row)
{
    Poi p;
    p.kind = PoiKind::Gossip;
    p.id = U(row, "ID");
    p.x = F(row, "PositionX");
    p.y = F(row, "PositionY");
    p.icon = U(row, "Icon");
    p.flags = U(row, "Flags");
    p.importance = U(row, "Importance");
    p.name = Str(row, "Name");
    return p;
}

nlohmann::json Poi::ToGossipRow() const
{
    return { { "ID", std::to_string(id) }, { "PositionX", Text(x) }, { "PositionY", Text(y) }, { "Icon", std::to_string(icon) },
             { "Flags", std::to_string(flags) }, { "Importance", std::to_string(importance) }, { "Name", name } };
}

Poi Poi::FromTeleRow(const nlohmann::json& row)
{
    Poi p;
    p.kind = PoiKind::Tele;
    p.id = U(row, "id");
    p.x = F(row, "position_x");
    p.y = F(row, "position_y");
    p.z = F(row, "position_z");
    p.o = F(row, "orientation");
    p.map = U(row, "map");
    p.name = Str(row, "name");
    return p;
}

nlohmann::json Poi::ToTeleRow() const
{
    return { { "id", std::to_string(id) }, { "position_x", Text(x) }, { "position_y", Text(y) }, { "position_z", Text(z) },
             { "orientation", Text(o) }, { "map", std::to_string(map) }, { "name", name } };
}

AreaPoiAdapter::AreaPoiAdapter(MpqChain& mpq, ChangeStore& store)
    : DbcTable(mpq, store, "AreaPOI",
               { { "ID", 0, 'i' }, { "Importance", 1, 'i' }, { "Icon[0]", 2, 'i' }, { "Icon[1]", 3, 'i' }, { "Icon[2]", 4, 'i' }, { "Icon[3]", 5, 'i' },
                 { "Icon[4]", 6, 'i' }, { "Icon[5]", 7, 'i' }, { "Icon[6]", 8, 'i' }, { "Icon[7]", 9, 'i' }, { "Icon[8]", 10, 'i' },
                 { "FactionID", 11, 'i' }, { "Pos[0]", 12, 'f' }, { "Pos[1]", 13, 'f' }, { "Pos[2]", 14, 'f' }, { "ContinentID", 15, 'i' },
                 { "Flags", 16, 'i' }, { "AreaID", 17, 'i' }, { "Name_lang", 18, 's' }, { "Name_lang_flags", 34, 'i' },
                 { "Description_lang", 35, 's' }, { "Description_lang_flags", 51, 'i' }, { "WorldStateID", 52, 'i' }, { "WorldMapLink", 53, 'i' } },
               54)
{
}

nlohmann::json AreaPoiAdapter::NewRow() const
{
    nlohmann::json row = EmptyRow();
    Read();
    if (!m_client.empty())
        for (const char* f : { "Name_lang_flags", "Description_lang_flags" }) row[f] = m_client.begin()->second[f];
    return row;
}

std::vector<Poi> ReadAreaPois(const MpqChain& chain, uint32_t map)
{
    std::vector<Poi> out;
    const auto bytes = chain.Read("DBFilesClient\\AreaPOI.dbc");
    Dbc dbc;
    if (!bytes || !dbc.Load(*bytes)) return out;
    // Layout by build (WoWDBDefs AreaPOI.dbd), told apart by field count: 3.3.5 has Icon[9] and 17-field locstrings (54
    // fields), 2.x one icon and 17-field locstrings (45), 1.x one icon and 9-field locstrings (29). Strings: the enUS slot.
    const uint32_t n = dbc.Fields();
    if (n != 54 && n != 45 && n != 29) return out;
    const uint32_t icons = n == 54 ? 9 : 1, loc = n == 29 ? 9 : 17, f = 2 + icons;   // f: FactionID
    for (uint32_t r = 0; r < dbc.Rows(); ++r)
    {
        if (dbc.U32(r, f + 4) != map) continue;
        Poi p;
        p.id = dbc.U32(r, 0);
        p.importance = dbc.U32(r, 1);
        p.icon = dbc.U32(r, 2);
        p.x = dbc.F32(r, f + 1);
        p.y = dbc.F32(r, f + 2);
        p.z = dbc.F32(r, f + 3);
        p.map = map;
        p.flags = dbc.U32(r, f + 5);
        p.area = dbc.U32(r, f + 6);
        p.name = dbc.Str(r, f + 7);
        p.description = dbc.Str(r, f + 7 + loc);
        p.worldState = dbc.U32(r, f + 7 + 2 * loc);
        if (n == 29)
        {
            // 1.x counts icons one lower and lacks flag 0x200, by the same landmarks in both clients (Dun Modr 6 / 5 -> 7 / 517,
            // Aerie Peak 4 / 13 -> 5 / 525, Tower of Ilgalar 6 / 4 -> 7 / 516).
            p.icon += 1;
            p.flags |= 0x200;
        }
        out.push_back(std::move(p));
    }
    return out;
}
