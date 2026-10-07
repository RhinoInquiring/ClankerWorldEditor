#include "Sounds.hpp"

#include "Mpq.hpp"
#include "Spawns.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <filesystem>
#include <fstream>
#include <map>

using namespace DirectX;

namespace
{
    /// "Base[i]", kept for the program's lifetime (DbcField holds the pointer).
    const char* Field(const char* base, int i)
    {
        static std::map<std::pair<std::string, int>, std::string> names;
        auto [it, added] = names.try_emplace({ base, i });
        if (added) it->second = std::string(base) + "[" + std::to_string(i) + "]";
        return it->second.c_str();
    }

    std::vector<DbcField> EntryFields()
    {
        std::vector<DbcField> f{ { "ID", 0, 'i' }, { "SoundType", 1, 'i' }, { "Name", 2, 's' } };
        for (int i = 0; i < 10; ++i) f.push_back({ Field("File", i), uint32_t(3 + i), 's' });
        for (int i = 0; i < 10; ++i) f.push_back({ Field("Freq", i), uint32_t(13 + i), 'i' });
        f.insert(f.end(), { { "DirectoryBase", 23, 's' }, { "VolumeFloat", 24, 'f' }, { "Flags", 25, 'i' }, { "MinDistance", 26, 'f' },
                            { "DistanceCutoff", 27, 'f' }, { "EAXDef", 28, 'i' }, { "SoundEntriesAdvancedID", 29, 'i' } });
        return f;
    }
}

Sounds::Sounds(MpqChain& mpq, ChangeStore& store)
    : entries(mpq, store, "SoundEntries", EntryFields(), 30),
      ambience(mpq, store, "SoundAmbience", { { "ID", 0, 'i' }, { "AmbienceID[0]", 1, 'i' }, { "AmbienceID[1]", 2, 'i' } }, 3),
      music(mpq, store, "ZoneMusic",
            { { "ID", 0, 'i' }, { "SetName", 1, 's' }, { "SilenceIntervalMin[0]", 2, 'i' }, { "SilenceIntervalMin[1]", 3, 'i' },
              { "SilenceIntervalMax[0]", 4, 'i' }, { "SilenceIntervalMax[1]", 5, 'i' }, { "Sounds[0]", 6, 'i' }, { "Sounds[1]", 7, 'i' } }, 8),
      intro(mpq, store, "ZoneIntroMusicTable",
            { { "ID", 0, 'i' }, { "Name", 1, 's' }, { "SoundID", 2, 'i' }, { "Priority", 3, 'i' }, { "MinDelayMinutes", 4, 'i' } }, 5),
      emitters(mpq, store, "SoundEmitters",
               { { "ID", 0, 'i' }, { "Position[0]", 1, 'f' }, { "Position[1]", 2, 'f' }, { "Position[2]", 3, 'f' }, { "Direction[0]", 4, 'f' },
                 { "Direction[1]", 5, 'f' }, { "Direction[2]", 6, 'f' }, { "SoundEntryAdvancedID", 7, 'i' }, { "MapID", 8, 'i' }, { "Name", 9, 's' } }, 10)
{
}

std::optional<SoundInfo> Sounds::Entry(uint32_t id) const
{
    const nlohmann::json& row = entries.Row(id);
    if (!id || row.is_null()) return std::nullopt;
    SoundInfo s;
    s.id = id;
    s.name = row.value("Name", std::string());
    std::string dir = row.value("DirectoryBase", std::string());
    if (!dir.empty() && dir.back() != '\\') dir += '\\';
    for (int i = 0; i < 10; ++i)
        if (const std::string file = row.value(Field("File", i), std::string()); !file.empty()) s.files.push_back(dir + file);
    return s;
}

SoundEmitter Sounds::FromRow(const nlohmann::json& row)
{
    SoundEmitter e;
    e.id = row.value("ID", 0u);
    e.map = row.value("MapID", 0u);
    e.sound = row.value("SoundEntryAdvancedID", 0u);
    e.name = row.value("Name", std::string());
    e.pos = ServerToEditor(row.value("Position[0]", 0.0f), row.value("Position[1]", 0.0f), row.value("Position[2]", 0.0f));
    return e;
}

nlohmann::json Sounds::ToRow(const SoundEmitter& e, nlohmann::json row)
{
    float x, y, z;
    EditorToServer(e.pos, x, y, z);
    row["ID"] = e.id;
    row["MapID"] = e.map;
    row["SoundEntryAdvancedID"] = e.sound;
    row["Name"] = e.name;
    row["Position[0]"] = x;
    row["Position[1]"] = y;
    row["Position[2]"] = z;
    for (const char* d : { "Direction[0]", "Direction[1]", "Direction[2]" })
        if (!row.contains(d)) row[d] = 0.0f;
    return row;
}

std::vector<SoundEmitter> Sounds::OnMap(uint32_t map) const
{
    std::vector<SoundEmitter> out;
    for (const auto& [id, row] : emitters.Rows())
        if (!row.is_null() && row.value("MapID", 0u) == map) out.push_back(FromRow(row));
    return out;
}

void StopSound()
{
    mciSendStringW(L"close wwe_sound", nullptr, 0, nullptr);
    std::error_code ec;   // the temporary copy goes with it
    for (const char* ext : { ".wav", ".mp3", ".ogg" })
        std::filesystem::remove(std::filesystem::temp_directory_path() / (std::string("wow-world-editor-sound") + ext), ec);
}

bool PlaySoundFile(const MpqChain& mpq, const std::string& path, std::string& error)
{
    StopSound();
    const auto bytes = mpq.Read(path);
    if (!bytes) { error = "not found: " + path; return false; }
    // The media player needs a file: one temporary copy, replaced by the next sound.
    const std::filesystem::path ext = std::filesystem::path(path).extension();
    const std::filesystem::path temp = std::filesystem::temp_directory_path() / ("wow-world-editor-sound" + ext.string());
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
        if (!f) { error = "cannot write " + temp.string(); return false; }
    }
    const std::wstring open = L"open \"" + temp.wstring() + L"\" type mpegvideo alias wwe_sound";
    if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0 || mciSendStringW(L"play wwe_sound", nullptr, 0, nullptr) != 0)
    {
        error = "Windows could not play " + path;
        StopSound();
        return false;
    }
    return true;
}
