#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include "../../../thirdparty/imgui/imgui.h"
#include "../../../thirdparty/imgui/imgui_impl_win32.h"
#include "../../../thirdparty/imgui/imgui_impl_dx12.h"

namespace g_Fuser {
    void enable();
    void draw_fov();
    void cleanup();
    void SetMainWindowHandle(HWND hwnd);
    void RemoveWindowTitlebar(HWND hwnd);
    void RestoreWindowTitlebar(HWND hwnd);

    extern bool isOverlayActive;
}