#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class MpqChain;

/// Every file the client's MPQs list, sorted by kind for the asset browser. Built once per project open from
/// the archives' listfiles (no file is read). Tags come from path segments, so a search for "elwynn tree"
/// matches World\Azeroth\Elwynn\PassiveDoodads\Trees\ElwynnTreeMid01.m2.
// ponytail: one MPQ chain, in memory; the spec's SQLite index arrives with extra sources (other clients, CDN).
class Catalog
{
public:
    enum class Kind : uint8_t { Doodad, Wmo, GroundTexture, Texture, Other, Count };

    struct Item
    {
        std::string path;    // as the listfile spells it
        std::string lower;   // for searching
        Kind kind = Kind::Other;
        uint16_t archive = 0;
    };

    /// A folder of one kind's items, with the number of items below it.
    struct Folder
    {
        std::string name;     // as first spelled in the listfile
        std::string prefix;   // lower-case path with a trailing backslash ("" = everything)
        size_t count = 0;
        std::map<std::string, Folder> children;   // by lower-case name (the listfile spells folders inconsistently)
    };

    void Build(const MpqChain& mpq);
    void Clear() { *this = Catalog{}; }
    bool Empty() const { return m_items.empty(); }

    size_t Count(Kind kind) const { return m_counts[size_t(kind)]; }
    const Folder& Tree(Kind kind) const { return m_trees[size_t(kind)]; }

    /// Items of a kind (Kind::Count = all) under a folder prefix whose path contains every word of `query`.
    std::vector<const Item*> Filter(Kind kind, const std::string& folderPrefix, const std::string& query) const;

    static Kind Classify(const std::string& lowerPath);
    /// Lower-case catalog path for a name as ADTs store it (doodads may say .mdx or .mdl where the file is .m2).
    static std::string Normalize(const std::string& name);

private:
    std::vector<Item> m_items;
    std::array<size_t, size_t(Kind::Count)> m_counts{};
    std::array<Folder, size_t(Kind::Count) + 1> m_trees{};   // last: all files
};

/// Classification and filtering checks; false on the first mismatch.
bool CatalogSelfTest();
