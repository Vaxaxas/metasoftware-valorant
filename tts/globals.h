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
    inline float kSensitivityX = 0.35f; // X 軸水平瞄準速度
    inline float kSensitivityY = 0.35f; // Y 軸垂直瞄準速度
    inline int kSmoothingFactor = 1;
    inline int kFOV = 65;
    inline float aim_deadzone = 2.5f;        // 瞄準死區半徑 (px，GUI 可即時調整，預設 2.5px)
    inline bool kPreferUppermost = false;
    inline bool kPreferTorso = false;
    inline bool kPreferLowermost = false;
    inline int aim_key = 1; // 預設 1 = RIGHT (右鍵)
    inline int hitbox = 0;
    inline int aim_method = 0; // 0 = MOVE (原版相對平滑), 1 = SILENCE (Makcu km.moveto 座標直接傳送)
    inline int aim_x_offset = 0; // 水平偏移微調 (正值向右，負值向左)
    inline int aim_y_offset = 0; // 垂直偏移微調 (正值向下，負值向上)

    // Aim Humanizer (擬人化微震與自然軌跡擾動)
    inline bool aim_humanizer_enabled = false;
    inline float aim_humanizer_strength = 1.0f; // 擬人化擾動幅度 (0.5 ~ 3.0px)

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

    // RCS (Recoil Control System 後座力壓槍補償系統)
    inline bool rcs_enabled = false;
    inline float rcs_strength = 2.0f;        // 垂直壓槍強度 (向下補償)
    inline int rcs_delay_ms = 120;           // 壓槍啟動延遲 (ms，避開首發 1~2 顆精準點射)

    inline std::string makcu_ip = "pi.local";
    inline int makcu_port = 5005;
    inline bool makcu_use_udp = true;

    const std::string kVersionString = "__version_1.2_release";
}
