// Command-line checks: the table RunCheck dispatches from, and helpers several checks share.

#include "Checks.hpp"

namespace checks
{
    bool SavePng(const std::wstring& path, UINT width, UINT height, std::vector<uint8_t> bgra)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
               SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               format == GUID_WICPixelFormat32bppBGRA &&
               SUCCEEDED(frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data())) && SUCCEEDED(frame->Commit()) &&
               SUCCEEDED(encoder->Commit());
    }

    /// Packs `folder` into `<scratch>/Data/<name>`, opens that Data folder as a client would, and compares every file
    /// read back with the one on disk. Returns how many differ (or are missing); prints a line.
    int PackAndVerify(const std::filesystem::path& folder, const std::filesystem::path& scratch, const std::string& name)
    {
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        const auto archive = scratch / "Data" / name;
        std::string error;
        size_t files = 0;
        if (!WriteMpq(archive, folder, error, &files)) { printf("  pack: %s\n", error.c_str()); return 1; }
        int bad = 0;
        {
            MpqChain mpq;
            mpq.Open((scratch / "Data").string());
            for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec))
            {
                if (!entry.is_regular_file()) continue;
                std::ifstream f(entry.path(), std::ios::binary);
                const std::vector<uint8_t> disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                const auto packed = mpq.Read(std::filesystem::relative(entry.path(), folder, ec).string());
                bad += !packed || *packed != disk;
            }
            printf("  packed %zu file(s) into %s (%.1f MB from %.1f MB), read back: %d differ\n", files, name.c_str(),
                   double(std::filesystem::file_size(archive, ec)) / 1048576.0, [&] {
                       uintmax_t total = 0;
                       for (const auto& e : std::filesystem::recursive_directory_iterator(folder, ec)) total += e.is_regular_file() ? e.file_size() : 0;
                       return double(total) / 1048576.0;
                   }(), bad);
        }
        std::filesystem::remove_all(scratch, ec);
        return bad;
    }
}

int RunCheck()
{
    struct Entry { const wchar_t* flag; int minArgs; int (*run)(); };   // minArgs: __argc, the exe and the flag included
    static const Entry kChecks[] = {
        { L"--model-check", 0, checks::ModelCheck },
        { L"--render", 0, checks::RenderCheck },
        { L"--loader-check", 0, checks::LoaderCheck },
        { L"--catalog-check", 0, checks::CatalogCheck },
        { L"--stream-check", 0, checks::StreamCheck },
        { L"--skin-check", 0, checks::SkinCheck },
        { L"--appearance-check", 0, checks::AppearanceCheck },
        { L"--ghost-check", 0, checks::GhostCheck },
        { L"--compare-check", 0, checks::CompareCheck },
        { L"--water-tool-check", 0, checks::WaterToolCheck },
        { L"--sculpt-check", 0, checks::SculptCheck },
        { L"--newmap-check", 0, checks::NewMapCheck },
        { L"--swap-check", 0, checks::SwapCheck },
        { L"--impass-check", 0, checks::ImpassCheck },
        { L"--water-check", 0, checks::WaterCheck },
        { L"--diff-check", 0, checks::DiffCheck },
        { L"--tiles-check", 0, checks::TilesCheck },
        { L"--minimap-check", 0, checks::MinimapCheck },
        { L"--mpq-check", 0, checks::MpqCheck },
        { L"--sources-check", 0, checks::SourcesCheck },
        { L"--scan-check", 0, checks::ScanCheck },
        { L"--triggers-check", 0, checks::TriggersCheck },
        { L"--poi-check", 0, checks::PoisCheck },
        { L"--poi-read", 0, checks::PoisRead },
        { L"--taxi-check", 0, checks::TaxiCheck },
        { L"--diff-objects", 0, checks::DiffObjects },
        { L"--blueprint-check", 0, checks::BlueprintCheck },
        { L"--asset-check", 0, checks::AssetCheck },
        { L"--casc-to-mpq", 0, checks::CascToMpq },
        { L"--export", 3, checks::ExportCmd },
        { L"--map-preview", 5, checks::MapPreviewCmd },
        { L"--where", 5, checks::WhereCmd },
        { L"--anim-check", 4, checks::AnimCheck },
        { L"--dialogue-check", 3, checks::DialogueCheck },
        { L"--npc-check", 3, checks::NpcCheck },
        { L"--weather-check", 3, checks::WeatherCheck },
        { L"--spawn-check", 3, checks::SpawnCheck },
        { L"--unit-catalog-check", 4, checks::UnitCatalogCheck },
        { L"--sql", 4, checks::SqlCmd },
        { L"--server-check", 3, checks::ServerCheck },
        { L"--cdn-check", 4, checks::CdnCheck },
        { L"--extract", 5, checks::ExtractCmd },
        { L"--ground-effects", 4, checks::GroundEffectsCmd },
        { L"--validate-refs", 4, checks::ValidateRefsCmd },
        { L"--validate", 0, checks::ValidateCmd },
        { L"--normals-check", 6, checks::NormalsCheck },
        { L"--find", 4, checks::FindCmd },
        { L"--shade-check", 0, checks::ShadeCheck },
        { L"--perf-check", 3, checks::PerfCheck },
        { L"--serverdata-check", 6, checks::ServerDataCheck },
        { L"--road-check", 3, checks::RoadCheck },
        { L"--sound-check", 3, checks::SoundCheck },
        { L"--light-check", 3, checks::LightCheck },
        { L"--plan-check", 0, [] {
             try { return checks::PlanCheck(); }
             catch (const std::exception& e) { printf("exception: %s\n", e.what()); return 1; }
         } },
        { L"--area-check", 3, checks::AreaCheck },
        { L"--rewrite-check", 0, checks::RewriteCheck },
        { L"--selftest", 0, [] {
             return FormatsSelfTest() && ChangesSelfTest() && TerrainSelfTest() && BlendSelfTest() && CatalogSelfTest() && BlueprintSelfTest() &&
                    DownportSelfTest() && TriggersSelfTest() && TransformSelfTest() && RoadSelfTest() &&
                    RacesSelfTest() ? 0 : 1;
         } },
        { L"--race-check", 3, checks::RaceCheck },
        { L"--race-look", 6, checks::RaceLookCheck },
        { L"--race-import-check", 5, checks::RaceImportCheck },
        { L"--race-edit-check", 3, checks::RaceEditCheck },
        { L"--race-server-check", 3, checks::RaceServerCheck },
        { L"--check", 0, checks::Check },
    };
    if (__argc < 2 || !__wargv) return -1;
    for (const Entry& e : kChecks)
        if (!wcscmp(__wargv[1], e.flag))
        {
            if (__argc < e.minArgs) { printf("%ls: missing arguments (see docs/development/checks.md)\n", e.flag); return 2; }
            return e.run();
        }
    return -1;
}
