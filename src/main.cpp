// wow-world-editor: window, Direct3D 11 device and the frame loop. The editor itself is App; the command-line
// checks (`--selftest`, `--check`, ...) are in Checks*.cpp.

#include "App.hpp"
#include "Checks.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    ComPtr<ID3D11Device> g_device;
    ComPtr<ID3D11DeviceContext> g_context;
    ComPtr<IDXGISwapChain> g_swapChain;
    ComPtr<ID3D11RenderTargetView> g_rtv;
    UINT g_resizeWidth = 0, g_resizeHeight = 0;
    App* g_app = nullptr;

    void CreateTarget()
    {
        ComPtr<ID3D11Texture2D> back;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
    }

    bool CreateDevice(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                                 &sd, &g_swapChain, &g_device, nullptr, &g_context)))
            return false;
        CreateTarget();
        return true;
    }

    LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
        switch (msg)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                g_resizeWidth = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // no ALT menu
            break;
        case WM_CLOSE:
            if (g_app) { g_app->RequestClose(); return 0; }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    if (const int rc = RunCheck(); rc >= 0) return rc;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"WowWorldEditor" };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"WoW World Editor", WS_OVERLAPPEDWINDOW, 80, 60, 1700, 980, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(hwnd))
    {
        MessageBoxW(hwnd, L"Could not create a Direct3D 11 device.", L"WoW World Editor", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    // The panel layout lives with the other per-user settings; a layout left next to the exe by older builds moves there.
    static const std::string iniPath = (SettingsDir() / "imgui.ini").string();
    if (std::error_code ec; !std::filesystem::exists(iniPath) && std::filesystem::exists("imgui.ini")) std::filesystem::copy_file("imgui.ini", iniPath, ec);
    const bool firstRun = !std::filesystem::exists(iniPath);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = iniPath.c_str();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    App app;
    if (!app.Init(hwnd, g_device.Get(), g_context.Get(), firstRun)) return 1;
    g_app = &app;

    auto last = std::chrono::steady_clock::now();
    while (!app.WantsQuit())
    {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            quit |= msg.message == WM_QUIT;
        }
        if (quit) break;

        if (g_resizeWidth && g_resizeHeight)
        {
            g_rtv.Reset();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateTarget();
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.Frame(dt);
        ImGui::Render();

        const float clear[4] = { 0.08f, 0.09f, 0.10f, 1 };
        g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // A minimised or covered window presents without waiting for vsync: keep the loop (autosave, loading) at ~20 fps.
        if (g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED || IsIconic(hwnd)) Sleep(50);
    }

    g_app = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
