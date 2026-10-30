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
    Vector2 activeAimError(999.0, 999.0); // 當前準星與加完 Hitbox Offset 後目標點的即時誤差向量
    bool crosshairInsideEnemy = false;    // 準星是否位於敵人左右紫色輪廓包夾的身體/頭部實體內部 (Hollow Interior 判定)
    std::atomic<bool> triggerFiringLock{ false }; // 延遲擊發期間鎖定，防止重複排程點擊執行緒
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
    constexpr double DEADZONE_RADIUS = 2.5;
    constexpr double ON_TARGET_THRESHOLD = 8.0;
    constexpr int ON_TARGET_FRAMES_REQUIRED = 3;
    constexpr int MAX_STEP_PER_FRAME = 60;

    struct HitboxOffset {
        int x, y;
    };

    // 改回原本結構：修正原版 {0, -5} 向上偏移導致打在頭髮頭皮的問題
    const HitboxOffset HITBOX_OFFSETS[] = {
        {0, 0},  // HEAD: 鎖在臉部/眉心
        {0, 5},  // NECK: 脖子/下巴
        {0, 12}  // BODY: 身體
    };

    void InitializePurpleLUT() {
        int totalPurple = 0;
        for (int r = 0; r < 256; ++r) {
            for (int g = 0; g < 256; ++g) {
                for (int b = 0; b < 256; ++b) {
                    int maxRGB = (std::max)(r, (std::max)(g, b));
                    int minRGB = (std::min)(r, (std::min)(g, b));

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

                    // 原版標準色彩區間：精準過濾背景雜光，只留純正敵人紫色高亮外框
                    bool isPurple = (h >= 130 && h <= 170 && s >= 80 && v >= 120);
                    purpleLUT[r][g][b] = isPurple;
                    if (isPurple) totalPurple++;
                }
            }
        }
        timelog("[AimAssist] Standard Purple LUT initialized. Matching RGB count: %d", totalPurple);
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
            // 距離完全由「準星到該敵人絕對真頭部 (headX, headY)」的歐氏距離決定，不取身體混合權重！
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
            // 確認準星上方不遠處也有頭頂或肩頸外框封口，避免誤判兩名分開敵人之間的空隙
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

        // 4. 移植 jsonmeister/color-aimbot 多人同框防跳滯後保護 (Hysteresis)：
        // 若新目標與上一幀目標距離 > 30px（代表畫面中出現另一名敵人），且原鎖定目標仍存在（minDistToPrev < 25px）
        // 且原目標距離準星未遠於新目標的 1.25 倍，則持續鎖定原目標，防止多人同框時準星左右抽動。
        if (hasSmoothedRaw && lockedCandidate.valid) {
            double jumpDist = (rawCandidate - smoothedRawTarget).Len();
            if (jumpDist > 30.0 && minDistToPrev < 25.0 && lockedCandidate.distToCenter * 0.80 <= bestCandidate.distToCenter) {
                rawCandidate = lockedCandidate.headRelPos;
            }
        }

        // 5. 真零抖動硬體鎖死死區 + 移動敵人速度前饋補償 (Hard Deadband + Velocity Feed-Forward)：
        if (!hasSmoothedRaw) {
            smoothedRawTarget = rawCandidate;
            targetVelocity = Vector2(0, 0);
            hasSmoothedRaw = true;
        } else {
            Vector2 instVel = rawCandidate - smoothedRawTarget;
            double delta = instVel.Len();
            if (delta <= 1.5) {
                // 硬體級靜止死區：白點座標 100% 鎖定不動，速度衰減
                targetVelocity = targetVelocity * 0.5;
            } else if (delta < 4.5) {
                double alpha = (delta - 1.5) / 3.0;
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

        return Vector2(target.x + baseOffset.x + g_Index::aim_x_offset, target.y + baseOffset.y + g_Index::aim_y_offset);
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

        double smoothing = clamp(g_Index::kSmoothingFactor, 1, 99) * 0.01;
        double responsiveness = (std::max)(0.01, 1.0 - smoothing);

        if (isOnTarget) {
            responsiveness *= 0.5;
        }

        currentTarget = currentTarget + (targetPos - currentTarget) * responsiveness;
        previousTarget = targetPos;

        return currentTarget;
    }

    void ExecuteMovement(const Vector2& target) {
        double distance = target.Len();
        if (distance < DEADZONE_RADIUS) return;

        // 移植 jsonmeister/color-aimbot 距離分級平滑因子：
        // 遠距離全速拉槍 (1.0)，中近距離 (<25px) 減速至 0.75，貼近頭部 (<10px) 細膩微調 0.45 防止準星在頭頂左右過衝抖動
        double distScale = 1.0;
        if (distance < 10.0) {
            distScale = 0.45;
        } else if (distance < 25.0) {
            distScale = 0.75;
        }

        if ((target.x * remainderX) < 0.0) remainderX = 0.0;
        if ((target.y * remainderY) < 0.0) remainderY = 0.0;

        double moveX = target.x * g_Index::kSensitivity * 0.7 * distScale;
        double moveY = target.y * g_Index::kSensitivity * 0.7 * distScale;

        remainderX += moveX;
        remainderY += moveY;

        int rawStepX = static_cast<int>(std::lround(remainderX));
        int rawStepY = static_cast<int>(std::lround(remainderY));

        int stepX = clamp(rawStepX, -MAX_STEP_PER_FRAME, MAX_STEP_PER_FRAME);
        int stepY = clamp(rawStepY, -MAX_STEP_PER_FRAME, MAX_STEP_PER_FRAME);

        remainderX -= stepX;
        remainderY -= stepY;

        if (stepX != 0 || stepY != 0) {
            if (g_Index::showDebug) {
                timelog("[AimAssist] Move (%d, %d) | Target Dist: %.1f | Makcu: %s",
                    stepX, stepY, distance, g_Index::g_makcuDevice.getTargetAddress().c_str());
            }
            int duration = clamp(static_cast<int>(8.0 / g_Index::kSensitivity), 2, 12);
            if (isOnTarget) {
                duration = (std::min)(14, duration * 2);
            }
            // 呼叫具備 Makcu 官方分段線性 / 三次貝茲硬體插值的 mouseMoveBezier 達成最滑順手感！
            g_Index::g_makcuDevice.mouseMoveBezier(stepX, stepY, duration, stepX, stepY);
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

        if (!isBindHeld || !g_Index::assistance_enabled) {
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

        Vector2 smoothedTarget = ApplySmoothing(activeAimError);

        if (smoothedTarget.LenSquared() > 0.5) {
            ExecuteMovement(smoothedTarget);
        }
    }

    bool IsTriggerKeyHeld();

    void NDI_CaptureLoop() {
        const int capW = static_cast<int>(g_Index::kScreenWidth);  // 256
        const int capH = static_cast<int>(g_Index::kScreenHeight); // 256

        // 提升擷取與瞄準執行緒優先級至 HIGHEST，防止系統排程搶佔造成微卡頓
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

        // 使用 CreateDIBSection 建立零拷貝 (Zero-Copy) 共享記憶體：
        // BitBlt 完成後 CPU 可直接讀取 dibPixels，徹底省去 GetDIBits 與 cv::cvtColor 兩次全圖拷貝！
        void* dibPixels = nullptr;
        HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &dibPixels, NULL, 0);
        HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

        cv::Mat frameBGRA(capH, capW, CV_8UC4, dibPixels);

        // 啟用 Windows 1ms 高精度計時器，將處理迴圈提升至 240Hz (~4.16ms) 極低延遲
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

                {
                    makcu::MouseButton btn = makcu::MouseButton::RIGHT;
                    switch (g_Index::aim_key) {
                    case 0: btn = makcu::MouseButton::LEFT; break;
                    case 1: btn = makcu::MouseButton::RIGHT; break;
                    case 2: btn = makcu::MouseButton::SIDE1; break;
                    case 3: btn = makcu::MouseButton::SIDE2; break;
                    case 4: btn = makcu::MouseButton::ALT; break;
                    case 5: btn = makcu::MouseButton::SHIFT; break;
                    default: btn = makcu::MouseButton::RIGHT; break;
                    }
                    bool held = (g_Index::aim_key >= 0 && g_Index::aim_key <= 5) ? g_Makcu::isButtonHeld(btn) : false;
                    SetBindState(held);
                }

                // 直接對零拷貝 BGRA 緩衝區執行目標定位與滑鼠移動，延遲降至最低
                ProcessFrame(frameBGRA);

                // --- Triggerbot 自動開火邏輯 (遵循 Makcu 官方 API km.click) ---
                static auto lastTriggerTime = clock::now();
                static int triggerFlashFrames = 0;
                const int cx = capW >> 1;
                const int cy = capH >> 1;

                if (g_Index::trigger_enabled && IsTriggerKeyHeld()) {
                    int tfov = (std::max)(1, (std::min)(g_Index::trigger_fov, 20));
                    // 採用橢圓形人體工學判定區 (垂直容許度 1.6x 水平半徑，移植 Colorant x<=4, y<=10 思維)
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

                    // --- Aim + Trigger 協同判定引擎 (Synergy Engine) ---
                    // 條件 A (直接外框觸發): 準星橢圓內有 >= 2 個紫色像素
                    bool outlineHit = (purpleInCrosshair >= 2);
                    // 條件 B (中空臉部/胸口鎖定觸發): Aim 已鎖定有效目標，且準星已經被 Aim 拉進目標點的核心半徑內，或位於左右紫框包夾的實體內部
                    double aimErrX = std::abs(activeAimError.x);
                    double aimErrY = std::abs(activeAimError.y);
                    bool aimLockedHit = hasValidTarget && (
                        (aimErrX <= tfov + 1.5 && aimErrY <= tfovY + 1.5) ||
                        (crosshairInsideEnemy && aimErrX <= tfov + 3.5 && aimErrY <= tfovY + 4.0)
                    );

                    // 條件 C (防甩槍早洩保護 Anti-Early-Fire Gate):
                    // 當玩家正按住 Aim 鍵高速甩槍 (Flicking) 時，若準星距離目標 Hitbox (如頭心) 還很遠 (> tfov * 2.2)，
                    // 即使準星半路擦過敵人的手臂或肩膀外框，也強制扣住板機，等 Aim 拉到頭心到位的那一毫秒才開火！
                    bool aimFlickGatePass = true;
                    if (g_Index::assistance_enabled && isBindHeld.load() && hasValidTarget) {
                        double maxAllowedAimDist = (std::max)(6.5, static_cast<double>(tfov) * 2.2);
                        if (activeAimError.Len() > maxAllowedAimDist) {
                            aimFlickGatePass = false; // 還在半路甩槍中，等待進圈再擊發
                        }
                    }

                    auto now = clock::now();
                    auto cooldownDur = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTriggerTime).count();

                    if ((outlineHit || aimLockedHit) && aimFlickGatePass && !triggerFiringLock.load() && cooldownDur >= g_Index::trigger_cooldown_ms) {
                        lastTriggerTime = now;
                        lastShotTimestamp = now;
                        triggerFlashFrames = 12;

                        if (g_Index::showDebug) {
                            timelog("[Trigger+Aim] FIRE! (OutlinePx: %d | AimErr: %.1f,%.1f | InsideBody: %s)",
                                purpleInCrosshair, activeAimError.x, activeAimError.y, crosshairInsideEnemy ? "YES" : "NO");
                        }

                        // 若 Delay == 0，直接在當前微秒級迴圈同步發送 UDP 點擊封包，達成 0.0ms 極限擊發延遲！
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
                                // 延遲結束後二次確認目標仍在準星附近才扣板機，避免敵人已閃進掩體還空放槍
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
                        isBindHeld ? "YES" : "NO",
                        debugPixelCount,
                        hasValidTarget ? "LOCKED" : "SEARCHING");
                }

                // 僅在開啟 Debug 預覽且 GUI 執行緒準備好接收新幀時，才進行顯示畫面渲染，平時 0 額外開銷
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

                    // 繪製 FOV 圓圈
                    cv::Scalar fovColor = isBindHeld ? cv::Scalar(0, 255, 255) : cv::Scalar(255, 255, 0);
                    cv::circle(display, cv::Point(cx, cy), g_Index::kFOV, fovColor, 1);

                    // 繪製螢幕中心準心
                    cv::line(display, cv::Point(cx - 8, cy), cv::Point(cx + 8, cy), cv::Scalar(0, 0, 255), 1);
                    cv::line(display, cv::Point(cx, cy - 8), cv::Point(cx, cy + 8), cv::Scalar(0, 0, 255), 1);

                    // 繪製 Trigger Zone 判定圈 (採用鮮明洋紅色 Magenta 區分 Aim FOV；開火時閃爍亮紅)
                    if (g_Index::trigger_enabled || g_Index::showDebug) {
                        cv::Scalar trigColor;
                        int trigThickness = 1;

                        if (triggerFlashFrames > 0) {
                            trigColor = cv::Scalar(0, 0, 255); // 觸發開火時：亮紅色
                            trigThickness = 2;
                            triggerFlashFrames--;
                            cv::putText(display, "FIRE!", cv::Point(cx + g_Index::trigger_fov + 4, cy - 4), cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 255), 2);
                        }
                        else if (g_Index::trigger_enabled) {
                            trigColor = cv::Scalar(255, 0, 255); // Triggerbot 啟用就緒：鮮明洋紅色 (Magenta)
                        }
                        else {
                            trigColor = cv::Scalar(90, 90, 90);  // Triggerbot 未啟用 (Debug 預覽)：灰色
                        }

                        cv::circle(display, cv::Point(cx, cy), g_Index::trigger_fov, trigColor, trigThickness);
                    }

                    // 繪製目標點
                    if (hasValidTarget) {
                        Vector2 offsetTarget = ApplyHitboxOffset(Vector2(debugTargetX, debugTargetY));
                        cv::Point targetPoint(cx + static_cast<int>(offsetTarget.x),
                            cy + static_cast<int>(offsetTarget.y));
                        cv::Scalar targetColor = isBindHeld ? cv::Scalar(0, 255, 0) : cv::Scalar(255, 255, 255);
                        cv::circle(display, targetPoint, 4, targetColor, -1);
                        cv::line(display, cv::Point(cx, cy), targetPoint, targetColor, 1);
                    }

                    // 繪製即時除錯 HUD 文字
                    if (g_Index::showDebug) {
                        std::string hudAssist = std::string("AIM: ") + (g_Index::assistance_enabled ? "ON" : "OFF");
                        std::string hudBind = std::string("KEY HELD: ") + (isBindHeld ? "YES" : "NO");
                        std::string hudTrigger = std::string("TRIGGER: ") + (g_Index::trigger_enabled ? "ON" : "OFF") + " [FOV: " + std::to_string(g_Index::trigger_fov) + "px]";
                        std::string hudPixels = "PURPLE PX: " + std::to_string(debugPixelCount);
                        std::string hudTarget = hasValidTarget ? ("TARGET: " + std::to_string(static_cast<int>(debugTargetX)) + "," + std::to_string(static_cast<int>(debugTargetY))) : "TARGET: NONE";

                        cv::putText(display, hudAssist, cv::Point(5, 15), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            g_Index::assistance_enabled ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255), 1);
                        cv::putText(display, hudBind, cv::Point(5, 30), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                            isBindHeld ? cv::Scalar(0, 255, 0) : cv::Scalar(200, 200, 200), 1);
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

        timeEndPeriod(1); // 釋放 1ms 高精度計時器
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
        return isBindHeld;
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
