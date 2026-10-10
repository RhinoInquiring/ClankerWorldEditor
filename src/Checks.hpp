#pragma once

// The command-line checks (docs/development/checks.md): each prints what it did and returns the exit code.

#include "App.hpp"
#include "Assets.hpp"
#include "Downport.hpp"
#include "Blend.hpp"
#include "Catalog.hpp"
#include "Formats.hpp"
#include "Ghosts.hpp"
#include "Minimap.hpp"
#include "Loader.hpp"
#include "ModelRenderer.hpp"
#include "Models.hpp"
#include "Mpq.hpp"
#include "Project.hpp"
#include "Changes.hpp"
#include "Renderer.hpp"
#include "Server.hpp"
#include "Looks.hpp"
#include "Paths.hpp"
#include "Spawns.hpp"
#include "Tables.hpp"
#include "Terrain.hpp"
#include "Flights.hpp"
#include "Pois.hpp"
#include "Triggers.hpp"
#include "Lights.hpp"
#include "Sounds.hpp"
#include "Roads.hpp"
#include "Races.hpp"
#include "ServerData.hpp"
#include <imgui.h>
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <windows.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <map>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <string>
#include <cstring>
#include <optional>
#include <tuple>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

/// The check named by the first argument (`--selftest`, `--check`, ...) run to its exit code; -1 when the command line names none.
int RunCheck();

namespace checks
{
    int Check();
    int RewriteCheck();
    int ShadeCheck();
    int PlanCheck();
    int ModelCheck();
    bool SavePng(const std::wstring& path, UINT width, UINT height, std::vector<uint8_t> bgra);
    int SkinCheck();
    int AppearanceCheck();
    int CatalogCheck();
    int BlueprintCheck();
    int CascToMpq();
    int AssetCheck();
    int CompareCheck();
    int DiffCheck();
    int DiffObjects();
    int PackAndVerify(const std::filesystem::path& folder, const std::filesystem::path& scratch, const std::string& name);
    int SourcesCheck();
    int ScanCheck();
    int MpqCheck();
    int TilesCheck();
    int WaterCheck();
    int WaterToolCheck();
    int SculptCheck();
    int NewMapCheck();
    int SwapCheck();
    int ImpassCheck();
    int GhostCheck();
    int StreamCheck();
    int MinimapCheck();
    int RaceCheck();
    int RaceLookCheck();
    int RaceImportCheck();
    int RaceEditCheck();
    int LoaderCheck();
    int RenderCheck();
    int PoisCheck();
    int TaxiCheck();
    int PoisRead();
    int TriggersCheck();
    int ExportCmd();
    int MapPreviewCmd();
    int WhereCmd();
    int AnimCheck();
    int DialogueCheck();
    int NpcCheck();
    int WeatherCheck();
    int SpawnCheck();
    int UnitCatalogCheck();
    int SqlCmd();
    int ServerCheck();
    int CdnCheck();
    int ExtractCmd();
    int GroundEffectsCmd();
    int ValidateRefsCmd();
    int ValidateCmd();
    int NormalsCheck();
    int FindCmd();
    int PerfCheck();
    int ServerDataCheck();
    int RoadCheck();
    int SoundCheck();
    int LightCheck();
    int AreaCheck();
}
