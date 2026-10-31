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
#include <algorithm>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

extern void UpdateNDIFrameReceived();
extern std::atomic<bool> g_NDIConnectionHealthy;

namespace g_AimAssist {
    cv::Scalar LOWER_COLOR;
    cv::Scalar UPPER_COLOR;

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

    alignas(64) static uint8_t purpleLUT[256][256][256];
    static std::once_flag lutInitFlag;

    std::atomic<bool> isBindHeld{ false };
    int debugPixelCount = 0;
    double debugTargetX = 0.0;
    double debugTargetY = 0.0;
    bool debugHasTarget = false;

    std::atomic<bool> captureThreadRunning{ true };
    cv::Mat sharedFrame;
    std::mutex frameMutex;
    std::condition_variable frameCV;
    std::atomic<bool> newFrameAvailable{ false };

    std::mutex targetStateMutex;
    Vector2 currentTarget(0, 0);
    Vector2 previousTarget(0, 0);
    Vector2 smoothedRawTarget(0, 0);
    Vector2 targetVelocity(0, 0);
    Vector2 activeAimError(999.0, 999.0);
    bool crosshairInsideEnemy = false;
    std::atomic<bool> triggerFiringLock{ false };
    std::chrono::steady_clock::time_point lastShotTimestamp;
    bool hasSmoothedRaw = false;
    double remainderX = 0.0;
    double remainderY = 0.0;
    bool hasValidTarget = false;
    bool isOnTarget = false;
    int stableFrames = 0;
    int onTargetFrames = 0;

    constexpr double UPDATE_RATE = 144.0;
    constexpr double TARGET_JUMP_THRESHOLD = 150.0;
    constexpr int STABILITY_FRAMES = 2;
    constexpr double ON_TARGET_THRESHOLD = 8.0;
    constexpr int ON_TARGET_FRAMES_REQUIRED = 3;
    constexpr int MAX_STEP_PER_FRAME = 60;

    struct HitboxOffset {
        int x, y;
    };

    const HitboxOffset HITBOX_OFFSETS[] = {
        {0, 0},  // HEAD: 眉心/臉部
        {0, 5},  // NECK: 頸部/下巴
        {0, 12}  // BODY: 身體
    };

    void InitializePurpleLUT() {
        int totalPurple = 0;
        for (int r = 0; r < 256; ++r) {
            for (int g = 0; g < 256; ++g) {
                for (int b = 0; b < 256; ++b) {
                    float rf = r / 255.0f;
                    float gf = g / 255.0f;
                    float bf = b / 255.0f;
                    float maxRGB = (std::max)({ rf, gf, bf });
                    float minRGB = (std::min)({ rf, gf, bf });
                    float delta = maxRGB - minRGB;

                    float h = 0.0f;
                    if (delta > 0.0001f) {
                        if (maxRGB == rf) {
                            h = 60.0f * (gf - bf) / delta;
                            if (h < 0.0f) h += 360.0f;
                        }
                        else if (maxRGB == gf) {
                            h = 60.0f * (bf - rf) / delta + 120.0f;
                        }
                        else {
                            h = 60.0f * (rf - gf) / delta + 240.0f;
                        }
                    }

                    float s = (maxRGB > 0.0001f) ? (delta / maxRGB) * 255.0f : 0.0f;
                    float v = maxRGB * 255.0f;
                    float h_half = h * 0.5f;

                    // 嚴格的 Valorant Tritanopia 敵人紫色色彩過濾
                    bool isPurple = (h_half >= 135.0f && h_half <= 168.0f) &&
                                    (s >= 70.0f) && (v >= 90.0f) &&
                                    (r > g + 15) && (b > g + 15) &&
                                    ((b - r) <= 22);

                    purpleLUT[r][g][b] = isPurple;
                    if (isPurple) totalPurple++;
                }
            }
        }
        timelog("[AimAssist] Optimized Tritanopia Purple LUT initialized. Matching RGB count: %d", totalPurple);
    }

    __forceinline bool IsPurpleColorFast(unsigned char r, unsigned char g, unsigned char b) {
        return purpleLUT[r][g][b] != 0;
    }

    bool IsPurpleColor(unsigned char r, unsigned char g, unsigned char b) {
        std::call_once(lutInitFlag, InitializePurpleLUT);
        return purpleLUT[r][g][b] != 0;
    }

    // 支援直接讀取 CreateDIBSection 零拷貝 BGRA 緩衝區 (4-byte aligned, 無 cvtColor 轉換開銷)
    Vector2 FindTargetCentroid(const cv::Mat& frameBGRA) {
        std::call_once(lutInitFlag, InitializePurpleLUT);

        const int height = frameBGRA.rows;
        const int width = frameBGRA.cols;
        const int centerX = width >> 1;
        const int centerY = height >> 1;

        const int fovRadius = (std::max)(5, (std::min)(static_cast<int>(g_Index::kFOV), width / 2));
        const int fovRadiusSq = fovRadius * fovRadius;

        // 全螢幕擷取框範圍 (256x256 全域掃描)：
        // 核心根治「準星指在低處時，敵人頭部剛好被 FOV 頂端切掉」導致下半身變成該切片內的最高點而鎖在腰腿的致命 Bug！
        const int minX = 0;
        const int maxX = width;
        const int minY = 0;
        const int maxY = height;

        static cv::Mat binaryMask(256, 256, CV_8UC1, cv::Scalar(0));
        static cv::Mat dilatedMask(256, 256, CV_8UC1, cv::Scalar(0));

        if (binaryMask.rows != height || binaryMask.cols != width) {
            binaryMask = cv::Mat::zeros(height, width, CV_8UC1);
            dilatedMask = cv::Mat::zeros(height, width, CV_8UC1);
        }
        binaryMask.setTo(0);

        int totalRawPurple = 0;
        int activeMinX = width, activeMaxX = 0;
        int activeMinY = height, activeMaxY = 0;

        for (int y = 0; y < height; y++) {
            const uint32_t* row = frameBGRA.ptr<uint32_t>(y);
            uchar* maskRow = binaryMask.ptr<uchar>(y);

            for (int x = 0; x < width; x++) {
                uint32_t px = row[x]; // BGRA layout: B=px&0xFF, G=(px>>8)&0xFF, R=(px>>16)&0xFF
                if (purpleLUT[(px >> 16) & 0xFF][(px >> 8) & 0xFF][px & 0xFF]) {
                    maskRow[x] = 255;
                    totalRawPurple++;
                    if (x < activeMinX) activeMinX = x;
                    if (x > activeMaxX) activeMaxX = x;
                    if (y < activeMinY) activeMinY = y;
                    if (y > activeMaxY) activeMaxY = y;
                }
            }
        }

        debugPixelCount = totalRawPurple;
        crosshairInsideEnemy = false;

        if (totalRawPurple == 0) {
            hasValidTarget = false;
            debugHasTarget = false;
            hasSmoothedRaw = false;
            targetVelocity = Vector2(0, 0);
            activeAimError = Vector2(999.0, 999.0);
            return Vector2(0, 0);
        }

        // 2. 子區域形態學膨脹 (使用 3x7 直立核，強力貫穿胸口空白，把頭肩與腰腿無縫黏合成同一人體連通域)
        static cv::Mat morphKernelVert = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 7));
        int roiX0 = (std::max)(0, activeMinX - 6);
        int roiY0 = (std::max)(0, activeMinY - 10);
        int roiX1 = (std::min)(width, activeMaxX + 7);
        int roiY1 = (std::min)(height, activeMaxY + 11);
        cv::Rect activeROI(roiX0, roiY0, roiX1 - roiX0, roiY1 - roiY0);

        cv::Mat subMask = binaryMask(activeROI);
        cv::Mat subDilated = dilatedMask(activeROI);
        cv::dilate(subMask, subDilated, morphKernelVert, cv::Point(-1, -1), 3);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(subDilated, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE, cv::Point(roiX0, roiY0));

        if (contours.empty()) {
            hasValidTarget = false;
            debugHasTarget = false;
            hasSmoothedRaw = false;
            return Vector2(0, 0);
        }

        // 3A. 提取所有連通域真實邊界，並執行「BlueFire 由上而下垂直柱狀歸併 (Top-Down Column Dominance)」：
        // 徹底根除胸口無紫框導致下半身 Contour 獨立、在準星偏低時下半身搶奪目標而「鎖腿/鎖胯」的致命缺陷！
        struct RawCluster {
            int x0 = 0, x1 = 0;
            int realTopY = INT_MAX, realBottomY = 0;
            int realCount = 0;
            bool absorbed = false;
        };

        std::vector<RawCluster> clusters;
        clusters.reserve(contours.size());

        for (const auto& contour : contours) {
            double area = cv::contourArea(contour);
            if (area < 8.0) continue;

            cv::Rect bbox = cv::boundingRect(contour);
            int x0 = (std::max)(bbox.x, minX);
            int x1 = (std::min)(bbox.x + bbox.width, maxX);
            int y0 = (std::max)(bbox.y, minY);
            int y1 = (std::min)(bbox.y + bbox.height, maxY);

            int rMinX = INT_MAX, rMaxX = -1;
            int rTopY = INT_MAX, rBotY = 0;
            int rCount = 0;

            for (int y = y0; y < y1; ++y) {
                const uchar* mRow = binaryMask.ptr<uchar>(y);
                for (int x = x0; x < x1; ++x) {
                    if (mRow[x]) {
                        if (x < rMinX) rMinX = x;
                        if (x > rMaxX) rMaxX = x;
                        if (y < rTopY) rTopY = y;
                        if (y > rBotY) rBotY = y;
                        rCount++;
                    }
                }
            }

            if (rCount < 2 || rTopY == INT_MAX) continue;
            clusters.push_back({ rMinX, rMaxX + 1, rTopY, rBotY, rCount, false });
        }

        if (clusters.empty()) {
            hasValidTarget = false;
            debugHasTarget = false;
            hasSmoothedRaw = false;
            return Vector2(0, 0);
        }

        // 依最高點 realTopY 嚴格由上往下排序 (頭部永遠排在最前方)
        std::sort(clusters.begin(), clusters.end(), [](const RawCluster& a, const RawCluster& b) {
            return a.realTopY < b.realTopY;
        });

        // 垂直柱狀合併：若下方 Contour 與上方主體的水平中軸對齊，100% 判定為同一名敵人的腰/腿，直接強制合併！
        for (size_t i = 0; i < clusters.size(); ++i) {
            if (clusters[i].absorbed) continue;
            for (size_t j = i + 1; j < clusters.size(); ++j) {
                if (clusters[j].absorbed) continue;

                double topMidX = 0.5 * (clusters[i].x0 + clusters[i].x1);
                double subMidX = 0.5 * (clusters[j].x0 + clusters[j].x1);
                double topW = static_cast<double>(clusters[i].x1 - clusters[i].x0);
                double subW = static_cast<double>(clusters[j].x1 - clusters[j].x0);

                int overlapX = (std::min)(clusters[i].x1, clusters[j].x1) - (std::max)(clusters[i].x0, clusters[j].x0);
                double centerDiffX = std::abs(topMidX - subMidX);
                double maxHorizDist = (std::max)(18.0, 0.60 * (std::max)(topW, subW));
                int vertGap = clusters[j].realTopY - clusters[i].realBottomY;

                if ((overlapX >= -6 || centerDiffX <= maxHorizDist) && vertGap < 70) {
                    clusters[i].x0 = (std::min)(clusters[i].x0, clusters[j].x0);
                    clusters[i].x1 = (std::max)(clusters[i].x1, clusters[j].x1);
                    clusters[i].realBottomY = (std::max)(clusters[i].realBottomY, clusters[j].realBottomY);
                    clusters[i].realCount += clusters[j].realCount;
                    clusters[j].absorbed = true; // 銷毀下半身獨立身分，禁止搶奪鎖定！
                }
            }
        }

        // 3B. 結合兩階段解剖學頭部定位 (Head Localization)：
        struct CandidateTarget {
            Vector2 headRelPos;
            double distToCenter;
            bool valid = false;
        };

        CandidateTarget bestCandidate;
        bestCandidate.distToCenter = 1e9;
        CandidateTarget lockedCandidate;
        lockedCandidate.distToCenter = 1e9;
        double minDistToPrev = 1e9;

        for (const auto& cl : clusters) {
            if (cl.absorbed || cl.realCount < 2) continue;

            int x0 = cl.x0;
            int x1 = cl.x1;
            int realTopY = cl.realTopY;
            int realBottomY = cl.realBottomY;
            double boxHeight = static_cast<double>(realBottomY - realTopY + 1);

            // 階段 1：頭頂錨點 CrownX (取最高 3px)
            int crownMaxY = (std::min)(realBottomY, realTopY + 3);
            double sumCrownX = 0.0;
            int crownCount = 0;
            for (int y = realTopY; y <= crownMaxY; ++y) {
                const uchar* mRow = binaryMask.ptr<uchar>(y);
                for (int x = x0; x < x1; ++x) {
                    if (mRow[x]) {
                        sumCrownX += x;
                        crownCount++;
                    }
                }
            }
            if (crownCount == 0) continue;
            double crownX = sumCrownX / static_cast<double>(crownCount);

            // 階段 2：解剖學頭寬窗格過濾 (裁切掉肩膀與外張手肘)
            int headBand = clamp(static_cast<int>(boxHeight * 0.12), 4, 12);
            int bandMaxY = (std::min)(realBottomY, realTopY + headBand);
            int maxHeadHalfW = clamp(static_cast<int>(headBand * 1.15), 6, 14);
            int headMinX = (std::max)(x0, static_cast<int>(std::lround(crownX)) - maxHeadHalfW);
            int headMaxX = (std::min)(x1, static_cast<int>(std::lround(crownX)) + maxHeadHalfW + 1);

            double sumMidX = 0.0;
            double sumRowWeight = 0.0;
            double sumMassX = 0.0;
            int headMassCount = 0;
            int validHeadRows = 0;

            for (int y = realTopY; y <= bandMaxY; ++y) {
                const uchar* mRow = binaryMask.ptr<uchar>(y);
                int rMinX = INT_MAX;
                int rMaxX = -1;
                for (int x = headMinX; x < headMaxX; ++x) {
                    if (mRow[x]) {
                        if (x < rMinX) rMinX = x;
                        if (x > rMaxX) rMaxX = x;
                        sumMassX += x;
                        headMassCount++;
                    }
                }
                if (rMaxX >= rMinX) {
                    double rowMidX = 0.5 * static_cast<double>(rMinX + rMaxX);
                    double weight = (y == realTopY) ? 0.8 : 1.2;
                    sumMidX += rowMidX * weight;
                    sumRowWeight += weight;
                    validHeadRows++;
                }
            }

            if (validHeadRows == 0 || sumRowWeight <= 0.0 || headMassCount == 0) continue;

            double midHeadX = sumMidX / sumRowWeight;
            double massHeadX = sumMassX / static_cast<double>(headMassCount);
            double headX = 0.70 * midHeadX + 0.30 * massHeadX;
            double headDrop = clamp(boxHeight * 0.055, 2.5, 6.5);
            double headY = static_cast<double>(realTopY) + headDrop;

            Vector2 relPos(headX - centerX, headY - centerY);

            // 檢查該敵人實體（頭、軀幹或雙腿）是否有任何部分交疊在當前 FOV 圓圈內：
            // 只要目標身體任何一部分在 FOV 內，即直接鎖定該敵人完完整整的「絕對頭部 (100% True Head)」，零模糊空間！
            double enemyClosestX = clamp(static_cast<double>(centerX), static_cast<double>(cl.x0), static_cast<double>(cl.x1));
            double enemyClosestY = clamp(static_cast<double>(centerY), static_cast<double>(cl.realTopY), static_cast<double>(cl.realBottomY));
            double dxToEnemy = enemyClosestX - centerX;
            double dyToEnemy = enemyClosestY - centerY;
            if (dxToEnemy * dxToEnemy + dyToEnemy * dyToEnemy > fovRadiusSq) {
                continue; // 該敵人完全在 FOV 圓圈外，跳過
            }

            // 絕對頭部鎖定 (Absolute Head Selection)：
            // 依照離準心最近的判定：由準星到該敵人絕對真頭部 (headX, headY) 的歐氏距離決定！
            double headDist = relPos.Len();

            if (headDist < bestCandidate.distToCenter) {
                bestCandidate.distToCenter = headDist;
                bestCandidate.headRelPos = relPos;
                bestCandidate.valid = true;
            }

            if (hasSmoothedRaw) {
                double distToPrev = (relPos - smoothedRawTarget).Len();
                if (distToPrev < minDistToPrev) {
                    minDistToPrev = distToPrev;
                    lockedCandidate.headRelPos = relPos;
                    lockedCandidate.distToCenter = headDist;
                    lockedCandidate.valid = true;
                }
            }
        }

        if (!bestCandidate.valid) {
            hasValidTarget = false;
            debugHasTarget = false;
            hasSmoothedRaw = false;
            activeAimError = Vector2(999.0, 999.0);
            return Vector2(0, 0);
        }

        // 檢測準星 (centerX, centerY) 是否位於敵人左右紫色外框包夾的實體內部 (解決開鏡/近距離時角色臉部與胸口中間沒有紫色像素的問題)
        {
            int scanRadiusX = 36;
            int scanRadiusY = 2;
            bool hasLeftEdge = false;
            bool hasRightEdge = false;
            bool hasTopCap = false;

            for (int sy = (std::max)(minY, centerY - scanRadiusY); sy <= (std::min)(maxY - 1, centerY + scanRadiusY); ++sy) {
                const uchar* mRow = binaryMask.ptr<uchar>(sy);
                for (int sx = (std::max)(minX, centerX - scanRadiusX); sx <= centerX; ++sx) {
                    if (mRow[sx]) { hasLeftEdge = true; break; }
                }
                for (int sx = centerX; sx <= (std::min)(maxX - 1, centerX + scanRadiusX); ++sx) {
                    if (mRow[sx]) { hasRightEdge = true; break; }
                }
            }
            for (int sy = (std::max)(minY, centerY - 45); sy <= centerY; ++sy) {
                const uchar* mRow = binaryMask.ptr<uchar>(sy);
                for (int sx = (std::max)(minX, centerX - 12); sx <= (std::min)(maxX - 1, centerX + 12); ++sx) {
                    if (mRow[sx]) { hasTopCap = true; break; }
                }
                if (hasTopCap) break;
            }
            crosshairInsideEnemy = (hasLeftEdge && hasRightEdge && hasTopCap);
        }

        Vector2 rawCandidate = bestCandidate.headRelPos;

        // 4. 移植 jsonmeister/color-aimbot 多人同框防跳滯後保護 (Hysteresis) 與目標黏滯期：
        static int targetSwitchFrames = 0;
        constexpr int PERSISTENCE_FRAMES_REQUIRED = 4; // 4 幀黏滯確認期 (~27ms)

        if (hasSmoothedRaw && lockedCandidate.valid) {
            double jumpDist = (rawCandidate - smoothedRawTarget).Len();
            if (jumpDist > 30.0 && minDistToPrev < 25.0 && lockedCandidate.distToCenter * 0.80 <= bestCandidate.distToCenter) {
                rawCandidate = lockedCandidate.headRelPos;
            }
            else if (jumpDist > 12.0) {
                bool significantlyCloser = (bestCandidate.distToCenter < lockedCandidate.distToCenter * 0.70);
                if (!significantlyCloser) {
                    targetSwitchFrames++;
                    if (targetSwitchFrames < PERSISTENCE_FRAMES_REQUIRED) {
                        rawCandidate = lockedCandidate.headRelPos;
                    }
                } else {
                    targetSwitchFrames = 0;
                }
            } else {
                targetSwitchFrames = 0;
            }
        } else {
            targetSwitchFrames = 0;
        }

        // 5. 真零抖動硬體鎖死死區 + 移動敵人速度前饋補償 (Hard Deadband + Velocity Feed-Forward，受 GUI 死區動態調控)：
        if (!hasSmoothedRaw) {
            smoothedRawTarget = rawCandidate;
            targetVelocity = Vector2(0, 0);
            hasSmoothedRaw = true;
        } else {
            Vector2 instVel = rawCandidate - smoothedRawTarget;
            double delta = instVel.Len();
            double deadbandThresh = static_cast<double>(g_Index::aim_deadzone) * 0.6; // 預設 2.5 * 0.6 = 1.5px
            if (g_Index::aim_deadzone > 0.05f && delta <= deadbandThresh) {
                // 硬體級靜止死區：白點座標 100% 鎖定不動，速度衰減
                targetVelocity = targetVelocity * 0.5;
            } else if (g_Index::aim_deadzone > 0.05f && delta < deadbandThresh * 3.0) {
                double alpha = (delta - deadbandThresh) / (deadbandThresh * 2.0);
                Vector2 prev = smoothedRawTarget;
                smoothedRawTarget = smoothedRawTarget + instVel * alpha;
                targetVelocity = targetVelocity * 0.6 + (smoothedRawTarget - prev) * 0.4;
            } else {
                // 高速移動或瞬間拉槍：100% 零延遲跟隨並更新移動速度向量
                Vector2 prev = smoothedRawTarget;
                smoothedRawTarget = rawCandidate;
                if (delta < 35.0) {
                    targetVelocity = targetVelocity * 0.5 + (smoothedRawTarget - prev) * 0.5;
                } else {
                    targetVelocity = Vector2(0, 0);
                }
            }
        }

        hasValidTarget = true;
        debugHasTarget = true;
        debugTargetX = smoothedRawTarget.x;
        debugTargetY = smoothedRawTarget.y;

        return smoothedRawTarget;
    }

    Vector2 ApplyHitboxOffset(const Vector2& target) {
        if (!hasValidTarget) return target;

        int hitboxIndex = clamp(g_Index::hitbox, 0, 2);
        const auto& baseOffset = HITBOX_OFFSETS[hitboxIndex];

        // 擬人化生理有機漫步 (Humanizer Organic Wander)
        static double humanWanderX = 0.0;
        static double humanWanderY = 0.0;
        static int wanderTick = 0;

        if (g_Index::aim_humanizer_enabled) {
            if (++wanderTick >= 8) { // 每 8 幀 (~55ms) 產生柔和的微動，模擬真人手部肌肉呼吸浮動
                wanderTick = 0;
                double angle = ((rand() % 360) * 3.141592653589793) / 180.0;
                double mag = ((rand() % 100) / 100.0) * static_cast<double>(g_Index::aim_humanizer_strength);
                humanWanderX = std::cos(angle) * mag;
                humanWanderY = std::sin(angle) * mag;
            }
        } else {
            humanWanderX = 0.0;
            humanWanderY = 0.0;
            wanderTick = 0;
        }

        return Vector2(target.x + baseOffset.x + g_Index::aim_x_offset + humanWanderX,
                       target.y + baseOffset.y + g_Index::aim_y_offset + humanWanderY);
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

        if (isOnTarget && distance <= static_cast<double>(g_Index::aim_deadzone)) {
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

        // 工業界標準步長平滑 (Smooth Divisor)：
        // 將 Smoothing 真正轉化為調控「位移步長與到達目標之幀數」，體感極度顯著：
        // 滑桿 1~5: 極速瞬移拉槍 (1~2 幀暴力拉頭)
        // 滑桿 15~25: 自然真人手臂拉槍 (約 100~150ms 勻速自然吸附)
        // 滑桿 40~80: 極度柔順慢速輔助瞄準
        double smoothDivisor = 1.0 + (static_cast<double>(g_Index::kSmoothingFactor) * 0.35);

        // 擬人化肌肉神經非線性擾動 (Humanizer Speed Noise)
        if (g_Index::aim_humanizer_enabled) {
            double humanNoise = 0.94 + ((rand() % 120) / 1000.0); // ±6% 隨機波動，破壞機械直線
            smoothDivisor *= humanNoise;
        }

        // 進圈動態阻尼煞車 (Error-scaled Dynamic Braking)：
        // 進入頭部周圍 (<16px) 時等比平滑制動，徹底根治由下往上甩槍衝破頭頂的慣性過衝
        double settleDamping = 1.0;
        if (distance < 16.0) {
            settleDamping = 0.45 + 0.55 * (distance / 16.0);
        }

        // 微分阻尼 (D-term 速度煞車)：
        // 偵測拉槍瞬時速度，在高速接近目標時自動產生反向阻尼，徹底吸收動能，防止 Overshoot
        Vector2 errorDelta = targetPos - previousTarget;
        previousTarget = targetPos;
        double dGain = 0.12;

        double stepX = (targetPos.x / smoothDivisor) * settleDamping - (errorDelta.x * dGain);
        double stepY = (targetPos.y / smoothDivisor) * settleDamping - (errorDelta.y * dGain);

        currentTarget = Vector2(stepX, stepY);
        return currentTarget;
    }

    void ExecuteMovement(const Vector2& stepDelta) {
        double distance = stepDelta.Len();
        if (distance < 0.05) return;

        // 結合 X/Y 軸獨立靈敏度輸出最終物理像素位移 (stepDelta 已包含 Smoothing 步長除數與進圈阻尼煞車)
        double moveX = stepDelta.x * (g_Index::kSensitivityX * 2.2);
        double moveY = stepDelta.y * (g_Index::kSensitivityY * 2.2);

        remainderX += moveX;
        remainderY += moveY;

        int rawStepX = static_cast<int>(std::lround(remainderX));
        int rawStepY = static_cast<int>(std::lround(remainderY));

        int stepX = clamp(rawStepX, -MAX_STEP_PER_FRAME, MAX_STEP_PER_FRAME);
        int stepY = clamp(rawStepY, -MAX_STEP_PER_FRAME, MAX_STEP_PER_FRAME);

        remainderX -= stepX;
        remainderY -= stepY;

        if (stepX != 0 || stepY != 0) {
            // 直接發送 km.move_now，消除 Makcu 韌體佇列延遲，實現 144 FPS 極速響應
            g_Index::g_makcuDevice.mouseMove(stepX, stepY);
        }
    }

    void ProcessFrame(const cv::Mat& frame) {
        std::lock_guard<std::mutex> lock(targetStateMutex);
        Vector2 rawTarget = FindTargetCentroid(frame);

        if (hasValidTarget) {
            activeAimError = ApplyHitboxOffset(rawTarget);
        } else {
            activeAimError = Vector2(999.0, 999.0);
        }

        if (!isBindHeld.load() || !g_Index::assistance_enabled) {
            currentTarget = Vector2(0, 0);
            remainderX = 0.0;
            remainderY = 0.0;
            isOnTarget = false;
            onTargetFrames = 0;
            stableFrames = 0;
            return;
        }

        if (!hasValidTarget) {
            remainderX = 0.0;
            remainderY = 0.0;
            isOnTarget = false;
            onTargetFrames = 0;
            return;
        }

        if (g_Index::aim_method == 1) {
            // SILENCE 靜默傳送模式：透過 Makcu km.moveto 直接傳送到螢幕絕對坐標
            double dist = activeAimError.Len();
            if (dist >= static_cast<double>(g_Index::aim_deadzone)) {
                int screenW = GetSystemMetrics(SM_CXSCREEN);
                int screenH = GetSystemMetrics(SM_CYSCREEN);
                int absX = (screenW / 2) + static_cast<int>(std::lround(activeAimError.x));
                int absY = (screenH / 2) + static_cast<int>(std::lround(activeAimError.y));

                g_Index::g_makcuDevice.mouseMoveTo(absX, absY);
            }
        }
        else {
            // MOVE 原版模式：144Hz 步長平滑與進圈阻尼煞車追瞄
            Vector2 smoothedTarget = ApplySmoothing(activeAimError);

            if (smoothedTarget.LenSquared() > 0.05) {
                ExecuteMovement(smoothedTarget);
            }
        }
    }

    bool IsTriggerKeyHeld();

    void NDI_CaptureLoop() {
        const int capW = static_cast<int>(g_Index::kScreenWidth);  // 256
        const int capH = static_cast<int>(g_Index::kScreenHeight); // 256

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        HDC hdcScreen = GetDC(NULL);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = capW;
        bi.bmiHeader.biHeight = -capH; // top-down DIB
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* dibPixels = nullptr;
        HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &dibPixels, NULL, 0);
        HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

        cv::Mat frameBGRA(capH, capW, CV_8UC4, dibPixels);

        timeBeginPeriod(1);

        using clock = std::chrono::steady_clock;
        const auto targetInterval = std::chrono::microseconds(1000000 / 144); // 144 FPS (6.94ms per tick)
        auto lastLogTime = clock::now();

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int capX = (screenW - capW) / 2;
        int capY = (screenH - capH) / 2;
        int metricCheckCounter = 0;

        while (captureThreadRunning.load()) {
            auto startTime = clock::now();

            if (++metricCheckCounter >= 240) {
                metricCheckCounter = 0;
                screenW = GetSystemMetrics(SM_CXSCREEN);
                screenH = GetSystemMetrics(SM_CYSCREEN);
                capX = (screenW - capW) / 2;
                capY = (screenH - capH) / 2;
            }

            if (BitBlt(hdcMem, 0, 0, capW, capH, hdcScreen, capX, capY, SRCCOPY)) {
                GdiFlush();
                UpdateNDIFrameReceived();

                ProcessFrame(frameBGRA);

                // --- RCS (Recoil Control System) 後座力壓槍補償 ---
                static auto lbuttonDownTime = clock::now();
                static bool lbuttonWasHeld = false;
                static double rcsRemainderY = 0.0;
                bool lbuttonHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                if (lbuttonHeld && !lbuttonWasHeld) {
                    lbuttonDownTime = clock::now();
                    lbuttonWasHeld = true;
                    rcsRemainderY = 0.0;
                } else if (!lbuttonHeld) {
                    lbuttonWasHeld = false;
                    rcsRemainderY = 0.0;
                }

                if (g_Index::rcs_enabled && lbuttonHeld) {
                    auto sprayMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - lbuttonDownTime).count();
                    if (sprayMs >= g_Index::rcs_delay_ms) {
                        // 在 144 FPS 下 (每 6.94ms)，持續平穩累積向下壓槍位移
                        rcsRemainderY += (static_cast<double>(g_Index::rcs_strength) * 0.12);
                        int rcsStepY = static_cast<int>(std::lround(rcsRemainderY));
                        if (rcsStepY > 0) {
                            rcsRemainderY -= rcsStepY;
                            if (!g_Index::assistance_enabled || !isBindHeld.load() || !hasValidTarget) {
                                g_Index::g_makcuDevice.mouseMove(0, rcsStepY);
                            } else {
                                std::lock_guard<std::mutex> lock(targetStateMutex);
                                remainderY += rcsStepY;
                            }
                        }
                    }
                }

                // --- Triggerbot 自動開火邏輯 (遵循 Makcu 官方 API km.click) ---
                static auto lastTriggerTime = clock::now();
                static int triggerFlashFrames = 0;
                const int cx = capW >> 1;
                const int cy = capH >> 1;

                if (g_Index::trigger_enabled && IsTriggerKeyHeld()) {
                    int tfov = (std::max)(1, (std::min)(g_Index::trigger_fov, 20));
                    int tfovY = (std::min)(30, static_cast<int>(std::lround(tfov * 1.6)));
                    double invRxSq = 1.0 / static_cast<double>(tfov * tfov);
                    double invRySq = 1.0 / static_cast<double>(tfovY * tfovY);

                    int purpleInCrosshair = 0;
                    for (int ty = cy - tfovY; ty <= cy + tfovY; ++ty) {
                        if (ty < 0 || ty >= capH) continue;
                        const uint32_t* rptr = frameBGRA.ptr<uint32_t>(ty);
                        int dty = ty - cy;
                        double normYSq = (dty * dty) * invRySq;
                        for (int tx = cx - tfov; tx <= cx + tfov; ++tx) {
                            if (tx < 0 || tx >= capW) continue;
                            int dtx = tx - cx;
                            if ((dtx * dtx) * invRxSq + normYSq <= 1.0) {
                                uint32_t px = rptr[tx];
                                if (IsPurpleColorFast((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF)) {
                                    purpleInCrosshair++;
                                }
                            }
                        }
                    }

                    bool outlineHit = (purpleInCrosshair >= 2);
                    double aimErrX = std::abs(activeAimError.x);
                    double aimErrY = std::abs(activeAimError.y);
                    bool aimLockedHit = hasValidTarget && (
                        (aimErrX <= tfov + 1.5 && aimErrY <= tfovY + 1.5) ||
                        (crosshairInsideEnemy && aimErrX <= tfov + 3.5 && aimErrY <= tfovY + 4.0)
                    );

                    bool aimFlickGatePass = true;
                    if (g_Index::assistance_enabled && isBindHeld.load() && hasValidTarget) {
                        double maxAllowedAimDist = (std::max)(6.5, static_cast<double>(tfov) * 2.2);
                        if (activeAimError.Len() > maxAllowedAimDist) {
                            aimFlickGatePass = false;
                        }
                    }

                    auto now = clock::now();
                    auto cooldownDur = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTriggerTime).count();

                    if ((outlineHit || aimLockedHit) && aimFlickGatePass && !triggerFiringLock.load() && cooldownDur >= g_Index::trigger_cooldown_ms) {
                        lastTriggerTime = now;
                        lastShotTimestamp = now;
                        triggerFlashFrames = 12;

                        int delayMs = g_Index::trigger_delay_ms;
                        int holdMs = g_Index::trigger_click_duration;
                        if (delayMs <= 0 && g_Index::g_makcuDevice.isUdpMode()) {
                            g_Index::g_makcuDevice.click(1, 1, holdMs);
                        } else {
                            triggerFiringLock.store(true);
                            std::thread([delayMs, holdMs]() {
                                if (delayMs > 0) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                                }
                                if (!g_Index::assistance_enabled || !isBindHeld.load() || activeAimError.Len() <= 14.0 || crosshairInsideEnemy) {
                                    g_Index::g_makcuDevice.click(1, 1, holdMs);
                                }
                                triggerFiringLock.store(false);
                            }).detach();
                        }
                    }
                }

                if (g_Index::showDebug && std::chrono::duration_cast<std::chrono::seconds>(startTime - lastLogTime).count() >= 3) {
                    lastLogTime = startTime;
                    timelog("[Debug Status] Capture: 144 FPS | Assistance: %s | BindKey Held: %s | Purple Px: %d | Target: %s",
                        g_Index::assistance_enabled ? "ON" : "OFF",
                        isBindHeld.load() ? "YES" : "NO",
                        debugPixelCount,
                        hasValidTarget ? "LOCKED" : "SEARCHING");
                }

                if (g_Index::showDebug && !newFrameAvailable.load()) {
                    cv::Mat display;
                    if (g_Index::show_mask_view) {
                        display = cv::Mat::zeros(capH, capW, CV_8UC3);
                        for (int y = 0; y < capH; ++y) {
                            const uint32_t* srcRow = frameBGRA.ptr<uint32_t>(y);
                            cv::Vec3b* dstRow = display.ptr<cv::Vec3b>(y);
                            for (int x = 0; x < capW; ++x) {
                                uint32_t px = srcRow[x];
                                if (IsPurpleColorFast((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF)) {
                                    dstRow[x] = cv::Vec3b(255, 255, 255);
                                }
                            }
                        }
                    }
                    else {
                        cv::cvtColor(frameBGRA, display, cv::COLOR_BGRA2BGR);
                    }

                    cv::Scalar fovColor = isBindHeld.load() ? cv::Scalar(0, 255, 255) : cv::Scalar(255, 255, 0);
                    cv::circle(display, cv::Point(cx, cy), g_Index::kFOV, fovColor, 1);

                    cv::line(display, cv::Point(cx - 8, cy), cv::Point(cx + 8, cy), cv::Scalar(0, 0, 255), 1);
                    cv::line(display, cv::Point(cx, cy - 8), cv::Point(cx, cy + 8), cv::Scalar(0, 0, 255), 1);

                    if (g_Index::trigger_enabled || g_Index::showDebug) {
                        cv::Scalar trigColor;
                        int trigThickness = 1;

                        if (triggerFlashFrames > 0) {
                            trigColor = cv::Scalar(0, 0, 255);
                            trigThickness = 2;
                            triggerFlashFrames--;
                            cv::putText(display, "FIRE!", cv::Point(cx + g_Index::trigger_fov + 4, cy - 4), cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 255), 2);
                        }
                        else if (g_Index::trigger_enabled) {
                            trigColor = cv::Scalar(255, 0, 255);
                        }
                        else {
                            trigColor = cv::Scalar(90, 90, 90);
                        }

                        cv::circle(display, cv::Point(cx, cy), g_Index::trigger_fov, trigColor, trigThickness);
                    }

                    if (hasValidTarget) {
                        Vector2 offsetTarget = ApplyHitboxOffset(Vector2(debugTargetX, debugTargetY));
                        cv::Point targetPoint(cx + static_cast<int>(offsetTarget.x),
                            cy + static_cast<int>(offsetTarget.y));
                        cv::Scalar targetColor = isBindHeld.load() ? cv::Scalar(0, 255, 0) : cv::Scalar(255, 255, 255);
                        cv::circle(display, targetPoint, 4, targetColor, -1);
                        cv::line(display, cv::Point(cx, cy), targetPoint, targetColor, 1);
                    }

                    if (g_Index::showDebug) {
                        std::string hudAssist = std::string("AIM: ") + (g_Index::assistance_enabled ? "ON" : "OFF") + " [" + (g_Index::aim_method == 1 ? "SILENCE" : "MOVE") + "]";
                        std::string hudBind = std::string("KEY HELD: ") + (isBindHeld.load() ? "YES" : "NO");
                        std::string hudTrigger = std::string("TRIGGER: ") + (g_Index::trigger_enabled ? "ON" : "OFF") + " [FOV: " + std::to_string(g_Index::trigger_fov) + "px]";
                        std::string hudPixels = "PURPLE PX: " + std::to_string(debugPixelCount);
                        std::string hudTarget = hasValidTarget ? ("TARGET: " + std::to_string(static_cast<int>(debugTargetX)) + "," + std::to_string(static_cast<int>(debugTargetY))) : "TARGET: NONE";

                        cv::putText(display, hudAssist, cv::Point(5, 15), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            g_Index::assistance_enabled ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255), 1);
                        cv::putText(display, hudBind, cv::Point(5, 30), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            isBindHeld.load() ? cv::Scalar(0, 255, 0) : cv::Scalar(200, 200, 200), 1);
                        cv::putText(display, hudTrigger, cv::Point(5, 45), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            g_Index::trigger_enabled ? cv::Scalar(255, 0, 255) : cv::Scalar(150, 150, 150), 1);
                        cv::putText(display, hudPixels, cv::Point(5, 60), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            debugPixelCount > 0 ? cv::Scalar(255, 0, 255) : cv::Scalar(150, 150, 150), 1);
                        cv::putText(display, hudTarget, cv::Point(5, 75), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            hasValidTarget ? cv::Scalar(0, 255, 255) : cv::Scalar(150, 150, 150), 1);

                        if (g_Index::show_mask_view) {
                            cv::putText(display, "[COLOR FILTER MASK VIEW]", cv::Point(5, 90), cv::FONT_HERSHEY_SIMPLEX, 0.38, cv::Scalar(0, 255, 255), 1);
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(frameMutex);
                        sharedFrame = std::move(display);
                        newFrameAvailable.store(true);
                    }
                    frameCV.notify_one();
                }
            }

            auto elapsed = clock::now() - startTime;
            if (elapsed < targetInterval) {
                std::this_thread::sleep_for(targetInterval - elapsed);
            }
        }

        SelectObject(hdcMem, oldBm);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);

        timeEndPeriod(1);
    }

    void GUI_Loop() {
        bool windowOpen = false;
        cv::Mat displayFrame;

        while (captureThreadRunning.load()) {
            if (!g_Index::showDebug) {
                if (windowOpen) {
                    cv::destroyAllWindows();
                    cv::waitKey(1);
                    windowOpen = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (!windowOpen) {
                cv::namedWindow("cv2 vision", cv::WINDOW_AUTOSIZE);
                cv::setWindowProperty("cv2 vision", cv::WND_PROP_TOPMOST, 1);
                HWND cvWnd = FindWindowA(NULL, "cv2 vision");
                if (cvWnd) {
                    SetWindowPos(cvWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                }
                windowOpen = true;
            }

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
                static int topMostCheck = 0;
                if (++topMostCheck % 30 == 0) {
                    HWND cvWnd = FindWindowA(NULL, "cv2 vision");
                    if (cvWnd) {
                        SetWindowPos(cvWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                }
            }

            int key = cv::waitKey(1);
            if (key == 27) {
                g_Index::showDebug = false;
                cv::destroyAllWindows();
                cv::waitKey(1);
                windowOpen = false;
                HWND consoleWnd = GetConsoleWindow();
                if (consoleWnd) ShowWindow(consoleWnd, SW_HIDE);
            }
        }

        if (windowOpen) {
            cv::destroyAllWindows();
            cv::waitKey(1);
        }
    }

    void Run() {
        captureThreadRunning.store(true);
        std::thread captureThread(NDI_CaptureLoop);
        std::thread guiThread(GUI_Loop);
        captureThread.detach();
        guiThread.detach();
    }

    void Shutdown() {
        captureThreadRunning.store(false);
        frameCV.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void SetBindState(bool held) {
        bool prev = isBindHeld.exchange(held);
        if (prev && !held) {
            std::lock_guard<std::mutex> lock(targetStateMutex);
            currentTarget = Vector2(0, 0);
            previousTarget = Vector2(0, 0);
            remainderX = 0.0;
            remainderY = 0.0;
            stableFrames = 0;
            isOnTarget = false;
            onTargetFrames = 0;
        }
    }

    bool GetBindState() {
        return isBindHeld.load();
    }

    bool IsTriggerKeyHeld() {
        switch (g_Index::trigger_key) {
        case 0: return true; // ALWAYS ON
        case 1: return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;  // LEFT
        case 2: return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;  // RIGHT
        case 3: return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;  // MIDDLE
        case 4: return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0; // SIDE1
        case 5: return (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0; // SIDE2
        case 6: return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;     // ALT
        case 7: return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;    // SHIFT
        case 8: return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;  // CTRL
        default: return false;
        }
    }
}
