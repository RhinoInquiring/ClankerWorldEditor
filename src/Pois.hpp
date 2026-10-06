#pragma once

#include "Areas.hpp"

/// The three kinds of point of interest:
/// - MapIcon: an AreaPOI.dbc row, a landmark icon (Interface\Minimap\POIIcons) with a name and description on the client's
///   world map. Client only: worldserver loads the file but never reads it.
/// - Gossip: a points_of_interest row, the flag a gossip option (gossip_menu_option.ActionPoiID) puts on the player's map.
///   x and y only, no map: it shows on whatever map the player is on.
/// - Tele: a game_tele row, a `.tele <name>` destination.
enum class PoiKind { MapIcon, Gossip, Tele };

struct Poi
{
    PoiKind kind = PoiKind::MapIcon;
    uint32_t id = 0, map = 0;                       // map: not for Gossip
    float x = 0, y = 0, z = 0, o = 0;               // server coordinates; z: MapIcon and Tele; o: Tele
    std::string name, description;                  // description: MapIcon
    uint32_t icon = 0, importance = 0, flags = 0;   // MapIcon and Gossip
    uint32_t area = 0, worldState = 0;              // MapIcon: AreaTable id, WorldStateID

    static Poi FromDbcRow(const nlohmann::json& row);
    /// `base` (the current row, or AreaPoiAdapter::NewRow) with the point's fields.
    nlohmann::json ToDbcRow(nlohmann::json base) const;
    static Poi FromGossipRow(const nlohmann::json& row);
    nlohmann::json ToGossipRow() const;
    static Poi FromTeleRow(const nlohmann::json& row);
    nlohmann::json ToTeleRow() const;
};

/// AreaPOI.dbc.
class AreaPoiAdapter final : public DbcTable
{
public:
    AreaPoiAdapter(MpqChain& mpq, ChangeStore& store);
    /// A row of zeros with the client's enUS string flags (new rows).
    nlohmann::json NewRow() const;
};

/// The landmarks of `map` in a client's AreaPOI.dbc, of any build from 1.x to 3.3.5 (another version's client).
std::vector<Poi> ReadAreaPois(const MpqChain& chain, uint32_t map);
