#pragma once
#include <string>
#include <ctime>
#include "../globals.h"

namespace g_Authentication {
    struct AuthData {
        bool success;
        std::string username;
        std::time_t expires;
        std::string hardware_id;
    };

    bool Auth();
    struct SystemSpecs {
        std::string cpu_name;
        std::string gpu_name;
        std::string ram_info;
        std::string os_version;
        std::string motherboard;
    };

    std::string GetFileHash(const std::string& filePath);
    std::string GetCurrentExecutablePath();
    SystemSpecs GetSystemSpecs();
    std::string SystemSpecsToJson(const SystemSpecs& specs);
    AuthData ParseAuthResponse(const std::string& response);
    std::string HttpGet(const std::string& url);
    extern std::string auth_key;
}