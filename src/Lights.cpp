#include "Lights.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace
{
    /// "Base[i]", with a lifetime as long as the program's (DbcField keeps the pointer).
    const char* Field(const char* base, int i)
    {
        static std::map<std::pair<std::string, int>, std::string> names;
        auto [it, added] = names.try_emplace({ base, i });
        if (added) it->second = std::string(base) + "[" + std::to_string(i) + "]";
        return it->second.c_str();
    }

    /// LightIntBand / LightFloatBand: ID, Num, Time[16], Data[16] (Data as `data` type).
    std::vector<DbcField> BandFields(char data)
    {
        std::vector<DbcField> fields{ { "ID", 0, 'i' }, { "Num", 1, 'i' } };
        for (int i = 0; i < 16; ++i) fields.push_back({ Field("Time", i), uint32_t(2 + i), 'i' });
        for (int i = 0; i < 16; ++i) fields.push_back({ Field("Data", i), uint32_t(18 + i), data });
        return fields;
    }

    float Lerp(float a, float b, float t) { return a + (b - a) * t; }
}

Lights::Lights(MpqChain& mpq, ChangeStore& store)
    : light(mpq, store, "Light",
            { { "ID", 0, 'i' }, { "ContinentID", 1, 'i' }, { "GameCoords[0]", 2, 'f' }, { "GameCoords[1]", 3, 'f' }, { "GameCoords[2]", 4, 'f' },
              { "GameFalloffStart", 5, 'f' }, { "GameFalloffEnd", 6, 'f' }, { "LightParamsID[0]", 7, 'i' }, { "LightParamsID[1]", 8, 'i' },
              { "LightParamsID[2]", 9, 'i' }, { "LightParamsID[3]", 10, 'i' }, { "LightParamsID[4]", 11, 'i' }, { "LightParamsID[5]", 12, 'i' },
              { "LightParamsID[6]", 13, 'i' }, { "LightParamsID[7]", 14, 'i' } }, 15),
      params(mpq, store, "LightParams",
             { { "ID", 0, 'i' }, { "HighlightSky", 1, 'i' }, { "LightSkyboxID", 2, 'i' }, { "Glow", 3, 'f' }, { "WaterShallowAlpha", 4, 'f' },
               { "WaterDeepAlpha", 5, 'f' }, { "OceanShallowAlpha", 6, 'f' }, { "OceanDeepAlpha", 7, 'f' }, { "Flags", 8, 'i' } }, 9),
      intBands(mpq, store, "LightIntBand", BandFields('i'), 34),
      floatBands(mpq, store, "LightFloatBand", BandFields('f'), 34),
      skyboxes(mpq, store, "LightSkybox", { { "ID", 0, 'i' }, { "Name", 1, 's' }, { "Flags", 2, 'i' } }, 3)
{
}

const nlohmann::json& Lights::Row(const DbcTable& table, uint32_t id, const Draft* draft) const
{
    if (draft)
        if (auto it = draft->find({ table.Name(), id }); it != draft->end()) return it->second;
    return table.Row(id);
}

LightVolume Lights::FromRow(const nlohmann::json& row)
{
    LightVolume v;
    v.id = row.value("ID", 0u);
    v.map = row.value("ContinentID", 0u);
    v.pos = { row.value("GameCoords[0]", 0.0f) / 36, row.value("GameCoords[1]", 0.0f) / 36, row.value("GameCoords[2]", 0.0f) / 36 };
    v.inner = row.value("GameFalloffStart", 0.0f) / 36;
    v.outer = row.value("GameFalloffEnd", 0.0f) / 36;
    for (int i = 0; i < 8; ++i) v.params[size_t(i)] = row.value(Field("LightParamsID", i), 0u);
    return v;
}

nlohmann::json Lights::ToRow(const LightVolume& v, nlohmann::json row)
{
    row["ID"] = v.id;
    row["ContinentID"] = v.map;
    row["GameCoords[0]"] = v.pos.x * 36;
    row["GameCoords[1]"] = v.pos.y * 36;
    row["GameCoords[2]"] = v.pos.z * 36;
    row["GameFalloffStart"] = v.inner * 36;
    row["GameFalloffEnd"] = v.outer * 36;
    for (int i = 0; i < 8; ++i) row[Field("LightParamsID", i)] = v.params[size_t(i)];
    return row;
}

std::vector<LightVolume> Lights::OnMap(uint32_t map, const Draft* draft) const
{
    std::vector<LightVolume> out;
    for (const auto& [id, row] : light.Rows())
    {
        const nlohmann::json& r = Row(light, id, draft);
        if (!r.is_null() && r.value("ContinentID", 0u) == map) out.push_back(FromRow(r));
    }
    if (draft)   // lights added in the draft only
        for (const auto& [key, r] : *draft)
            if (key.first == light.Name() && !r.is_null() && r.value("ContinentID", 0u) == map && light.Row(key.second).is_null())
                out.push_back(FromRow(r));
    std::stable_partition(out.begin(), out.end(), [](const LightVolume& v) { return v.Global(); });
    return out;
}

std::vector<std::pair<int, uint32_t>> Lights::Keys(const nlohmann::json& band)
{
    std::vector<std::pair<int, uint32_t>> keys;
    if (band.is_null()) return keys;
    const uint32_t n = std::min(band.value("Num", 0u), 16u);
    for (uint32_t i = 0; i < n; ++i)
    {
        const nlohmann::json& d = band.at(Field("Data", int(i)));
        uint32_t raw = 0;
        if (d.is_number_float()) raw = FloatBits(d.get<float>());
        else raw = d.get<uint32_t>();
        keys.push_back({ band.value(Field("Time", int(i)), 0), raw });
    }
    return keys;
}

nlohmann::json Lights::SetKeys(nlohmann::json band, const std::vector<std::pair<int, uint32_t>>& keys)
{
    const bool floats = band.contains("Data[0]") && band.at("Data[0]").is_number_float();
    std::vector<std::pair<int, uint32_t>> sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (sorted.size() > 16) sorted.resize(16);
    band["Num"] = uint32_t(sorted.size());
    for (int i = 0; i < 16; ++i)
    {
        const bool used = size_t(i) < sorted.size();
        band[Field("Time", i)] = used ? sorted[size_t(i)].first : 0;
        if (floats) band[Field("Data", i)] = used ? FloatOf(sorted[size_t(i)].second) : 0.0f;
        else band[Field("Data", i)] = used ? sorted[size_t(i)].second : 0u;
    }
    return band;
}

XMFLOAT3 Lights::Color(uint32_t raw)
{
    // 0x00RRGGBB, as the client's bands store it.
    return { ((raw >> 16) & 0xFF) / 255.0f, ((raw >> 8) & 0xFF) / 255.0f, (raw & 0xFF) / 255.0f };
}

uint32_t Lights::Raw(const XMFLOAT3& c)
{
    auto b = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return b(c.x) << 16 | b(c.y) << 8 | b(c.z);
}

uint32_t Lights::FloatBits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }
float Lights::FloatOf(uint32_t bits) { float v; std::memcpy(&v, &bits, 4); return v; }

LightState Lights::Params(uint32_t paramsId, int time, const Draft* draft) const
{
    LightState s;
    if (!paramsId) return s;
    time = ((time % kDayHalfMinutes) + kDayHalfMinutes) % kDayHalfMinutes;
    // The two keys around `time` (wrapping over midnight) and how far between them it is.
    auto around = [&](const std::vector<std::pair<int, uint32_t>>& keys, uint32_t& a, uint32_t& b, float& t) {
        if (keys.empty()) return false;
        size_t next = 0;
        while (next < keys.size() && keys[next].first <= time) ++next;
        const auto& k1 = keys[(next + keys.size() - 1) % keys.size()];
        const auto& k2 = keys[next % keys.size()];
        int span = k2.first - k1.first, into = time - k1.first;
        if (span <= 0) span += kDayHalfMinutes;
        if (into < 0) into += kDayHalfMinutes;
        a = k1.second;
        b = k2.second;
        t = keys.size() == 1 ? 0.0f : std::clamp(float(into) / float(span), 0.0f, 1.0f);
        return true;
    };
    for (int i = 0; i < 18; ++i)
    {
        uint32_t a, b;
        float t;
        if (!around(Keys(Row(intBands, paramsId * 18 - 17 + uint32_t(i), draft)), a, b, t)) continue;
        const XMFLOAT3 ca = Color(a), cb = Color(b);
        s.colors[size_t(i)] = { Lerp(ca.x, cb.x, t), Lerp(ca.y, cb.y, t), Lerp(ca.z, cb.z, t) };
    }
    for (int i = 0; i < 6; ++i)
    {
        uint32_t a, b;
        float t;
        if (!around(Keys(Row(floatBands, paramsId * 6 - 5 + uint32_t(i), draft)), a, b, t)) continue;
        s.floats[size_t(i)] = Lerp(FloatOf(a), FloatOf(b), t);
    }
    s.floats[0] /= 36;   // fog distance: inches in the file
    const nlohmann::json& p = Row(params, paramsId, draft);
    if (!p.is_null())
    {
        s.skybox = p.value("LightSkyboxID", 0u);
        s.glow = p.value("Glow", 0.0f);
    }
    return s;
}

LightState Lights::At(uint32_t map, const XMFLOAT3& pos, int time, int slot, const Draft* draft) const
{
    return At(OnMap(map, draft), pos, time, slot, draft);
}

LightState Lights::At(const std::vector<LightVolume>& all, const XMFLOAT3& pos, int time, int slot, const Draft* draft) const
{
    auto setOf = [&](const LightVolume& v) { return v.params[size_t(slot)] ? v.params[size_t(slot)] : v.params[0]; };
    LightState s;
    if (!all.empty() && all.front().Global())
        s = Params(setOf(all.front()), time, draft);
    else if (const nlohmann::json& fallback = Row(light, 1, draft); !fallback.is_null())   // no default of its own: the client uses Light 1
        s = Params(setOf(FromRow(fallback)), time, draft);

    // Local lights: full inside the inner radius, fading out to the outer one.
    // ponytail: blended in order of weight (strongest last); the client's exact rule for overlaps is unverified.
    std::vector<std::pair<float, const LightVolume*>> local;
    for (const LightVolume& v : all)
    {
        if (v.Global()) continue;
        const float dx = pos.x - v.pos.x, dy = pos.y - v.pos.y, dz = pos.z - v.pos.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= v.outer) continue;
        const float w = d <= v.inner || v.outer <= v.inner ? 1.0f : 1.0f - (d - v.inner) / (v.outer - v.inner);
        local.push_back({ w, &v });
    }
    std::sort(local.begin(), local.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [w, v] : local)
    {
        const LightState l = Params(setOf(*v), time, draft);
        for (size_t i = 0; i < 18; ++i)
            s.colors[i] = { Lerp(s.colors[i].x, l.colors[i].x, w), Lerp(s.colors[i].y, l.colors[i].y, w), Lerp(s.colors[i].z, l.colors[i].z, w) };
        for (size_t i = 0; i < 6; ++i) s.floats[i] = Lerp(s.floats[i], l.floats[i], w);
        if (w >= 0.5f) { s.skybox = l.skybox; s.glow = l.glow; }
    }
    return s;
}

uint32_t Lights::FreeParamsId(uint32_t first, uint32_t last) const
{
    for (uint32_t id = std::max(first, 1u); id <= last && id; ++id)
    {
        bool free = params.Row(id).is_null();
        for (uint32_t i = 0; free && i < 18; ++i) free = intBands.Row(id * 18 - 17 + i).is_null();
        for (uint32_t i = 0; free && i < 6; ++i) free = floatBands.Row(id * 6 - 5 + i).is_null();
        if (free) return id;
    }
    return 0;
}

std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> Lights::CopyParams(uint32_t from, uint32_t to, const Draft* draft)
{
    std::vector<std::tuple<DbcTable*, uint32_t, nlohmann::json>> rows;
    nlohmann::json p = Row(params, from, draft);
    if (p.is_null()) return rows;
    p["ID"] = to;
    rows.push_back({ &params, to, p });
    for (uint32_t i = 0; i < 18; ++i)
    {
        nlohmann::json b = Row(intBands, from * 18 - 17 + i, draft);
        if (b.is_null()) continue;
        b["ID"] = to * 18 - 17 + i;
        rows.push_back({ &intBands, to * 18 - 17 + i, b });
    }
    for (uint32_t i = 0; i < 6; ++i)
    {
        nlohmann::json b = Row(floatBands, from * 6 - 5 + i, draft);
        if (b.is_null()) continue;
        b["ID"] = to * 6 - 5 + i;
        rows.push_back({ &floatBands, to * 6 - 5 + i, b });
    }
    return rows;
}

size_t Lights::Users(uint32_t paramsId, const Draft* draft) const
{
    size_t n = 0;
    for (const auto& [id, row] : light.Rows())
    {
        const nlohmann::json& r = Row(light, id, draft);
        if (r.is_null()) continue;
        for (int i = 0; i < 8; ++i)
            if (r.value(Field("LightParamsID", i), 0u) == paramsId) { ++n; break; }
    }
    return n;
}
