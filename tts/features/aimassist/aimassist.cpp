#include "aimassist.h"
#include <list>
#include <atomic>
#include <chrono>
#include <limits>
#include <vector>
#include <cmath>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <immintrin.h>

extern void UpdateNDIFrameReceived();
extern std::atomic<bool> g_NDIConnectionHealthy;

namespace g_AimAssist {
    template<typename T>
    inline T clamp(const T& val, const T& min_val, const T& max_val) {
        return (val < min_val) ? min_val : ((val > max_val) ? max_val : val);
    }

    struct Vector2 {
        double x, y;
        Vector2(double x = 0, double y = 0) : x(x), y(y) {}
        Vector2 operator+(const Vector2& other) const { return Vector2(x + other.x, y + other.y); }
        Vector2 operator-(const Vector2& other) const { return Vector2(x - other.x, y - other.y); }
        Vector2 operator*(double scalar) const { return Vector2(x * scalar, y * scalar); }
        double LenSquared() const { return x * x + y * y; }
        double Len() const { return std::sqrt(LenSquared()); }
    };

    static bool purpleLUT[256][256][256];
    static std::once_flag lutInitFlag;

    bool isBindHeld = false;
    std::atomic<bool> captureThreadRunning{ true };
    cv::Mat sharedFrame;
    std::mutex frameMutex;
    std::condition_variable frameCV;
    std::atomic<bool> newFrameAvailable{ false };

    Vector2 currentTarget(0, 0);
    Vector2 previousTarget(0, 0);
    Vector2 lastMovementTarget(0, 0);
    bool hasValidTarget = false;
    bool isOnTarget = false;
    int stableFrames = 0;
    int onTargetFrames = 0;

    constexpr double UPDATE_RATE = 120.0;
    constexpr double TARGET_JUMP_THRESHOLD = 150.0;
    constexpr int STABILITY_FRAMES = 3;
    constexpr double DEADZONE_RADIUS = 8.0;
    constexpr double ON_TARGET_THRESHOLD = 12.0;
    constexpr int ON_TARGET_FRAMES_REQUIRED = 5;

    struct HitboxOffset {
        int x, y;
    };

    const HitboxOffset HITBOX_OFFSETS[] = {
        {0, -7},
        {0, -4},
        {0, 10}
    };

    void InitializePurpleLUT() {
        for (int r = 0; r < 256; ++r) {
            for (int g = 0; g < 256; ++g) {
                for (int b = 0; b < 256; ++b) {
                    int maxRGB = std::max(r, std::max(g, b));
                    int minRGB = std::min(r, std::min(g, b));

                    if (maxRGB == 0) {
                        purpleLUT[r][g][b] = false;
                        continue;
                    }

                    int delta = maxRGB - minRGB;
                    int h = 0;

                    if (delta != 0) {
                        if (maxRGB == r) {
                            h = 60 * ((g - b) / delta);
                            if (g < b) h += 360;
                        }
                        else if (maxRGB == g) {
                            h = 60 * ((b - r) / delta) + 120;
                        }
                        else {
                            h = 60 * ((r - g) / delta) + 240;
                        }
                    }

                    int s = (delta * 255) / maxRGB;
                    int v = maxRGB;
                    h = h / 2;

                    purpleLUT[r][g][b] = (h >= 130 && h <= 170 && s >= 80 && v >= 120);
                }
            }
        }
    }

    bool IsPurpleColor(unsigned char r, unsigned char g, unsigned char b) {
        std::call_once(lutInitFlag, InitializePurpleLUT);
        return purpleLUT[r][g][b];
    }

    Vector2 FindTargetCentroid(const cv::Mat& frame) {
        if (!isBindHeld) {
            hasValidTarget = false;
            return Vector2(0, 0);
        }

        const int height = frame.rows;
        const int width = frame.cols;
        const int centerX = width >> 1;
        const int centerY = height >> 1;

        const int fovRadius = clamp(g_Index::kFOV, 50, 500);
        const int minX = std::max(centerX - fovRadius, 0);
        const int maxX = std::min(centerX + fovRadius, width);
        const int minY = std::max(centerY - fovRadius, 0);
        const int maxY = std::min(centerY + fovRadius, height);

        std::vector<Vector2> purplePixels;
        purplePixels.reserve(1000);
        int topMostY = INT_MAX;

        for (int y = minY; y < maxY; y++) {
            const cv::Vec3b* row = frame.ptr<cv::Vec3b>(y);
            for (int x = minX; x < maxX; x++) {
                const cv::Vec3b& pixel = row[x];
                if (IsPurpleColor(pixel[2], pixel[1], pixel[0])) {
                    purplePixels.emplace_back(x, y);
                    if (y < topMostY) {
                        topMostY = y;
                    }
                }
            }
        }

        if (purplePixels.empty()) {
            hasValidTarget = false;
            return Vector2(0, 0);
        }

        std::vector<Vector2> topPixels;
        for (const auto& pixel : purplePixels) {
            if (pixel.y == topMostY) {
                topPixels.push_back(pixel);
            }
        }

        if (topPixels.empty()) {
            hasValidTarget = false;
            return Vector2(0, 0);
        }

        double sumX = 0;
        for (const auto& pixel : topPixels) {
            sumX += pixel.x;
        }

        double headCenterX = sumX / topPixels.size();
        double headCenterY = topMostY + 3;

        hasValidTarget = true;
        return Vector2(headCenterX - centerX, headCenterY - centerY);
    }

    Vector2 ApplyHitboxOffset(const Vector2& target) {
        if (!hasValidTarget) return target;

        int hitboxIndex = clamp(g_Index::hitbox, 0, 2);
        const auto& baseOffset = HITBOX_OFFSETS[hitboxIndex];

        int adjustedOffsetX = baseOffset.x;
        int adjustedOffsetY = baseOffset.y;

        return Vector2(target.x + adjustedOffsetX, target.y + adjustedOffsetY);
    }

    Vector2 ApplySmoothing(const Vector2& targetPos) {
        double distance = targetPos.Len();

        if (distance <= ON_TARGET_THRESHOLD) {
            onTargetFrames++;
            if (onTargetFrames >= ON_TARGET_FRAMES_REQUIRED) {
                isOnTarget = true;
            }
        }
        else {
            onTargetFrames = 0;
            isOnTarget = false;
        }

        if (isOnTarget && distance <= DEADZONE_RADIUS) {
            return Vector2(0, 0);
        }

        double jumpDistance = (targetPos - previousTarget).Len();
        bool isJumping = jumpDistance > TARGET_JUMP_THRESHOLD;

        if (isJumping && hasValidTarget) {
            stableFrames = 0;
            isOnTarget = false;
            onTargetFrames = 0;
        }
        else {
            stableFrames++;
        }

        if (stableFrames < STABILITY_FRAMES) {
            previousTarget = targetPos;
            return Vector2(0, 0);
        }

        double smoothing = clamp(g_Index::kSmoothingFactor, 1, 100) * 0.01;
        double responsiveness = 1.0 - smoothing;

        if (isOnTarget) {
            responsiveness *= 0.3;
        }

        currentTarget = currentTarget + (targetPos - currentTarget) * responsiveness;
        previousTarget = targetPos;

        return currentTarget;
    }

    void ExecuteMovement(const Vector2& target) {
        double distance = target.Len();
        if (distance < DEADZONE_RADIUS) return;

        if (isOnTarget && distance < ON_TARGET_THRESHOLD) {
            return;
        }

        double moveX = target.x * g_Index::kSensitivity * 0.5;
        double moveY = target.y * g_Index::kSensitivity * 0.5;

        if (isOnTarget) {
            moveX *= 0.4;
            moveY *= 0.4;
        }

        double maxMove = isOnTarget ? 8.0 : 15.0;
        double moveLen = std::sqrt(moveX * moveX + moveY * moveY);

        if (moveLen > maxMove) {
            double scale = maxMove / moveLen;
            moveX *= scale;
            moveY *= scale;
        }

        int duration = clamp(static_cast<int>(10.0 / g_Index::kSensitivity), 2, 15);
        if (isOnTarget) {
            duration *= 2;
        }

        lastMovementTarget = target;
        g_Index::g_makcuDevice.mouseMoveBezier(moveX, moveY, duration, moveX, moveY);
    }

    void ProcessFrame(const cv::Mat& frame) {
        if (!isBindHeld || !g_NDIConnectionHealthy.load()) {
            hasValidTarget = false;
            currentTarget = Vector2(0, 0);
            isOnTarget = false;
            onTargetFrames = 0;
            stableFrames = 0;
            return;
        }

        Vector2 rawTarget = FindTargetCentroid(frame);
        if (!hasValidTarget) {
            isOnTarget = false;
            onTargetFrames = 0;
            return;
        }

        Vector2 hitboxTarget = ApplyHitboxOffset(rawTarget);
        Vector2 smoothedTarget = ApplySmoothing(hitboxTarget);

        if (smoothedTarget.LenSquared() > 1.0) {
            ExecuteMovement(smoothedTarget);
        }
    }

    void NDI_CaptureLoop() {
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
        cv::Mat frameBGR, hsvFrame, mask;

        while (captureThreadRunning.load()) {
            if (!g_Index::g_NDIReceiver) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            NDIlib_video_frame_v2_t video_frame;
            auto result = NDIlib_recv_capture_v2(g_Index::g_NDIReceiver, &video_frame, nullptr, nullptr, 500);

            if (result == NDIlib_frame_type_video) {
                if (!video_frame.p_data || video_frame.xres == 0 || video_frame.yres == 0) {
                    NDIlib_recv_free_video_v2(g_Index::g_NDIReceiver, &video_frame);
                    continue;
                }

                cv::Mat frame(video_frame.yres, video_frame.xres, CV_8UC4,
                    video_frame.p_data, video_frame.line_stride_in_bytes);

                if (!frame.empty()) {
                    cv::cvtColor(frame, frameBGR, cv::COLOR_BGRA2BGR);
                    UpdateNDIFrameReceived();

                    if (g_Index::assistance_enabled) {
                        ProcessFrame(frameBGR);
                    }

                    if (!newFrameAvailable.load()) {
                        cv::Mat display = frameBGR.clone();

                        const int cx = display.cols >> 1;
                        const int cy = display.rows >> 1;
                        cv::circle(display, cv::Point(cx, cy), g_Index::kFOV, cv::Scalar(255, 255, 0), 2);
                        cv::line(display, cv::Point(cx - 10, cy), cv::Point(cx + 10, cy), cv::Scalar(0, 0, 255), 2);
                        cv::line(display, cv::Point(cx, cy - 10), cv::Point(cx, cy + 10), cv::Scalar(0, 0, 255), 2);

                        cv::circle(display, cv::Point(cx, cy), static_cast<int>(DEADZONE_RADIUS), cv::Scalar(255, 0, 0), 1);

                        if (hasValidTarget) {
                            cv::Point targetPoint(cx + static_cast<int>(currentTarget.x),
                                cy + static_cast<int>(currentTarget.y));
                            cv::Scalar color = isOnTarget ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 255, 255);
                            cv::circle(display, targetPoint, 5, color, -1);
                        }

                        {
                            std::lock_guard<std::mutex> lock(frameMutex);
                            sharedFrame = std::move(display);
                            newFrameAvailable.store(true);
                        }
                        frameCV.notify_one();
                    }
                }

                NDIlib_recv_free_video_v2(g_Index::g_NDIReceiver, &video_frame);
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    void GUI_Loop() {
        cv::namedWindow("cv2 vision", cv::WINDOW_AUTOSIZE);
        cv::Mat displayFrame;
        cv::Mat placeholder(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
        cv::putText(placeholder, "Waiting for NDI...", cv::Point(200, 240),
            cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(255, 255, 255), 2);

        while (captureThreadRunning.load()) {
            {
                std::unique_lock<std::mutex> lock(frameMutex);
                if (frameCV.wait_for(lock, std::chrono::milliseconds(30),
                    [] { return newFrameAvailable.load(); })) {
                    sharedFrame.copyTo(displayFrame);
                    newFrameAvailable.store(false);
                }
            }

            if (!displayFrame.empty()) {
                cv::imshow("cv2 vision", displayFrame);
            }
            else {
                cv::imshow("cv2 vision", placeholder);
            }

            int key = cv::waitKey(1);
            if (key == 27) {
                captureThreadRunning.store(false);
                break;
            }
        }

        cv::destroyAllWindows();
    }

    void Run() {
        captureThreadRunning.store(true);
        std::thread ndiThread(NDI_CaptureLoop);
        std::thread guiThread(GUI_Loop);
        ndiThread.detach();
        guiThread.detach();
    }

    void Shutdown() {
        captureThreadRunning.store(false);
        frameCV.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void SetBindState(bool held) {
        isBindHeld = held;
        if (!held) {
            hasValidTarget = false;
            currentTarget = Vector2(0, 0);
            stableFrames = 0;
            isOnTarget = false;
            onTargetFrames = 0;
        }
    }

    bool GetBindState() {
        return isBindHeld;
    }
}