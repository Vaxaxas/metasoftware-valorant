#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <functional>
#include <unordered_map>
#include <string>
#include <cmath>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

namespace makcu {
    enum class MouseButton {
        LEFT = 0,
        RIGHT = 1,
        MIDDLE = 2,
        SIDE1 = 3,
        SIDE2 = 4,
        ALT = 5,
        SHIFT = 6
    };

    class Device {
    public:
        Device();
        ~Device();

        bool connect();
        bool connect(const std::string& host, int port);
        void disconnect();
        bool isConnected() const;
        bool isUdpMode() const { return m_useUdp; }

        void setMouseButtonCallback(std::function<void(MouseButton, bool)> callback);
        void mouseDown(MouseButton button);
        void mouseUp(MouseButton button);
        void click(int button = 1, int count = 1, int hold_ms = 50);
        void mouseMove(int dx, int dy);
        void mouseMoveTo(int absX, int absY);
        void mouseSilent(int absX, int absY);
        void mouseMoveBezier(double x, double y, int duration_ms, double targetX, double targetY);

        std::string getTargetAddress() const;

    private:
        void sendCommand(const std::string& cmd);

        bool m_connected;
        bool m_useUdp;
        SOCKET m_udpSocket;
        sockaddr_in m_udpAddr;
        std::string m_targetHost;
        int m_targetPort;
        std::mutex m_sendMutex;
        std::function<void(MouseButton, bool)> m_buttonCallback;
    };
}

namespace g_Makcu {
    void mouseButtonHandler(makcu::MouseButton button, bool pressed);
    bool isButtonHeld(makcu::MouseButton button);
}
