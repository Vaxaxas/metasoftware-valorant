#include "fuser.h"
#include "../../../globals.h"
#include <iostream>
#include <thread>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace g_Fuser {
    bool isOverlayActive = false;
    HWND overlayHwnd = nullptr;
    HWND mainWindowHwnd = nullptr;
    std::thread overlayThread;
    bool shouldExit = false;
    ULONG_PTR gdiplusToken;
    bool lastDrawFov = false;
    bool lastDrawWatermark = false;
    int lastFovValue = 0;

    LRESULT WINAPI OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_DESTROY:
            shouldExit = true;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                shouldExit = true;
                PostQuitMessage(0);
                return 0;
            }
            break;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            Graphics graphics(hdc);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);

            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            SolidBrush blackBrush(Color(200, 0, 0, 0));
            graphics.FillRectangle(&blackBrush, 0, 0, screenWidth, screenHeight);

            if (g_Index::draw_fov) {
                float centerX = screenWidth * 0.5f;
                float centerY = screenHeight * 0.5f;
                float fovRadius = static_cast<float>(g_Index::kFOV);

                Pen yellowPen(Color(255, 255, 255, 0), 2.0f);
                graphics.DrawEllipse(&yellowPen,
                    centerX - fovRadius, centerY - fovRadius,
                    fovRadius * 2, fovRadius * 2);
            }

            if (g_Index::draw_watermark) {
                SolidBrush textBrush(Color(255, 255, 255, 255));
                FontFamily fontFamily(L"Arial");
                Font font(&fontFamily, 16, FontStyleRegular, UnitPixel);
                graphics.DrawString(L"metasoftware.me", -1, &font,
                    PointF(10, 10), &textBrush);
            }

            EndPaint(hWnd, &ps);
            return 0;
        }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void OverlayThreadFunction() {
        GdiplusStartupInput gdiplusStartupInput;
        if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
            std::cerr << "Failed to initialize GDI+" << std::endl;
            return;
        }

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"FuserOverlay";

        if (!RegisterClassExW(&wc)) {
            std::cerr << "Failed to register overlay window class" << std::endl;
            GdiplusShutdown(gdiplusToken);
            return;
        }

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        overlayHwnd = CreateWindowExW(
            WS_EX_LAYERED,
            wc.lpszClassName,
            L"Fuser Overlay",
            WS_POPUP | WS_VISIBLE,
            0, 0, screenWidth, screenHeight,
            nullptr, nullptr, wc.hInstance, nullptr
        );

        if (!overlayHwnd) {
            std::cerr << "Failed to create overlay window" << std::endl;
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            GdiplusShutdown(gdiplusToken);
            return;
        }

        SetLayeredWindowAttributes(overlayHwnd, 0, 255, LWA_ALPHA);

        ShowWindow(overlayHwnd, SW_SHOW);
        UpdateWindow(overlayHwnd);

        if (mainWindowHwnd) {
            SetWindowPos(mainWindowHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }

        MSG msg = {};
        while (!shouldExit) {
            while (PeekMessage(&msg, overlayHwnd, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    shouldExit = true;
                    break;
                }
            }

            if (shouldExit) break;

            bool needsRedraw = (lastDrawFov != g_Index::draw_fov ||
                lastDrawWatermark != g_Index::draw_watermark ||
                lastFovValue != g_Index::kFOV);

            if (needsRedraw) {
                lastDrawFov = g_Index::draw_fov;
                lastDrawWatermark = g_Index::draw_watermark;
                lastFovValue = g_Index::kFOV;
                InvalidateRect(overlayHwnd, nullptr, TRUE);
                UpdateWindow(overlayHwnd);
            }

            if (mainWindowHwnd && overlayHwnd) {
                HWND topWindow = GetForegroundWindow();
                if (topWindow != mainWindowHwnd) {
                    SetWindowPos(overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    SetWindowPos(mainWindowHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    BringWindowToTop(mainWindowHwnd);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (overlayHwnd) {
            DestroyWindow(overlayHwnd);
            overlayHwnd = nullptr;
        }

        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        GdiplusShutdown(gdiplusToken);
        isOverlayActive = false;
    }

    void enable() {
        if (isOverlayActive) {
            return;
        }

        shouldExit = false;
        isOverlayActive = true;
        lastDrawFov = g_Index::draw_fov;
        lastDrawWatermark = g_Index::draw_watermark;
        lastFovValue = g_Index::kFOV;

        overlayThread = std::thread(OverlayThreadFunction);
        overlayThread.detach();

        std::cout << "Fuser overlay enabled" << std::endl;
    }

    void draw_fov() {
    }

    void cleanup() {
        if (!isOverlayActive) {
            return;
        }

        shouldExit = true;

        if (overlayHwnd) {
            PostMessage(overlayHwnd, WM_CLOSE, 0, 0);
        }

        if (mainWindowHwnd) {
            SetWindowPos(mainWindowHwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        isOverlayActive = false;
        std::cout << "Fuser overlay disabled" << std::endl;
    }

    void SetMainWindowHandle(HWND hwnd) {
        mainWindowHwnd = hwnd;
    }
}