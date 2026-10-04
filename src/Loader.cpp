#include "Loader.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <cctype>

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    }
}

void Loader::Start(const MpqChain* mpq)
{
    Stop();
    m_mpq = mpq;
    m_stop = false;
    m_thread = std::thread([this] { Run(); });
}

void Loader::Stop()
{
    {
        std::lock_guard lock(m_lock);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lock(m_lock);
    m_wanted.clear();
    m_working.clear();
    m_ready.clear();
    m_seen.clear();
    m_images.clear();
    m_meshes.clear();
    m_map.clear();
    m_pending = 0;
}

void Loader::Want(const std::string& map, bool bigAlpha, std::vector<int> keys)
{
    {
        std::lock_guard lock(m_lock);
        if (map != m_map)   // another map: nothing prepared for the old one is useful
        {
            m_map = map;
            m_working.clear();
            m_ready.clear();
        }
        m_bigAlpha = bigAlpha;
        std::erase_if(keys, [&](int k) { return m_working.count(k) != 0; });
        m_wanted = std::move(keys);
        m_pending = m_wanted.size() + m_working.size();
    }
    m_wake.notify_one();
}

std::optional<Loader::Tile> Loader::TakeTile()
{
    std::lock_guard lock(m_lock);
    for (auto it = m_ready.begin(); it != m_ready.end(); ++it)
        if (it->map == m_map)
        {
            Tile t = std::move(*it);
            m_ready.erase(it);
            m_working.erase(t.key);
            return t;
        }
    m_ready.clear();   // only other maps' tiles left
    return std::nullopt;
}

std::optional<BlpImage> Loader::TakeImage(const std::string& lowerName)
{
    std::lock_guard lock(m_lock);
    auto it = m_images.find(lowerName);
    if (it == m_images.end()) return std::nullopt;
    BlpImage image = std::move(it->second);
    m_images.erase(it);
    return image;
}

std::optional<ModelMesh> Loader::TakeMesh(const std::string& lowerName)
{
    std::lock_guard lock(m_lock);
    auto it = m_meshes.find(lowerName);
    if (it == m_meshes.end()) return std::nullopt;
    ModelMesh mesh = std::move(it->second);
    m_meshes.erase(it);
    return mesh;
}

void Loader::PrepareImage(const std::string& name)
{
    const std::string key = Lower(name);
    {
        std::lock_guard lock(m_lock);
        if (name.empty() || !m_seen.insert(key).second) return;
    }
    if (auto bytes = m_mpq->Read(name))
        if (auto image = ParseBlp(*bytes))
        {
            std::lock_guard lock(m_lock);
            m_images[key] = std::move(*image);
        }
}

void Loader::PrepareModel(const std::string& name, bool wmo)
{
    const std::string key = Lower(name);
    {
        std::lock_guard lock(m_lock);
        if (!m_seen.insert("model:" + key).second) return;
    }
    std::optional<ModelMesh> mesh;
    if (wmo)
    {
        uint32_t groups = 0;
        float bounds[6];
        if (auto root = m_mpq->Read(name); root && WmoRootInfo(*root, groups, bounds))
        {
            std::vector<std::vector<uint8_t>> files;
            for (uint32_t g = 0; g < groups; ++g) files.push_back(m_mpq->Read(WmoGroupName(name, g)).value_or(std::vector<uint8_t>{}));
            mesh = ParseWmo(*root, files);
        }
    }
    else
        mesh = LoadM2(name, [this](const std::string& path) { return m_mpq->Read(path); });
    if (!mesh) return;   // the renderer tries again itself and remembers the failure
    for (const auto& b : mesh->batches) PrepareImage(b.texture);
    // A WMO's doodads first, so they are ready by the time the WMO is (the renderer adds them with it).
    for (const auto& d : mesh->doodads)
        if (!d.model.empty()) PrepareModel(M2Name(d.model), false);
    std::lock_guard lock(m_lock);
    m_meshes[key] = std::move(*mesh);
}

void Loader::Run()
{
    for (;;)
    {
        Tile tile;
        bool bigAlpha = false;
        {
            std::unique_lock lock(m_lock);
            m_wake.wait(lock, [&] { return m_stop || !m_wanted.empty(); });
            if (m_stop) return;
            tile.map = m_map;
            tile.key = m_wanted.front();
            m_wanted.erase(m_wanted.begin());
            m_working.insert(tile.key);
            bigAlpha = m_bigAlpha;
        }
        const std::string name = "World\\Maps\\" + tile.map + "\\" + tile.map + "_" + std::to_string(tile.key % 64) + "_" +
                                 std::to_string(tile.key / 64) + ".adt";
        if (auto bytes = m_mpq->Read(name))
        {
            tile.adt = ParseAdt(*bytes, bigAlpha);
            tile.bytes = std::move(*bytes);
        }
        if (tile.adt)
        {
            for (const std::string& t : tile.adt->textures) PrepareImage(t);
            for (const auto& d : tile.adt->doodads) PrepareModel(M2Name(d.model), false);
            for (const auto& w : tile.adt->wmos) PrepareModel(w.model, true);
        }
        std::lock_guard lock(m_lock);
        if (m_stop) return;
        if (tile.map == m_map) m_ready.push_back(std::move(tile));
        else m_working.erase(tile.key);
    }
}
