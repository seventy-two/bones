// bones: a plain file browser. Dear ImGui on Direct3D 11, drawn only
// when something happens, so it costs nothing while it sits there.
//
//   bones.exe [folder]              opens there (else where it was left)
//   bones.exe -bench <dir> <out>    times thumbnails for a folder, no window
//
// Test runs: BONES_DATA (state and cache elsewhere; runs beside the real
// one), BONES_POS=x,y,w,h, BONES_VIEW=thumbs|details|sizes,
// BONES_SORT=name|modified|created|type|size[-desc], BONES_SELECT=<name>.
#include <windows.h>
#include <d3d11.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr UINT WM_APP_WAKE = WM_APP + 1;
const wchar_t* kClassName = L"bones";

ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapChain = nullptr;
ID3D11RenderTargetView* target = nullptr;
UINT resizeW = 0, resizeH = 0;

HWND hwnd = nullptr;
std::unique_ptr<bones::App> app;
int framesWanted = 2;

void createTarget() {
    ID3D11Texture2D* back = nullptr;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    device->CreateRenderTargetView(back, nullptr, &target);
    back->Release();
}

void releaseTarget() {
    if (target) target->Release();
    target = nullptr;
}

bool createDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) return false;
    createTarget();
    return true;
}

void destroyDevice() {
    releaseTarget();
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
}

void renderFrame() {
    static bool rendering = false;  // WM_SIZE can arrive while drawing
    if (rendering || !app) return;
    rendering = true;
    if (resizeW != 0) {
        releaseTarget();
        swapChain->ResizeBuffers(0, resizeW, resizeH, DXGI_FORMAT_UNKNOWN, 0);
        resizeW = resizeH = 0;
        createTarget();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app->Frame();
    ImGui::Render();
    const ImVec4& bg = bones::Colours().bg;
    const float clear[4] = {bg.x, bg.y, bg.z, 1};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain->Present(1, 0);
    rendering = false;
}

// hitTest: there's no frame, so a band inside the edges resizes and the
// title bar's empty parts drag the window.
LRESULT hitTest(HWND h, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(h, &pt);
    if (!IsZoomed(h)) {
        RECT rc;
        GetClientRect(h, &rc);
        UINT dpi = GetDpiForWindow(h);
        int band = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        int corner = band * 2;
        bool left = pt.x < band, right = pt.x >= rc.right - band;
        bool top = pt.y < band, bottom = pt.y >= rc.bottom - band;
        bool nearLeft = pt.x < corner, nearRight = pt.x >= rc.right - corner;
        bool nearTop = pt.y < corner, nearBottom = pt.y >= rc.bottom - corner;
        if ((top && nearLeft) || (left && nearTop)) return HTTOPLEFT;
        if ((top && nearRight) || (right && nearTop)) return HTTOPRIGHT;
        if ((bottom && nearLeft) || (left && nearBottom)) return HTBOTTOMLEFT;
        if ((bottom && nearRight) || (right && nearBottom)) return HTBOTTOMRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }
    if (app && app->InCaption(float(pt.x), float(pt.y))) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Explorer's own menu draws some of its items through us.
    if (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR) {
        LRESULT r = 0;
        if (bones::HandleMenuMessage(msg, wParam, lParam, r)) return r;
    }
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) {
            // No frame: all client area, keeping the frame's styles so it
            // still resizes, snaps and has a shadow.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(h)) {
                // Maximised, it hangs over the screen edges by its frame
                // width: inset it by that on every side.
                RECT before = params->rgrc[0];
                DefWindowProcW(h, msg, wParam, lParam);
                params->rgrc[0].top = before.top + (params->rgrc[0].left - before.left);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return hitTest(h, lParam);
    case WM_ACTIVATE:
        framesWanted = 2;  // the title dims when it's not in front
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            resizeW = LOWORD(lParam);
            resizeH = HIWORD(lParam);
            // While the window is dragged to a new size, Windows runs its
            // own message loop, so draw from here.
            if (IsWindowVisible(h)) {
                renderFrame();
                renderFrame();
            }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(h);
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = {LONG(720 * scale), LONG(420 * scale)};
        return 0;
    }
    case WM_DPICHANGED: {
        if (app) app->SetScale(float(HIWORD(wParam)) / 96.0f);
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::wstring> paths;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);
            paths.push_back(p);
        }
        DragFinish(drop);
        if (app) app->Dropped(paths);
        framesWanted = 3;
        return 0;
    }
    case WM_DEVICECHANGE:
        if (app && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)) app->DrivesChanged();
        return TRUE;
    case WM_ACTIVATEAPP:
        if (wParam && app) app->DrivesChanged();  // free space may have changed
        break;
    case WM_CLOSE:
        if (app) app->Save();
        DestroyWindow(h);
        return 0;
    case WM_ENDSESSION:
        if (wParam && app) app->Save();
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

std::wstring env(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v ? v : L"";
}

int bench(const std::wstring& dir, const std::wstring& out) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
        return 1;
    std::string report = bones::Thumbs::Bench(device, dir, true);
    FILE* f = _wfopen(out.c_str(), L"w");
    if (f) {
        fputs(report.c_str(), f);
        fclose(f);
    }
    context->Release();
    device->Release();
    CoUninitialize();
    return 0;
}

}  // namespace

void BonesAssert(const char* expr, const char* file, int line) {
    static int logged = 0;
    if (logged++ > 50) return;
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, dir);
    FILE* f = _wfopen((std::wstring(dir, n) + L"bones-assert.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "%s:%d: %s\n", file, line, expr);
    fclose(f);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // never a dialog
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 4 && wcscmp(argv[1], L"-bench") == 0) return bench(argv[2], argv[3]);
    std::wstring start = argc >= 2 ? argv[1] : L"";

    ImGui_ImplWin32_EnableDpiAwareness();
    OleInitialize(nullptr);  // the clipboard, drag and drop, the shell

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClassName, L"bones", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           int(1200 * scale), int(780 * scale), nullptr, nullptr, instance, nullptr);

    // Back where it was, if that's still on a screen.
    bool maximised = false;
    RECT place{};
    std::wstring pos = env(L"BONES_POS");
    bool test = !pos.empty();
    if (test) {
        int x, y, w, h;
        if (swscanf(pos.c_str(), L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) place = {x, y, x + w, y + h};
    } else {
        bones::App::Placement(place, maximised);
    }
    if (place.right > place.left && MonitorFromRect(&place, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = place;
        wp.showCmd = SW_HIDE;
        SetWindowPlacement(hwnd, &wp);
    }
    // A sliver of DWM frame under the client area keeps the shadow; the
    // app paints over it. Then recalculate the frame without its border.
    MARGINS margins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (!createDevice()) {
        MessageBoxW(nullptr, L"Couldn't start Direct3D 11.", L"bones", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigInputTrickleEventQueue = false;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    app = std::make_unique<bones::App>(hwnd, device, [] { PostMessageW(hwnd, WM_APP_WAKE, 0, 0); });
    app->SetScale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));
    if (std::wstring v = env(L"BONES_VIEW"); v == L"sizes") app->SetSizes();
    else if (!v.empty()) app->SetView(v == L"thumbs");
    if (std::wstring v = env(L"BONES_SORT"); !v.empty()) {
        bool desc = v.size() > 5 && v.substr(v.size() - 5) == L"-desc";
        if (desc) v = v.substr(0, v.size() - 5);
        const wchar_t* names[] = {L"name", L"modified", L"created", L"type", L"size"};
        for (int k = 0; k < 5; k++)
            if (v == names[k]) app->SetSort(bones::SortKey(k), desc);
    }
    if (start.empty()) start = env(L"BONES_OPEN");
    app->Start(start);
    if (std::wstring sel = env(L"BONES_SELECT"); !sel.empty()) app->SelectName(sel);

    ShowWindow(hwnd, test ? SW_SHOWNOACTIVATE : maximised ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    // Draw only when something happens: input, or word from a thread.
    bool running = true;
    while (running) {
        bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
        DWORD timeout = !visible ? INFINITE : framesWanted > 0 ? 0 : app->Busy() ? 150 : INFINITE;
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            framesWanted = 2;  // ImGui needs a couple of frames to settle
        }
        if (!running) break;
        app->Pump();
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
        renderFrame();
        if (framesWanted > 0) framesWanted--;
    }

    app.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    OleUninitialize();
    return 0;
}
