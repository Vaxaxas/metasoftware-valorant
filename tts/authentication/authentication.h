#pragma once
#include <string>
#include <ctime>
#include "../globals.h"

namespace g_Authentication {
    bool Auth();
    extern std::string auth_key;
}
