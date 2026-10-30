#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "makcu/makcu.h"
#include "ndi/ndicontroller.h"
#include <string>

namespace g_Index {
    extern makcu::Device g_makcuDevice;
    extern NDIlib_recv_instance_t g_NDIReceiver;
    extern NDIlib_find_instance_t g_NDIFind;

    constexpr const double kScreenWidth = 256;
    constexpr const double kScreenHeight = 256;
    constexpr const double kScreenCenterX = kScreenWidth / 2;
    constexpr const double kScreenCenterY = kScreenHeight / 2;

    inline double kSlowdownThreshold = 20;
    inline float kSensitivity = 0.35f;
    inline int kSmoothingFactor = 1;
    inline int kFOV = 65;
    inline bool kPreferUppermost = false;
    inline bool kPreferTorso = false;
    inline bool kPreferLowermost = false;
    inline int aim_key = 1; // 預設 1 = RIGHT (右鍵)
    inline int hitbox = 0;
    inline int aim_x_offset = 0; // 水平偏移微調 (正值向右，負值向左)
    inline int aim_y_offset = 0; // 垂直偏移微調 (正值向下，負值向上)

    inline int show_menu_key = VK_INSERT;
    inline bool fuser_enabled = false;
    inline bool draw_watermark = false;
    inline bool draw_fov = true;
    inline bool showDebug = true; // 預設開啟 Debug 模式
    inline bool show_mask_view = false; // 顏色過濾後二值化遮罩檢視 (查看程式實際辨識到的色塊)

    inline int panic_button = VK_END;

    inline bool assistance_enabled = true; // 預設開啟瞄準輔助

    // Triggerbot 自動開火配置 (遵循 Makcu 官方 API: km.click)
    inline bool trigger_enabled = false;
    inline int trigger_key = 0;              // 0=Always On, 1=ALT, 2=SHIFT, 3=SIDE1, 4=SIDE2, 5=RIGHT
    inline int trigger_fov = 4;              // 準星中心觸發半徑 (px)
    inline int trigger_delay_ms = 35;        // 開火前反應延遲 (ms)
    inline int trigger_cooldown_ms = 180;    // 每次開火後冷卻時間 (ms)
    inline int trigger_click_duration = 50;  // 點擊按住時間 (ms，符合 Makcu 35~75ms 規範)

    inline std::string makcu_ip = "192.168.137.19";
    inline int makcu_port = 5005;
    inline bool makcu_use_udp = true;

    const std::string kVersionString = "__version_1.2_release";
}
