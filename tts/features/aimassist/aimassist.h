#pragma once
#include "../../globals.h"
#include "../../macros.h"

#include <opencv2/opencv.hpp>
#include <atomic>

namespace g_AimAssist {
    extern cv::Scalar LOWER_COLOR;
    extern cv::Scalar UPPER_COLOR;

    extern std::atomic<bool> isBindHeld;
    extern int debugPixelCount;
    extern double debugTargetX;
    extern double debugTargetY;
    extern bool debugHasTarget;

    void ProcessImage(std::vector<std::vector<cv::Point>>& contours,
        std::vector<cv::Vec4i>& hierarchy);
    void Run();
    void Shutdown();
    void SetBindState(bool held);
    bool GetBindState();
}
