#pragma once
#include "../../globals.h"
#include "../../macros.h"

#include <opencv2/opencv.hpp>
#include <atomic>

namespace g_AimAssist {
    extern std::atomic<bool> isBindHeld;
    extern int debugPixelCount;
    extern double debugTargetX;
    extern double debugTargetY;
    extern bool debugHasTarget;

    void Run();
    void Shutdown();
    void SetBindState(bool held);
    bool GetBindState();
}
