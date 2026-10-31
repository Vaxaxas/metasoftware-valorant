#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <tchar.h>
#include "macros.h"
#include "ndi/ndicontroller.h"
#include "config/config.h"

#include "makcu/makcu.h"
#include "features/aimassist/aimassist.h"
#include "authentication/authentication.h"

#include "thirdparty/imgui/imgui.h"
#include "thirdparty/imgui/imgui_impl_win32.h"
#include "thirdparty/imgui/imgui_impl_dx12.h"
#include "globals.h"
#include "features/overlay/fuser/fuser.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

const char* mouse_options[] = { "LEFT", "RIGHT", "SIDE1", "SIDE2", "ALT", "SHIFT" };
const char* hitbox_options[] = { "HEAD", "NECK", "BODY" };
const char* trigger_bind_options[] = { "ALWAYS ON", "LEFT", "RIGHT", "MIDDLE", "SIDE1", "SIDE2", "ALT", "SHIFT", "CTRL" };

inline const char* GetAimKeyName(int key) {
    if (key >= 0 && key < static_cast<int>(IM_ARRAYSIZE(mouse_options))) {
        return mouse_options[key];
    }
    return "UNKNOWN";
}

inline const char* GetTriggerBindName(int key) {
    if (key >= 0 && key < static_cast<int>(IM_ARRAYSIZE(trigger_bind_options))) {
        return trigger_bind_options[key];
    }
    return "ALWAYS ON";
}

// NDI Connection monitoring globals
std::atomic<bool> g_NDIConnectionHealthy{ false };
std::atomic<bool> g_NDIReconnecting{ false };
std::atomic<int> g_NDIReconnectAttempts{ 0 };
std::chrono::steady_clock::time_point g_LastNDIFrameTime;
std::mutex g_NDIStatusMutex;
const int MAX_NDI_RECONNECT_ATTEMPTS = 10;
const int NDI_FRAME_TIMEOUT_MS = 3000;  // 3 seconds without frames = considered disconnected

struct FrameContext
{
    ID3D12CommandAllocator* CommandAllocator;
    UINT64                  FenceValue;
};

static int const                    NUM_FRAMES_IN_FLIGHT = 3;
static FrameContext                 g_frameContext[NUM_FRAMES_IN_FLIGHT] = {};
static UINT                         g_frameIndex = 0;

static int const                    NUM_BACK_BUFFERS = 3;
static ID3D12Device* g_pd3dDevice = nullptr;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
static ID3D12Fence* g_fence = nullptr;
static HANDLE                       g_fenceEvent = nullptr;
static UINT64                       g_fenceLastSignaledValue = 0;
static IDXGISwapChain3* g_pSwapChain = nullptr;
static HANDLE                       g_hSwapChainWaitableObject = nullptr;
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE  g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS] = {};

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void WaitForLastSubmittedFrame();
FrameContext* WaitForNextFrameResources();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Enhanced NDI connection functions (Adapted for 1-PC Mode)
bool WarmUpNDIDiscovery(int maxAttempts = 3) {
    timelog("1-PC Mode: Initializing local screen capture..");
    g_Index::g_NDIReceiver = g_NDI::connect_to_default_device();
    g_NDIConnectionHealthy = true;
    g_LastNDIFrameTime = std::chrono::steady_clock::now();
    timelog("1-PC Mode: Local screen capture ready!");
    return true;
}

void UpdateNDIFrameReceived() {
    g_LastNDIFrameTime = std::chrono::steady_clock::now();
    g_NDIConnectionHealthy = true;
}

bool ForceNDIReconnect() {
    timelog("Manual NDI reconnection requested");

    g_NDIReconnecting = true;
    g_NDIReconnectAttempts = 0; // Reset counter for manual reconnect

    // Clean up current connection
    std::lock_guard<std::mutex> lock(g_NDIStatusMutex);
    if (g_Index::g_NDIReceiver != nullptr) {
        NDIlib_recv_destroy(g_Index::g_NDIReceiver);
        g_Index::g_NDIReceiver = nullptr;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool success = WarmUpNDIDiscovery(3);
    g_NDIReconnecting = false;

    if (success) {
        timelog("Manual NDI reconnection successful!");
    }
    else {
        timelog("Manual NDI reconnection failed!");
    }

    return success;
}

void SetupImGuiStyle()
{
    // Photoshop style by Derydoca from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6000000238418579f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 4.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 2.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 13.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabMinSize = 7.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 1.0f;
    style.TabMinWidthForCloseButton = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.4980392158031464f, 0.4980392158031464f, 0.4980392158031464f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1764705926179886f, 0.1764705926179886f, 0.1764705926179886f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.2784313857555389f, 0.2784313857555389f, 0.2784313857555389f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.3098039329051971f, 0.3098039329051971f, 0.3098039329051971f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.2627451121807098f, 0.2627451121807098f, 0.2627451121807098f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.1568627506494522f, 0.1568627506494522f, 0.1568627506494522f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2000000029802322f, 0.2000000029802322f, 0.2000000029802322f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.2784313857555389f, 0.2784313857555389f, 0.2784313857555389f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1450980454683304f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1450980454683304f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1450980454683304f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.1921568661928177f, 0.1921568661928177f, 0.1921568661928177f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.1568627506494522f, 0.1568627506494522f, 0.1568627506494522f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.2745098173618317f, 0.2745098173618317f, 0.2745098173618317f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.2980392277240753f, 0.2980392277240753f, 0.2980392277240753f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.3882353007793427f, 0.3882353007793427f, 0.3882353007793427f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.1560000032186508f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.3910000026226044f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.3098039329051971f, 0.3098039329051971f, 0.3098039329051971f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.4666666686534882f, 0.4666666686534882f, 0.4666666686534882f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.4666666686534882f, 0.4666666686534882f, 0.4666666686534882f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.2627451121807098f, 0.2627451121807098f, 0.2627451121807098f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.3882353007793427f, 0.3882353007793427f, 0.3882353007793427f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.6700000166893005f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.09411764889955521f, 0.09411764889955521f, 0.09411764889955521f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.3490196168422699f, 0.3490196168422699f, 0.3490196168422699f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.1921568661928177f, 0.1921568661928177f, 0.1921568661928177f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.09411764889955521f, 0.09411764889955521f, 0.09411764889955521f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.1921568661928177f, 0.1921568661928177f, 0.1921568661928177f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.4666666686534882f, 0.4666666686534882f, 0.4666666686534882f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.5843137502670288f, 0.5843137502670288f, 0.5843137502670288f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882352977991104f, 0.1882352977991104f, 0.2000000029802322f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.3098039329051971f, 0.3098039329051971f, 0.3490196168422699f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.2274509817361832f, 0.2274509817361832f, 0.2470588237047195f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.05999999865889549f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.1560000032186508f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 0.3882353007793427f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5860000252723694f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5860000252723694f);
}

void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Console application entry point
int main(int argc, char* argv[]) {
    // 設置高 DPI 識別，避免 125%/150% 縮放下螢幕中心座標計算偏差 (修復 Finding 5)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int kTabIndex = 0;

    // Process command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--authentication-key" && i + 1 < argc) {
            g_Authentication::auth_key = argv[i + 1];
            i++;
        }
    }

    if (!g_Authentication::Auth()) {
        timelog("Authentication failed! If there is output above it should be self disagnosable");
        timelog("If you require more help, open a ticket.");
        delay_exit(default_exit_value);
    }

    // Initialize NDI connection monitoring
    g_LastNDIFrameTime = std::chrono::steady_clock::now();
    WarmUpNDIDiscovery(3);

    timelog("Loading config.json");
    g_Config::LoadConfig("config.json");

    timelog("1-PC Mode: Initializing local mouse controller..");
    g_Index::g_makcuDevice.connect();
    timelog("1-PC Mode: Local mouse controller connected!");

    // 根據 showDebug 設定同步切換 Console 顯示狀態
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, g_Index::showDebug ? SW_SHOW : SW_HIDE);
    }

    timelog("kSensValue: X=%.2f, Y=%.2f", g_Index::kSensitivityX, g_Index::kSensitivityY);
    timelog("Triggerbot: %s (Bind: %s, FOV: %d px, Delay: %d ms, Cooldown: %d ms)",
        g_Index::trigger_enabled ? "ENABLED" : "DISABLED",
        GetTriggerBindName(g_Index::trigger_key),
        g_Index::trigger_fov,
        g_Index::trigger_delay_ms,
        g_Index::trigger_cooldown_ms);

    std::atomic<bool> aimThreadRunning{ true };
    std::thread aimThread([&aimThreadRunning]() {
        while (aimThreadRunning.load()) {
            makcu::MouseButton btn = makcu::MouseButton::RIGHT;
            switch (g_Index::aim_key) {
            case 0: btn = makcu::MouseButton::LEFT; break;
            case 1: btn = makcu::MouseButton::RIGHT; break;
            case 2: btn = makcu::MouseButton::SIDE1; break;
            case 3: btn = makcu::MouseButton::SIDE2; break;
            case 4: btn = makcu::MouseButton::ALT; break;
            case 5: btn = makcu::MouseButton::SHIFT; break;
            default: btn = makcu::MouseButton::RIGHT; break;
            }
            bool held = (g_Index::aim_key >= 0 && g_Index::aim_key <= 5) ? g_Makcu::isButtonHeld(btn) : false;
            g_AimAssist::SetBindState(held); // 統一呼叫狀態重置函式 (修復 Finding 6, 7, 8, 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    aimThread.detach();

    g_AimAssist::Run();

    HWND consoleWindow = GetConsoleWindow();

    int height = 300, width = 500;

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(nullptr),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"metasoftware.me",
        nullptr
    };

    if (!::RegisterClassExW(&wc)) {
        std::cerr << "Failed to register window class" << std::endl;
        return 1;
    }

    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"metasoftware.me",
        WS_POPUP | WS_BORDER,
        100, 100,
        width, height,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    g_Fuser::SetMainWindowHandle(hwnd);
    if (!RegisterHotKey(hwnd, 1, 0, g_Index::show_menu_key)) {
        std::cerr << "Failed to register menu hotkey" << std::endl;
    }
    if (!RegisterHotKey(hwnd, 2, 0, g_Index::panic_button)) {
        std::cerr << "Failed to register panic hotkey" << std::endl;
    }

    if (!hwnd) {
        std::cerr << "Failed to create window" << std::endl;
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd)) {
        std::cerr << "Failed to create D3D12 device" << std::endl;
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    SetupImGuiStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_pd3dDevice, NUM_FRAMES_IN_FLIGHT,
        DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
        g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
        g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool done = false;

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_Fuser::isOverlayActive && g_Index::draw_fov) {
            g_Fuser::draw_fov();
        }

        RECT rect;
        GetClientRect(hwnd, &rect);
        int window_width = rect.right - rect.left;
        int window_height = rect.bottom - rect.top;

        ImGui::SetNextWindowSize(ImVec2((float)window_width, (float)window_height), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);

        ImGui::Begin("metasoftware.me", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize);

        ImGui::Text("MetaSoftware.me");
        ImGui::Separator();

        if (ImGui::Button("Main")) {
            kTabIndex = 0;
        } ImGui::SameLine();
        if (ImGui::Button("Aim")) {
            kTabIndex = 1;
        } ImGui::SameLine();
        if (ImGui::Button("Trigger")) {
            kTabIndex = 2;
        } ImGui::SameLine();
        if (ImGui::Button("Visuals")) {
            kTabIndex = 3;
        } ImGui::SameLine();
        if (ImGui::Button("Config")) {
            kTabIndex = 4;
        } ImGui::SameLine();
        if (ImGui::Button("Debug")) {
            kTabIndex = 5;
        } ImGui::SameLine();
        if (ImGui::Button("Exit")) {
            done = true;
        }

        if (kTabIndex == 0) {
            ImGui::Text("System: 1-PC Standalone Mode");
            ImGui::Separator();

            ImGui::Text("Screen Capture");
            if (g_NDIConnectionHealthy) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Active (Zero-Copy 256x256 @ 240 FPS)");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Initializing...");
            }

            ImGui::Separator();

            ImGui::Text("Makcu Mouse Control");
            if (g_Index::g_makcuDevice.isConnected()) {
                std::string target = g_Index::g_makcuDevice.getTargetAddress();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected: %s", target.c_str());
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
            }

            if (ImGui::Button("Test movement")) {
                g_Index::g_makcuDevice.mouseMove(5, 5);
            } ImGui::SameLine();
            HelpMarker("Tests mouse movement (moves cursor +5, +5 via Makcu)"); ImGui::SameLine();
            if (ImGui::Button("Reconnect")) {
                g_Index::g_makcuDevice.connect(g_Index::makcu_ip, g_Index::makcu_port);
            }
            ImGui::Separator();
        }
        else if (kTabIndex == 1) {
            ImGui::Checkbox("Enable Aim-Assist", &g_Index::assistance_enabled); 

            if (g_Index::assistance_enabled) {
                ImGui::SliderFloat("Speed X", &g_Index::kSensitivityX, 0.01f, 10.0f, "%.3f"); ImGui::SameLine(); HelpMarker("Horizontal tracking speed on the X axis");
                ImGui::SliderFloat("Speed Y", &g_Index::kSensitivityY, 0.01f, 10.0f, "%.3f"); ImGui::SameLine(); HelpMarker("Vertical tracking speed on the Y axis");
                ImGui::SliderInt("Smoothness", &g_Index::kSmoothingFactor, 1, 100, "%d"); ImGui::SameLine(); HelpMarker("The speed at which the aim assist will move to a given target");
                ImGui::SliderInt("FOV", &g_Index::kFOV, 1, 150, "%d"); ImGui::SameLine(); HelpMarker("The FOV (Field of view) the aim assist will scan for targets within");
                ImGui::SliderFloat("Deadzone", &g_Index::aim_deadzone, 0.0f, 10.0f, "%.1f px"); ImGui::SameLine(); HelpMarker("Target deadzone radius: inside this radius, mouse movement stops to prevent micro-jitter (default: 2.5 px)");
                ImGui::Checkbox("Enable Damping (Anti-Overshoot)", &g_Index::aim_damping_enabled); ImGui::SameLine(); HelpMarker("Enables close-range dynamic braking and derivative (D-term) velocity damping to prevent overshooting the target");
                if (ImGui::BeginCombo("Preferred Hitbox", (g_Index::hitbox >= 0 && g_Index::hitbox < IM_ARRAYSIZE(hitbox_options)) ? hitbox_options[g_Index::hitbox] : "HEAD")) {
                    for (int n = 0; n < IM_ARRAYSIZE(hitbox_options); n++) {
                        bool is_selected = (g_Index::hitbox == n);
                        if (ImGui::Selectable(hitbox_options[n], is_selected)) {
                            g_Index::hitbox = n;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                } ImGui::SameLine(); HelpMarker("The hitbox the bot will prefer to aim at if available");

                const char* aim_methods[] = { "MOVE (Original)", "SILENCE (Makcu moveto)" };
                if (ImGui::BeginCombo("Method", (g_Index::aim_method >= 0 && g_Index::aim_method < 2) ? aim_methods[g_Index::aim_method] : "MOVE (Original)")) {
                    for (int n = 0; n < 2; n++) {
                        bool is_selected = (g_Index::aim_method == n);
                        if (ImGui::Selectable(aim_methods[n], is_selected)) {
                            g_Index::aim_method = n;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                } ImGui::SameLine(); HelpMarker("MOVE = Original smooth 144Hz relative tracking via km.move_now\nSILENCE = Directly teleports mouse to target absolute coordinates via Makcu km.moveto");
                ImGui::Separator();

                if (ImGui::BeginCombo("Bind", GetAimKeyName(g_Index::aim_key))) {
                    for (int n = 0; n < IM_ARRAYSIZE(mouse_options); n++) {
                        bool is_selected = (g_Index::aim_key == n);
                        if (ImGui::Selectable(mouse_options[n], is_selected)) {
                            g_Index::aim_key = n;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                } ImGui::SameLine(); HelpMarker("The key you will have to press on your mouse to activate the bot");

                ImGui::Separator();
                ImGui::Text("Aim Offset Tuning");
                ImGui::SliderInt("Y Offset", &g_Index::aim_y_offset, -20, 20, "%d px");
                ImGui::SameLine(); HelpMarker("Adjust vertical height (positive = lower onto face/neck, negative = higher towards head)");

                ImGui::SliderInt("X Offset", &g_Index::aim_x_offset, -20, 20, "%d px");
                ImGui::SameLine(); HelpMarker("Adjust horizontal point (positive = right, negative = left)");

                if (ImGui::Button("Reset Offsets")) {
                    g_Index::aim_x_offset = 0;
                    g_Index::aim_y_offset = 0;
                }

                ImGui::Separator();
                ImGui::Checkbox("Enable RCS (Recoil Control System)", &g_Index::rcs_enabled);
                ImGui::SameLine(); HelpMarker("Automatically compensates for weapon recoil by pulling down mouse when spraying");
                if (g_Index::rcs_enabled) {
                    ImGui::SliderFloat("RCS Strength", &g_Index::rcs_strength, 0.5f, 6.0f, "%.1f");
                    ImGui::SameLine(); HelpMarker("Downwards compensation strength applied while spraying (holding left click)");
                    ImGui::SliderInt("RCS Start Delay", &g_Index::rcs_delay_ms, 0, 300, "%d ms");
                    ImGui::SameLine(); HelpMarker("Delay before RCS starts pulling down (prevents pulling down first-shot taps)");
                }

                ImGui::Separator();
                ImGui::Checkbox("Enable Aim Humanizer", &g_Index::aim_humanizer_enabled);
                ImGui::SameLine(); HelpMarker("Adds natural muscle micro-tremor and organic aim point wandering to simulate real human aiming");
                if (g_Index::aim_humanizer_enabled) {
                    ImGui::SliderFloat("Humanizer Strength", &g_Index::aim_humanizer_strength, 0.5f, 3.0f, "%.1f px");
                    ImGui::SameLine(); HelpMarker("Amplitude of natural muscle tremor and target organic wander (default: 1.0 px)");
                }
            }
        }
        else if (kTabIndex == 2) {
            ImGui::Checkbox("Enable Triggerbot (Auto Fire)", &g_Index::trigger_enabled);
            ImGui::SameLine();
            HelpMarker("Automatically clicks when crosshair is directly on enemy purple outline");

            if (g_Index::trigger_enabled) {
                if (ImGui::BeginCombo("Trigger Bind", GetTriggerBindName(g_Index::trigger_key))) {
                    for (int n = 0; n < IM_ARRAYSIZE(trigger_bind_options); n++) {
                        bool is_selected = (g_Index::trigger_key == n);
                        if (ImGui::Selectable(trigger_bind_options[n], is_selected)) {
                            g_Index::trigger_key = n;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                } ImGui::SameLine(); HelpMarker("Hold this key/mouse button to activate triggerbot, or select ALWAYS ON");

                ImGui::SliderInt("Trigger FOV (Radius)", &g_Index::trigger_fov, 1, 15, "%d px");
                ImGui::SameLine(); HelpMarker("Radius around center crosshair that checks for enemy target");

                ImGui::SliderInt("Reaction Delay", &g_Index::trigger_delay_ms, 0, 150, "%d ms");
                ImGui::SameLine(); HelpMarker("Human-like reaction delay before shooting (0-150ms)");

                ImGui::SliderInt("Fire Cooldown", &g_Index::trigger_cooldown_ms, 50, 500, "%d ms");
                ImGui::SameLine(); HelpMarker("Cooldown interval between shots to prevent rapid spamming");

                ImGui::SliderInt("Hold Duration", &g_Index::trigger_click_duration, 20, 100, "%d ms");
                ImGui::SameLine(); HelpMarker("Mouse click hold duration via Makcu km.click(1,1,hold_ms)");

                ImGui::Separator();
                if (ImGui::Button("Test Trigger Click (Makcu km.click)")) {
                    g_Index::g_makcuDevice.click(1, 1, g_Index::trigger_click_duration);
                } ImGui::SameLine();
                HelpMarker("Tests shooting trigger via Makcu km.click(1)");
            }
        }
        else if (kTabIndex == 3) {
            ImGui::Checkbox("Fuser Mode", &g_Index::fuser_enabled);
            ImGui::SameLine();
            HelpMarker("Enables an overlay allowing users owning a fuser to have drawn on FOV markers & a watermark");

            if (g_Index::fuser_enabled) {
                if (!g_Fuser::isOverlayActive) {
                    g_Fuser::enable();
                }

                ImGui::Checkbox("Draw watermark", &g_Index::draw_watermark); ImGui::SameLine(); HelpMarker("Draws the METASOFTWARE.ME watermark");
                ImGui::Checkbox("Draw FOV", &g_Index::draw_fov); ImGui::SameLine(); HelpMarker("Draws your bot vision (FOV)");
            }
            else if (g_Fuser::isOverlayActive) {
                g_Fuser::cleanup();
            }
        }
        else if (kTabIndex == 4) {
            if (ImGui::Button("Load config")) {
                g_Config::LoadConfig();
            }

            if (ImGui::Button("Save config")) {
                g_Config::SaveConfig();
            }
        }
        else if (kTabIndex == 5) {
            ImGui::Text("=== Diagnostic & Debug Mode ===");
            if (ImGui::Checkbox("Enable On-Screen HUD & Console Logs", &g_Index::showDebug)) {
                HWND consoleWnd = GetConsoleWindow();
                if (consoleWnd) {
                    ShowWindow(consoleWnd, g_Index::showDebug ? SW_SHOW : SW_HIDE);
                }
            }
            ImGui::Checkbox("Color Filter Mask (Binary View)", &g_Index::show_mask_view);
            ImGui::SameLine(); HelpMarker("Displays the black & white binary mask showing exactly what purple pixels the bot detects");
            ImGui::Separator();

            ImGui::Text("Aim Assist: %s", g_Index::assistance_enabled ? "ENABLED" : "DISABLED");
            ImGui::Text("Triggerbot: %s", g_Index::trigger_enabled ? "ENABLED" : "DISABLED");
            ImGui::Text("Aim Key: %s (Held: %s)", GetAimKeyName(g_Index::aim_key), g_AimAssist::isBindHeld ? "YES" : "NO");
            ImGui::Text("Detected Purple Pixels: %d", g_AimAssist::debugPixelCount);
            if (g_AimAssist::debugHasTarget) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Target Locked: dx=%.1f, dy=%.1f", g_AimAssist::debugTargetX, g_AimAssist::debugTargetY);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Target: Searching within FOV %d...", g_Index::kFOV);
            }

            ImGui::Separator();
            ImGui::Text("Makcu Output: %s", g_Index::g_makcuDevice.getTargetAddress().c_str());

            if (ImGui::Button("Test Move (+10, 0)")) {
                g_Index::g_makcuDevice.mouseMove(10, 0);
            } ImGui::SameLine();
            if (ImGui::Button("Test Move (-10, 0)")) {
                g_Index::g_makcuDevice.mouseMove(-10, 0);
            } ImGui::SameLine();
            if (ImGui::Button("Test km.click(1)")) {
                g_Index::g_makcuDevice.click(1, 1, g_Index::trigger_click_duration);
            }
        }

        ImGui::End();

        ImGui::Render();

        FrameContext* frameCtx = WaitForNextFrameResources();
        UINT backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();
        frameCtx->CommandAllocator->Reset();

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
        g_pd3dCommandList->ResourceBarrier(1, &barrier);

        const float clear_color_with_alpha[4] = {
            clear_color.x * clear_color.w,
            clear_color.y * clear_color.w,
            clear_color.z * clear_color.w,
            clear_color.w
        };

        g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
        g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
        g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->Close();

        g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pd3dCommandList);
        g_pSwapChain->Present(1, 0);

        UINT64 fenceValue = g_fenceLastSignaledValue + 1;
        g_pd3dCommandQueue->Signal(g_fence, fenceValue);
        g_fenceLastSignaledValue = fenceValue;
        frameCtx->FenceValue = fenceValue;
    }

    std::cout << "Shutting down..." << std::endl;

    aimThreadRunning.store(false);
    g_AimAssist::Shutdown();
    if (g_Fuser::isOverlayActive) {
        g_Fuser::cleanup();
    }

    WaitForLastSubmittedFrame();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    g_NDI::cleanup_device();

    std::cout << "Application closed successfully" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    return 0;
}

// [Rest of the helper functions - CreateDeviceD3D, CleanupDeviceD3D, etc.]
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC1 sd;
    {
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = NUM_BACK_BUFFERS;
        sd.Width = 0;
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Stereo = FALSE;
    }

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D12CreateDevice(nullptr, featureLevel, IID_PPV_ARGS(&g_pd3dDevice));
    if (FAILED(hr)) {
        std::cerr << "D3D12CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = NUM_BACK_BUFFERS;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK)
            return false;

        SIZE_T rtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
            g_mainRenderTargetDescriptor[i] = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK)
            return false;
    }

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_pd3dCommandQueue)) != S_OK)
            return false;
    }

    for (UINT i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
        if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK)
            return false;

    if (g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
        g_pd3dCommandList->Close() != S_OK)
        return false;

    if (g_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
        return false;

    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_fenceEvent == nullptr)
        return false;

    {
        IDXGIFactory4* dxgiFactory = nullptr;
        IDXGISwapChain1* swapChain1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK)
            return false;
        if (dxgiFactory->CreateSwapChainForHwnd(g_pd3dCommandQueue, hWnd, &sd, nullptr, nullptr, &swapChain1) != S_OK)
            return false;
        if (swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain)) != S_OK)
            return false;
        swapChain1->Release();
        dxgiFactory->Release();
        g_pSwapChain->SetMaximumFrameLatency(NUM_BACK_BUFFERS);
        g_hSwapChainWaitableObject = g_pSwapChain->GetFrameLatencyWaitableObject();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->SetFullscreenState(false, nullptr); g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_hSwapChainWaitableObject != nullptr) { CloseHandle(g_hSwapChainWaitableObject); }
    for (UINT i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
        if (g_frameContext[i].CommandAllocator) { g_frameContext[i].CommandAllocator->Release(); g_frameContext[i].CommandAllocator = nullptr; }
    if (g_pd3dCommandQueue) { g_pd3dCommandQueue->Release(); g_pd3dCommandQueue = nullptr; }
    if (g_pd3dCommandList) { g_pd3dCommandList->Release(); g_pd3dCommandList = nullptr; }
    if (g_pd3dRtvDescHeap) { g_pd3dRtvDescHeap->Release(); g_pd3dRtvDescHeap = nullptr; }
    if (g_pd3dSrvDescHeap) { g_pd3dSrvDescHeap->Release(); g_pd3dSrvDescHeap = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
        ID3D12Resource* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_mainRenderTargetDescriptor[i]);
        g_mainRenderTargetResource[i] = pBackBuffer;
    }
}

void CleanupRenderTarget() {
    WaitForLastSubmittedFrame();
    for (UINT i = 0; i < NUM_BACK_BUFFERS; i++)
        if (g_mainRenderTargetResource[i]) { g_mainRenderTargetResource[i]->Release(); g_mainRenderTargetResource[i] = nullptr; }
}

void WaitForLastSubmittedFrame() {
    FrameContext* frameCtx = &g_frameContext[g_frameIndex % NUM_FRAMES_IN_FLIGHT];
    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue == 0)
        return;

    frameCtx->FenceValue = 0;
    if (g_fence->GetCompletedValue() >= fenceValue)
        return;

    g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
}

FrameContext* WaitForNextFrameResources() {
    UINT nextFrameIndex = g_frameIndex + 1;
    g_frameIndex = nextFrameIndex;

    HANDLE waitableObjects[] = { g_hSwapChainWaitableObject, nullptr };
    DWORD numWaitableObjects = 1;

    FrameContext* frameCtx = &g_frameContext[nextFrameIndex % NUM_FRAMES_IN_FLIGHT];
    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue != 0) {
        frameCtx->FenceValue = 0;
        g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
        waitableObjects[1] = g_fenceEvent;
        numWaitableObjects = 2;
    }

    WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);
    return frameCtx;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    static bool isMinimized = false;

    switch (msg) {
    case WM_NCHITTEST:
    {
        // Extract coordinates manually
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);

        POINT pt = { x, y };
        ScreenToClient(hWnd, &pt);

        // Check if mouse is over any ImGui elements that are actually interactive
        ImGuiIO& io = ImGui::GetIO();

        // Only block dragging if ImGui is actively using the mouse (hovering over buttons, sliders, etc.)
        if (io.WantCaptureMouse && (ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive())) {
            return HTCLIENT; // Let ImGui handle it
        }

        // If not over interactive ImGui elements, make the window draggable
        return HTCAPTION;
    }

    case WM_HOTKEY:
        if (wParam == 1) {
            if (isMinimized) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                BringWindowToTop(hWnd);
                isMinimized = false;
            }
            else {
                ShowWindow(hWnd, SW_MINIMIZE);
                isMinimized = true;
            }
            return 0;
        }
        else if (wParam == 2) {
            ::PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            isMinimized = true;
        }
        else if (wParam == SIZE_RESTORED) {
            isMinimized = false;
        }

        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            WaitForLastSubmittedFrame();
            CleanupRenderTarget();
            HRESULT result = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            assert(SUCCEEDED(result) && "Failed to resize swapchain.");
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        UnregisterHotKey(hWnd, 1);
        UnregisterHotKey(hWnd, 2);
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}