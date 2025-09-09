#pragma once
#include <Windows.h>
#include <makcu/makcu.h>
#include "ndi/ndicontroller.h"
#include <string>

namespace g_Index {
    extern makcu::Device g_makcuDevice;
    extern NDIlib_recv_instance_t g_NDIReceiver;
    extern NDIlib_find_instance_t g_NDIFind;

    inline int dilation_size = 1;
    inline double kDistanceThreshold = 250;
    inline std::string enemy_outline_color = "purple";


    constexpr const double kScreenWidth = 256;
    constexpr const double kScreenHeight = 256;
    constexpr const double kScreenCenterX = kScreenWidth / 2;
    constexpr const double kScreenCenterY = kScreenHeight / 2;

    inline double kSlowdownThreshold = 20;
    inline float kSensitivity = 0.19f;
    inline int kSmoothingFactor = 1;
    inline int kFOV = 25;
    inline bool kPreferUppermost = false;
    inline bool kPreferTorso = false;
    inline bool kPreferLowermost = false;
    inline int aim_key = 0;
    inline int hitbox = 0;

    inline int show_menu_key = VK_INSERT;
    inline bool fuser_enabled = false;
    inline bool draw_watermark = false;
    inline bool draw_fov = false;
    inline bool showDebug = false;

    inline int panic_button = VK_END;

    inline bool assistance_enabled = false;

    const std::string kVersionString = "__version_1.2_release";
}