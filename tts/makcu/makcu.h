#pragma once
#include "../globals.h"

namespace g_Makcu {
    void mouseButtonHandler(makcu::MouseButton button, bool pressed);
    bool isButtonHeld(makcu::MouseButton button);
}
