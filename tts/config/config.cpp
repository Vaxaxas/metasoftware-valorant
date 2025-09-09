#include "config.h"
#include <Windows.h>
#include "../globals.h"
#include "../macros.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace g_Config {
    void LoadConfig(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            timelog("Could not open config.json, creating one with default values!");

            json j;
            j["image_processing"]["color"]["enemy_outline"] = "purple";

            j["aim"]["enabled"] = g_Index::assistance_enabled;
            j["aim"]["kSlowdownThreshold"] = g_Index::kSlowdownThreshold;
            j["aim"]["kSensitivity"] = g_Index::kSensitivity;
            j["aim"]["fov"] = 25;
            j["aim"]["key"] = g_Index::aim_key;
            j["aim"]["hitbox"] = g_Index::hitbox;

            j["visuals"]["gui"]["show_menu"] = VK_INSERT;
            j["visuals"]["fuser"] = false;

            j["safety"]["panic_button"] = VK_END;

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

            // Image processing
            if (j.contains("image_processing")) {
                const auto& ip = j["image_processing"];
                if (ip.contains("dilation_size")) g_Index::dilation_size = ip["dilation_size"].get<int>();
                if (ip.contains("kDistanceThreshold")) g_Index::kDistanceThreshold = ip["kDistanceThreshold"].get<double>();

                if (ip.contains("color") && ip["color"].contains("enemy_outline"))
                    g_Index::enemy_outline_color = ip["color"]["enemy_outline"].get<std::string>();
            }

            // Aim
            if (j.contains("aim")) {
                const auto& aim = j["aim"];
                if (aim.contains("enabled")) g_Index::assistance_enabled = aim["enabled"].get<bool>();
                if (aim.contains("kSlowdownThreshold")) g_Index::kSlowdownThreshold = aim["kSlowdownThreshold"].get<double>();
                if (aim.contains("kSensitivity")) g_Index::kSensitivity = aim["kSensitivity"].get<float>();
                if (aim.contains("fov")) g_Index::kFOV = aim["fov"].get<int>();
                if (aim.contains("key")) g_Index::aim_key = aim["key"].get<int>();
                if (aim.contains("hitbox")) g_Index::hitbox = aim["hitbox"].get<int>();
            }

            // Visuals
            if (j.contains("visuals")) {
                const auto& vis = j["visuals"];
                if (vis.contains("gui") && vis["gui"].contains("show_menu"))
                    g_Index::show_menu_key = vis["gui"]["show_menu"].get<int>();
                if (vis.contains("fuser")) g_Index::fuser_enabled = vis["fuser"].get<bool>();
            }

            // Safety
            if (j.contains("safety") && j["safety"].contains("panic_button"))
                g_Index::panic_button = j["safety"]["panic_button"].get<int>();

            timelog("Config loaded successfully");
        }
        catch (const std::exception& e) {
            timelog("Config failed to parse!");
        }
    }
    void SaveConfig(const std::string& filepath) {
        try {
            json j;

            j["image_processing"]["color"]["enemy_outline"] = g_Index::enemy_outline_color;

            j["aim"]["enabled"] = g_Index::assistance_enabled;
            j["aim"]["kSlowdownThreshold"] = g_Index::kSlowdownThreshold;
            j["aim"]["kSensitivity"] = g_Index::kSensitivity;
            j["aim"]["fov"] = g_Index::kFOV;
            j["aim"]["key"] = g_Index::aim_key;
            j["aim"]["hitbox"] = g_Index::hitbox;


            j["visuals"]["gui"]["show_menu"] = g_Index::show_menu_key;
            j["visuals"]["fuser"] = g_Index::fuser_enabled;


            j["safety"]["panic_button"] = g_Index::panic_button;


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
        catch (const std::exception& e) {
            timelog("Failed to save config");
        }
    }
}
