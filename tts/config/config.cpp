#include "config.h"
#include <Windows.h>
#include "../globals.h"
#include "../macros.h"
#include "../thirdparty/nlohmann/json.hpp"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace g_Config {
    void LoadConfig(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            timelog("Could not open config.json, creating one with default values!");

            json j;

            j["aim"]["enabled"] = g_Index::assistance_enabled;
            j["aim"]["kSlowdownThreshold"] = g_Index::kSlowdownThreshold;
            j["aim"]["kSensitivity"] = g_Index::kSensitivity;
            j["aim"]["kSmoothingFactor"] = g_Index::kSmoothingFactor;
            j["aim"]["fov"] = g_Index::kFOV;
            j["aim"]["key"] = g_Index::aim_key;
            j["aim"]["hitbox"] = g_Index::hitbox;
            j["aim"]["x_offset"] = g_Index::aim_x_offset;
            j["aim"]["y_offset"] = g_Index::aim_y_offset;

            j["trigger"]["enabled"] = g_Index::trigger_enabled;
            j["trigger"]["key"] = g_Index::trigger_key;
            j["trigger"]["fov"] = g_Index::trigger_fov;
            j["trigger"]["delay_ms"] = g_Index::trigger_delay_ms;
            j["trigger"]["cooldown_ms"] = g_Index::trigger_cooldown_ms;
            j["trigger"]["click_duration"] = g_Index::trigger_click_duration;

            j["visuals"]["gui"]["show_menu"] = VK_INSERT;
            j["visuals"]["fuser"] = false;
            j["visuals"]["debug"] = g_Index::showDebug;
            j["debug"] = g_Index::showDebug;

            j["safety"]["panic_button"] = VK_END;

            j["makcu"]["ip"] = g_Index::makcu_ip;
            j["makcu"]["port"] = g_Index::makcu_port;
            j["makcu"]["use_udp"] = g_Index::makcu_use_udp;

            std::ofstream outfile(filepath);
            if (outfile.is_open()) {
                outfile << j.dump(4);
                outfile.close();
                timelog("Config created & saved");
            }
            else {
                timelog("Failed to save config file!");
            }

            return;
        }

        try {
            json j;
            file >> j;

            // Aim
            if (j.contains("aim")) {
                const auto& aim = j["aim"];
                if (aim.contains("enabled")) g_Index::assistance_enabled = aim["enabled"].get<bool>();
                if (aim.contains("kSlowdownThreshold")) g_Index::kSlowdownThreshold = aim["kSlowdownThreshold"].get<double>();
                if (aim.contains("kSensitivity")) g_Index::kSensitivity = aim["kSensitivity"].get<float>();
                if (aim.contains("kSmoothingFactor")) {
                    g_Index::kSmoothingFactor = aim["kSmoothingFactor"].get<int>();
                    if (g_Index::kSmoothingFactor < 1 || g_Index::kSmoothingFactor > 100) g_Index::kSmoothingFactor = 1;
                }
                if (aim.contains("fov")) g_Index::kFOV = aim["fov"].get<int>();
                if (aim.contains("key")) {
                    g_Index::aim_key = aim["key"].get<int>();
                    if (g_Index::aim_key < 0 || g_Index::aim_key > 5) g_Index::aim_key = 1;
                }
                if (aim.contains("hitbox")) {
                    g_Index::hitbox = aim["hitbox"].get<int>();
                    if (g_Index::hitbox < 0 || g_Index::hitbox > 2) g_Index::hitbox = 0;
                }
                if (aim.contains("x_offset")) {
                    g_Index::aim_x_offset = aim["x_offset"].get<int>();
                    if (g_Index::aim_x_offset < -20 || g_Index::aim_x_offset > 20) g_Index::aim_x_offset = 0;
                }
                if (aim.contains("y_offset")) {
                    g_Index::aim_y_offset = aim["y_offset"].get<int>();
                    if (g_Index::aim_y_offset < -20 || g_Index::aim_y_offset > 20) g_Index::aim_y_offset = 0;
                }
            }

            // Trigger
            if (j.contains("trigger")) {
                const auto& trig = j["trigger"];
                if (trig.contains("enabled")) g_Index::trigger_enabled = trig["enabled"].get<bool>();
                if (trig.contains("key")) {
                    g_Index::trigger_key = trig["key"].get<int>();
                    if (g_Index::trigger_key < 0 || g_Index::trigger_key > 8) g_Index::trigger_key = 0;
                }
                if (trig.contains("fov")) {
                    g_Index::trigger_fov = trig["fov"].get<int>();
                    if (g_Index::trigger_fov < 1 || g_Index::trigger_fov > 30) g_Index::trigger_fov = 4;
                }
                if (trig.contains("delay_ms")) {
                    g_Index::trigger_delay_ms = trig["delay_ms"].get<int>();
                    if (g_Index::trigger_delay_ms < 0 || g_Index::trigger_delay_ms > 500) g_Index::trigger_delay_ms = 35;
                }
                if (trig.contains("cooldown_ms")) {
                    g_Index::trigger_cooldown_ms = trig["cooldown_ms"].get<int>();
                    if (g_Index::trigger_cooldown_ms < 20 || g_Index::trigger_cooldown_ms > 2000) g_Index::trigger_cooldown_ms = 180;
                }
                if (trig.contains("click_duration")) {
                    g_Index::trigger_click_duration = trig["click_duration"].get<int>();
                    if (g_Index::trigger_click_duration < 10 || g_Index::trigger_click_duration > 500) g_Index::trigger_click_duration = 50;
                }
            }

            // Visuals
            if (j.contains("visuals")) {
                const auto& vis = j["visuals"];
                if (vis.contains("gui") && vis["gui"].contains("show_menu"))
                    g_Index::show_menu_key = vis["gui"]["show_menu"].get<int>();
                if (vis.contains("fuser")) g_Index::fuser_enabled = vis["fuser"].get<bool>();
                if (vis.contains("debug")) g_Index::showDebug = vis["debug"].get<bool>();
            }

            if (j.contains("debug")) {
                g_Index::showDebug = j["debug"].get<bool>();
            }

            // Safety
            if (j.contains("safety") && j["safety"].contains("panic_button"))
                g_Index::panic_button = j["safety"]["panic_button"].get<int>();

            // Makcu
            if (j.contains("makcu")) {
                const auto& mk = j["makcu"];
                if (mk.contains("ip")) g_Index::makcu_ip = mk["ip"].get<std::string>();
                if (mk.contains("port")) g_Index::makcu_port = mk["port"].get<int>();
                if (mk.contains("use_udp")) g_Index::makcu_use_udp = mk["use_udp"].get<bool>();
            }

            // 同步切換控制台視窗顯示/隱藏
            HWND consoleWnd = GetConsoleWindow();
            if (consoleWnd) {
                ShowWindow(consoleWnd, g_Index::showDebug ? SW_SHOW : SW_HIDE);
            }

            timelog("Config loaded successfully");
        }
        catch (const std::exception&) {
            timelog("Config failed to parse!");
        }
    }

    void SaveConfig(const std::string& filepath) {
        try {
            json j;

            j["aim"]["enabled"] = g_Index::assistance_enabled;
            j["aim"]["kSlowdownThreshold"] = g_Index::kSlowdownThreshold;
            j["aim"]["kSensitivity"] = g_Index::kSensitivity;
            j["aim"]["kSmoothingFactor"] = g_Index::kSmoothingFactor;
            j["aim"]["fov"] = g_Index::kFOV;
            j["aim"]["key"] = g_Index::aim_key;
            j["aim"]["hitbox"] = g_Index::hitbox;
            j["aim"]["x_offset"] = g_Index::aim_x_offset;
            j["aim"]["y_offset"] = g_Index::aim_y_offset;

            j["trigger"]["enabled"] = g_Index::trigger_enabled;
            j["trigger"]["key"] = g_Index::trigger_key;
            j["trigger"]["fov"] = g_Index::trigger_fov;
            j["trigger"]["delay_ms"] = g_Index::trigger_delay_ms;
            j["trigger"]["cooldown_ms"] = g_Index::trigger_cooldown_ms;
            j["trigger"]["click_duration"] = g_Index::trigger_click_duration;

            j["visuals"]["gui"]["show_menu"] = g_Index::show_menu_key;
            j["visuals"]["fuser"] = g_Index::fuser_enabled;
            j["visuals"]["debug"] = g_Index::showDebug;
            j["debug"] = g_Index::showDebug;

            j["safety"]["panic_button"] = g_Index::panic_button;

            j["makcu"]["ip"] = g_Index::makcu_ip;
            j["makcu"]["port"] = g_Index::makcu_port;
            j["makcu"]["use_udp"] = g_Index::makcu_use_udp;

            std::ofstream outfile(filepath);
            if (outfile.is_open()) {
                outfile << j.dump(4);
                outfile.close();
                timelog("Config saved successfully");
            }
            else {
                timelog("Failed to open config file for writing!");
            }
        }
        catch (const std::exception&) {
            timelog("Failed to save config");
        }
    }
}
