#include "authentication.h"
#include <iostream>
#include <sstream>
#include <ctime>
#include <chrono>
#include <windows.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <comdef.h>
#include <Wbemidl.h>
#include "../macros.h"

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace g_Authentication {
    std::string auth_key = "";

    std::string WMIQuery(const std::string& query, const std::string& property) {
        HRESULT hres;
        hres = CoInitializeEx(0, COINIT_MULTITHREADED);
        if (FAILED(hres)) return "Unknown";
        hres = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
        IWbemLocator* pLoc = NULL;
        hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, (LPVOID*)&pLoc);
        if (FAILED(hres)) {
            CoUninitialize();
            return "Unknown";
        }
        IWbemServices* pSvc = NULL;
        hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
        if (FAILED(hres)) {
            pLoc->Release();
            CoUninitialize();
            return "Unknown";
        }
        hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
        IEnumWbemClassObject* pEnumerator = NULL;
        hres = pSvc->ExecQuery(bstr_t("WQL"), bstr_t(query.c_str()),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
        std::string result = "Unknown";
        if (!FAILED(hres)) {
            IWbemClassObject* pclsObj = NULL;
            ULONG uReturn = 0;
            if (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == WBEM_S_NO_ERROR) {
                VARIANT vtProp;
                hres = pclsObj->Get(_bstr_t(property.c_str()), 0, &vtProp, 0, 0);
                if (!FAILED(hres) && vtProp.vt == VT_BSTR) {
                    _bstr_t bstrVal(vtProp.bstrVal);
                    result = std::string(bstrVal);
                }
                VariantClear(&vtProp);
                pclsObj->Release();
            }
            pEnumerator->Release();
        }
        pSvc->Release();
        pLoc->Release();
        CoUninitialize();
        return result;
    }

    SystemSpecs GetSystemSpecs() {
        SystemSpecs specs;
        specs.cpu_name = WMIQuery("SELECT Name FROM Win32_Processor", "Name");
        specs.gpu_name = WMIQuery("SELECT Name FROM Win32_VideoController", "Name");
        std::string ramSize = WMIQuery("SELECT TotalPhysicalMemory FROM Win32_ComputerSystem", "TotalPhysicalMemory");
        if (ramSize != "Unknown") {
            long long ram = std::stoll(ramSize) / (1024 * 1024 * 1024);
            specs.ram_info = std::to_string(ram) + " GB";
        }
        else {
            specs.ram_info = "Unknown";
        }
        specs.os_version = WMIQuery("SELECT Caption FROM Win32_OperatingSystem", "Caption");
        specs.motherboard = WMIQuery("SELECT Product FROM Win32_BaseBoard", "Product");
        return specs;
    }

    std::string SystemSpecsToJson(const SystemSpecs& specs) {
        nlohmann::json j;
        j["cpu_name"] = specs.cpu_name;
        j["gpu_name"] = specs.gpu_name;
        j["ram_info"] = specs.ram_info;
        j["os_version"] = specs.os_version;
        j["motherboard"] = specs.motherboard;
        return j.dump();
    }

    std::string GetCurrentExecutablePath() {
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        return std::string(buffer);
    }

    std::string GetFileHash(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) return "";
        std::hash<std::string> hasher;
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t hashValue = hasher(content);
        std::stringstream ss;
        ss << std::hex << hashValue;
        return ss.str();
    }

    std::string GetHardwareID() {
        std::string hwid = "";
        int cpuInfo[4] = { 0 };
        __cpuid(cpuInfo, 0);
        char cpuString[13] = { 0 };
        *reinterpret_cast<int*>(cpuString) = cpuInfo[1];
        *reinterpret_cast<int*>(cpuString + 4) = cpuInfo[3];
        *reinterpret_cast<int*>(cpuString + 8) = cpuInfo[2];
        hwid += cpuString;
        IP_ADAPTER_INFO adapterInfo[16];
        DWORD dwBufLen = sizeof(adapterInfo);
        DWORD dwStatus = GetAdaptersInfo(adapterInfo, &dwBufLen);
        if (dwStatus == ERROR_SUCCESS) {
            PIP_ADAPTER_INFO pAdapterInfo = adapterInfo;
            if (pAdapterInfo) {
                char macStr[18];
                sprintf_s(macStr, "%02X-%02X-%02X-%02X-%02X-%02X",
                    pAdapterInfo->Address[0], pAdapterInfo->Address[1],
                    pAdapterInfo->Address[2], pAdapterInfo->Address[3],
                    pAdapterInfo->Address[4], pAdapterInfo->Address[5]);
                hwid += macStr;
            }
        }
        DWORD volumeSerial;
        if (GetVolumeInformationA("C:\\", NULL, 0, &volumeSerial, NULL, NULL, NULL, 0)) {
            hwid += std::to_string(volumeSerial);
        }
        return hwid;
    }

    size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string HttpGet(const std::string& url) {
        CURL* curl;
        CURLcode res;
        std::string readBuffer;
        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            if (res != CURLE_OK) {
                return "";
            }
        }
        return readBuffer;
    }

    AuthData ParseAuthResponse(const std::string& response) {
        AuthData authData = { false, "", 0, "" };
        try {
            nlohmann::json j = nlohmann::json::parse(response);
            if (j.contains("success")) {
                authData.success = j["success"].get<bool>();
            }
            if (j.contains("username")) {
                authData.username = j["username"].get<std::string>();
            }
            if (j.contains("expires")) {
                authData.expires = j["expires"].get<std::time_t>();
            }
            if (j.contains("hardware_id")) {
                authData.hardware_id = j["hardware_id"].get<std::string>();
            }
        }
        catch (const nlohmann::json::exception& e) {
            authData.success = false;
        }
        return authData;
    }

    bool Auth() {
        if (auth_key.empty()) {
            timelog("Authentication key not provided");
            return false;
        }
        std::string currentHwid = GetHardwareID();
        std::string exePath = GetCurrentExecutablePath();
        std::string fileHash = g_Index::kVersionString;
        SystemSpecs specs = GetSystemSpecs();
        std::string specsJson = SystemSpecsToJson(specs);
        std::string url = "https://metasoftware.me/client/authenticate?key=" + auth_key +
            "&hardware_id=" + currentHwid + "&client_hash=" + fileHash +
            "&system_specs=" + curl_easy_escape(nullptr, specsJson.c_str(), specsJson.size());
        std::string response = HttpGet(url);
        if (response.empty()) {
            return false;
        }
        AuthData authData = ParseAuthResponse(response);
        if (!authData.success) {
            return false;
        }
        auto currentTime = std::chrono::system_clock::now();
        auto currentEpoch = std::chrono::duration_cast<std::chrono::seconds>(currentTime.time_since_epoch()).count();
        if (authData.expires <= currentEpoch) {
            std::cerr << "Authentication failed: License expired" << std::endl;
            std::cerr << "License expired at: " << authData.expires << ", current time: " << currentEpoch << std::endl;
            return false;
        }

        std::time_t expiryTime = authData.expires;
        std::tm tmBuf;
        localtime_s(&tmBuf, &expiryTime);
        char expiryStr[64];
        std::strftime(expiryStr, sizeof(expiryStr), "%m/%d/%Y (%H:%M:%S)", &tmBuf);

        timelog("Welcome %s!", authData.username);
        timelog("Your subscription expires: %s", expiryStr);
        return true;
    }
}
