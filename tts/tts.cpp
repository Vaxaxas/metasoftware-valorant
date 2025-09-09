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

#include <makcu/makcu.h>
#include "makcu/makcu.h"
#include "ndi/ndicontroller.h"
#include "features/aimassist/aimassist.h"
#include "authentication/authentication.h"
#include "config/config.h"

#include "thirdparty/imgui/imgui.h"
#include "thirdparty/imgui/imgui_impl_win32.h"
#include "thirdparty/imgui/imgui_impl_dx12.h"
#include "globals.h"
#include "authentication/authentication.h"
#include "features/overlay/fuser/fuser.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

const char* mouse_options[] = { "LEFT", "RIGHT", "SIDE1", "SIDE2" };
const char* hitbox_options[] = { "HEAD", "NECK", "BODY" };

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

// Enhanced NDI connection functions
bool WarmUpNDIDiscovery(int maxAttempts = 3) {
    timelog("Warming up NDI discovery..");

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        timelog("NDI discovery attempt %i/%i", attempt, maxAttempts);

        // Clean up any existing connection first
        if (g_Index::g_NDIReceiver != nullptr) {
            NDIlib_recv_destroy(g_Index::g_NDIReceiver);
            g_Index::g_NDIReceiver = nullptr;
        }

        // Wait a bit for network stabilization
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        g_Index::g_NDIReceiver = g_NDI::connect_to_default_device();

        if (g_Index::g_NDIReceiver != nullptr) {
            timelog("NDI warm-up successful!");
            g_NDIConnectionHealthy = true;
            g_LastNDIFrameTime = std::chrono::steady_clock::now();
            return true;
        }

        timelog("NDI warm-up attempt failed");

        if (attempt < maxAttempts) {
            timelog("Failed to find NDI device, waiting before we retry..");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Longer wait between attempts
        }
    }

    timelog("Warmup completed but no NDI device was found, attempting to continue..");
    g_NDIConnectionHealthy = false;
    return false;
}

void NDIConnectionMonitor() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Check every second

        if (g_Index::g_NDIReceiver == nullptr) {
            g_NDIConnectionHealthy = false;
            continue;
        }

        // Check if we've received frames recently
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_LastNDIFrameTime).count();

        if (timeSinceLastFrame > NDI_FRAME_TIMEOUT_MS) {
            timelog("NDI connection timeout detected (%lld ms since last frame)", timeSinceLastFrame);
            g_NDIConnectionHealthy = false;

            // Attempt reconnection if not already reconnecting
            if (!g_NDIReconnecting && g_NDIReconnectAttempts < MAX_NDI_RECONNECT_ATTEMPTS) {
                g_NDIReconnecting = true;
                g_NDIReconnectAttempts++;

                timelog("Attempting NDI reconnection #%d", g_NDIReconnectAttempts.load());

                // Clean up current connection
                std::lock_guard<std::mutex> lock(g_NDIStatusMutex);
                if (g_Index::g_NDIReceiver != nullptr) {
                    NDIlib_recv_destroy(g_Index::g_NDIReceiver);
                    g_Index::g_NDIReceiver = nullptr;
                }

                // Wait a moment for cleanup
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                // Attempt reconnection
                bool reconnectSuccess = WarmUpNDIDiscovery(2);

                if (reconnectSuccess) {
                    timelog("NDI automatic reconnection successful!");
                    g_NDIReconnectAttempts = 0; // Reset counter on success
                }
                else {
                    timelog("NDI automatic reconnection failed");
                }

                g_NDIReconnecting = false;
            }
            else if (g_NDIReconnectAttempts >= MAX_NDI_RECONNECT_ATTEMPTS) {
                timelog("Maximum NDI reconnection attempts reached. Manual intervention required.");
                // Don't reset attempts here - let user manually reconnect to reset counter
            }
        }
    }
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

    bool ndiWarmedUp = WarmUpNDIDiscovery(3);

    if (!ndiWarmedUp) {
        timelog("Initial NDI warm-up failed, but continuing...");
    }

    // Start NDI connection monitor thread
    std::thread ndiMonitorThread(NDIConnectionMonitor);
    ndiMonitorThread.detach();

    timelog("Trying to connect to MAKCU");

    if (!g_Index::g_makcuDevice.connect()) {
        timelog("Failed to connect to MAKCU device!!");
        timelog("Please ensure your MAKCU is connected in COM mode and is flashed properly, If you run into more errors, you can open a ticket.");
        delay_exit(default_exit_value);
    }
    g_Index::g_makcuDevice.setMouseButtonCallback(
        [](makcu::MouseButton btn, bool pressed) {
            g_Makcu::mouseButtonHandler(btn, pressed);
        }
    );

    timelog("Connected to MAKCU");
    timelog("Loading config.json");

    g_Config::LoadConfig("config.json");

    timelog("kSensValue: %.2f", g_Index::kSensitivity);
    timelog("dilation_size: %i", g_Index::dilation_size);
    timelog("kSlowdownThreshold: %.2g", g_Index::kSlowdownThreshold);

    std::thread aimThread([]() {
        while (true) {
            if (g_Index::aim_key == 0) {
                g_AimAssist::isBindHeld = g_Makcu::isButtonHeld(makcu::MouseButton::LEFT);
            } else if (g_Index::aim_key == 1) {
                g_AimAssist::isBindHeld = g_Makcu::isButtonHeld(makcu::MouseButton::RIGHT);
            }
            else if (g_Index::aim_key == 2) {
                g_AimAssist::isBindHeld = g_Makcu::isButtonHeld(makcu::MouseButton::SIDE1);
            }
            else if (g_Index::aim_key == 3) {
                g_AimAssist::isBindHeld = g_Makcu::isButtonHeld(makcu::MouseButton::SIDE2);
            }
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
    if (!RegisterHotKey(hwnd, 1, 0, VK_INSERT)) {
        std::cerr << "Failed to register hotkey" << std::endl;
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
        if (ImGui::Button("Aim Assist")) {
            kTabIndex = 1;
        } ImGui::SameLine();
        if (ImGui::Button("Visuals")) {
            kTabIndex = 2;
        } ImGui::SameLine();
        if (ImGui::Button("Config")) {
            kTabIndex = 3;
        } ImGui::SameLine();
        if (ImGui::Button("Debug")) {
            kTabIndex = 4;
        } ImGui::SameLine();
        if (ImGui::Button("Exit")) {
            exit(0);
        }

        if (kTabIndex == 0) {
            ImGui::Text("NDI Status");

            // Enhanced NDI status display
            if (g_Index::g_NDIReceiver && g_NDIConnectionHealthy) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected & Healthy");
            }
            else if (g_Index::g_NDIReceiver && !g_NDIConnectionHealthy) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Connected but No Data");
            }
            else if (g_NDIReconnecting) {
                ImGui::TextColored(ImVec4(0.0f, 0.0f, 1.0f, 1.0f), "Reconnecting...");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
            }

            // Show reconnection attempts
            if (g_NDIReconnectAttempts > 0) {
                ImGui::Text("Reconnect attempts: %d/%d", g_NDIReconnectAttempts.load(), MAX_NDI_RECONNECT_ATTEMPTS);
            }

            // Manual reconnect button
            if (ImGui::Button("Force NDI Reconnect")) {
                std::thread([] {
                    ForceNDIReconnect();
                    }).detach();
            } ImGui::SameLine();
            HelpMarker("Force a manual NDI reconnection with full cleanup and re-initialization");

            ImGui::Separator();

            ImGui::Text("Makcu Status");
            if (g_Index::g_makcuDevice.isConnected()) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
            }

            if (ImGui::Button("Test movement")) {
                g_Index::g_makcuDevice.mouseMove(5, 5);
            } ImGui::SameLine();
            HelpMarker("Tests to see if your MAKCU is working by moving it a set value (5,5)"); ImGui::SameLine();
            if (ImGui::Button("Reconnect MAKCU")) {
                g_Index::g_makcuDevice.disconnect();
                g_Index::g_makcuDevice.connect();
            } ImGui::SameLine(); HelpMarker("Reconnects your MAKCU in the event of no mouse movement, etc");
            ImGui::Separator();
        }
        else if (kTabIndex == 1) {
            ImGui::Checkbox("Enable Aim-Assist", &g_Index::assistance_enabled); 

            if (g_Index::assistance_enabled) {
                ImGui::SliderFloat("Speed", &g_Index::kSensitivity, 0.01, 10, "%.3f"); ImGui::SameLine(); HelpMarker("The speed at which the aim assist will move to a given target");
                ImGui::SliderInt("Smoothness", &g_Index::kSmoothingFactor, 1, 100, "%d"); ImGui::SameLine(); HelpMarker("The speed at which the aim assist will move to a given target");
                ImGui::SliderInt("FOV", &g_Index::kFOV, 1, 150, "%d"); ImGui::SameLine(); HelpMarker("The FOV (Field of view) the aim assist will scan for targets within");
                if (ImGui::BeginCombo("Preferred Hitbox", hitbox_options[g_Index::hitbox])) {
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
                ImGui::Separator();

                if (ImGui::BeginCombo("Bind", mouse_options[g_Index::aim_key])) {
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
            }
        }
        else if (kTabIndex == 2) {
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
        else if (kTabIndex == 3) {
            if (ImGui::Button("Load config")) {
                g_Config::LoadConfig();
            }

            if (ImGui::Button("Save config")) {
                g_Config::SaveConfig();
            }
        }
        else if (kTabIndex == 4) {
            ImGui::BeginDisabled();
            ImGui::Checkbox("Show debug window (CV2)", &g_Index::showDebug);
            ImGui::EndDisabled();
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
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}