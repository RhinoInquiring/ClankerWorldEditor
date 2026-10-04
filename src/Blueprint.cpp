#include "Blueprint.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;

bool Blueprint::Save(const fs::path& dir, std::string& error)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (file.empty())
    {
        // File name from the blueprint's name: letters, digits, - and _ only, numbered if taken.
        std::string base;
        for (char c : name) base += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
        if (base.empty()) base = "blueprint";
        file = dir / (base + ".json");
        for (int n = 2; fs::exists(file); ++n) file = dir / (base + "-" + std::to_string(n) + ".json");
    }
    const nlohmann::json j = { { "format", 1 }, { "name", name }, { "notes", notes }, { "map", map }, { "created", created },
                               { "thumbSize", thumbSize }, { "thumb", Base64Encode(thumb.data(), thumb.size()) }, { "area", clip.ToJson() } };
    std::ofstream f(file);
    f << j.dump(1) << "\n";
    if (!f) { error = "Cannot write " + file.string(); return false; }
    return true;
}

std::optional<Blueprint> Blueprint::Load(const fs::path& path, std::string& error)
{
    try
    {
        std::ifstream f(path);
        if (!f) { error = "Cannot read " + path.string(); return std::nullopt; }
        const nlohmann::json j = nlohmann::json::parse(f);
        Blueprint b;
        b.file = path;
        b.name = j.value("name", path.stem().string());
        b.notes = j.value("notes", "");
        b.map = j.value("map", "");
        b.created = j.value("created", "");
        b.thumbSize = j.value("thumbSize", 0u);
        b.thumb = Base64Decode(j.value("thumb", ""));
        if (b.thumb.size() != size_t(b.thumbSize) * b.thumbSize * 4) { b.thumb.clear(); b.thumbSize = 0; }
        b.clip = TerrainClipboard::FromJson(j.at("area"));
        return b;
    }
    catch (const std::exception& e)
    {
        error = path.filename().string() + ": " + e.what();
        return std::nullopt;
    }
}

std::vector<Blueprint> Blueprint::LoadAll(const fs::path& dir, std::vector<std::string>& errors)
{
    std::vector<Blueprint> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (entry.path().extension() != ".json") continue;
        std::string error;
        if (auto b = Load(entry.path(), error)) out.push_back(std::move(*b));
        else errors.push_back(error);
    }
    std::sort(out.begin(), out.end(), [](const Blueprint& a, const Blueprint& b) { return a.name < b.name; });
    return out;
}

bool BlueprintSelfTest()
{
    Blueprint b;
    b.name = "Test: ruins/1";
    b.map = "Azeroth";
    b.clip.originX = 517;
    b.clip.originZ = 772;
    TerrainClipboard::Entry e{ 1, 0 };
    for (size_t k = 0; k < 145; ++k) e.heights[k] = 10.0f + float(k) * 0.37f;
    e.holes = 0x0801;
    b.clip.chunks.push_back(e);
    b.clip.doodads.push_back({ "World\\x.m2", { 1, 2, 3 }, { 4, 5, 6 }, 1.5f, 42 });
    b.thumbSize = 2;
    b.thumb = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    const fs::path dir = fs::temp_directory_path() / "wow-world-editor-blueprint-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string error;
    if (!b.Save(dir, error) || b.file.filename() != "Test__ruins_1.json") return false;
    const auto back = Blueprint::Load(b.file, error);
    fs::remove_all(dir, ec);
    if (!back || back->name != b.name || back->thumb != b.thumb || back->clip.originX != 517 || back->clip.chunks.size() != 1) return false;
    const auto& c = back->clip.chunks[0];
    if (c.dx != 1 || c.heights != e.heights || c.holes != 0x0801) return false;
    if (back->clip.doodads.size() != 1 || back->clip.doodads[0].scale != 1.5f || back->clip.doodads[0].pos[2] != 3.0f) return false;
    // As a tile: chunk at its copied spot, heights relative to the lowest vertex.
    const Adt adt = back->clip.ToAdt();
    return adt.chunks.size() == 1 && adt.chunks[0].baseX == 518 * kChunkSize && adt.chunks[0].baseY == 10.0f && adt.doodads[0].pos[0] == 1 + 517 * kChunkSize;
}
