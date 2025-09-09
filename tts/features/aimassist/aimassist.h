#pragma once
#include "../../globals.h"
#include "../../macros.h"

#include <opencv2/opencv.hpp>

namespace g_AimAssist {
    extern cv::Scalar LOWER_COLOR;
    extern cv::Scalar UPPER_COLOR;

    extern bool isBindHeld;

    void ProcessImage(std::vector<std::vector<cv::Point>>& contours,
        std::vector<cv::Vec4i>& hierarchy);
    void Run();
}