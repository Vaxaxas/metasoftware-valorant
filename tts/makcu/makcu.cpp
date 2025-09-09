#include "makcu.h"

static std::unordered_map<makcu::MouseButton, bool> buttonStates;

namespace g_Makcu {
    void mouseButtonHandler(makcu::MouseButton button, bool pressed) {
        buttonStates[button] = pressed;
        if (pressed) {
            g_Index::g_makcuDevice.mouseDown(button);
        }
        else {
            g_Index::g_makcuDevice.mouseUp(button);
        }
    }

    bool isButtonHeld(makcu::MouseButton button) {
        auto it = buttonStates.find(button);
        return (it != buttonStates.end()) ? it->second : false;
    }
}