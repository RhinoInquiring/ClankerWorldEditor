#pragma once

#include "Areas.hpp"

#include <DirectXMath.h>

#include <optional>

/// A sound the client can play (SoundEntries): its name and the files it picks from.
struct SoundInfo
{
    uint32_t id = 0;
    std::string name;
    std::vector<std::string> files;   // full MPQ paths (DirectoryBase + File[i])
};

/// A SoundEmitters.dbc row: a sound placed in the world (waterfalls, braziers, crowds), in the editor's axes.
struct SoundEmitter
{
    uint32_t id = 0, map = 0, sound = 0;   // sound: SoundEntries id (the column is called SoundEntryAdvancedID, but every stock row names a SoundEntries row)
    DirectX::XMFLOAT3 pos{};
    std::string name;
};

/// The sound tables zones and emitters use. AreaTable (and WMOAreaTable) point at SoundAmbience (day / night ambience),
/// ZoneMusic (day / night music with silences between) and ZoneIntroMusicTable (a piece played on entering).
class Sounds
{
public:
    Sounds(MpqChain& mpq, ChangeStore& store);

    DbcTable entries, ambience, music, intro, emitters;
    std::vector<DbcTable*> Tables() { return { &entries, &ambience, &music, &intro, &emitters }; }

    std::optional<SoundInfo> Entry(uint32_t id) const;

    static SoundEmitter FromRow(const nlohmann::json& row);
    static nlohmann::json ToRow(const SoundEmitter& e, nlohmann::json row);
    std::vector<SoundEmitter> OnMap(uint32_t map) const;
};

/// Plays a sound file from the MPQs (wav or mp3) through Windows' media player; stops the one playing before.
/// Returns false when the file cannot be read or played.
bool PlaySoundFile(const MpqChain& mpq, const std::string& path, std::string& error);
void StopSound();
