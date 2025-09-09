#pragma once
#include <string>

namespace g_Config {
    void LoadConfig(const std::string& filepath = "config.json");
    void SaveConfig(const std::string& filepath = "config.json");
}
