#include "authentication.h"
#include <iostream>
#include <sstream>
#include <ctime>
#include <chrono>
#include <windows.h>
#include <fstream>
#include <iomanip>
#include "../macros.h"

namespace g_Authentication {
    std::string auth_key = "";

    bool Auth() {
        timelog("1-PC Mode Activated: Offline Mode / Authentication Bypassed.");
        timelog("Welcome to MetaSoftware 1-PC Edition!");
        return true;
    }
}
