#include "Catalog.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool EndsWith(const std::string& s, const char* suffix)
    {
        const size_t n = std::char_traits<char>::length(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }
}

Catalog::Kind Catalog::Classify(const std::string& p)
{
    if (EndsWith(p, ".m2")) return Kind::Doodad;
    if (EndsWith(p, ".wmo"))
    {
        // Group files (Name_000.wmo, newer LODs Name_000_lod1.wmo) belong to their root; only roots are placeable.
        size_t n = p.size();
        if (n > 9 && p.compare(n - 9, 4, "_lod") == 0 && std::isdigit((unsigned char)p[n - 5])) n -= 5;
        const bool group = n > 8 && p[n - 8] == '_' && std::isdigit((unsigned char)p[n - 7]) && std::isdigit((unsigned char)p[n - 6]) &&
                           std::isdigit((unsigned char)p[n - 5]);
        return group ? Kind::Other : Kind::Wmo;
    }
    if (EndsWith(p, ".blp"))
    {
        // Terrain textures live under Tileset\; their _s (specular) and _h (height) companions are not paintable.
        if (p.rfind("tileset\\", 0) == 0 && !EndsWith(p, "_s.blp") && !EndsWith(p, "_h.blp")) return Kind::GroundTexture;
        return Kind::Texture;
    }
    return Kind::Other;
}

std::string Catalog::Normalize(const std::string& name)
{
    std::string n = Lower(name);
    if (EndsWith(n, ".mdx") || EndsWith(n, ".mdl")) n.replace(n.size() - 4, 4, ".m2");
    return n;
}

void Catalog::Build(const MpqChain& mpq)
{
    Clear();
    for (auto& e : mpq.List())
    {
        Item item{ std::move(e.name), {}, Kind::Other, uint16_t(e.archive) };
        item.lower = Lower(item.path);
        item.kind = Classify(item.lower);
        m_items.push_back(std::move(item));
    }
    std::sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) { return a.lower < b.lower; });

    for (const Item& item : m_items)
    {
        ++m_counts[size_t(item.kind)];
        for (Folder* root : { &m_trees[size_t(item.kind)], &m_trees.back() })
        {
            Folder* f = root;
            ++f->count;
            size_t start = 0;
            for (size_t slash = item.path.find('\\'); slash != std::string::npos; slash = item.path.find('\\', start))
            {
                auto [it, added] = f->children.try_emplace(item.lower.substr(start, slash - start));
                if (added)
                {
                    it->second.name = item.path.substr(start, slash - start);
                    it->second.prefix = item.lower.substr(0, slash + 1);
                }
                f = &it->second;
                ++f->count;
                start = slash + 1;
            }
        }
    }
}

std::vector<const Catalog::Item*> Catalog::Filter(Kind kind, const std::string& folderPrefix, const std::string& query) const
{
    std::vector<std::string> words;
    std::istringstream in(Lower(query));
    for (std::string w; in >> w;) words.push_back(w);

    std::vector<const Item*> out;
    // Items are sorted by path, so a folder is one contiguous range.
    auto it = std::lower_bound(m_items.begin(), m_items.end(), folderPrefix, [](const Item& a, const std::string& p) { return a.lower < p; });
    for (; it != m_items.end() && it->lower.compare(0, folderPrefix.size(), folderPrefix) == 0; ++it)
    {
        if (kind != Kind::Count && it->kind != kind) continue;
        bool all = true;
        for (const std::string& w : words) all = all && it->lower.find(w) != std::string::npos;
        if (all) out.push_back(&*it);
    }
    return out;
}

bool CatalogSelfTest()
{
    using K = Catalog::Kind;
    if (Catalog::Classify("world\\azeroth\\elwynn\\passivedoodads\\trees\\elwynntreemid01.m2") != K::Doodad) return false;
    if (Catalog::Classify("world\\wmo\\azeroth\\buildings\\stormwind\\stormwind.wmo") != K::Wmo) return false;
    if (Catalog::Classify("world\\wmo\\azeroth\\buildings\\stormwind\\stormwind_012.wmo") != K::Other) return false;
    if (Catalog::Classify("world\\wmo\\azeroth\\buildings\\stormwind\\stormwind_012_lod1.wmo") != K::Other) return false;
    if (Catalog::Classify("tileset\\elwynn\\elwynngrassbase.blp") != K::GroundTexture) return false;
    if (Catalog::Classify("tileset\\elwynn\\elwynngrassbase_s.blp") != K::Texture) return false;
    if (Catalog::Classify("interface\\icons\\inv_misc_qblp.blp") != K::Texture) return false;
    if (Catalog::Normalize("World\\Foo\\Bar.MDX") != "world\\foo\\bar.m2") return false;
    return true;
}
